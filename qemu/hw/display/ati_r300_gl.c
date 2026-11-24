/*
 * QEMU ATI SVGA emulation
 * Radeon 9600 (R300) 3D engine: drawing on the host GPU (macOS OpenGL).
 *
 * See ati_r300_gl.h. In outline:
 *
 * Surfaces: each colour or depth buffer the card draws to has a host
 * texture covering it (pitch x rows), reloaded before use if video memory
 * under it changed since it was last in step; after drawing it is "dirty"
 * (newer than video memory) until written back by ati_r300_gl_flush.
 *
 * Changes to video memory are tracked per page: QEMU's dirty log (which
 * sees the guest CPU's writes, the 2D engine's and everything else that
 * marks video memory dirty for the display) is harvested once per batch
 * into a generation number per page, and a host copy is stale when a
 * page under it has a newer generation than the copy. Harvesting clears
 * the log, so the display is then told to redraw in full.
 *
 * Textures: uploaded from video memory or the GART into a cache keyed by
 * address and layout. A texture that is a dirty surface (render to
 * texture, as Core Image does) is copied from the surface on the GPU.
 *
 * Programs: the R300 fragment program (US) is translated to GLSL, which
 * mirrors the instruction semantics of the software engine in
 * ati_r300.c, and cached.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "ati_int.h"
#include "ati_regs.h"
#include "ati_r300_gl.h"
#include "system/ram_addr.h"
#include "hw/core/cpu.h"
#include "ui/console.h"

#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <OpenGL/gl3ext.h>

#define NSURF       64
#define NTEX        256
#define NPROG       256
#define MAX_ATTRIBS 16

/* registers read here directly */
#define TX_FILTER0_0            0x4400
#define TX_FORMAT0_0            0x4480
#define TX_FORMAT1_0            0x44c0
#define TX_FORMAT2_0            0x4500
#define TX_OFFSET_0             0x4540
#define TX_BORDER_COLOR_0       0x45c0
#define TX_ENABLE               0x4104

typedef struct GLSurf {
    bool used;
    bool depth;
    uint64_t off;               /* in video memory */
    uint32_t pitch;             /* pixels */
    uint32_t rows;              /* allocated */
    int fmt;                    /* colour format, or depth bytes */
    int bpp;
    GLuint tex;
    bool dirty;                 /* newer than video memory */
    int dx0, dy0, dx1, dy1;     /* the dirty rectangle, inclusive */
    uint32_t synced;            /* page generation it last matched */
    uint32_t gen;               /* bumped by each draw into it */
    uint32_t last_use;
} GLSurf;

typedef struct GLTex {
    bool used;
    /* key */
    bool vram;
    uint64_t addr;              /* video memory offset or card address */
    int fmt, w, h, levels, swap;
    uint32_t pitch;             /* bytes, level 0 */
    uint64_t len;               /* bytes, all levels */
    /* contents */
    GLuint tex;
    uint32_t synced;            /* in video memory: page generation */
    uint32_t epoch;             /* in system memory: batch */
    GLSurf *alias;              /* copied from this surface... */
    uint32_t alias_gen;         /* ...at this generation */
    int compmap[4];             /* GL channel holding X, Y, Z, W */
    uint32_t last_use;
} GLTex;

typedef struct GLProg {
    bool used;
    uint64_t key;
    GLuint prog;                /* 0: failed to build */
    GLint u_size, u_k, u_fog, u_fogc, u_aref;
    uint32_t last_use;
} GLProg;

typedef struct R300GL {
    bool tried, ok;
    bool held;                  /* ctx current on this thread, locked */
    CGLContextObj ctx;
    GLuint vao, vbo, fbo, fbo_read, fbo_copy;
    GLSurf surf[NSURF];
    GLTex tex[NTEX];
    GLProg prog[NPROG];
    uint32_t epoch, clock;
    /* video memory pages: generation of the last change from outside */
    uint32_t *pgen;
    uint8_t *ours;              /* pages we wrote since the last harvest */
    uint32_t npages, gen;
    bool harvested;
    /* the draw being built */
    bool active;
    R300GLDesc d;
    GLSurf *color, *depth;
    GLProg *p;
    float *verts;
    size_t nverts, cap;
    int bx0, by0, bx1, by1;     /* bounding box of the triangles */
} R300GL;

/* Debugging: PPCOSX_ATI_3D_STATS prints where the time went. */
static struct {
    int64_t hash, upload, readback, draw, tex;
    uint64_t hash_bytes, upload_bytes, readback_bytes, flushes, draws;
    uint64_t reported;
} prof;
static int prof_on = -1;

static bool prof_enabled(void)
{
    if (prof_on < 0) {
        prof_on = getenv("PPCOSX_ATI_3D_STATS") != NULL;
    }
    return prof_on;
}

#define PROF_START() int64_t prof_t0 = prof_enabled() ? get_clock() : 0
#define PROF_END(field) do { \
        if (prof_t0) { \
            prof.field += get_clock() - prof_t0; \
        } \
    } while (0)

static R300GL *gl_state(ATIVGAState *s)
{
    if (!s->r300_gl) {
        s->r300_gl = g_new0(R300GL, 1);
    }
    return s->r300_gl;
}

/* ---- the context --------------------------------------------------------- */

static bool gl_init(ATIVGAState *s, R300GL *g)
{
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAAccelerated,
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
        kCGLPFAAllowOfflineRenderers,
        0
    };
    CGLPixelFormatObj pf;
    GLint n;

    g->tried = true;
    if (getenv("PPCOSX_ATI_GPU") && !strcmp(getenv("PPCOSX_ATI_GPU"), "0")) {
        return false;
    }
    if (CGLChoosePixelFormat(attrs, &pf, &n) != kCGLNoError || !pf) {
        warn_report("ati: no host OpenGL 4.1 renderer; R300 3D in software");
        return false;
    }
    if (CGLCreateContext(pf, NULL, &g->ctx) != kCGLNoError) {
        CGLDestroyPixelFormat(pf);
        warn_report("ati: no host OpenGL context; R300 3D in software");
        return false;
    }
    CGLDestroyPixelFormat(pf);
    CGLSetCurrentContext(g->ctx);
    glGenVertexArrays(1, &g->vao);
    glGenBuffers(1, &g->vbo);
    glGenFramebuffers(1, &g->fbo);
    glGenFramebuffers(1, &g->fbo_read);
    glGenFramebuffers(1, &g->fbo_copy);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    g->epoch = 1;
    g->npages = s->vga.vram_size >> 12;
    g->pgen = g_new0(uint32_t, g->npages);
    g->ours = g_new0(uint8_t, g->npages);
    g->gen = 1;
    g->ok = true;
    info_report("ati: R300 3D on the host GPU (%s)",
                (const char *)glGetString(GL_RENDERER));
    return true;
}

/*
 * The context is used from whichever thread runs the card: the command
 * processor's thread for drawing, vCPU threads for 2D syncs and fences
 * (all under the BQL). Apple's GL wants a context current on one thread
 * at a time, so every entry point takes it and lets it go on the way out.
 */
static void gl_acquire(R300GL *g)
{
    if (!g->held) {
        CGLLockContext(g->ctx);
        CGLSetCurrentContext(g->ctx);
        g->held = true;
    }
}

static void gl_release(R300GL *g)
{
    if (g->held) {
        g->held = false;
        CGLSetCurrentContext(NULL);
        CGLUnlockContext(g->ctx);
    }
}

static R300GL *gl_get(ATIVGAState *s)
{
    R300GL *g = gl_state(s);

    if (!g->tried) {
        gl_init(s, g);
        if (g->ok) {
            CGLSetCurrentContext(NULL);
        }
    }
    if (!g->ok) {
        return NULL;
    }
    gl_acquire(g);
    return g;
}

/* ---- video memory -------------------------------------------------------- */

/* ---- page generations --------------------------------------------------- */

static void pages_of(R300GL *g, uint64_t off, uint64_t len, uint32_t *p0,
                     uint32_t *p1)
{
    *p0 = MIN(off >> 12, (uint64_t)g->npages);
    *p1 = len ? MIN((off + len - 1) >> 12, (uint64_t)g->npages - 1) + 1 : *p0;
}

