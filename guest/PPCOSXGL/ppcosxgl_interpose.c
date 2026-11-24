/*
 * libPPCOSXGL: redirects an unmodified application's OpenGL to the host GPU.
 *
 * The dynamic loader inserts this library into an application
 * (DYLD_INSERT_LIBRARIES) and, through the __interpose table at the end of
 * this file, rebinds the application's calls to Apple's gl*() functions to
 * the pxi_gl*() wrappers here.
 *
 * For a context that draws into a window:
 *   - state-setting calls go to the host AND on to Apple's software
 *     renderer, which therefore keeps correct state and answers every
 *     query, but
 *   - drawing calls go to the host only, so the software renderer never
 *     rasterises anything, and
 *   - at buffer swap the host's finished frame goes to the host overlay
 *     for the window (see ppcosx-gpu.h): the window surface is filled once
 *     with the overlay's key colour and Apple's swap is skipped, so the
 *     window server still decides what of the window is visible. Where
 *     overlays are not possible the frame is instead copied back into the
 *     window's back buffer with Apple's glDrawPixels before Apple's swap.
 *
 * Vertex arrays are mirrored here and sent whole with each draw. A context
 * that uses anything the host path lacks is handed back to Apple's renderer
 * permanently (pxi_unsupported).
 *
 * Contexts drawing to a pixel buffer, offscreen memory or the full screen
 * are left to Apple's renderer.
 * Calls made from inside this library are not interposed, so gl*() below
 * always means Apple's.
 *
 * Licensed under the GNU GPL, version 2 or later.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <objc/objc.h>
#include <objc/objc-class.h>
#include <objc/objc-runtime.h>
#include <ApplicationServices/ApplicationServices.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

#include "pxcore.h"
#include "ppcosx-gpu.h"
#include "ppcosx-gl-ops.h"
#include "ppcosx-gl-image.h"

/* Window-server surface lookups; exported by the system, not in headers. */
typedef int CGSConnectionID;
typedef int CGSWindowID;
typedef int CGSSurfaceID;
extern CGLError CGLGetSurface(CGLContextObj ctx, CGSConnectionID *cid,
                              CGSWindowID *wid, CGSSurfaceID *sid);
extern CGError CGSGetWindowBounds(CGSConnectionID cid, CGSWindowID wid,
                                  CGRect *bounds);
extern CGError CGSGetSurfaceBounds(CGSConnectionID cid, CGSWindowID wid,
                                   CGSSurfaceID sid, CGRect *bounds);

/* Objective-C runtime: drop cached method lookups for a class. */
extern void _objc_flush_caches(Class cls);

#define kMaxContexts    16
#define kMaxTries       1       /* attempts to redirect a context */
#define kPlaceholderSize 16     /* drawable size before a window exists */

#define kTexUnits       8

/* Where one client array lives, as the application last set it. */
typedef struct PXIArrayState {
    int enabled;
    int size;
    GLenum type;
    int stride;
    const void *pointer;
} PXIArrayState;

typedef struct PXISlot {
    CGLContextObj ctx;          /* NULL: free */
    PXConn *px;                 /* NULL: not redirected */
    int tries;
    int single;                 /* single-buffered: presents on glFlush */
    int keyed;                  /* window surface shows the overlay key */
    int rect[4];                /* where the overlay was last placed */

    /* Client vertex-array state, mirrored so draws can be sent whole. */
    PXIArrayState vertex, normal, color, texcoord[kTexUnits];
    int client_unit;            /* glClientActiveTexture, as an index */
    int arrays_stale;           /* re-read everything from Apple's GL */
} PXISlot;

static PXISlot gSlots[kMaxContexts];
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static int gDebug = -1;

static int pxi_debug(void)
{
    if (gDebug < 0)
        gDebug = getenv("PPCOSXGL_DEBUG") != 0;
    return gDebug;
}

/* Size of the context's window surface; 0 if it has none. */
static int pxi_surface_size(CGLContextObj ctx, int *width, int *height)
{
    CGSConnectionID cid = 0;
    CGSWindowID wid = 0;
    CGSSurfaceID sid = 0;
    CGRect bounds;

    if (CGLGetSurface(ctx, &cid, &wid, &sid) != kCGLNoError || !wid)
        return 0;
    if (CGSGetSurfaceBounds(cid, wid, sid, &bounds) != 0)
        return 0;
    *width = (int) bounds.size.width;
    *height = (int) bounds.size.height;
    return *width > 0 && *height > 0;
}

static void pxi_hook_appkit(void);

/*
 * The buffers the context's pixel format gave it. Asked of Apple's
 * renderer, with the context made current for the question.
 */
static uint32_t pxi_format(CGLContextObj ctx)
{
    CGLContextObj previous = CGLGetCurrentContext();
    GLint alpha = 0, depth = 0, stencil = 0;

    if (previous != ctx)
        CGLSetCurrentContext(ctx);
    glGetIntegerv(GL_ALPHA_BITS, &alpha);
    glGetIntegerv(GL_DEPTH_BITS, &depth);
    glGetIntegerv(GL_STENCIL_BITS, &stencil);
    if (previous != ctx)
        CGLSetCurrentContext(previous);
    return PPCOSX_GPU_GL_FORMAT(alpha, depth, stencil);
}

static PXISlot *pxi_find(CGLContextObj ctx)
{
    int i;

    for (i = 0; i < kMaxContexts; i++)
        if (gSlots[i].ctx == ctx)
            return &gSlots[i];
    return 0;
}

/* The host connection for a context, creating it when the context first
 * turns out to draw into a window. */
/* Off for this process (PPCOSXGL_DISABLE set) or for the whole system
 * (the marker file exists)? */
