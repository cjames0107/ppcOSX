/*
 * ppcosx-gpu host GL renderer (macOS).
 *
 * Guest GL calls are replayed 1:1 into legacy-profile CGL contexts. On
 * Apple silicon that profile is OpenGL 2.1 implemented by Apple on top of
 * Metal, which covers the fixed-function GL that PowerPC Mac OS X uses.
 *
 * Each guest context gets its own CGL context rendering into a private
 * framebuffer object. Guest calls can only reach the generated decoder's
 * entry points, none of which can rebind the framebuffer or change how
 * pixels are read back.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#define GL_SILENCE_DEPRECATION

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "hw/display/ppcosx-gpu.h"
#include "hw/display/ppcosx-gl-ops.h"
#include "hw/display/ppcosx-gl-image.h"
#include "ppcosx-gpu-gl.h"
#include "trace.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

typedef struct PPCOSXGLContext {
    CGLContextObj cgl;
    GLuint fbo;
    GLuint color;
    GLuint depth;
    uint32_t width;
    uint32_t height;
    uint8_t *readback;
    GByteArray *blob;           /* staged pixel data from the guest */
} PPCOSXGLContext;

struct PPCOSXGL {
    CGLPixelFormatObj pixel_format;
    PPCOSXGLContext ctx[PPCOSX_GPU_GL_MAX_CONTEXTS + 1];
    PPCOSXGLContext *bound;
};

static inline uint32_t rd_u(const uint8_t *p)
{
    return ldl_be_p(p);
}

static inline float rd_f(const uint8_t *p)
{
    union { uint32_t u; float f; } v = { .u = ldl_be_p(p) };
    return v.f;
}

static inline double rd_d(const uint8_t *p)
{
    union { uint64_t u; double d; } v = { .u = ldq_be_p(p) };
    return v.d;
}

#include "ppcosx-gl-decode.c.inc"

PPCOSXGL *ppcosx_gl_init(void)
{
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAAccelerated,
        kCGLPFAColorSize, 24,
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_Legacy,
        0
    };
    PPCOSXGL *gl = g_new0(PPCOSXGL, 1);
    GLint count = 0;

    if (CGLChoosePixelFormat(attrs, &gl->pixel_format, &count) != kCGLNoError
        || !gl->pixel_format) {
        warn_report("ppcosx-gpu: no host OpenGL pixel format; GL disabled");
        g_free(gl);
        return NULL;
    }
    return gl;
}

static PPCOSXGLContext *ppcosx_gl_lookup(PPCOSXGL *gl, uint32_t id)
{
    if (id < 1 || id > PPCOSX_GPU_GL_MAX_CONTEXTS || !gl->ctx[id].cgl) {
        return NULL;
    }
    return &gl->ctx[id];
}

static void ppcosx_gl_free_ctx(PPCOSXGL *gl, PPCOSXGLContext *c)
{
    if (gl->bound == c) {
        gl->bound = NULL;
    }
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(c->cgl);      /* frees its FBO and renderbuffers too */
    g_free(c->readback);
    g_byte_array_unref(c->blob);
    memset(c, 0, sizeof(*c));
}

/*
 * (Re)allocate the current context's buffers to match the guest's pixel
 * format: colour with or without alpha, and depth and stencil only if the
 * guest has them.
 */
static bool ppcosx_gl_alloc_storage(PPCOSXGLContext *c, uint32_t width,
                                    uint32_t height, uint32_t format)
{
    uint32_t depth = PPCOSX_GPU_GL_FORMAT_DEPTH(format);
    uint32_t stencil = PPCOSX_GPU_GL_FORMAT_STENCIL(format);

    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, c->color);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT,
                             PPCOSX_GPU_GL_FORMAT_ALPHA(format) ? GL_RGBA8
                                                                : GL_RGB8,
                             width, height);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                                 GL_RENDERBUFFER_EXT, c->color);

    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, c->depth);
    if (stencil) {
        /* Packed is the only stencil format; attach only what exists. */
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT,
                                 width, height);
    } else if (depth) {
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT,
                                 depth <= 16 ? GL_DEPTH_COMPONENT16
                                             : GL_DEPTH_COMPONENT24,
                                 width, height);
    }
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT,
                                 GL_RENDERBUFFER_EXT, depth ? c->depth : 0);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT,
                                 GL_RENDERBUFFER_EXT, stencil ? c->depth : 0);

    if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT)
        != GL_FRAMEBUFFER_COMPLETE_EXT) {
        return false;
    }
    c->width = width;
    c->height = height;
    g_free(c->readback);
    c->readback = g_malloc((size_t)width * height * 4);
    return true;
}