/* The newest change to [off, off + len). */
static uint32_t range_gen(R300GL *g, uint64_t off, uint64_t len)
{
    uint32_t p0, p1, m = 0;

    pages_of(g, off, len, &p0, &p1);
    for (uint32_t p = p0; p < p1; p++) {
        m = MAX(m, g->pgen[p]);
    }
    return m;
}

/* [off, off + len) changed: a new generation; returns it. */
static uint32_t mark_range(R300GL *g, uint64_t off, uint64_t len, bool ours)
{
    uint32_t p0, p1, gen = ++g->gen;

    pages_of(g, off, len, &p0, &p1);
    for (uint32_t p = p0; p < p1; p++) {
        g->pgen[p] = gen;
        if (ours) {
            g->ours[p] = 1;
        }
    }
    return gen;
}

/*
 * Take in QEMU's dirty log for video memory: pages written by anyone but
 * this backend get a new generation. Done once per batch: the guest CPU
 * cannot write during one.
 */
static void harvest(ATIVGAState *s, R300GL *g)
{
    DirtyBitmapSnapshot *snap;
    uint32_t gen = 0;
    bool any = false;

    if (g->harvested) {
        return;
    }
    PROF_START();
    g->harvested = true;
    if (current_cpu) {
        /*
         * On a vCPU thread (the command processor run from a register
         * write): memory_region_snapshot_and_clear_dirty() ends by waiting
         * for every other vCPU to finish its translation block, and one
         * waiting to run exclusive work (a TLB flush) waits for this one:
         * a deadlock with several CPUs. The wait guards against another
         * CPU's write racing the clear; a write that slips through still
         * reaches video memory, it is only missed by this check. TCG has
         * no dirty log to sync first, so take the snapshot directly.
         */
        snap = cpu_physical_memory_snapshot_and_clear_dirty(
            &s->vga.vram, 0, s->vga.vram_size, DIRTY_MEMORY_VGA);
    } else {
        snap = memory_region_snapshot_and_clear_dirty(&s->vga.vram, 0,
                                                      s->vga.vram_size,
                                                      DIRTY_MEMORY_VGA);
    }
    for (uint32_t p = 0; p < g->npages; p++) {
        if (!memory_region_snapshot_get_dirty(&s->vga.vram, snap,
                                              (hwaddr)p << 12, 4096)) {
            continue;
        }
        any = true;
        if (!g->ours[p]) {
            if (!gen) {
                gen = ++g->gen;
            }
            g->pgen[p] = gen;
        }
    }
    g_free(snap);
    PROF_END(hash);
    memset(g->ours, 0, g->npages);
    if (any) {
        /* the display used that log too: have it redraw everything */
        graphic_hw_invalidate(s->vga.con);
    }
}

/* Video memory rows for an upload, in the card's byte order. */
static const uint8_t *vram_src(ATIVGAState *s, uint64_t off, uint64_t len,
                               uint8_t **tmp)
{
    *tmp = NULL;
    if (!s->vram_xor) {
        return s->vga.vram_ptr + off;
    }
    *tmp = g_malloc(len);
    ati_vram_read(s, off, *tmp, len);
    return *tmp;
}

static bool ranges_overlap64(uint64_t a, uint64_t alen, uint64_t b,
                             uint64_t blen)
{
    return a < b + blen && b < a + alen;
}

/* ---- surfaces ------------------------------------------------------------ */

typedef struct GLFmt {
    GLenum internal, format, type;
} GLFmt;

static bool color_glfmt(int fmt, GLFmt *f)
{
    switch (fmt) {
    case 6:     /* ARGB8888: bytes B, G, R, A (the host is little-endian) */
        *f = (GLFmt){ GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV };
        return true;
    case 10:    /* ARGB16161616 float: R, G, B, A halves */
        *f = (GLFmt){ GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT };
        return true;
    case 7:     /* ARGB32323232 float */
        *f = (GLFmt){ GL_RGBA32F, GL_RGBA, GL_FLOAT };
        return true;
    case 4:     /* RGB565 */
        *f = (GLFmt){ GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5 };
        return true;
    case 3:     /* ARGB1555 */
        *f = (GLFmt){ GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV };
        return true;
    case 15:    /* ARGB4444 */
        *f = (GLFmt){ GL_RGBA4, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV };
        return true;
    default:
        return false;
    }
}

static GLFmt surf_glfmt(GLSurf *sf)
{
    GLFmt f;

    if (sf->depth) {
        return sf->bpp == 4 ?
            (GLFmt){ GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL,
                     GL_UNSIGNED_INT_24_8 } :
            (GLFmt){ GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT,
                     GL_UNSIGNED_SHORT };
    }
    color_glfmt(sf->fmt, &f);
    return f;
}

static uint64_t surf_len(GLSurf *sf)
{
    return (uint64_t)sf->pitch * sf->rows * sf->bpp;
}

static void surf_attach(R300GL *g, GLuint fbo, GLSurf *sf)
{
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    if (sf->depth) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER,
                               sf->bpp == 4 ? GL_DEPTH_STENCIL_ATTACHMENT
                                            : GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D, sf->tex, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    } else {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, sf->tex, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    }
}

static void surf_upload(ATIVGAState *s, GLSurf *sf)
{
    PROF_START();
    GLFmt f = surf_glfmt(sf);
    uint64_t len = MIN(surf_len(sf), s->vga.vram_size - sf->off);
    uint32_t rows = len / ((uint64_t)sf->pitch * sf->bpp);
    uint8_t *tmp;
    const uint8_t *src = vram_src(s, sf->off, len, &tmp);

    glBindTexture(GL_TEXTURE_2D, sf->tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, sf->pitch);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sf->pitch, rows, f.format,
                    f.type, src);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    g_free(tmp);
    PROF_END(upload);
    prof.upload_bytes += len;
}

/* Write a surface's dirty rectangle back to video memory. */
static void surf_writeback(ATIVGAState *s, R300GL *g, GLSurf *sf)
{
    GLFmt f = surf_glfmt(sf);
    int x0 = sf->dx0, y0 = sf->dy0;
    int w = sf->dx1 - sf->dx0 + 1, h = sf->dy1 - sf->dy0 + 1;
    uint64_t start = sf->off + ((uint64_t)y0 * sf->pitch + x0) * sf->bpp;
    uint64_t span = ((uint64_t)(h - 1) * sf->pitch + w) * sf->bpp;

    sf->dirty = false;
    if (w <= 0 || h <= 0 || start + span > s->vga.vram_size) {
        return;
    }
    PROF_START();
    surf_attach(g, g->fbo_read, sf);
    glPixelStorei(GL_PACK_ROW_LENGTH, sf->pitch);
    if (!s->vram_xor) {
        glReadPixels(x0, y0, w, h, f.format, f.type, s->vga.vram_ptr + start);
    } else {
        uint8_t *tmp = g_malloc(span);

        glReadPixels(x0, y0, w, h, f.format, f.type, tmp);
        /*
         * Row by row: tmp holds only the rectangle's columns of each row,
         * and what lies between them in video memory must stay.
         */
        for (int r = 0; r < h; r++) {
            uint64_t o = (uint64_t)r * sf->pitch * sf->bpp;

            ati_vram_write(s, start + o, tmp + o, (uint32_t)w * sf->bpp);
        }
        g_free(tmp);
    }
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    PROF_END(readback);
    prof.readback_bytes += span;
    memory_region_set_dirty(&s->vga.vram, start, span);

    /*
     * Video memory now holds what the GPU has: other copies of it are
     * stale (a new generation), this one and its GPU-side texture copies
     * are in step.
     */
    uint32_t gen = mark_range(g, start, span, true);

    sf->synced = gen;
    for (int i = 0; i < NTEX; i++) {
        GLTex *t = &g->tex[i];

        if (t->used && t->alias == sf && t->alias_gen == sf->gen) {
            t->alias = NULL;
            t->synced = gen;
        }
    }
}