static int pxi_disabled(void)
{
    static int disabled = -1;

    if (disabled < 0)
        disabled = getenv("PPCOSXGL_DISABLE") != 0 ||
                   access("/Library/Preferences/org.ppcosx.gl.disabled",
                          F_OK) == 0;
    return disabled;
}

static PXConn *pxi_for_context(CGLContextObj ctx)
{
    PXISlot *slot;
    int width, height;

    if (!ctx || pxi_disabled())
        return 0;
    slot = pxi_find(ctx);
    if (slot && (slot->px || slot->tries >= kMaxTries))
        return slot->px;

    pthread_mutex_lock(&gLock);
    slot = pxi_find(ctx);
    if (!slot) {
        slot = pxi_find(0);
        if (slot) {
            memset(slot, 0, sizeof(*slot));
            slot->ctx = ctx;
        }
    }
    if (slot && !slot->px && slot->tries < kMaxTries) {
        GLboolean doubled = GL_FALSE;
        int sized = pxi_surface_size(ctx, &width, &height);

        /*
         * Follow the context from its first GL call, even before it has a
         * window: programs load textures and build display lists early, and
         * the host has to see that. The drawable gets its real size when
         * the window surface appears (pxi_sync_size).
         *
         * Double-buffered contexts show a frame at buffer swap,
         * single-buffered ones at glFlush / glFinish.
         */
        if (!sized)
            width = height = kPlaceholderSize;
        glGetBooleanv(GL_DOUBLEBUFFER, &doubled);
        slot->single = !doubled;
        slot->tries = kMaxTries;        /* one attempt per context */
        /* Until there is a window the format may not be final either;
         * pxi_sync_size updates both. */
        slot->px = px_open(width, height, pxi_format(ctx));
        if (slot->px) {
            slot->px->owner = slot;
            slot->arrays_stale = 1;
            pxi_hook_appkit();
        }
        if (pxi_debug())
            fprintf(stderr, "PPCOSXGL: context %p %dx%d%s, format %06x, "
                    "%s\n", ctx, width, height,
                    sized ? "" : " (no window yet)",
                    slot->px ? (unsigned) slot->px->format : 0,
                    !slot->px ? "NOT redirected (no host connection)" :
                    doubled ? "redirected to host" :
                    "redirected to host (single-buffered)");
    }
    pthread_mutex_unlock(&gLock);
    return slot ? slot->px : 0;
}

static PXConn *pxi_current(void)
{
    return pxi_for_context(CGLGetCurrentContext());
}

/*
 * A context used something the host path does not implement. Hand it back
 * to Apple's renderer for good: that renderer has seen
 * every state change, so it carries on correctly from the next frame.
 */
static void pxi_restore_window(CGLContextObj ctx, PXConn *px, int single);

static void pxi_decline(CGLContextObj ctx, const char *what)
{
    PXISlot *slot;
    PXConn *px = 0;
    int changed = 0, keyed = 0, single = 0;

    if (!ctx)
        return;
    pthread_mutex_lock(&gLock);
    slot = pxi_find(ctx);
    if (!slot) {
        /* Not looked at yet: record it so it is never redirected. */
        slot = pxi_find(0);
        if (slot) {
            memset(slot, 0, sizeof(*slot));
            slot->ctx = ctx;
        }
    }
    if (slot && (slot->px || slot->tries < kMaxTries)) {
        px = slot->px;
        keyed = slot->keyed;
        single = slot->single;
        slot->px = 0;
        slot->keyed = 0;
        slot->tries = kMaxTries;
        changed = 1;
    }
    pthread_mutex_unlock(&gLock);
    if (changed && pxi_debug())
        fprintf(stderr, "PPCOSXGL: context %p uses %s; %s the software "
                "renderer\n", ctx, what, px ? "back to" : "staying with");
    /* The window shows only the key colour so far; give it the last
     * frame before the overlay goes. */
    if (px && keyed)
        pxi_restore_window(ctx, px, single);
    px_close(px);
}

static void pxi_unsupported(const char *what)
{
    pxi_decline(CGLGetCurrentContext(), what);
}

/* Follow the window: its size, and the buffers it ended up with. Returns 1
 * if the drawable changed. */
static int pxi_sync_size(CGLContextObj ctx, PXConn *px)
{
    int width, height;
    uint32_t format;

    if (!pxi_surface_size(ctx, &width, &height))
        return 0;
    format = pxi_format(ctx);
    if (width == px->width && height == px->height && format == px->format)
        return 0;
    px_resize(px, width, height, format);
    if (pxi_debug())
        fprintf(stderr, "PPCOSXGL: context %p now %dx%d, format %06x\n",
                ctx, width, height, (unsigned) format);
    return 1;
}

/*
 * Testing aid: PPCOSXGL_MATCH_SOFTWARE=1 makes the host copy known bugs of
 * Apple's software renderer, so the two can be compared pixel for pixel.
 * They are departures from the OpenGL spec that real graphics hardware
 * does not have, so this is never for normal use.
 *
 *   - With GL_SEPARATE_SPECULAR_COLOR the specular term is dropped
 *     entirely instead of being added after texturing. Copied by giving
 *     the host's lights no specular while that mode is on.
 */
static int pxi_match_software(void)
{
    static int match = -1;

    if (match < 0)
        match = getenv("PPCOSXGL_MATCH_SOFTWARE") != 0;
    return match;
}