uint32_t ppcosx_gl_ctx_create(PPCOSXGL *gl, uint32_t id, uint32_t width,
                              uint32_t height, uint32_t format)
{
    PPCOSXGLContext *c;

    if (id < 1 || id > PPCOSX_GPU_GL_MAX_CONTEXTS || gl->ctx[id].cgl ||
        width < 1 || width > PPCOSX_GPU_GL_MAX_SIZE ||
        height < 1 || height > PPCOSX_GPU_GL_MAX_SIZE) {
        return PPCOSX_GPU_ST_BAD_ARG;
    }
    c = &gl->ctx[id];
    if (CGLCreateContext(gl->pixel_format, NULL, &c->cgl) != kCGLNoError) {
        c->cgl = NULL;
        return PPCOSX_GPU_ST_UNSUPPORTED;
    }
    CGLSetCurrentContext(c->cgl);

    glGenFramebuffersEXT(1, &c->fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, c->fbo);
    glGenRenderbuffersEXT(1, &c->color);
    glGenRenderbuffersEXT(1, &c->depth);
    if (!ppcosx_gl_alloc_storage(c, width, height, format)) {
        ppcosx_gl_free_ctx(gl, c);
        return PPCOSX_GPU_ST_UNSUPPORTED;
    }
    /* A window-system framebuffer starts with these set to its size. */
    glViewport(0, 0, width, height);
    glScissor(0, 0, width, height);
    /* Guest pixel data always arrives tightly packed. */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    c->blob = g_byte_array_new();
    CGLSetCurrentContext(NULL);
    return PPCOSX_GPU_ST_OK;
}

uint32_t ppcosx_gl_ctx_resize(PPCOSXGL *gl, uint32_t id, uint32_t width,
                              uint32_t height, uint32_t format)
{
    PPCOSXGLContext *c = ppcosx_gl_lookup(gl, id);
    bool ok;

    if (!c || width < 1 || width > PPCOSX_GPU_GL_MAX_SIZE ||
        height < 1 || height > PPCOSX_GPU_GL_MAX_SIZE) {
        return PPCOSX_GPU_ST_BAD_ARG;
    }
    CGLSetCurrentContext(c->cgl);
    ok = ppcosx_gl_alloc_storage(c, width, height, format);
    CGLSetCurrentContext(gl->bound ? gl->bound->cgl : NULL);
    if (!ok) {
        ppcosx_gl_free_ctx(gl, c);
        return PPCOSX_GPU_ST_UNSUPPORTED;
    }
    return PPCOSX_GPU_ST_OK;
}

uint32_t ppcosx_gl_readback(PPCOSXGL *gl, uint8_t *dst, uint64_t dst_size)
{
    PPCOSXGLContext *c = gl->bound;

    if (!c || (uint64_t)c->width * c->height * 4 > dst_size) {
        return PPCOSX_GPU_ST_BAD_ARG;
    }
    /* Same byte layout as ppcosx_gl_present(), left in GL's row order. */
    glReadPixels(0, 0, c->width, c->height, GL_BGRA,
                 GL_UNSIGNED_INT_8_8_8_8, dst);
    return PPCOSX_GPU_ST_OK;
}

uint32_t ppcosx_gl_ctx_destroy(PPCOSXGL *gl, uint32_t id)
{
    PPCOSXGLContext *c = ppcosx_gl_lookup(gl, id);

    if (!c) {
        return PPCOSX_GPU_ST_BAD_ARG;
    }
    ppcosx_gl_free_ctx(gl, c);
    return PPCOSX_GPU_ST_OK;
}