/* Write back dirty surfaces overlapping [off, off + len), except `skip`. */
static void flush_range(ATIVGAState *s, R300GL *g, uint64_t off, uint64_t len,
                        GLSurf *skip)
{
    for (int i = 0; i < NSURF; i++) {
        GLSurf *sf = &g->surf[i];

        if (sf->used && sf->dirty && sf != skip &&
            ranges_overlap64(sf->off, surf_len(sf), off, len)) {
            surf_writeback(s, g, sf);
        }
    }
}

static void surf_free(ATIVGAState *s, R300GL *g, GLSurf *sf)
{
    if (sf->dirty) {
        surf_writeback(s, g, sf);
    }
    for (int i = 0; i < NTEX; i++) {
        if (g->tex[i].alias == sf) {
            g->tex[i].alias = NULL;
            g->tex[i].synced = 0;
        }
    }
    glDeleteTextures(1, &sf->tex);
    memset(sf, 0, sizeof(*sf));
}

/* A free slot, else the least recently used one. */
static GLSurf *lru_surf(R300GL *g)
{
    GLSurf *v = &g->surf[0];

    for (int i = 0; i < NSURF; i++) {
        if (!g->surf[i].used) {
            return &g->surf[i];
        }
        if (g->surf[i].last_use < v->last_use) {
            v = &g->surf[i];
        }
    }
    return v;
}

/* Make sure a host copy of the buffer exists, `rows` tall, and is current. */
static GLSurf *surf_get(ATIVGAState *s, R300GL *g, bool depth, uint64_t off,
                        uint32_t pitch, int fmt, int bpp, uint32_t rows)
{
    GLSurf *sf = NULL, *victim = NULL;
    uint64_t max_rows = (s->vga.vram_size - off) / ((uint64_t)pitch * bpp);

    rows = MIN(ROUND_UP(rows, 64), max_rows);
    if (!rows) {
        return NULL;
    }
    for (int i = 0; i < NSURF; i++) {
        GLSurf *c = &g->surf[i];

        if (c->used && c->depth == depth && c->off == off &&
            c->pitch == pitch && c->fmt == fmt) {
            sf = c;
            break;
        }
    }
    victim = lru_surf(g);
    if (sf && sf->rows < rows) {
        surf_free(s, g, sf);            /* writes back if dirty */
        victim = sf;
        sf = NULL;
    }
    if (!sf) {
        GLFmt f;

        if (victim->used) {
            surf_free(s, g, victim);
        }
        sf = victim;
        sf->used = true;
        sf->depth = depth;
        sf->off = off;
        sf->pitch = pitch;
        sf->rows = rows;
        sf->fmt = fmt;
        sf->bpp = bpp;
        f = surf_glfmt(sf);
        glGenTextures(1, &sf->tex);
        glBindTexture(GL_TEXTURE_2D, sf->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, f.internal, pitch, rows, 0, f.format,
                     f.type, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        sf->synced = 0;
    }
    sf->last_use = ++g->clock;
    /* other buffers drawn into this memory go back to it first */
    flush_range(s, g, sf->off, surf_len(sf), sf);
    if (!sf->dirty && range_gen(g, sf->off, surf_len(sf)) > sf->synced) {
        surf_upload(s, sf);
        sf->synced = g->gen;
    }
    return sf;
}

/* ---- textures ------------------------------------------------------------ */

typedef struct TexFmt {
    GLenum internal, format, type;
    int bpp;
} TexFmt;

static bool tex_glfmt(int fmt, TexFmt *f)
{
    switch (fmt) {
    case 0x0: *f = (TexFmt){ GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1 }; break;
    case 0x1: *f = (TexFmt){ GL_R16, GL_RED, GL_UNSIGNED_SHORT, 2 }; break;
    case 0x3: *f = (TexFmt){ GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2 }; break;
    case 0x4: *f = (TexFmt){ GL_RG16, GL_RG, GL_UNSIGNED_SHORT, 4 }; break;
    case 0x6:
        *f = (TexFmt){ GL_RGB8, GL_RGB, GL_UNSIGNED_SHORT_5_6_5_REV, 2 };
        break;
    case 0xa:
        *f = (TexFmt){ GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4_REV, 2 };
        break;
    case 0xb:
        *f = (TexFmt){ GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, 2 };
        break;
    case 0xc: case 0x13:
        *f = (TexFmt){ GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4 };
        break;
    case 0xd:
        *f = (TexFmt){ GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV,
                       4 };
        break;
    case 0xe: *f = (TexFmt){ GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT, 8 }; break;
    case 0x18: *f = (TexFmt){ GL_R16F, GL_RED, GL_HALF_FLOAT, 2 }; break;
    case 0x19: *f = (TexFmt){ GL_RG16F, GL_RG, GL_HALF_FLOAT, 4 }; break;
    case 0x1a: *f = (TexFmt){ GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8 }; break;
    case 0x1b: *f = (TexFmt){ GL_R32F, GL_RED, GL_FLOAT, 4 }; break;
    case 0x1c: *f = (TexFmt){ GL_RG32F, GL_RG, GL_FLOAT, 8 }; break;
    case 0x1d: *f = (TexFmt){ GL_RGBA32F, GL_RGBA, GL_FLOAT, 16 }; break;
    default:
        return false;
    }
    return true;
}

/* Swap staged system-memory texels per TX_OFFSET's endian field. */
static void tex_swap(uint8_t *p, uint64_t len, int mode)
{
    for (uint64_t i = 0; i + 4 <= len; i += 4) {
        uint32_t w = ldl_le_p(p + i);

        switch (mode) {
        case 1: w = ((w & 0x00ff00ff) << 8) | ((w >> 8) & 0x00ff00ff); break;
        case 2: w = bswap32(w); break;
        case 3: w = (w << 16) | (w >> 16); break;
        }
        stl_le_p(p + i, w);
    }
}

typedef struct TexInfo {
    int fmt, w, h, levels, swap;
    uint32_t pitch[12], lw[12], lh[12];
    uint64_t loff[12], len;
    uint32_t addr;
    TexFmt f;
} TexInfo;

/* Decode texture unit n's registers; false if the GPU cannot sample it. */
static bool tex_info(ATIVGAState *s, int n, TexInfo *ti)
{
    uint32_t f0 = s->shadow[(TX_FORMAT0_0 >> 2) + n];
    uint32_t f1 = s->shadow[(TX_FORMAT1_0 >> 2) + n];
    uint32_t f2 = s->shadow[(TX_FORMAT2_0 >> 2) + n];
    uint32_t filt = s->shadow[(TX_FILTER0_0 >> 2) + n];
    uint32_t off = s->shadow[(TX_OFFSET_0 >> 2) + n];
    int max_level = (f0 >> 26) & 0xf;

    memset(ti, 0, sizeof(*ti));
    ti->fmt = f1 & 0x1f;
    if (!tex_glfmt(ti->fmt, &ti->f) || ((f1 >> 5) & 0xf) ||
        ((f1 >> 25) & 3)) {
        return false;           /* format, signed components, cube/3D */
    }
    ti->w = (f0 & 0x7ff) + 1;
    ti->h = ((f0 >> 11) & 0x7ff) + 1;
    ti->swap = off & 3;
    ti->addr = off & ~0x1fu;
    ti->levels = 1;
    ti->lw[0] = ti->w;
    ti->lh[0] = ti->h;
    ti->pitch[0] = (f0 & (1u << 31)) ? ((f2 & 0x3fff) + 1) * ti->f.bpp
                                     : ROUND_UP(ti->w * ti->f.bpp, 32);
    ti->len = (uint64_t)ti->pitch[0] * ti->h;
    if ((filt >> 13) & 3) {
        while (ti->levels <= max_level && ti->levels < 12 &&
               (ti->lw[ti->levels - 1] > 1 || ti->lh[ti->levels - 1] > 1)) {
            int l = ti->levels++;

            ti->lw[l] = MAX(ti->lw[l - 1] >> 1, 1);
            ti->lh[l] = MAX(ti->lh[l - 1] >> 1, 1);
            ti->pitch[l] = ROUND_UP(ti->lw[l] * ti->f.bpp, 32);
            ti->loff[l] = ROUND_UP(ti->len, 32);
            ti->len = ti->loff[l] + (uint64_t)ti->pitch[l] * ti->lh[l];
        }
    }
    return ti->len <= 64 * MiB;
}

static void tex_params(ATIVGAState *s, int n, GLTex *t)
{
    static const GLenum wrap[8] = {
        GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE,
        GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_BORDER,
        GL_CLAMP_TO_BORDER,
    };
    uint32_t filt = s->shadow[(TX_FILTER0_0 >> 2) + n];
    uint32_t f1 = s->shadow[(TX_FORMAT1_0 >> 2) + n];
    uint32_t border = s->shadow[(TX_BORDER_COLOR_0 >> 2) + n];
    int mag = (filt >> 9) & 3, min = (filt >> 11) & 3, mip = (filt >> 13) & 3;
    bool lin = min == 2;
    GLenum minf;
    /* the border is ARGB; as X, Y, Z, W components it is B, G, R, A */
    float bc[4] = {
        (border & 0xff) / 255.0f, ((border >> 8) & 0xff) / 255.0f,
        ((border >> 16) & 0xff) / 255.0f, (border >> 24) / 255.0f,
    };
    float bcm[4];
    int sel[4] = { (f1 >> 12) & 7, (f1 >> 15) & 7, (f1 >> 18) & 7,
                   (f1 >> 9) & 7 };
    GLint sw[4];

    if (!mip || t->levels == 1) {
        minf = lin ? GL_LINEAR : GL_NEAREST;
    } else if (mip == 2) {
        minf = lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    } else {
        minf = lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    mag == 2 ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap[filt & 7]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap[(filt >> 3) & 7]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, t->levels - 1);
    /* the border colour is given in channel order of the GL texture */
    for (int i = 0; i < 4; i++) {
        bcm[i] = 0;
    }
    for (int c = 0; c < 4; c++) {
        int ch = t->compmap[c] - GL_RED;

        if (ch >= 0 && ch < 4) {
            bcm[ch] = bc[c];
        }
    }
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, bcm);
    /* TX_FORMAT1 selects: X, Y, Z, W, zero, one */
    for (int i = 0; i < 4; i++) {
        sw[i] = sel[i] < 4 ? t->compmap[sel[i]] :
                (sel[i] == 5 || sel[i] == 7) ? GL_ONE : GL_ZERO;
    }
    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, sw);
}