/* Called before each primitive is drawn on the host. */
static void pxi_before_draw(PXConn *px)
{
    GLint control = GL_SINGLE_COLOR, lights = 8;
    int i;

    if (!pxi_match_software() || px->compiling)
        return;
    glGetIntegerv(GL_LIGHT_MODEL_COLOR_CONTROL, &control);
    glGetIntegerv(GL_MAX_LIGHTS, &lights);
    if (lights > 8)
        lights = 8;
    for (i = 0; i < lights; i++) {
        GLfloat specular[4] = { 0, 0, 0, 0 };
        uint32_t *w;
        int k;

        if (control != GL_SEPARATE_SPECULAR_COLOR)
            glGetLightfv(GL_LIGHT0 + i, GL_SPECULAR, specular);
        w = px_emit(px, PPCOSX_GL_Lightfv, 6);
        if (!w)
            return;
        w = wr_u(w, GL_LIGHT0 + i);
        w = wr_u(w, GL_SPECULAR);
        for (k = 0; k < 4; k++)
            w = wr_f(w, specular[k]);
    }
}

#include "gl_interpose.inc"

/* Applications set the viewport when their window changes size, which is
 * the earliest point at which the host drawable can follow. */
static void pxi_glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    PXConn *px = pxi_current();

    if (px) {
        uint32_t *w;

        pxi_sync_size(CGLGetCurrentContext(), px);
        w = px_emit(px, PPCOSX_GL_Viewport, 4);
        if (w) {
            w = wr_u(w, x);
            w = wr_u(w, y);
            w = wr_u(w, width);
            w = wr_u(w, height);
        }
    }
    glViewport(x, y, width, height);
}

/* ---- vertex arrays ------------------------------------------------------ */

static PXISlot *pxi_slot(PXConn *px)
{
    return (PXISlot *) px->owner;
}

static void pxi_read_array(PXIArrayState *a, GLenum enable, GLenum size,
                           GLenum type, GLenum stride, GLenum pointer)
{
    GLint v = 0;
    GLvoid *p = 0;

    a->enabled = glIsEnabled(enable);
    if (size) {
        glGetIntegerv(size, &v);
        a->size = v;
    } else {
        a->size = 3;                    /* normals */
    }
    glGetIntegerv(type, &v);
    a->type = (GLenum) v;
    glGetIntegerv(stride, &v);
    a->stride = v;
    glGetPointerv(pointer, &p);
    a->pointer = p;
}

/* Take the whole array state from Apple's renderer, which has seen every
 * call. Needed at the start and after calls that change it wholesale. */
static void pxi_refresh_arrays(PXISlot *slot)
{
    GLint unit = GL_TEXTURE0, units = 1;
    int i;

    pxi_read_array(&slot->vertex, GL_VERTEX_ARRAY, GL_VERTEX_ARRAY_SIZE,
                   GL_VERTEX_ARRAY_TYPE, GL_VERTEX_ARRAY_STRIDE,
                   GL_VERTEX_ARRAY_POINTER);
    pxi_read_array(&slot->normal, GL_NORMAL_ARRAY, 0, GL_NORMAL_ARRAY_TYPE,
                   GL_NORMAL_ARRAY_STRIDE, GL_NORMAL_ARRAY_POINTER);
    pxi_read_array(&slot->color, GL_COLOR_ARRAY, GL_COLOR_ARRAY_SIZE,
                   GL_COLOR_ARRAY_TYPE, GL_COLOR_ARRAY_STRIDE,
                   GL_COLOR_ARRAY_POINTER);

    glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &unit);
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
    if (units > kTexUnits)
        units = kTexUnits;
    for (i = 0; i < kTexUnits; i++)
        slot->texcoord[i].enabled = 0;
    for (i = 0; i < units; i++) {
        glClientActiveTexture(GL_TEXTURE0 + i);
        pxi_read_array(&slot->texcoord[i], GL_TEXTURE_COORD_ARRAY,
                       GL_TEXTURE_COORD_ARRAY_SIZE,
                       GL_TEXTURE_COORD_ARRAY_TYPE,
                       GL_TEXTURE_COORD_ARRAY_STRIDE,
                       GL_TEXTURE_COORD_ARRAY_POINTER);
    }
    glClientActiveTexture(unit);
    slot->client_unit = unit - GL_TEXTURE0;
    slot->arrays_stale = 0;
}

static PXIArrayState *pxi_array_for(PXISlot *slot, GLenum cap)
{
    switch (cap) {
    case GL_VERTEX_ARRAY:
        return &slot->vertex;
    case GL_NORMAL_ARRAY:
        return &slot->normal;
    case GL_COLOR_ARRAY:
        return &slot->color;
    case GL_TEXTURE_COORD_ARRAY:
        if (slot->client_unit >= 0 && slot->client_unit < kTexUnits)
            return &slot->texcoord[slot->client_unit];
        return 0;
    default:
        return 0;
    }
}

static void pxi_glEnableClientState(GLenum cap)
{
    PXConn *px = pxi_current();

    if (px) {
        PXIArrayState *a = pxi_array_for(pxi_slot(px), cap);

        if (a)
            a->enabled = 1;
        else
            pxi_unsupported("a client array the host path lacks");
    }
    glEnableClientState(cap);
}

static void pxi_glDisableClientState(GLenum cap)
{
    PXConn *px = pxi_current();

    if (px) {
        PXIArrayState *a = pxi_array_for(pxi_slot(px), cap);

        if (a)
            a->enabled = 0;
    }
    glDisableClientState(cap);
}

static void pxi_set_array(GLenum cap, GLint size, GLenum type, GLsizei stride,
                          const GLvoid *pointer)
{
    PXConn *px = pxi_current();

    if (px) {
        PXIArrayState *a = pxi_array_for(pxi_slot(px), cap);

        if (a) {
            a->size = size;
            a->type = type;
            a->stride = stride;
            a->pointer = pointer;
        }
    }
}