uint32_t ppcosx_gl_ctx_bind(PPCOSXGL *gl, uint32_t id)
{
    PPCOSXGLContext *c = ppcosx_gl_lookup(gl, id);

    if (!c) {
        return PPCOSX_GPU_ST_BAD_ARG;
    }
    CGLSetCurrentContext(c->cgl);
    gl->bound = c;
    return PPCOSX_GPU_ST_OK;
}

void ppcosx_gl_unbind(PPCOSXGL *gl)
{
    if (gl->bound) {
        CGLSetCurrentContext(NULL);
        gl->bound = NULL;
    }
}

/*
 * Take the staged pixel data for a width x height image, checking that the
 * guest sent exactly enough, and set up byte swapping for it. Returns NULL
 * if the data is missing or the format is not one we size.
 */
static const uint8_t *ppcosx_gl_take_pixels(PPCOSXGLContext *c, uint32_t width,
                                            uint32_t height, uint32_t format,
                                            uint32_t type)
{
    uint64_t need = (uint64_t)width * height *
                    ppcosx_gl_pixel_bytes(format, type);

    if (need == 0 || need != c->blob->len) {
        return NULL;
    }
    /* Multi-byte components are big-endian from the guest. */
    glPixelStorei(GL_UNPACK_SWAP_BYTES,
                  HOST_BIG_ENDIAN ? GL_FALSE
                                  : ppcosx_gl_type_unit(type) > 1);
    return c->blob->data;
}

/* Convert `count` big-endian elements of `unit` bytes to host order. */
static void ppcosx_gl_to_host(uint8_t *data, uint64_t count, uint32_t unit)
{
    if (HOST_BIG_ENDIAN || unit == 1) {
        return;
    }
    for (uint64_t i = 0; i < count; i++, data += unit) {
        switch (unit) {
        case 2:
            stw_he_p(data, lduw_be_p(data));
            break;
        case 4:
            stl_he_p(data, ldl_be_p(data));
            break;
        case 8:
            stq_he_p(data, ldq_be_p(data));
            break;
        }
    }
}