static GLTex *tex_slot(R300GL *g, const TexInfo *ti, bool vram, uint64_t addr)
{
    GLTex *victim = NULL;

    for (int i = 0; i < NTEX; i++) {
        GLTex *t = &g->tex[i];

        if (t->used && t->vram == vram && t->addr == addr &&
            t->fmt == ti->fmt && t->w == ti->w && t->h == ti->h &&
            t->levels == ti->levels && t->swap == ti->swap &&
            t->pitch == ti->pitch[0]) {
            t->last_use = ++g->clock;
            return t;
        }
        if (!victim || !t->used ||
            (victim->used && t->last_use < victim->last_use)) {
            if (!victim || victim->used) {
                victim = t;
            }
        }
    }
    if (victim->used) {
        glDeleteTextures(1, &victim->tex);
    }
    memset(victim, 0, sizeof(*victim));
    victim->used = true;
    victim->vram = vram;
    victim->addr = addr;
    victim->fmt = ti->fmt;
    victim->w = ti->w;
    victim->h = ti->h;
    victim->levels = ti->levels;
    victim->swap = ti->swap;
    victim->pitch = ti->pitch[0];
    victim->len = ti->len;
    victim->synced = 0;
    victim->last_use = ++g->clock;
    glGenTextures(1, &victim->tex);
    return victim;
}

static void tex_upload(GLTex *t, const TexInfo *ti, const uint8_t *data)
{
    PROF_START();
    glBindTexture(GL_TEXTURE_2D, t->tex);
    for (int l = 0; l < ti->levels; l++) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, ti->pitch[l] / ti->f.bpp);
        glTexImage2D(GL_TEXTURE_2D, l, ti->f.internal, ti->lw[l], ti->lh[l],
                     0, ti->f.format, ti->f.type, data + ti->loff[l]);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    for (int i = 0; i < 4; i++) {
        t->compmap[i] = GL_RED + i;
    }
    PROF_END(tex);
    prof.upload_bytes += ti->len;
}

/*
 * A dirty surface sampled as a texture of matching layout: copy it on the
 * GPU. Colour ARGB8888 as W8Z8Y8X8 (X is blue), float buffers as the
 * float formats.
 */
static bool tex_from_surface(ATIVGAState *s, R300GL *g, const TexInfo *ti,
                             uint64_t voff, GLTex **out)
{
    for (int i = 0; i < NSURF; i++) {
        GLSurf *sf = &g->surf[i];
        bool argb = sf->fmt == 6 && (ti->fmt == 0xc || ti->fmt == 0x13);
        bool fp = (sf->fmt == 10 && ti->fmt == 0x1a) ||
                  (sf->fmt == 7 && ti->fmt == 0x1d);
        GLTex *t;
        GLFmt f;

        if (!sf->used || !sf->dirty || sf->depth || sf->off != voff ||
            (uint64_t)sf->pitch * sf->bpp != ti->pitch[0] ||
            ti->levels != 1 || (uint32_t)ti->w > sf->pitch ||
            (uint32_t)ti->h > sf->rows || !(argb || fp)) {
            continue;
        }
        t = tex_slot(g, ti, true, voff);
        if (t->alias != sf || t->alias_gen != sf->gen) {
            color_glfmt(sf->fmt, &f);
            glBindTexture(GL_TEXTURE_2D, t->tex);
            glTexImage2D(GL_TEXTURE_2D, 0, f.internal, ti->w, ti->h, 0,
                         f.format, f.type, NULL);
            surf_attach(g, g->fbo_read, sf);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g->fbo_copy);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, t->tex, 0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, g->fbo_read);
            glBlitFramebuffer(0, 0, ti->w, ti->h, 0, 0, ti->w, ti->h,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
            t->alias = sf;
            t->alias_gen = sf->gen;
            t->synced = 0;
            if (argb) {
                t->compmap[0] = GL_BLUE;
                t->compmap[1] = GL_GREEN;
                t->compmap[2] = GL_RED;
                t->compmap[3] = GL_ALPHA;
            } else {
                for (int c = 0; c < 4; c++) {
                    t->compmap[c] = GL_RED + c;
                }
            }
        }
        *out = t;
        return true;
    }
    return false;
}

/* Texture unit n as a host texture; NULL if it must be drawn in software. */
static GLTex *tex_get(ATIVGAState *s, R300GL *g, int n)
{
    TexInfo ti;
    uint64_t voff;
    GLTex *t;
    bool vram;

    if (!tex_info(s, n, &ti)) {
        return NULL;
    }
    vram = ati_mc_vram_range(s, ti.addr, ti.len, &voff);
    if (vram) {
        if (tex_from_surface(s, g, &ti, voff, &t)) {
            return t;
        }
        flush_range(s, g, voff, ti.len, NULL);
        t = tex_slot(g, &ti, true, voff);
        if (t->alias) {
            t->alias = NULL;
            t->synced = 0;
        }
        if (!t->synced || range_gen(g, voff, ti.len) > t->synced) {
            uint8_t *tmp;
            const uint8_t *src = vram_src(s, voff, ti.len, &tmp);

            tex_upload(t, &ti, src);
            g_free(tmp);
            t->synced = g->gen;
        }
        return t;
    }
    /* system memory: staged and swapped per batch */
    t = tex_slot(g, &ti, false, ti.addr);
    if (t->epoch != g->epoch) {
        uint8_t *data = g_malloc(ti.len);

        if (!ati_mc_read(s, ti.addr, data, ti.len)) {
            g_free(data);
            return NULL;
        }
        if (ti.swap) {
            tex_swap(data, ti.len, ti.swap);
        }
        tex_upload(t, &ti, data);
        g_free(data);
        t->epoch = g->epoch;
    }
    return t;
}