static void pxi_glVertexPointer(GLint size, GLenum type, GLsizei stride,
                                const GLvoid *pointer)
{
    pxi_set_array(GL_VERTEX_ARRAY, size, type, stride, pointer);
    glVertexPointer(size, type, stride, pointer);
}

static void pxi_glNormalPointer(GLenum type, GLsizei stride,
                                const GLvoid *pointer)
{
    pxi_set_array(GL_NORMAL_ARRAY, 3, type, stride, pointer);
    glNormalPointer(type, stride, pointer);
}

static void pxi_glColorPointer(GLint size, GLenum type, GLsizei stride,
                               const GLvoid *pointer)
{
    pxi_set_array(GL_COLOR_ARRAY, size, type, stride, pointer);
    glColorPointer(size, type, stride, pointer);
}

static void pxi_glTexCoordPointer(GLint size, GLenum type, GLsizei stride,
                                  const GLvoid *pointer)
{
    pxi_set_array(GL_TEXTURE_COORD_ARRAY, size, type, stride, pointer);
    glTexCoordPointer(size, type, stride, pointer);
}

static void pxi_glClientActiveTexture(GLenum texture)
{
    PXConn *px = pxi_current();

    if (px)
        pxi_slot(px)->client_unit = (int) texture - GL_TEXTURE0;
    glClientActiveTexture(texture);
}

static void pxi_glClientActiveTextureARB(GLenum texture)
{
    pxi_glClientActiveTexture(texture);
}

/* These replace the array state in ways not worth following call by call. */
static void pxi_glInterleavedArrays(GLenum format, GLsizei stride,
                                    const GLvoid *pointer)
{
    PXConn *px = pxi_current();

    if (px)
        pxi_slot(px)->arrays_stale = 1;
    glInterleavedArrays(format, stride, pointer);
}

static void pxi_glPopClientAttrib(void)
{
    PXConn *px = pxi_current();

    if (px)
        pxi_slot(px)->arrays_stale = 1;
    glPopClientAttrib();
}

/* Array data in buffer objects is not followed; plain memory only. */
static void pxi_glBindBuffer(GLenum target, GLuint buffer)
{
    if (buffer)
        pxi_unsupported("vertex buffer objects");
    glBindBuffer(target, buffer);
}

static void pxi_glBindBufferARB(GLenum target, GLuint buffer)
{
    if (buffer)
        pxi_unsupported("vertex buffer objects");
    glBindBufferARB(target, buffer);
}

/*
 * Send one draw to the host. Returns 1 if the host has it and the software
 * renderer should skip it, 0 if the caller should make the real call (not
 * redirected, compiling a display list, or just fell back).
 */
static int pxi_draw(GLenum mode, GLint first, GLsizei count,
                    GLenum index_type, const GLvoid *indices)
{
    PXConn *px = pxi_current();
    PXArray arrays[PPCOSX_GL_MAX_ARRAYS];
    PXISlot *slot;
    int n = 0, i;

    if (!px)
        return 0;
    slot = pxi_slot(px);
    if (slot->arrays_stale)
        pxi_refresh_arrays(slot);

#define PXI_ADD(state, array_kind) \
    if ((state).enabled) { \
        arrays[n].kind = (array_kind); \
        arrays[n].size = (state).size; \
        arrays[n].type = (state).type; \
        arrays[n].stride = (state).stride; \
        arrays[n].pointer = (state).pointer; \
        n++; \
    }
    PXI_ADD(slot->vertex, PPCOSX_GL_ARRAY_VERTEX)
    PXI_ADD(slot->normal, PPCOSX_GL_ARRAY_NORMAL)
    PXI_ADD(slot->color, PPCOSX_GL_ARRAY_COLOR)
    for (i = 0; i < kTexUnits; i++)
        PXI_ADD(slot->texcoord[i], PPCOSX_GL_ARRAY_TEXCOORD0 + i)
#undef PXI_ADD

    pxi_before_draw(px);

    /* Without a vertex array GL draws nothing, and so do we. */
    if (slot->vertex.enabled &&
        px_draw(px, mode, first, count, index_type, indices, arrays, n) != 0) {
        pxi_unsupported("a vertex array layout the host path lacks");
        return 0;
    }
    return !px->compiling;
}

static void pxi_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    if (!pxi_draw(mode, first, count, 0, 0))
        glDrawArrays(mode, first, count);
}

static void pxi_glDrawElements(GLenum mode, GLsizei count, GLenum type,
                               const GLvoid *indices)
{
    if (!indices || !pxi_draw(mode, 0, count, type, indices))
        glDrawElements(mode, count, type, indices);
}

static void pxi_glDrawRangeElements(GLenum mode, GLuint start, GLuint end,
                                    GLsizei count, GLenum type,
                                    const GLvoid *indices)
{
    if (!indices || !pxi_draw(mode, 0, count, type, indices))
        glDrawRangeElements(mode, start, end, count, type, indices);
}

static void pxi_glDrawRangeElementsEXT(GLenum mode, GLuint start, GLuint end,
                                       GLsizei count, GLenum type,
                                       const GLvoid *indices)
{
    pxi_glDrawRangeElements(mode, start, end, count, type, indices);
}

static void pxi_glMultiDrawArrays(GLenum mode, const GLint *first,
                                  const GLsizei *count, GLsizei primcount)
{
    GLsizei i;

    for (i = 0; i < primcount; i++)
        pxi_glDrawArrays(mode, first[i], count[i]);
}

static void pxi_glMultiDrawArraysEXT(GLenum mode, const GLint *first,
                                     const GLsizei *count, GLsizei primcount)
{
    pxi_glMultiDrawArrays(mode, first, count, primcount);
}