static uint32_t ppcosx_gl_draw(PPCOSXGLContext *c, const uint8_t *p,
                               uint32_t words)
{
    uint32_t mode, vertices, indices, arrays;
    uint32_t kind[PPCOSX_GL_MAX_ARRAYS], size[PPCOSX_GL_MAX_ARRAYS];
    uint32_t type[PPCOSX_GL_MAX_ARRAYS];
    uint64_t offset[PPCOSX_GL_MAX_ARRAYS], total = 0;
    uint8_t *data = c->blob->data;
    bool have_vertex = false;

    if (words < 4) {
        return PPCOSX_GPU_ST_BAD_ARG;
    }
    mode = rd_u(p);
    vertices = rd_u(p + 4);
    indices = rd_u(p + 8);
    arrays = rd_u(p + 12);
    if (mode > GL_POLYGON || vertices > PPCOSX_GL_MAX_VERTICES ||
        indices > PPCOSX_GL_MAX_INDICES || arrays > PPCOSX_GL_MAX_ARRAYS ||
        words != 4 + arrays * 3) {
        return PPCOSX_GPU_ST_BAD_ARG;
    }

    for (uint32_t i = 0; i < arrays; i++) {
        uint32_t unit;
        bool ok;

        kind[i] = rd_u(p + 16 + i * 12);
        size[i] = rd_u(p + 20 + i * 12);
        type[i] = rd_u(p + 24 + i * 12);
        unit = ppcosx_gl_array_unit(type[i]);

        /* Only the combinations fixed-function GL accepts. */
        switch (kind[i]) {
        case PPCOSX_GL_ARRAY_VERTEX:
            ok = size[i] >= 2 && size[i] <= 4 && unit > 1 &&
                 type[i] != GL_UNSIGNED_SHORT && type[i] != GL_UNSIGNED_INT;
            have_vertex = have_vertex || ok;
            break;
        case PPCOSX_GL_ARRAY_NORMAL:
            ok = size[i] == 3 && unit && type[i] != GL_UNSIGNED_BYTE &&
                 type[i] != GL_UNSIGNED_SHORT && type[i] != GL_UNSIGNED_INT;
            break;
        case PPCOSX_GL_ARRAY_COLOR:
            ok = (size[i] == 3 || size[i] == 4) && unit;
            break;
        default:
            ok = kind[i] >= PPCOSX_GL_ARRAY_TEXCOORD0 &&
                 kind[i] < PPCOSX_GL_ARRAY_TEXCOORD0 + 8 &&
                 size[i] >= 1 && size[i] <= 4 && unit > 1 &&
                 type[i] != GL_UNSIGNED_SHORT && type[i] != GL_UNSIGNED_INT;
            break;
        }
        for (uint32_t j = 0; ok && j < i; j++) {
            ok = kind[j] != kind[i];
        }
        if (!ok) {
            return PPCOSX_GPU_ST_BAD_ARG;
        }
        offset[i] = total;
        total += (uint64_t)vertices * size[i] * unit;
    }
    if (!have_vertex || total + (uint64_t)indices * 4 != c->blob->len) {
        return PPCOSX_GPU_ST_BAD_ARG;
    }

    for (uint32_t i = 0; i < arrays; i++) {
        ppcosx_gl_to_host(data + offset[i], (uint64_t)vertices * size[i],
                          ppcosx_gl_array_unit(type[i]));
    }
    ppcosx_gl_to_host(data + total, indices, 4);
    for (uint32_t i = 0; i < indices; i++) {
        /* An index past the data would read beyond the staging buffer. */
        if (ldl_he_p(data + total + (uint64_t)i * 4) >= vertices) {
            return PPCOSX_GPU_ST_BAD_ARG;
        }
    }

    for (uint32_t i = 0; i < arrays; i++) {
        const uint8_t *ptr = data + offset[i];

        switch (kind[i]) {
        case PPCOSX_GL_ARRAY_VERTEX:
            glVertexPointer(size[i], type[i], 0, ptr);
            glEnableClientState(GL_VERTEX_ARRAY);
            break;
        case PPCOSX_GL_ARRAY_NORMAL:
            glNormalPointer(type[i], 0, ptr);
            glEnableClientState(GL_NORMAL_ARRAY);
            break;
        case PPCOSX_GL_ARRAY_COLOR:
            glColorPointer(size[i], type[i], 0, ptr);
            glEnableClientState(GL_COLOR_ARRAY);
            break;
        default:
            glClientActiveTexture(GL_TEXTURE0 + kind[i]
                                  - PPCOSX_GL_ARRAY_TEXCOORD0);
            glTexCoordPointer(size[i], type[i], 0, ptr);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            break;
        }
    }

    if (indices) {
        glDrawElements(mode, indices, GL_UNSIGNED_INT, data + total);
    } else {
        glDrawArrays(mode, 0, vertices);
    }

    /* Leave no pointers into the staging buffer behind. */
    for (uint32_t i = 0; i < arrays; i++) {
        switch (kind[i]) {
        case PPCOSX_GL_ARRAY_VERTEX:
            glDisableClientState(GL_VERTEX_ARRAY);
            break;
        case PPCOSX_GL_ARRAY_NORMAL:
            glDisableClientState(GL_NORMAL_ARRAY);
            break;
        case PPCOSX_GL_ARRAY_COLOR:
            glDisableClientState(GL_COLOR_ARRAY);
            break;
        default:
            glClientActiveTexture(GL_TEXTURE0 + kind[i]
                                  - PPCOSX_GL_ARRAY_TEXCOORD0);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            break;
        }
    }
    glClientActiveTexture(GL_TEXTURE0);
    return PPCOSX_GPU_ST_OK;
}