/* ---- fragment programs --------------------------------------------------- */

static uint64_t fnv(uint64_t h, const void *p, size_t len)
{
    const uint8_t *b = p;

    for (size_t i = 0; i < len; i++) {
        h = (h ^ b[i]) * 0x100000001b3ull;
    }
    return h;
}

static uint64_t prog_key(const R300GLDesc *d, uint32_t units)
{
    uint64_t h = 0xcbf29ce484222325ull;
    uint32_t af = d->alpha_func & 0xf00;

    h = fnv(h, &d->nnodes, sizeof(d->nnodes));
    h = fnv(h, d->nodes, sizeof(d->nodes[0]) * d->nnodes);
    for (int n = 0; n < d->nnodes; n++) {
        for (int i = d->nodes[n].tex_start; i <= d->nodes[n].tex_end; i++) {
            h = fnv(h, &d->tex_inst[i & 31], 4);
        }
        for (int i = d->nodes[n].alu_start; i <= d->nodes[n].alu_end; i++) {
            h = fnv(h, d->alu[i & 63], 16);
        }
    }
    h = fnv(h, &d->nvary, sizeof(d->nvary));
    h = fnv(h, d->vary_dst, sizeof(d->vary_dst[0]) * d->nvary);
    h = fnv(h, &af, 4);
    h = fnv(h, &d->fog_on, 1);
    h = fnv(h, &units, 4);
    return h;
}

static void src_expr(GString *c, int addr)
{
    if (addr & 0x20) {
        g_string_append_printf(c, "K[%d]", addr & 31);
    } else {
        g_string_append_printf(c, "t[%d]", addr & 31);
    }
}

static const char *rgb_sel_expr(int sel, char *buf, size_t n)
{
    static const char *const sw[4] = { "xyz", "xxx", "yyy", "zzz" };
    static const char *const rot[2] = { "yzx", "zxy" };

    if (sel < 12) {
        snprintf(buf, n, "r%d.%s", sel / 4, sw[sel % 4]);
    } else if (sel < 15) {
        snprintf(buf, n, "vec3(a%d)", sel - 12);
    } else if (sel == 15) {
        snprintf(buf, n, "sp.xyz");
    } else if (sel <= 19) {
        snprintf(buf, n, "vec3(sp.%c)", "xyzw"[sel - 16]);
    } else if (sel == 20) {
        snprintf(buf, n, "vec3(0.0)");
    } else if (sel == 21) {
        snprintf(buf, n, "vec3(1.0)");
    } else if (sel == 22) {
        snprintf(buf, n, "vec3(0.5)");
    } else if (sel <= 28) {
        snprintf(buf, n, "r%d.%s", (sel - 23) % 3, rot[(sel - 23) / 3]);
    } else {
        int k = sel - 29;

        snprintf(buf, n, "vec3(a%d, r%d.z, r%d.y)", k, k, k);
    }
    return buf;
}

static const char *alpha_sel_expr(int sel, char *buf, size_t n)
{
    if (sel < 9) {
        snprintf(buf, n, "r%d.%c", sel / 3, "xyz"[sel % 3]);
    } else if (sel < 12) {
        snprintf(buf, n, "a%d", sel - 9);
    } else if (sel <= 15) {
        snprintf(buf, n, "sp.%c", "xyzw"[sel - 12]);
    } else if (sel == 16) {
        snprintf(buf, n, "0.0");
    } else if (sel == 17) {
        snprintf(buf, n, "1.0");
    } else if (sel == 18) {
        snprintf(buf, n, "0.5");
    } else {
        snprintf(buf, n, "0.0");
    }
    return buf;
}

static void mod_expr(GString *c, const char *v, int mod)
{
    const char *neg = (mod & 1) ? "-" : "";

    if (mod & 2) {
        g_string_append_printf(c, "%sabs(%s)", neg, v);
    } else {
        g_string_append_printf(c, "%s(%s)", neg, v);
    }
}

static const char *presub_expr(int op, bool rgb)
{
    if (rgb) {
        switch (op) {
        case 0: return "vec3(1.0) - 2.0 * r0.xyz";
        case 1: return "r1.xyz - r0.xyz";
        case 2: return "r1.xyz + r0.xyz";
        default: return "vec3(1.0) - r0.xyz";
        }
    }
    switch (op) {
    case 0: return "1.0 - 2.0 * a0";
    case 1: return "a1 - a0";
    case 2: return "a1 + a0";
    default: return "1.0 - a0";
    }
}

static const char *omod_mul(int m)
{
    static const char *const f[8] = {
        "1.0", "2.0", "4.0", "8.0", "0.5", "0.25", "0.125", "1.0"
    };
    return f[m & 7];
}

static void gen_alu(GString *c, const uint32_t *w)
{
    uint32_t ra = w[0], ri = w[1], aa = w[2], ai = w[3];
    int rop = (ri >> 23) & 0xf, aop = (ai >> 23) & 0xf;
    char b[64];

    g_string_append(c, "    {\n");
    for (int k = 0; k < 3; k++) {
        g_string_append_printf(c, "        vec4 r%d = ", k);
        src_expr(c, (ra >> (6 * k)) & 0x3f);
        g_string_append_printf(c, ";\n        float a%d = ", k);
        src_expr(c, (aa >> (6 * k)) & 0x3f);
        g_string_append(c, ".w;\n");
    }
    g_string_append_printf(c, "        vec4 sp = vec4(%s, %s);\n",
                           presub_expr((ri >> 21) & 3, true),
                           presub_expr((ai >> 21) & 3, false));
    static const char *const an[3] = { "A", "B", "C" };
    for (int k = 0; k < 3; k++) {
        g_string_append_printf(c, "        vec3 %s = ", an[k]);
        mod_expr(c, rgb_sel_expr((ri >> (7 * k)) & 0x1f, b, sizeof(b)),
                 (ri >> (7 * k + 5)) & 3);
        g_string_append_printf(c, ";\n        float %sa = ", an[k]);
        mod_expr(c, alpha_sel_expr((ai >> (7 * k)) & 0x1f, b, sizeof(b)),
                 (ai >> (7 * k + 5)) & 3);
        g_string_append(c, ";\n");
    }
    /* the dot product unit, shared by the RGB DP ops and alpha's DP */
    switch (rop) {
    case 1: g_string_append(c, "        float dt = dot(A, B);\n"); break;
    case 2: g_string_append(c, "        float dt = dot(A, B) + Aa * Ba;\n");
        break;
    case 3: g_string_append(c, "        float dt = A.x * B.x + A.y * B.y + "
                               "C.z;\n");
        break;
    default: g_string_append(c, "        float dt = 0.0;\n"); break;
    }
    g_string_append(c, "        float ra = ");
    switch (aop) {
    case 0: g_string_append(c, "Aa * Ba + Ca"); break;
    case 1: g_string_append(c, "dt"); break;
    case 2: g_string_append(c, "min(Aa, Ba)"); break;
    case 3: g_string_append(c, "max(Aa, Ba)"); break;
    case 5: g_string_append(c, "Ca > 0.5 ? Aa : Ba"); break;
    case 6: g_string_append(c, "Ca >= 0.0 ? Aa : Ba"); break;
    case 7: g_string_append(c, "fract(Aa)"); break;
    case 8: g_string_append(c, "exp2(Aa)"); break;
    case 9: g_string_append(c, "log2(abs(Aa))"); break;
    case 10: g_string_append(c, "1.0 / Aa"); break;
    case 11: g_string_append(c, "inversesqrt(abs(Aa))"); break;
    default: g_string_append(c, "0.0"); break;
    }
    g_string_append(c, ";\n        vec3 rr = ");
    switch (rop) {
    case 0: g_string_append(c, "A * B + C"); break;
    case 1: case 2: case 3: g_string_append(c, "vec3(dt)"); break;
    case 4: g_string_append(c, "min(A, B)"); break;
    case 5: g_string_append(c, "max(A, B)"); break;
    case 7: g_string_append(c, "mix(B, A, greaterThan(C, vec3(0.5)))"); break;
    case 8: g_string_append(c, "mix(B, A, greaterThanEqual(C, vec3(0.0)))");
        break;
    case 9: g_string_append(c, "fract(A)"); break;
    case 10: g_string_append(c, "vec3(ra)"); break;
    default: g_string_append(c, "vec3(0.0)"); break;
    }
    g_string_append(c, ";\n");
    g_string_append_printf(c, "        rr *= %s;\n        ra *= %s;\n",
                           omod_mul((ri >> 27) & 7), omod_mul((ai >> 27) & 7));
    if (ri & (1u << 30)) {
        g_string_append(c, "        rr = clamp(rr, 0.0, 1.0);\n");
    }
    if (ai & (1u << 30)) {
        g_string_append(c, "        ra = clamp(ra, 0.0, 1.0);\n");
    }
    int rd = (ra >> 18) & 0x1f, ad = (aa >> 18) & 0x1f;
    for (int i = 0; i < 3; i++) {
        if (ra & (1u << (23 + i))) {
            g_string_append_printf(c, "        t[%d].%c = rr.%c;\n", rd,
                                   "xyz"[i], "xyz"[i]);
        }
        if (ra & (1u << (26 + i))) {
            g_string_append_printf(c, "        oc.%c = rr.%c;\n", "xyz"[i],
                                   "xyz"[i]);
        }
    }
    if (aa & (1u << 23)) {
        g_string_append_printf(c, "        t[%d].w = ra;\n", ad);
    }
    if (aa & (1u << 24)) {
        g_string_append(c, "        oc.w = ra;\n");
    }
    if (aa & (1u << 27)) {
        g_string_append(c, "        od = ra;\n");
    }
    g_string_append(c, "    }\n");
}