static void pxi_glMultiDrawElements(GLenum mode, const GLsizei *count,
                                    GLenum type, const GLvoid **indices,
                                    GLsizei primcount)
{
    GLsizei i;

    for (i = 0; i < primcount; i++)
        pxi_glDrawElements(mode, count[i], type, indices[i]);
}

static void pxi_glMultiDrawElementsEXT(GLenum mode, const GLsizei *count,
                                       GLenum type, const GLvoid **indices,
                                       GLsizei primcount)
{
    pxi_glMultiDrawElements(mode, count, type, indices, primcount);
}

static void pxi_glNewList(GLuint list, GLenum mode)
{
    PXConn *px = pxi_current();

    if (px) {
        uint32_t *w = px_emit(px, PPCOSX_GL_NewList, 2);

        if (w) {
            w = wr_u(w, list);
            w = wr_u(w, mode);
        }
        px->compiling = 1;
    }
    glNewList(list, mode);
}

static void pxi_glEndList(void)
{
    PXConn *px = pxi_current();

    if (px) {
        px_emit(px, PPCOSX_GL_EndList, 0);
        px->compiling = 0;
    }
    glEndList();
}

/* The application's pixel layout, as Apple's renderer has recorded it. */
static void pxi_get_unpack(PXUnpack *unpack)
{
    GLint v[5] = { 4, 0, 0, 0, 0 };

    glGetIntegerv(GL_UNPACK_ALIGNMENT, &v[0]);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &v[1]);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &v[2]);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &v[3]);
    glGetIntegerv(GL_UNPACK_SWAP_BYTES, &v[4]);
    unpack->alignment = v[0];
    unpack->row_length = v[1];
    unpack->skip_rows = v[2];
    unpack->skip_pixels = v[3];
    unpack->swap_bytes = v[4];
}

static void pxi_glTexImage2D(GLenum target, GLint level, GLenum internalformat,
                             GLsizei width, GLsizei height, GLint border,
                             GLenum format, GLenum type, const GLvoid *pixels)
{
    PXConn *px = pxi_current();

    if (px) {
        PXUnpack unpack;

        pxi_get_unpack(&unpack);
        if (px_tex_image_2d(px, &unpack, target, level, internalformat,
                            width, height, border, format, type, pixels) != 0)
            pxi_unsupported("a glTexImage2D format the host path lacks");
    }
    glTexImage2D(target, level, internalformat, width, height, border, format,
                 type, pixels);
}

static void pxi_glTexSubImage2D(GLenum target, GLint level, GLint xoffset,
                                GLint yoffset, GLsizei width, GLsizei height,
                                GLenum format, GLenum type,
                                const GLvoid *pixels)
{
    PXConn *px = pxi_current();

    if (px) {
        PXUnpack unpack;

        pxi_get_unpack(&unpack);
        if (px_tex_sub_image_2d(px, &unpack, target, level, xoffset, yoffset,
                                width, height, format, type, pixels) != 0)
            pxi_unsupported("a glTexSubImage2D format the host path lacks");
    }
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format,
                    type, pixels);
}

static void pxi_glDeleteTextures(GLsizei n, const GLuint *textures)
{
    PXConn *px = pxi_current();

    if (px)
        px_delete_textures(px, n, (const uint32_t *) textures);
    glDeleteTextures(n, textures);
}

#include "ppcosxgl_glu.inc"

/* The application's pack settings, as Apple's renderer has recorded them. */
static void pxi_get_pack(PXUnpack *pack)
{
    GLint v[5] = { 4, 0, 0, 0, 0 };

    glGetIntegerv(GL_PACK_ALIGNMENT, &v[0]);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &v[1]);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &v[2]);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &v[3]);
    glGetIntegerv(GL_PACK_SWAP_BYTES, &v[4]);
    pack->alignment = v[0];
    pack->row_length = v[1];
    pack->skip_rows = v[2];
    pack->skip_pixels = v[3];
    pack->swap_bytes = v[4];
}

/* The picture is on the host, so that is where pixels are read from. */
static void pxi_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                             GLenum format, GLenum type, GLvoid *pixels)
{
    PXConn *px = pxi_current();

    if (px && width > 0 && height > 0) {
        PXUnpack pack;

        pxi_get_pack(&pack);
        if (px_read_pixels(px, &pack, x, y, width, height, format, type,
                           pixels) == 0)
            return;
        pxi_unsupported("a glReadPixels format the host path lacks");
    }
    glReadPixels(x, y, width, height, format, type, pixels);
}

/* Selection and feedback need the software renderer to see the geometry. */
static GLint pxi_glRenderMode(GLenum mode)
{
    if (mode != GL_RENDER)
        pxi_unsupported("glRenderMode (selection or feedback)");
    return glRenderMode(mode);
}

/* The host draws into one buffer per context: the one that gets shown. */
static void pxi_glDrawBuffer(GLenum mode)
{
    PXConn *px = pxi_current();

    if (px && mode != (pxi_slot(px)->single ? GL_FRONT : GL_BACK))
        pxi_unsupported("glDrawBuffer away from the displayed buffer");
    glDrawBuffer(mode);
}

/* Put the host's frame into the buffer Apple's renderer shows next. */
static void pxi_blit_into(PXConn *px, const uint32_t *pixels, int single)
{
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_FOG);
    glDisable(GL_LIGHTING);
    glDisable(GL_DITHER);
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_TEXTURE_1D);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_TEXTURE_3D);
    glDisable(GL_TEXTURE_RECTANGLE_EXT);
    glDisable(GL_TEXTURE_CUBE_MAP);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);
    glDrawBuffer(single ? GL_FRONT : GL_BACK);
    glPixelZoom(1.0f, 1.0f);
    glPixelTransferi(GL_MAP_COLOR, GL_FALSE);
    glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
    glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    glWindowPos2i(0, 0);
    glDrawPixels(px->width, px->height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                 pixels);

    glPopClientAttrib();
    glPopAttrib();
}