static uint32_t ppcosx_gl_image_call(PPCOSXGLContext *c, uint32_t op,
                                     const uint8_t *p, uint32_t words)
{
    uint32_t st = PPCOSX_GPU_ST_BAD_ARG;

    switch (op) {
    case PPCOSX_GL_BLOB: {
        uint32_t bytes;

        if (words < 1) {
            return PPCOSX_GPU_ST_BAD_ARG;
        }
        bytes = rd_u(p);
        if (bytes > (words - 1) * 4 ||
            c->blob->len + (uint64_t)bytes > PPCOSX_GL_MAX_BLOB) {
            g_byte_array_set_size(c->blob, 0);
            return PPCOSX_GPU_ST_BAD_ARG;
        }
        g_byte_array_append(c->blob, p + 4, bytes);
        return PPCOSX_GPU_ST_OK;        /* keep the data for what follows */
    }
    case PPCOSX_GL_TexImage2D: {
        uint32_t width, height, has_data;
        const uint8_t *pixels = NULL;

        if (words != 9) {
            break;
        }
        width = rd_u(p + 12);
        height = rd_u(p + 16);
        has_data = rd_u(p + 32);
        if (width > PPCOSX_GL_MAX_TEX_SIZE + 2 ||
            height > PPCOSX_GL_MAX_TEX_SIZE + 2) {
            break;
        }
        if (has_data) {
            pixels = ppcosx_gl_take_pixels(c, width, height, rd_u(p + 24),
                                           rd_u(p + 28));
            if (!pixels) {
                break;
            }
        }
        glTexImage2D(rd_u(p), rd_u(p + 4), rd_u(p + 8), width, height,
                     rd_u(p + 20), rd_u(p + 24), rd_u(p + 28), pixels);
        st = PPCOSX_GPU_ST_OK;
        break;
    }
    case PPCOSX_GL_TexSubImage2D: {
        uint32_t width, height;
        const uint8_t *pixels;

        if (words != 8) {
            break;
        }
        width = rd_u(p + 16);
        height = rd_u(p + 20);
        if (width > PPCOSX_GL_MAX_TEX_SIZE + 2 ||
            height > PPCOSX_GL_MAX_TEX_SIZE + 2) {
            break;
        }
        pixels = ppcosx_gl_take_pixels(c, width, height, rd_u(p + 24),
                                       rd_u(p + 28));
        if (!pixels) {
            break;
        }
        glTexSubImage2D(rd_u(p), rd_u(p + 4), rd_u(p + 8), rd_u(p + 12),
                        width, height, rd_u(p + 24), rd_u(p + 28), pixels);
        st = PPCOSX_GPU_ST_OK;
        break;
    }
    case PPCOSX_GL_DeleteTextures: {
        uint32_t n;

        if (words < 1 || (n = rd_u(p)) != words - 1) {
            break;
        }
        for (uint32_t i = 0; i < n; i++) {
            GLuint id = rd_u(p + 4 + i * 4);
            glDeleteTextures(1, &id);
        }
        st = PPCOSX_GPU_ST_OK;
        break;
    }
    case PPCOSX_GL_Draw:
        st = ppcosx_gl_draw(c, p, words);
        break;
    default:
        st = PPCOSX_GPU_ST_UNSUPPORTED;
        break;
    }

    g_byte_array_set_size(c->blob, 0);
    return st;
}

uint32_t ppcosx_gl_call(PPCOSXGL *gl, uint32_t op, const uint8_t *args,
                        uint32_t words)
{
    if (!gl->bound) {
        return PPCOSX_GPU_ST_BAD_ARG;
    }
    if (op >= PPCOSX_GL_BLOB) {
        trace_ppcosx_gl_call("(data command)", op, words,
                             words > 0 ? rd_u(args) : 0,
                             words > 1 ? rd_u(args + 4) : 0,
                             words > 2 ? rd_u(args + 8) : 0);
        return ppcosx_gl_image_call(gl->bound, op, args, words);
    }
    if (trace_event_get_state_backends(TRACE_PPCOSX_GL_CALL)) {
        trace_ppcosx_gl_call(op < PPCOSX_GL_OP_END
                             ? ppcosx_gl_names[op - 0x1000] : "?",
                             op, words,
                             words > 0 ? rd_u(args) : 0,
                             words > 1 ? rd_u(args + 4) : 0,
                             words > 2 ? rd_u(args + 8) : 0);
    }
    return ppcosx_gl_decode(op, args, words);
}