static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];

        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        qemu_log_mask(LOG_UNIMP, "ati: R300 GLSL did not compile: %s\n%s\n",
                      log, src);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint build_program(const R300GLDesc *d, uint32_t units,
                            bool *writes_depth)
{
    GString *vs = g_string_new("#version 410\n");
    GString *fs = g_string_new("#version 410\n");
    GLuint v, f, prog = 0;
    GLint ok = 0;
    bool wdepth = false;

    /* vertex shader: window coordinates to clip space */
    g_string_append(vs, "layout(location = 0) in vec4 a_pos;\n");
    for (int i = 0; i < d->nvary; i++) {
        g_string_append_printf(vs, "layout(location = %d) in vec4 a_v%d;\n"
                               "out vec4 v%d;\n", i + 1, i, i);
    }
    g_string_append_printf(vs, "layout(location = %d) in float a_fog;\n"
                           "out float v_fog;\nuniform vec2 u_size;\n"
                           "void main() {\n"
                           "    float w = a_pos.w > 0.0 ? 1.0 / a_pos.w : 1.0;\n"
                           "    gl_Position = vec4((a_pos.x / u_size.x * 2.0 - 1.0) * w,\n"
                           "        (a_pos.y / u_size.y * 2.0 - 1.0) * w,\n"
                           "        (a_pos.z * 2.0 - 1.0) * w, w);\n",
                           d->nvary + 1);
    for (int i = 0; i < d->nvary; i++) {
        g_string_append_printf(vs, "    v%d = a_v%d;\n", i, i);
    }
    g_string_append(vs, "    v_fog = a_fog;\n}\n");

    /* fragment shader: the US program */
    for (int i = 0; i < d->nvary; i++) {
        g_string_append_printf(fs, "in vec4 v%d;\n", i);
    }
    g_string_append(fs, "in float v_fog;\nout vec4 o_color;\n"
                    "uniform vec4 K[32];\nuniform vec4 u_fog;\n"
                    "uniform vec3 u_fogc;\nuniform float u_aref;\n");
    for (int n = 0; n < 16; n++) {
        if (units & (1u << n)) {
            g_string_append_printf(fs, "uniform sampler2D s%d;\n", n);
        }
    }
    g_string_append(fs, "void main() {\n    vec4 t[32];\n"
                    "    for (int i = 0; i < 32; i++) t[i] = vec4(0.0);\n"
                    "    vec4 oc = vec4(0.0, 0.0, 0.0, 1.0);\n"
                    "    float od = gl_FragCoord.z;\n");
    for (int i = 0; i < d->nvary; i++) {
        g_string_append_printf(fs, "    t[%d] = v%d;\n", d->vary_dst[i], i);
    }
    for (int n = 0; n < d->nnodes; n++) {
        for (int i = d->nodes[n].tex_start; i <= d->nodes[n].tex_end; i++) {
            uint32_t ti = d->tex_inst[i & 31];
            int src = ti & 0x1f, dst = (ti >> 6) & 0x1f;
            int unit = (ti >> 11) & 0xf, op = (ti >> 15) & 7;

            if (op == 2) {
                g_string_append_printf(fs, "    if (any(lessThan(t[%d], "
                                       "vec4(0.0)))) discard;\n", src);
            } else if (!(units & (1u << unit))) {
                g_string_append_printf(fs, "    t[%d] = vec4(0.0, 0.0, 0.0, "
                                       "1.0);\n", dst);
            } else if (op == 3) {
                g_string_append_printf(fs, "    t[%d] = texture(s%d, t[%d].w "
                                       "!= 0.0 ? t[%d].xy / t[%d].w : "
                                       "t[%d].xy);\n", dst, unit, src, src,
                                       src, src);
            } else if (op == 4) {
                g_string_append_printf(fs, "    t[%d] = texture(s%d, t[%d].xy, "
                                       "t[%d].w);\n", dst, unit, src, src);
            } else {
                g_string_append_printf(fs, "    t[%d] = texture(s%d, "
                                       "t[%d].xy);\n", dst, unit, src);
            }
        }
        for (int i = d->nodes[n].alu_start; i <= d->nodes[n].alu_end; i++) {
            gen_alu(fs, d->alu[i & 63]);
            wdepth |= d->alu[i & 63][2] & (1u << 27);
        }
    }
    if (d->alpha_func & (1u << 11)) {
        int fn = (d->alpha_func >> 8) & 7;

        g_string_append_printf(fs, "    {\n        float al = clamp(oc.w, 0.0, "
                               "1.0);\n        if (!(%s%s%s false)) "
                               "discard;\n    }\n",
                               (fn & 1) ? "al < u_aref ||" : "",
                               (fn & 2) ? "al == u_aref ||" : "",
                               (fn & 4) ? "al > u_aref ||" : "");
    }
    if (d->fog_on) {
        g_string_append(fs, "    oc.xyz = mix(oc.xyz, u_fogc, clamp(v_fog * "
                        "u_fog.x + u_fog.y, 0.0, 1.0));\n");
    }
    if (wdepth) {
        g_string_append(fs, "    gl_FragDepth = clamp(od, 0.0, 1.0);\n");
    }
    g_string_append(fs, "    o_color = oc;\n}\n");

    v = compile(GL_VERTEX_SHADER, vs->str);
    f = compile(GL_FRAGMENT_SHADER, fs->str);
    if (v && f) {
        prog = glCreateProgram();
        glAttachShader(prog, v);
        glAttachShader(prog, f);
        glBindFragDataLocation(prog, 0, "o_color");
        glLinkProgram(prog);
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[2048];

            glGetProgramInfoLog(prog, sizeof(log), NULL, log);
            qemu_log_mask(LOG_UNIMP, "ati: R300 GLSL did not link: %s\n",
                          log);
            glDeleteProgram(prog);
            prog = 0;
        }
    }
    if (v) {
        glDeleteShader(v);
    }
    if (f) {
        glDeleteShader(f);
    }
    if (getenv("PPCOSX_R300_GLSL")) {
        fprintf(stderr, "r300 glsl:\n%s\n", fs->str);
    }
    g_string_free(vs, true);
    g_string_free(fs, true);
    *writes_depth = wdepth;
    return prog;
}