/* Before a real buffer swap: fetch the host's frame into the back buffer. */
static void pxi_blit(PXConn *px, const uint32_t *pixels)
{
    pxi_blit_into(px, pixels, ((PXISlot *) px->owner)->single);
}

/* Draw the host's latest frame into the window through Apple's renderer
 * and show it, for a context about to leave the host. */
static void pxi_restore_window(CGLContextObj ctx, PXConn *px, int single)
{
    CGLContextObj previous = CGLGetCurrentContext();
    const uint32_t *pixels;
    int width, height;

    if (!pxi_surface_size(ctx, &width, &height) || width != px->width ||
        height != px->height || !(pixels = px_readback(px)))
        return;
    if (previous != ctx)
        CGLSetCurrentContext(ctx);
    pxi_blit_into(px, pixels, single);
    if (single)
        glFlush();
    else
        CGLFlushDrawable(ctx);
    if (previous != ctx)
        CGLSetCurrentContext(previous);
}

/* Copy-back presentation: put the host's frame into the window's back
 * buffer through Apple's renderer, before Apple's own swap. */
static void pxi_copy_back(CGLContextObj ctx, PXConn *px)
{
    CGLContextObj previous;
    const uint32_t *pixels;
    int width, height;

    /* A frame drawn before a resize was noticed has the wrong size; drop
     * it rather than show it stretched or torn. Nothing can be shown
     * before the window exists either. */
    if (pxi_sync_size(ctx, px) || !pxi_surface_size(ctx, &width, &height) ||
        width != px->width || height != px->height)
        return;
    pixels = px_readback(px);
    if (!pixels) {
        if (pxi_debug() == 1) {
            gDebug = 2;
            fprintf(stderr, "PPCOSXGL: readback failed (link %s)\n",
                    px->failed ? "down" : "up");
        }
        return;
    }

    previous = CGLGetCurrentContext();
    if (previous != ctx)
        CGLSetCurrentContext(ctx);
    pxi_blit(px, pixels);
    if (pxi_debug() == 1) {
        gDebug = 2;             /* report the first frame only */
        fprintf(stderr, "PPCOSXGL: first frame %dx%d, pixels %08x %08x "
                "(centre), GL error 0x%x\n", px->width, px->height,
                (unsigned) pixels[0],
                (unsigned) pixels[(px->height / 2) * px->width +
                                  px->width / 2],
                (unsigned) glGetError());
    }
    if (previous != ctx)
        CGLSetCurrentContext(previous);
}

/* A single-buffered context shows what it has drawn when it flushes. */
/* ---- presentation through host overlays --------------------------------- */

/*
 * Whether frames can go straight to the host's overlays (see ppcosx-gpu.h).
 * Only with a single display, ours: overlays are composited onto our
 * scanout. PPCOSXGL_COPYBACK=1 forces copy-back, which copies each frame
 * back into the window through Apple's renderer.
 */
static int pxi_overlay_mode(void)
{
    static int mode = -1;

    if (mode < 0) {
        CGDirectDisplayID ids[2];
        CGDisplayCount count = 0;

        mode = 0;
        if (!getenv("PPCOSXGL_COPYBACK") &&
            CGGetActiveDisplayList(2, ids, &count) == kCGErrorSuccess &&
            count == 1) {
            io_service_t port = CGDisplayIOServicePort(ids[0]);

            mode = port && IOObjectConformsTo(port, "PPCOSXFramebuffer");
        }
        if (pxi_debug())
            fprintf(stderr, "PPCOSXGL: presenting through %s\n",
                    mode ? "host overlays" : "copy-back");
    }
    return mode;
}

/* The context's window surface on screen, as x, y, width, height in
 * display pixels from the top left. */
static int pxi_surface_rect(CGLContextObj ctx, int r[4])
{
    CGSConnectionID cid = 0;
    CGSWindowID wid = 0;
    CGSSurfaceID sid = 0;
    CGRect window, surface;

    if (CGLGetSurface(ctx, &cid, &wid, &sid) != kCGLNoError || !wid ||
        CGSGetWindowBounds(cid, wid, &window) != 0 ||
        CGSGetSurfaceBounds(cid, wid, sid, &surface) != 0)
        return 0;
    r[0] = (int) (window.origin.x + surface.origin.x);
    r[1] = (int) (window.origin.y + surface.origin.y);
    r[2] = (int) surface.size.width;
    r[3] = (int) surface.size.height;
    if (pxi_debug() == 1) {
        static int told;

        if (!told) {
            told = 1;
            fprintf(stderr, "PPCOSXGL: window at %g,%g %gx%g, surface at "
                    "%g,%g %gx%g\n", window.origin.x, window.origin.y,
                    window.size.width, window.size.height,
                    surface.origin.x, surface.origin.y, surface.size.width,
                    surface.size.height);
        }
    }
    return r[2] > 0 && r[3] > 0;
}

/* Fill the window surface with the context's key colour, through Apple's
 * renderer, so the host shows the overlay wherever the window is visible. */