uint32_t ppcosx_gl_present(PPCOSXGL *gl, int32_t x, int32_t y, uint8_t *dst,
                           uint32_t dst_width, uint32_t dst_height,
                           uint32_t dst_stride, PPCOSXGLRect *damage)
{
    PPCOSXGLContext *c = gl->bound;
    int64_t x0, y0, x1, y1;

    if (!c) {
        return PPCOSX_GPU_ST_BAD_ARG;
    }
    memset(damage, 0, sizeof(*damage));

    x0 = MAX((int64_t)x, 0);
    y0 = MAX((int64_t)y, 0);
    x1 = MIN((int64_t)x + c->width, dst_width);
    y1 = MIN((int64_t)y + c->height, dst_height);
    if (x0 >= x1 || y0 >= y1) {
        return PPCOSX_GPU_ST_OK;        /* entirely off screen */
    }

    /*
     * As a packed 32-bit value on this little-endian host, BGRA /
     * 8_8_8_8 lands in memory as A,R,G,B bytes: big-endian xRGB, which is
     * the scanout format.
     */
    glReadPixels(0, 0, c->width, c->height, GL_BGRA,
                 GL_UNSIGNED_INT_8_8_8_8, c->readback);

    for (int64_t row = y0; row < y1; row++) {
        /* GL's origin is bottom-left; the scanout's is top-left. */
        uint32_t src_row = c->height - 1 - (uint32_t)(row - y);
        memcpy(dst + (size_t)row * dst_stride + (size_t)x0 * 4,
               c->readback + ((size_t)src_row * c->width + (x0 - x)) * 4,
               (size_t)(x1 - x0) * 4);
    }

    damage->x = x0;
    damage->y = y0;
    damage->width = x1 - x0;
    damage->height = y1 - y0;
    return PPCOSX_GPU_ST_OK;
}

uint32_t ppcosx_gl_read_pixels(PPCOSXGL *gl, int32_t x, int32_t y,
                               uint32_t width, uint32_t height,
                               uint32_t format, uint32_t type,
                               uint8_t *dst, uint64_t dst_size)
{
    uint32_t bpp = ppcosx_gl_pixel_bytes(format, type);
    bool swap = !HOST_BIG_ENDIAN && ppcosx_gl_type_unit(type) > 1;

    if (!gl->bound) {
        return PPCOSX_GPU_ST_BAD_ARG;
    }
    if (!bpp) {
        return PPCOSX_GPU_ST_UNSUPPORTED;
    }
    if (width > PPCOSX_GPU_GL_MAX_SIZE || height > PPCOSX_GPU_GL_MAX_SIZE ||
        (uint64_t)width * height * bpp > dst_size) {
        return PPCOSX_GPU_ST_BAD_ARG;
    }
    if (width == 0 || height == 0) {
        return PPCOSX_GPU_ST_OK;
    }
    /* Pack state is ours alone: the guest's is applied on its side. */
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_SWAP_BYTES, swap);
    glReadPixels(x, y, width, height, format, type, dst);
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
    return PPCOSX_GPU_ST_OK;
}

uint32_t ppcosx_gl_bound_id(PPCOSXGL *gl)
{
    return gl->bound ? (uint32_t)(gl->bound - gl->ctx) : 0;
}

bool ppcosx_gl_frame_size(PPCOSXGL *gl, uint32_t *width, uint32_t *height)
{
    if (!gl->bound) {
        return false;
    }
    *width = gl->bound->width;
    *height = gl->bound->height;
    return true;
}

void ppcosx_gl_read_frame(PPCOSXGL *gl, uint8_t *dst)
{
    PPCOSXGLContext *c = gl->bound;
    size_t row = (size_t)c->width * 4;

    /* Same byte layout as ppcosx_gl_present(); flipped to top row first. */
    glReadPixels(0, 0, c->width, c->height, GL_BGRA,
                 GL_UNSIGNED_INT_8_8_8_8, c->readback);
    for (uint32_t y = 0; y < c->height; y++) {
        memcpy(dst + y * row, c->readback + (c->height - 1 - y) * row, row);
    }
}

void ppcosx_gl_reset(PPCOSXGL *gl)
{
    for (int i = 1; i <= PPCOSX_GPU_GL_MAX_CONTEXTS; i++) {
        if (gl->ctx[i].cgl) {
            ppcosx_gl_free_ctx(gl, &gl->ctx[i]);
        }
    }
    gl->bound = NULL;
}