static GLProg *prog_get(R300GL *g, const R300GLDesc *d, uint32_t units)
{
    uint64_t key = prog_key(d, units);
    GLProg *victim = NULL;
    bool wd;

    for (int i = 0; i < NPROG; i++) {
        GLProg *p = &g->prog[i];

        if (p->used && p->key == key) {
            p->last_use = ++g->clock;
            return p;
        }
        if (!victim || !p->used ||
            (victim->used && p->last_use < victim->last_use)) {
            if (!victim || victim->used) {
                victim = p;
            }
        }
    }
    if (victim->used && victim->prog) {
        glDeleteProgram(victim->prog);
    }
    memset(victim, 0, sizeof(*victim));
    victim->used = true;
    victim->key = key;
    victim->last_use = ++g->clock;
    victim->prog = build_program(d, units, &wd);
    if (victim->prog) {
        GLuint p = victim->prog;

        victim->u_size = glGetUniformLocation(p, "u_size");
        victim->u_k = glGetUniformLocation(p, "K");
        victim->u_fog = glGetUniformLocation(p, "u_fog");
        victim->u_fogc = glGetUniformLocation(p, "u_fogc");
        victim->u_aref = glGetUniformLocation(p, "u_aref");
        glUseProgram(p);
        for (int n = 0; n < 16; n++) {
            char name[8];

            if (units & (1u << n)) {
                snprintf(name, sizeof(name), "s%d", n);
                glUniform1i(glGetUniformLocation(p, name), n);
            }
        }
    }
    return victim;
}

/* ---- state --------------------------------------------------------------- */

static GLenum blend_factor(int f)
{
    switch (f) {
    case 32: return GL_ZERO;
    case 33: return GL_ONE;
    case 34: return GL_SRC_COLOR;
    case 35: return GL_ONE_MINUS_SRC_COLOR;
    case 36: return GL_DST_COLOR;
    case 37: return GL_ONE_MINUS_DST_COLOR;
    case 38: return GL_SRC_ALPHA;
    case 39: return GL_ONE_MINUS_SRC_ALPHA;
    case 40: return GL_DST_ALPHA;
    case 41: return GL_ONE_MINUS_DST_ALPHA;
    case 42: return GL_SRC_ALPHA_SATURATE;
    case 43: return GL_CONSTANT_COLOR;
    case 44: return GL_ONE_MINUS_CONSTANT_COLOR;
    case 45: return GL_CONSTANT_ALPHA;
    case 46: return GL_ONE_MINUS_CONSTANT_ALPHA;
    default: return GL_ONE;
    }
}

static GLenum blend_eq(int fcn)
{
    switch (fcn) {
    case 2: case 3: return GL_FUNC_SUBTRACT;
    case 4: return GL_MIN;
    case 5: return GL_MAX;
    case 6: case 7: return GL_FUNC_REVERSE_SUBTRACT;
    default: return GL_FUNC_ADD;
    }
}

static GLenum cmp_func(int f)
{
    static const GLenum fn[8] = {
        GL_NEVER, GL_LESS, GL_LEQUAL, GL_EQUAL, GL_GEQUAL, GL_GREATER,
        GL_NOTEQUAL, GL_ALWAYS,
    };
    return fn[f & 7];
}

static GLenum stencil_op(int op)
{
    static const GLenum o[8] = {
        GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT,
        GL_INCR_WRAP, GL_DECR_WRAP,
    };
    return o[op & 7];
}

static void set_state(R300GL *g, const R300GLDesc *d)
{
    bool fp = d->color_fmt == 10 || d->color_fmt == 7;

    glViewport(0, 0, g->color->pitch, g->color->rows);
    glEnable(GL_SCISSOR_TEST);
    glScissor(d->clip_x0, d->clip_y0, d->clip_x1 - d->clip_x0 + 1,
              d->clip_y1 - d->clip_y0 + 1);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_CLAMP);
    /* front faces: see draw_triangle in ati_r300.c */
    glFrontFace((d->cull & 4) ? GL_CCW : GL_CW);

    if (d->cblend & 1) {
        uint32_t ab = (d->cblend & 2) ? d->ablend : d->cblend;

        glEnable(GL_BLEND);
        glBlendFuncSeparate(blend_factor((d->cblend >> 16) & 0x3f),
                            blend_factor((d->cblend >> 24) & 0x3f),
                            blend_factor((ab >> 16) & 0x3f),
                            blend_factor((ab >> 24) & 0x3f));
        glBlendEquationSeparate(blend_eq((d->cblend >> 12) & 7),
                                blend_eq((ab >> 12) & 7));
        glBlendColor(((d->blend_color >> 16) & 0xff) / 255.0f,
                     ((d->blend_color >> 8) & 0xff) / 255.0f,
                     (d->blend_color & 0xff) / 255.0f,
                     (d->blend_color >> 24) / 255.0f);
    } else {
        glDisable(GL_BLEND);
    }
    if ((d->rop & 4) && !fp) {
        glEnable(GL_COLOR_LOGIC_OP);
        glLogicOp(GL_CLEAR + ((d->rop >> 8) & 0xf));
    } else {
        glDisable(GL_COLOR_LOGIC_OP);
    }
    /* RB3D_COLOR_CHANNEL_MASK: B 0, G 1, R 2, A 3 */
    glColorMask(!!(d->cmask & 4), !!(d->cmask & 2), !!(d->cmask & 1),
                !!(d->cmask & 8));

    if (g->depth && d->z_on) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(cmp_func(d->zs_cntl & 7));
        glDepthMask(!!(d->zb_cntl & 4));
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }
    if (g->depth && d->s_on && g->depth->bpp == 4) {
        int sh = (d->zb_cntl & (1u << 4)) ? 12 : 0;

        glEnable(GL_STENCIL_TEST);
        glStencilFuncSeparate(GL_FRONT, cmp_func((d->zs_cntl >> 3) & 7),
                              d->stencil_ref, d->stencil_mask);
        glStencilOpSeparate(GL_FRONT, stencil_op((d->zs_cntl >> 6) & 7),
                            stencil_op((d->zs_cntl >> 12) & 7),
                            stencil_op((d->zs_cntl >> 9) & 7));
        glStencilFuncSeparate(GL_BACK, cmp_func((d->zs_cntl >> (3 + sh)) & 7),
                              d->stencil_ref, d->stencil_mask);
        glStencilOpSeparate(GL_BACK,
                            stencil_op((d->zs_cntl >> (6 + sh)) & 7),
                            stencil_op((d->zs_cntl >> (12 + sh)) & 7),
                            stencil_op((d->zs_cntl >> (9 + sh)) & 7));
        glStencilMask(d->stencil_wmask);
    } else {
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0);
    }
}

/* ---- drawing ------------------------------------------------------------- */