static void pxi_key_surface(CGLContextObj ctx, PXISlot *slot)
{
    CGLContextObj previous = CGLGetCurrentContext();
    GLfloat green = slot->px->ctx_id / 255.0f;
    int pass;

    if (previous != ctx)
        CGLSetCurrentContext(ctx);
    /* Both buffers of a double-buffered context, in case they alternate. */
    for (pass = 0; pass < (slot->single ? 1 : 2); pass++) {
        glPushAttrib(GL_COLOR_BUFFER_BIT | GL_SCISSOR_BIT);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DITHER);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDrawBuffer(slot->single ? GL_FRONT : GL_BACK);
        glClearColor(1.0f, green, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glPopAttrib();
        if (slot->single)
            glFlush();
        else
            CGLFlushDrawable(ctx);
    }
    if (previous != ctx)
        CGLSetCurrentContext(previous);
}

/*
 * Follows window moves that happen without a new frame, such as a static
 * picture being dragged around. Until it catches up the window shows its
 * key colour at the new position.
 */
static void *pxi_tracker(void *unused)
{
    for (;;) {
        int i;

        usleep(50 * 1000);
        pthread_mutex_lock(&gLock);
        for (i = 0; i < kMaxContexts; i++) {
            PXISlot *slot = &gSlots[i];
            int r[4];

            if (!slot->ctx || !slot->px || !slot->keyed)
                continue;
            if (pxi_surface_rect(slot->ctx, r) &&
                memcmp(r, slot->rect, sizeof(r)) != 0) {
                px_set_overlay(slot->px, r[0], r[1], r[2], r[3], 1);
                memcpy(slot->rect, r, sizeof(r));
            }
        }
        pthread_mutex_unlock(&gLock);
    }
    return 0;
}

static void pxi_start_tracker(void)
{
    static int started;
    pthread_t thread;

    if (started)
        return;
    started = 1;
    if (pthread_create(&thread, 0, pxi_tracker, 0) == 0)
        pthread_detach(thread);
}

/* Overlay presentation. Returns 1 if Apple's own swap should be skipped. */
static int pxi_overlay_present(CGLContextObj ctx, PXISlot *slot)
{
    PXConn *px = slot->px;
    int resized = pxi_sync_size(ctx, px);
    int r[4];

    if (!pxi_surface_rect(ctx, r))
        return 0;               /* no window yet: nothing to show */
    if (resized)
        slot->keyed = 0;        /* the surface changed with the window */
    if (!slot->keyed) {
        pxi_key_surface(ctx, slot);
        slot->keyed = 1;
    }

    pthread_mutex_lock(&gLock);
    if (memcmp(r, slot->rect, sizeof(r)) != 0) {
        px_set_overlay(px, r[0], r[1], r[2], r[3], 1);
        memcpy(slot->rect, r, sizeof(r));
    }
    pthread_mutex_unlock(&gLock);

    /* A drawable that was just resized holds nothing worth showing. */
    if (!resized)
        px_overlay_frame(px);
    pxi_start_tracker();
    return 1;
}

/* Show the frame just finished in ctx. Returns 1 if Apple's own swap or
 * flush should be skipped. */
static int pxi_show(CGLContextObj ctx)
{
    PXISlot *slot = ctx ? pxi_find(ctx) : 0;
    PXConn *px = slot ? slot->px : 0;

    if (!px)
        return 0;
    if (pxi_overlay_mode())
        return pxi_overlay_present(ctx, slot);
    pxi_copy_back(ctx, px);
    return 0;
}

static void pxi_glFlush(void)
{
    PXConn *px = pxi_current();

    if (px) {
        px_emit(px, PPCOSX_GL_Flush, 0);
        if (pxi_slot(px)->single && pxi_show(CGLGetCurrentContext()))
            return;
    }
    glFlush();
}

static void pxi_glFinish(void)
{
    PXConn *px = pxi_current();

    if (px) {
        px_emit(px, PPCOSX_GL_Finish, 0);
        if (pxi_slot(px)->single && pxi_show(CGLGetCurrentContext()))
            return;
    }
    glFinish();
}

/* Programs that call CGL themselves (and AGL) swap through this. */
static CGLError pxi_CGLFlushDrawable(CGLContextObj ctx)
{
    if (pxi_show(ctx))
        return kCGLNoError;
    return CGLFlushDrawable(ctx);
}

/*
 * Cocoa and GLUT programs swap through -[NSOpenGLContext flushBuffer].
 * AppKit finds CGLFlushDrawable by name at run time, which dyld interposing
 * does not cover, so that method is replaced instead.
 */
static IMP gOrigFlushBuffer;
static IMP gOrigSetPixelBuffer, gOrigSetOffScreen, gOrigSetFullScreen;

static id pxi_flushBuffer(id self, SEL _cmd)
{
    CGLContextObj ctx = (CGLContextObj)
        objc_msgSend(self, sel_getUid("CGLContextObj"));

    if (pxi_show(ctx))
        return self;
    return gOrigFlushBuffer(self, _cmd);
}

/* Replace one NSOpenGLContext method, returning the original. */
static IMP pxi_swap_method(Class cls, const char *selector, IMP replacement)
{
    Method method = class_getInstanceMethod(cls, sel_getUid(selector));
    IMP original;

    if (!method)
        return 0;
    original = method->method_imp;
    method->method_imp = replacement;
    return original;
}

static id pxi_setPixelBuffer(id self, SEL _cmd, id pbuffer, GLenum face,
                             GLint level, GLint screen);
static id pxi_setOffScreen(id self, SEL _cmd, void *baseaddr, GLsizei width,
                           GLsizei height, GLint rowbytes);
static id pxi_setFullScreen(id self, SEL _cmd);

static void pxi_hook_appkit(void)
{
    static int done;
    Class cls;

    if (done)
        return;
    done = 1;
    cls = (Class) objc_lookUpClass("NSOpenGLContext");
    if (!cls)
        return;                 /* not a Cocoa program */
    gOrigFlushBuffer = pxi_swap_method(cls, "flushBuffer",
                                       (IMP) pxi_flushBuffer);
    gOrigSetPixelBuffer = pxi_swap_method(cls,
        "setPixelBuffer:cubeMapFace:mipMapLevel:currentVirtualScreen:",
        (IMP) pxi_setPixelBuffer);
    gOrigSetOffScreen = pxi_swap_method(cls,
        "setOffScreen:width:height:rowbytes:", (IMP) pxi_setOffScreen);
    gOrigSetFullScreen = pxi_swap_method(cls, "setFullScreen",
                                         (IMP) pxi_setFullScreen);
    _objc_flush_caches(cls);
}

/*
 * Pixel buffers, offscreen memory and full-screen drawables are read or
 * shown in ways the host path does not reproduce, so a context given one
 * goes back to Apple's renderer. CGL and AGL programs reach these
 * functions directly; Cocoa programs go through NSOpenGLContext.
 */
static CGLError pxi_CGLSetPBuffer(CGLContextObj ctx, CGLPBufferObj pbuffer,
                                  GLenum face, GLint level, GLint screen)
{
    pxi_decline(ctx, "a pixel buffer drawable");
    return CGLSetPBuffer(ctx, pbuffer, face, level, screen);
}

static CGLError pxi_CGLSetOffScreen(CGLContextObj ctx, GLsizei width,
                                    GLsizei height, GLint rowbytes,
                                    void *baseaddr)
{
    pxi_decline(ctx, "an offscreen drawable");
    return CGLSetOffScreen(ctx, width, height, rowbytes, baseaddr);
}

static CGLError pxi_CGLSetFullScreen(CGLContextObj ctx)
{
    pxi_decline(ctx, "a full-screen drawable");
    return CGLSetFullScreen(ctx);
}

static CGLContextObj pxi_cgl_of(id context)
{
    return (CGLContextObj) objc_msgSend(context, sel_getUid("CGLContextObj"));
}

static id pxi_setPixelBuffer(id self, SEL _cmd, id pbuffer, GLenum face,
                             GLint level, GLint screen)
{
    pxi_decline(pxi_cgl_of(self), "a pixel buffer drawable");
    return ((id (*)(id, SEL, id, GLenum, GLint, GLint)) gOrigSetPixelBuffer)
               (self, _cmd, pbuffer, face, level, screen);
}

static id pxi_setOffScreen(id self, SEL _cmd, void *baseaddr, GLsizei width,
                           GLsizei height, GLint rowbytes)
{
    pxi_decline(pxi_cgl_of(self), "an offscreen drawable");
    return ((id (*)(id, SEL, void *, GLsizei, GLsizei, GLint))
               gOrigSetOffScreen)(self, _cmd, baseaddr, width, height,
                                  rowbytes);
}

static id pxi_setFullScreen(id self, SEL _cmd)
{
    pxi_decline(pxi_cgl_of(self), "a full-screen drawable");
    return gOrigSetFullScreen(self, _cmd);
}

static CGLError pxi_CGLDestroyContext(CGLContextObj ctx)
{
    PXISlot *slot;
    PXConn *px = 0;

    pthread_mutex_lock(&gLock);
    slot = ctx ? pxi_find(ctx) : 0;
    if (slot) {
        px = slot->px;
        slot->px = 0;
        slot->ctx = 0;
    }
    pthread_mutex_unlock(&gLock);
    px_close(px);
    return CGLDestroyContext(ctx);
}

/* dyld reads this table when the library is inserted and rebinds every
 * other image's references from `original` to `replacement`. */
typedef struct pxi_interpose {
    const void *replacement;
    const void *original;
} pxi_interpose_t;

#define PXI_REPLACE(fn) { (const void *) pxi_##fn, (const void *) fn },

static const pxi_interpose_t pxi_interposers[]
    __attribute__((used, section("__DATA,__interpose"))) = {
    PXI_GL_REPLACEMENTS
    PXI_GLU_REPLACEMENTS
    PXI_REPLACE(glTexImage2D)
    PXI_REPLACE(glReadPixels)
    PXI_REPLACE(glTexSubImage2D)
    PXI_REPLACE(glDeleteTextures)
    PXI_REPLACE(glRenderMode)
    PXI_REPLACE(glDrawBuffer)
    PXI_REPLACE(glEnableClientState)
    PXI_REPLACE(glDisableClientState)
    PXI_REPLACE(glVertexPointer)
    PXI_REPLACE(glNormalPointer)
    PXI_REPLACE(glColorPointer)
    PXI_REPLACE(glTexCoordPointer)
    PXI_REPLACE(glClientActiveTexture)
    PXI_REPLACE(glClientActiveTextureARB)
    PXI_REPLACE(glInterleavedArrays)
    PXI_REPLACE(glPopClientAttrib)
    PXI_REPLACE(glBindBuffer)
    PXI_REPLACE(glBindBufferARB)
    PXI_REPLACE(glDrawArrays)
    PXI_REPLACE(glDrawElements)
    PXI_REPLACE(glDrawRangeElements)
    PXI_REPLACE(glDrawRangeElementsEXT)
    PXI_REPLACE(glMultiDrawArrays)
    PXI_REPLACE(glMultiDrawArraysEXT)
    PXI_REPLACE(glMultiDrawElements)
    PXI_REPLACE(glMultiDrawElementsEXT)
    PXI_REPLACE(CGLFlushDrawable)
    PXI_REPLACE(CGLSetPBuffer)
    PXI_REPLACE(CGLSetOffScreen)
    PXI_REPLACE(CGLSetFullScreen)
    PXI_REPLACE(CGLDestroyContext)
};