static bool gl_begin(ATIVGAState *s, R300GL *g, const R300GLDesc *d)
{
    GLFmt cf;
    GLTex *tex[16] = { NULL };
    uint32_t units = 0;
    uint32_t rows;

    if (!color_glfmt(d->color_fmt, &cf) ||
        d->nvary + 2 > MAX_ATTRIBS) {
        return false;
    }
    harvest(s, g);
    /* textures the program samples */
    for (int n = 0; n < d->nnodes; n++) {
        for (int i = d->nodes[n].tex_start; i <= d->nodes[n].tex_end; i++) {
            uint32_t ti = d->tex_inst[i & 31];
            int unit = (ti >> 11) & 0xf;

            if (((ti >> 15) & 7) != 2 &&
                (s->shadow[TX_ENABLE >> 2] & (1u << unit))) {
                units |= 1u << unit;
            }
        }
    }
    for (int n = 0; n < 16; n++) {
        if ((units & (1u << n)) && !(tex[n] = tex_get(s, g, n))) {
            return false;
        }
    }
    g->p = prog_get(g, d, units);
    if (!g->p->prog) {
        return false;
    }

    rows = d->clip_y1 + 1;
    g->color = surf_get(s, g, false, d->color_off, d->color_pitch,
                        d->color_fmt, d->color_bpp, rows);
    g->depth = NULL;
    if (g->color && (d->z_on || d->s_on)) {
        g->depth = surf_get(s, g, true, d->depth_off, d->depth_pitch,
                            d->depth_bpp, d->depth_bpp, rows);
    }
    if (!g->color || ((d->z_on || d->s_on) && !g->depth)) {
        return false;
    }
    /* sampling what is being drawn into: keep it in video memory order */
    for (int n = 0; n < 16; n++) {
        if (tex[n] && tex[n]->alias == g->color) {
            tex[n]->alias_gen = ~0u;
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g->color->tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, 0, 0);
    if (g->depth) {
        glFramebufferTexture2D(GL_FRAMEBUFFER,
                               g->depth->bpp == 4 ? GL_DEPTH_STENCIL_ATTACHMENT
                                                  : GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D, g->depth->tex, 0);
    }
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
        GL_FRAMEBUFFER_COMPLETE) {
        return false;
    }
    for (int n = 0; n < 16; n++) {
        if (tex[n]) {
            glActiveTexture(GL_TEXTURE0 + n);
            glBindTexture(GL_TEXTURE_2D, tex[n]->tex);
            tex_params(s, n, tex[n]);
        }
    }
    glActiveTexture(GL_TEXTURE0);

    glUseProgram(g->p->prog);
    glUniform2f(g->p->u_size, g->color->pitch, g->color->rows);
    glUniform4fv(g->p->u_k, 32, &d->consts[0][0]);
    glUniform4f(g->p->u_fog, d->fog_scale, d->fog_offset, 0, 0);
    glUniform3fv(g->p->u_fogc, 1, d->fog_color);
    glUniform1f(g->p->u_aref, (d->alpha_func & 0xff) / 255.0f);
    set_state(g, d);

    g->d = *d;
    g->active = true;
    g->nverts = 0;
    g->bx0 = g->by0 = INT_MAX;
    g->bx1 = g->by1 = INT_MIN;
    return true;
}

bool ati_r300_gl_begin(ATIVGAState *s, const R300GLDesc *d)
{
    R300GL *g = gl_get(s);

    if (!g) {
        return false;
    }
    if (!gl_begin(s, g, d)) {
        gl_release(g);
        return false;
    }
    return true;                /* held until ati_r300_gl_end */
}

void ati_r300_gl_triangle(ATIVGAState *s, const float *v0, const float *v1,
                          const float *v2)
{
    R300GL *g = s->r300_gl;
    size_t stride = R300GL_VERTEX_FLOATS(g->d.nvary);
    const float *v[3] = { v0, v1, v2 };

    if (!g || !g->active) {
        return;
    }
    if (g->nverts + 3 > g->cap) {
        g->cap = MAX(g->cap * 2, 96);
        g->verts = g_renew(float, g->verts, g->cap * stride);
    }
    for (int i = 0; i < 3; i++) {
        memcpy(g->verts + (g->nverts + i) * stride, v[i],
               stride * sizeof(float));
        g->bx0 = MIN(g->bx0, (int)floorf(v[i][0]));
        g->by0 = MIN(g->by0, (int)floorf(v[i][1]));
        g->bx1 = MAX(g->bx1, (int)ceilf(v[i][0]));
        g->by1 = MAX(g->by1, (int)ceilf(v[i][1]));
    }
    g->nverts += 3;
}

static void mark_dirty(GLSurf *sf, int x0, int y0, int x1, int y1)
{
    x0 = MAX(x0, 0);
    y0 = MAX(y0, 0);
    x1 = MIN(x1, (int)sf->pitch - 1);
    y1 = MIN(y1, (int)sf->rows - 1);
    if (x0 > x1 || y0 > y1) {
        return;
    }
    if (!sf->dirty) {
        sf->dirty = true;
        sf->dx0 = x0;
        sf->dy0 = y0;
        sf->dx1 = x1;
        sf->dy1 = y1;
    } else {
        sf->dx0 = MIN(sf->dx0, x0);
        sf->dy0 = MIN(sf->dy0, y0);
        sf->dx1 = MAX(sf->dx1, x1);
        sf->dy1 = MAX(sf->dy1, y1);
    }
    sf->gen++;
}

static void gl_end(ATIVGAState *s)
{
    R300GL *g = s->r300_gl;
    const R300GLDesc *d;
    size_t stride;
    int x0, y0, x1, y1;

    if (!g || !g->active) {
        return;
    }
    g->active = false;
    if (!g->nverts) {
        return;
    }
    d = &g->d;
    stride = R300GL_VERTEX_FLOATS(d->nvary) * sizeof(float);
    glBindVertexArray(g->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    glBufferData(GL_ARRAY_BUFFER, g->nverts * stride, g->verts,
                 GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, (void *)0);
    for (int i = 0; i < d->nvary; i++) {
        glEnableVertexAttribArray(i + 1);
        glVertexAttribPointer(i + 1, 4, GL_FLOAT, GL_FALSE, stride,
                              (void *)(uintptr_t)((4 + 4 * i) * 4));
    }
    glEnableVertexAttribArray(d->nvary + 1);
    glVertexAttribPointer(d->nvary + 1, 1, GL_FLOAT, GL_FALSE, stride,
                          (void *)(uintptr_t)((4 + 4 * d->nvary) * 4));
    for (int i = d->nvary + 2; i < MAX_ATTRIBS; i++) {
        glDisableVertexAttribArray(i);
    }
    PROF_START();
    glDrawArrays(GL_TRIANGLES, 0, g->nverts);
    PROF_END(draw);
    prof.draws++;

    x0 = MAX(g->bx0 - 1, d->clip_x0);
    y0 = MAX(g->by0 - 1, d->clip_y0);
    x1 = MIN(g->bx1 + 1, d->clip_x1);
    y1 = MIN(g->by1 + 1, d->clip_y1);
    if (d->cmask) {
        mark_dirty(g->color, x0, y0, x1, y1);
    }
    if (g->depth && ((d->z_on && (d->zb_cntl & 4)) ||
                     (d->s_on && d->stencil_wmask))) {
        mark_dirty(g->depth, x0, y0, x1, y1);
    }
}

void ati_r300_gl_end(ATIVGAState *s)
{
    R300GL *g = s->r300_gl;

    if (g && g->ok) {
        gl_end(s);
        gl_release(g);
    }
}

void ati_r300_gl_flush(ATIVGAState *s)
{
    R300GL *g = s->r300_gl;

    if (!g || !g->ok) {
        return;
    }
    prof.flushes++;
    if (prof_enabled() && prof.draws >= prof.reported + 1000) {
        prof.reported = prof.draws;
        fprintf(stderr, "r300gl: %" PRIu64 " flushes %" PRIu64 " draws: harvest "
                "%.1f ms (%.1f MB), surface upload %.1f ms, texture upload "
                "%.1f ms (%.1f MB in all), readback %.1f ms (%.1f MB), draw "
                "%.1f ms\n", prof.flushes, prof.draws, prof.hash / 1e6,
                prof.hash_bytes / 1e6, prof.upload / 1e6, prof.tex / 1e6,
                prof.upload_bytes / 1e6, prof.readback / 1e6,
                prof.readback_bytes / 1e6, prof.draw / 1e6);
    }
    gl_acquire(g);
    for (int i = 0; i < NSURF; i++) {
        if (g->surf[i].used && g->surf[i].dirty) {
            surf_writeback(s, g, &g->surf[i]);
        }
    }
    gl_release(g);
}

void ati_r300_gl_touch(ATIVGAState *s)
{
    R300GL *g = s->r300_gl;

    if (g && g->ok) {
        g->epoch++;             /* system memory textures: reload */
        g->harvested = false;   /* video memory: look at the log again */
    }
}

void ati_r300_gl_sync_range(ATIVGAState *s, uint64_t off, uint64_t len)
{
    R300GL *g = s->r300_gl;

    if (!g || !g->ok) {
        return;
    }
    gl_acquire(g);
    flush_range(s, g, off, len, NULL);
    gl_release(g);
}

void ati_r300_gl_changed(ATIVGAState *s, uint64_t off, uint64_t len)
{
    R300GL *g = s->r300_gl;

    if (!g || !g->ok) {
        return;
    }
    mark_range(g, off, len, false);
}
