/*
 * QEMU ATI SVGA emulation
 * Radeon (R100) 3D engine.
 *
 * The RV100 has no transform and lighting unit: drivers send vertices
 * already in window coordinates, so what is emulated here is setup and
 * rasterisation. Triangles are rasterised on the host straight into video
 * memory, through the same state registers the hardware uses:
 *
 *   RB3D_*   colour and depth buffers, blending, stencil, plane mask
 *   PP_*     texture units, texture combiners, alpha test, fog
 *   SE_*     culling and shading, vertex format
 *   RE_*     scissor
 *
 * Pixels are read and written in the card's byte order (see vram_xor), so
 * the 2D engine, the display and the CPU all see what was drawn.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "ati_int.h"
#include "ati_regs.h"

/* ---- registers ----------------------------------------------------------- */

#define PP_MISC                 0x1c14
#define PP_FOG_COLOR            0x1c18
#define RE_SOLID_COLOR          0x1c1c
#define RB3D_BLENDCNTL          0x1c20
#define RB3D_DEPTHOFFSET        0x1c24
#define RB3D_DEPTHPITCH         0x1c28
#define RB3D_ZSTENCILCNTL       0x1c2c
#define PP_CNTL                 0x1c38
#define RB3D_CNTL               0x1c3c
#define RB3D_COLOROFFSET        0x1c40
#define RE_WIDTH_HEIGHT         0x1c44
#define RB3D_COLORPITCH         0x1c48
#define SE_CNTL                 0x1c4c
#define SE_COORD_FMT            0x1c50
#define PP_TXFILTER_0           0x1c54
#define PP_TXFORMAT_0           0x1c58
#define PP_TXOFFSET_0           0x1c5c
#define PP_TXCBLEND_0           0x1c60
#define PP_TXABLEND_0           0x1c64
#define PP_TFACTOR_0            0x1c68
#define PP_TEX_UNIT_STRIDE      0x18
#define PP_TEX_SIZE_0           0x1d04
#define PP_TEX_PITCH_0          0x1d08
#define PP_TEX_SIZE_STRIDE      0x08
#define PP_BORDER_COLOR_0       0x1d40
#define RB3D_STENCILREFMASK     0x1d7c
#define RB3D_ROPCNTL            0x1d80
#define RB3D_PLANEMASK          0x1d84
#define RE_SCISSOR_TL_0         0x1cd8      /* BR_0 at +4, then _1, _2 */
#define RE_AUX_SCISSOR_CNTL     0x26f0
#define RE_TOP_LEFT             0x26c0
#define RB3D_DEPTHCLEARVALUE    0x3230

/* RB3D_CNTL */
#define ALPHA_BLEND_ENABLE      (1u << 0)
#define PLANE_MASK_ENABLE       (1u << 1)
#define ROP_ENABLE              (1u << 6)
#define STENCIL_ENABLE          (1u << 7)
#define Z_ENABLE                (1u << 8)

/* PP_CNTL */
#define TEX_ENABLE(n)           (1u << (4 + (n)))
#define TEX_BLEND_ENABLE(n)     (1u << (12 + (n)))
#define SPECULAR_ENABLE         (1u << 21)
#define FOG_ENABLE              (1u << 22)
#define ALPHA_TEST_ENABLE       (1u << 23)

/* RB3D_ZSTENCILCNTL */
#define Z_WRITE_ENABLE          (1u << 30)

/* vertex format (SE_VTX_FMT / packet word) */
#define VTX_W0                  (1u << 0)
#define VTX_FPCOLOR             (1u << 1)
#define VTX_FPALPHA             (1u << 2)
#define VTX_PKCOLOR             (1u << 3)
#define VTX_FPSPEC              (1u << 4)
#define VTX_FPFOG               (1u << 5)
#define VTX_PKSPEC              (1u << 6)
#define VTX_ST0                 (1u << 7)
#define VTX_ST1                 (1u << 8)
#define VTX_Q1                  (1u << 9)
#define VTX_ST2                 (1u << 10)
#define VTX_Q2                  (1u << 11)
#define VTX_ST3                 (1u << 12)
#define VTX_Q3                  (1u << 13)
#define VTX_Q0                  (1u << 14)
#define VTX_Z                   (1u << 31)

#define ATI_TEX_UNITS           3

/* Debugging: PPCOSX_ATI_3D_STATS prints counts every 1000 draws. */
static struct {
    uint64_t draws, tris, culled, pixels, ztest_fail, written;
} stats;

static void stats_tick(ATIVGAState *s)
{
    static int enabled = -1;

    if (enabled < 0) {
        enabled = getenv("PPCOSX_ATI_3D_STATS") != NULL;
    }
    if (enabled && ++stats.draws % 1000 == 0) {
        fprintf(stderr, "ati3d: draws %" PRIu64 " tris %" PRIu64 " culled %"
                PRIu64 " pixels %" PRIu64 " zfail %" PRIu64 " written %"
                PRIu64 " cntl %08x pp %08x se %08x color %08x/%08x "
                "re %08x/%08x\n", stats.draws, stats.tris, stats.culled,
                stats.pixels, stats.ztest_fail, stats.written,
                s->shadow[0x1c3c >> 2], s->shadow[0x1c38 >> 2],
                s->shadow[0x1c4c >> 2], s->shadow[0x1c40 >> 2],
                s->shadow[0x1c48 >> 2], s->shadow[0x26c0 >> 2],
                s->shadow[0x1c44 >> 2]);
    }
}

static inline uint32_t reg(ATIVGAState *s, uint32_t r)
{
    return s->shadow[r >> 2];
}

static inline float u2f(uint32_t v)
{
    union { uint32_t u; float f; } c = { .u = v };
    return c.f;
}

/* ---- video memory pixels ------------------------------------------------- */

#define vram_ld ati_vram_ld
#define vram_st ati_vram_st

/* ---- state for one draw -------------------------------------------------- */

typedef struct ATIVertex {
    float x, y, z, w;           /* w is 1/w (W0) */
    float c[4];                 /* diffuse r, g, b, a */
    float sp[4];                /* specular r, g, b, fog */
    float tc[ATI_TEX_UNITS][3]; /* s, t, q */
} ATIVertex;

typedef struct ATITexture {
    bool enabled;
    uint32_t format;            /* TXFORMAT_* */
    int bpp;                    /* bytes per texel; 0 for compressed */
    int w, h;
    uint32_t pitch;             /* bytes */
    bool vram;                  /* in video memory (else staged) */
    uint64_t vram_off;
    uint8_t *data;              /* staged copy in the card's order */
    uint32_t swap;              /* TXOFFSET endian swap */
    bool micro_tile;            /* TXOFFSET MICRO_TILE_X2 */
    bool mag_linear, min_linear;
    int min_mode;               /* TXFILTER MIN_FILTER */
    int levels;                 /* mipmap levels present */
    uint64_t lvl_off[12];
    int lvl_w[12], lvl_h[12];
    uint32_t lvl_pitch[12];
    float lod;                  /* level of detail for the triangle */
    int wrap_s, wrap_t;
    bool perspective;
    bool nonparametric;         /* SE_COORD_FMT VTX_STn_NONPARAMETRIC */
    float border[4];
    uint32_t cblend, ablend;
    float tfactor[4];
} ATITexture;

typedef struct ATIDraw {
    ATIVGAState *s;
    uint32_t rb3d_cntl, pp_cntl, se_cntl, zcntl, blend, pp_misc;
    uint64_t color_off;         /* in video memory... */
    uint8_t *color_stage;       /* ...or rows staged from system memory */
    uint32_t color_mc;          /* card address of the staged buffer */
    uint32_t color_swap;        /* COLOR_ENDIAN, for system memory */
    uint32_t coord_fmt;         /* SE_COORD_FMT */
    uint32_t color_pitch;       /* pixels */
    int color_fmt, color_bpp;
    uint64_t depth_off;
    uint32_t depth_pitch;       /* pixels */
    int depth_bpp;
    int clip_x0, clip_y0, clip_x1, clip_y1;     /* inclusive */
    uint32_t planemask;
    int rop;                    /* RB3D_ROPCNTL logic op, GL order */
    uint32_t stencil_ref, stencil_mask, stencil_wmask;
    float fog_color[3];
    ATITexture tex[ATI_TEX_UNITS];
    int dirty_y0, dirty_y1;
    /* Rage 128: its own combiners and logic op (see r128_draw_begin) */
    bool r128;
    uint32_t r128_comb[2];      /* PRIM_ and SEC_TEXTURE_COMBINE_CNTL_C */
    float const_color[4];       /* CONSTANT_COLOR_C */
    uint32_t rop3;              /* DP_GUI_MASTER_CNTL ROP3, 0xcc = copy */
} ATIDraw;

/* ---- textures ------------------------------------------------------------ */

static int tex_bpp(uint32_t fmt)
{
    switch (fmt & 0x1f) {
    case 0: case 2: case 8:     /* I8, RGB332, Y8 */
        return 1;
    case 1: case 3: case 4: case 5: case 10: case 11:
        return 2;               /* AI88, 1555, 565, 4444, YUV422 */
    case 6: case 7:
        return 4;               /* ARGB8888, RGBA8888 */
    default:
        return 0;               /* DXTn */
    }
}

static inline uint32_t tex_word(ATIDraw *d, ATITexture *t, uint64_t off);

static void tex_setup(ATIDraw *d, int n)
{
    ATIVGAState *s = d->s;
    ATITexture *t = &d->tex[n];
    uint32_t base = n * PP_TEX_UNIT_STRIDE;
    uint32_t filter = reg(s, PP_TXFILTER_0 + base);
    uint32_t offset = reg(s, PP_TXOFFSET_0 + base);
    uint32_t border = reg(s, PP_BORDER_COLOR_0 + n * 4);
    uint32_t tf = reg(s, PP_TFACTOR_0 + base);
    uint64_t len;

    memset(t, 0, sizeof(*t));
    t->cblend = reg(s, PP_TXCBLEND_0 + base);
    t->ablend = reg(s, PP_TXABLEND_0 + base);
    t->tfactor[0] = ((tf >> 16) & 0xff) / 255.0f;
    t->tfactor[1] = ((tf >> 8) & 0xff) / 255.0f;
    t->tfactor[2] = (tf & 0xff) / 255.0f;
    t->tfactor[3] = (tf >> 24) / 255.0f;
    if (!(d->pp_cntl & TEX_ENABLE(n))) {
        return;
    }
    t->format = reg(s, PP_TXFORMAT_0 + base);
    t->bpp = tex_bpp(t->format);
    if (t->format & (1u << 7)) {            /* NON_POWER2: explicit size */
        uint32_t size = reg(s, PP_TEX_SIZE_0 + n * PP_TEX_SIZE_STRIDE);

        t->w = (size & 0x7ff) + 1;
        t->h = ((size >> 16) & 0x7ff) + 1;
        t->pitch = (reg(s, PP_TEX_PITCH_0 + n * PP_TEX_SIZE_STRIDE) &
                    0x3fe0) + 32;
    } else {
        t->w = 1 << ((t->format >> 8) & 0xf);
        t->h = 1 << ((t->format >> 12) & 0xf);
        t->pitch = MAX(t->w * MAX(t->bpp, 1), 32);
    }
    t->swap = offset & 3;
    t->micro_tile = offset & (1u << 3);
    t->mag_linear = filter & 1;
    t->min_mode = (filter >> 1) & 0xf;
    t->min_linear = t->min_mode == 1 || (t->min_mode & 4);
    t->wrap_s = (filter >> 22) & 7;
    t->wrap_t = (filter >> 27) & 7;
    t->perspective = true;
    t->nonparametric = reg(s, SE_COORD_FMT) & (1u << (8 + n));
    t->border[0] = ((border >> 16) & 0xff) / 255.0f;
    t->border[1] = ((border >> 8) & 0xff) / 255.0f;
    t->border[2] = (border & 0xff) / 255.0f;
    t->border[3] = (border >> 24) / 255.0f;
    if (!t->bpp) {
        static bool warned;

        if (!warned) {
            warned = true;
            qemu_log_mask(LOG_UNIMP, "ati: compressed texture format %u "
                          "not implemented\n", t->format & 0x1f);
        }
        return;
    }
    /*
     * Mipmaps follow level 0, each level's rows a multiple of 32 bytes and
     * each level starting on a 32-byte boundary.
     */
    t->levels = 1;
    t->lvl_off[0] = 0;
    t->lvl_w[0] = t->w;
    t->lvl_h[0] = t->h;
    t->lvl_pitch[0] = t->pitch;
    len = (uint64_t)t->pitch * t->h;
    if (t->min_mode >= 2 && !(t->format & (1u << 7))) {
        int max_level = (filter >> 16) & 0xf;

        while (t->levels <= max_level && t->levels < 12 &&
               (t->lvl_w[t->levels - 1] > 1 || t->lvl_h[t->levels - 1] > 1)) {
            int l = t->levels++;

            t->lvl_w[l] = MAX(t->lvl_w[l - 1] >> 1, 1);
            t->lvl_h[l] = MAX(t->lvl_h[l - 1] >> 1, 1);
            t->lvl_pitch[l] = ROUND_UP(MAX(t->lvl_w[l] * t->bpp, 32), 32);
            t->lvl_off[l] = ROUND_UP(len, 32);
            len = t->lvl_off[l] + (uint64_t)t->lvl_pitch[l] * t->lvl_h[l];
        }
    }
    if (ati_mc_vram_range(s, offset & ~0x1fu, len, &t->vram_off)) {
        t->vram = true;
    } else if (len <= 64 * MiB) {
        t->data = g_malloc(len);
        if (!ati_mc_read(s, offset & ~0x1fu, t->data, len)) {
            g_free(t->data);
            t->data = NULL;
            return;
        }
    } else {
        return;
    }
    t->enabled = true;
    if (getenv("PPCOSX_ATI_3D_STATS")) {
        static uint32_t seen[64][2];
        static int nseen;
        bool found = false;

        for (int i = 0; i < nseen; i++) {
            found |= seen[i][0] == offset && seen[i][1] == t->format;
        }
        if (!found && nseen < 64) {
            seen[nseen][0] = offset;
            seen[nseen++][1] = t->format;
            fprintf(stderr, "ati3d: tex%d offset %08x format %08x filter "
                    "%08x cblend %08x ablend %08x size %08x pitch %08x "
                    "-> %dx%d pitch %u bpp %d vram %d first %08x %08x\n",
                    n, offset, t->format, filter, t->cblend, t->ablend,
                    reg(s, PP_TEX_SIZE_0 + n * PP_TEX_SIZE_STRIDE),
                    reg(s, PP_TEX_PITCH_0 + n * PP_TEX_SIZE_STRIDE),
                    t->w, t->h, t->pitch, t->bpp, t->vram,
                    tex_word(d, t, 0), tex_word(d, t, 4));
        }
    }
}

static void tex_release(ATIDraw *d)
{
    for (int n = 0; n < ATI_TEX_UNITS; n++) {
        g_free(d->tex[n].data);
        d->tex[n].data = NULL;
    }
}

/* Wrap a texel coordinate per the CLAMP_* mode; -1 means border. */
static int tex_wrap(int i, int size, int mode)
{
    switch (mode) {
    case 0:                                     /* wrap */
        i %= size;
        return i < 0 ? i + size : i;
    case 1: {                                   /* mirror */
        int period = 2 * size;

        i %= period;
        if (i < 0) {
            i += period;
        }
        return i < size ? i : period - 1 - i;
    }
    case 4: case 5:                             /* clamp to border */
        return i < 0 || i >= size ? -1 : i;
    default:                                    /* clamp to edge */
        return i < 0 ? 0 : i >= size ? size - 1 : i;
    }
}

static inline uint32_t tex_word(ATIDraw *d, ATITexture *t, uint64_t off)
{
    uint32_t w;

    off &= ~3ull;
    if (t->vram) {
        /* endian swapping applies to reads over the bus only */
        return vram_ld(d->s, t->vram_off + off, 4);
    }
    w = ldl_le_p(t->data + off);
    switch (t->swap) {
    case 1:
        return ((w & 0x00ff00ff) << 8) | ((w >> 8) & 0x00ff00ff);
    case 2:
        return bswap32(w);
    case 3:
        return (w << 16) | (w >> 16);
    default:
        return w;
    }
}

static void tex_texel(ATIDraw *d, ATITexture *t, int l, int x, int y,
                      float out[4])
{
    uint64_t off;
    uint32_t v;

    x = tex_wrap(x, t->lvl_w[l], t->wrap_s);
    y = tex_wrap(y, t->lvl_h[l], t->wrap_t);
    if (x < 0 || y < 0) {
        memcpy(out, t->border, sizeof(t->border));
        return;
    }
    /*
     * Tiled textures (TXOFFSET tile bits) are stored linearly here: the
     * blits that fill them (DST_TILE) and the CPU's surfaces are linear.
     */
    off = t->lvl_off[l] + (uint64_t)y * t->lvl_pitch[l] +
          (uint64_t)x * t->bpp;
    v = tex_word(d, t, off) >> (8 * (off & 3));
    switch (t->format & 0x1f) {
    case 0:                                     /* I8 */
        out[0] = out[1] = out[2] = out[3] = (v & 0xff) / 255.0f;
        break;
    case 1:                                     /* AI88 */
        out[0] = out[1] = out[2] = (v & 0xff) / 255.0f;
        out[3] = ((v >> 8) & 0xff) / 255.0f;
        break;
    case 2:                                     /* RGB332 */
        out[0] = ((v >> 5) & 7) / 7.0f;
        out[1] = ((v >> 2) & 7) / 7.0f;
        out[2] = (v & 3) / 3.0f;
        out[3] = 1;
        break;
    case 3:                                     /* ARGB1555 */
        out[0] = ((v >> 10) & 0x1f) / 31.0f;
        out[1] = ((v >> 5) & 0x1f) / 31.0f;
        out[2] = (v & 0x1f) / 31.0f;
        out[3] = (v >> 15) & 1;
        break;
    case 4:                                     /* RGB565 */
        out[0] = ((v >> 11) & 0x1f) / 31.0f;
        out[1] = ((v >> 5) & 0x3f) / 63.0f;
        out[2] = (v & 0x1f) / 31.0f;
        out[3] = 1;
        break;
    case 5:                                     /* ARGB4444 */
        out[0] = ((v >> 8) & 0xf) / 15.0f;
        out[1] = ((v >> 4) & 0xf) / 15.0f;
        out[2] = (v & 0xf) / 15.0f;
        out[3] = ((v >> 12) & 0xf) / 15.0f;
        break;
    case 6:                                     /* ARGB8888 */
        out[0] = ((v >> 16) & 0xff) / 255.0f;
        out[1] = ((v >> 8) & 0xff) / 255.0f;
        out[2] = (v & 0xff) / 255.0f;
        out[3] = (v >> 24) / 255.0f;
        break;
    case 7:                                     /* RGBA8888 */
        out[0] = (v >> 24) / 255.0f;
        out[1] = ((v >> 16) & 0xff) / 255.0f;
        out[2] = ((v >> 8) & 0xff) / 255.0f;
        out[3] = (v & 0xff) / 255.0f;
        break;
    case 8:                                     /* Y8 */
        out[0] = out[1] = out[2] = (v & 0xff) / 255.0f;
        out[3] = 1;
        break;
    case 10: case 11: {                         /* YUV 4:2:2 */
        uint32_t pair = tex_word(d, t, off & ~3ull);
        float yy = ((x & 1) ? (pair >> 24) : (pair >> 8)) & 0xff;
        float u = (pair & 0xff) - 128.0f, vv = ((pair >> 16) & 0xff) - 128.0f;

        if ((t->format & 0x1f) == 11) {
            yy = ((x & 1) ? (pair >> 16) : pair) & 0xff;
            u = ((pair >> 8) & 0xff) - 128.0f;
            vv = (pair >> 24) - 128.0f;
        }
        yy = 1.164f * (yy - 16);
        out[0] = (yy + 1.596f * vv) / 255.0f;
        out[1] = (yy - 0.813f * vv - 0.391f * u) / 255.0f;
        out[2] = (yy + 2.018f * u) / 255.0f;
        out[3] = 1;
        for (int i = 0; i < 3; i++) {
            out[i] = out[i] < 0 ? 0 : out[i] > 1 ? 1 : out[i];
        }
        break;
    }
    default:
        out[0] = out[1] = out[2] = out[3] = 1;
    }
    if (!(t->format & (1u << 6)) && (t->format & 0x1f) <= 2) {
        /* I8 and RGB332 without ALPHA_IN_MAP have opaque alpha */
        if ((t->format & 0x1f) != 1) {
            out[3] = (t->format & 0x1f) == 0 ? out[3] : 1;
        }
    }
}

/* Sample level `l`, nearest or bilinear. */
static void tex_sample_level(ATIDraw *d, ATITexture *t, int l, bool linear,
                             float s, float tt, float out[4])
{
    /* non-parametric coordinates are in texels already */
    float su = t->nonparametric ? s : s * t->lvl_w[l];
    float tv = t->nonparametric ? tt : tt * t->lvl_h[l];
    float u = su - 0.5f, v = tv - 0.5f;

    if (!linear) {
        tex_texel(d, t, l, (int)floorf(su), (int)floorf(tv), out);
        return;
    }
    int x0 = (int)floorf(u), y0 = (int)floorf(v);
    float fx = u - x0, fy = v - y0;
    float a[4], b[4], c[4], e[4];

    tex_texel(d, t, l, x0, y0, a);
    tex_texel(d, t, l, x0 + 1, y0, b);
    tex_texel(d, t, l, x0, y0 + 1, c);
    tex_texel(d, t, l, x0 + 1, y0 + 1, e);
    for (int i = 0; i < 4; i++) {
        float top = a[i] + (b[i] - a[i]) * fx;
        float bot = c[i] + (e[i] - c[i]) * fx;

        out[i] = top + (bot - top) * fy;
    }
}

static void tex_sample(ATIDraw *d, ATITexture *t, float s, float tt,
                       float out[4])
{
    float lod = t->lod;

    if (lod <= 0 || t->levels == 1) {
        tex_sample_level(d, t, 0, lod <= 0 ? t->mag_linear : t->min_linear,
                         s, tt, out);
        return;
    }
    if (lod > t->levels - 1) {
        lod = t->levels - 1;
    }
    int l0 = (int)floorf(lod);
    if (!(t->min_mode & 1) || l0 + 1 >= t->levels) {
        /* nearest level */
        tex_sample_level(d, t, MIN((int)(lod + 0.5f), t->levels - 1),
                         t->min_linear, s, tt, out);
        return;
    }
    /* between two levels */
    float a[4], b[4], f = lod - l0;

    tex_sample_level(d, t, l0, t->min_linear, s, tt, a);
    tex_sample_level(d, t, l0 + 1, t->min_linear, s, tt, b);
    for (int i = 0; i < 4; i++) {
        out[i] = a[i] + (b[i] - a[i]) * f;
    }
}

/* ---- texture combiners --------------------------------------------------- */

typedef struct ATIFragment {
    float diffuse[4], specular[4];
    float texel[ATI_TEX_UNITS][4];
    float cur[4];
} ATIFragment;

static float color_arg(ATIFragment *f, ATITexture *t, unsigned int sel,
                       int ch)
{
    switch (sel) {
    case 2: return f->cur[ch];
    case 3: return f->cur[3];
    case 4: return f->diffuse[ch];
    case 5: return f->diffuse[3];
    case 6: return f->specular[ch];
    case 7: return f->specular[3];
    case 8: return t->tfactor[ch];
    case 9: return t->tfactor[3];
    case 10: case 12: case 14:
        return f->texel[(sel - 10) / 2][ch];
    case 11: case 13: case 15:
        return f->texel[(sel - 11) / 2][3];
    default: return 0;
    }
}

static float alpha_arg(ATIFragment *f, ATITexture *t, unsigned int sel)
{
    switch (sel) {
    case 1: return f->cur[3];
    case 2: return f->diffuse[3];
    case 3: return f->specular[3];
    case 4: return t->tfactor[3];
    case 5: case 6: case 7:
        return f->texel[sel - 5][3];
    default: return 0;
    }
}

static float combine(uint32_t ctl, float a, float b, float c)
{
    float r;

    if (ctl & (1u << 15)) {
        a = 1 - a;
    }
    if (ctl & (1u << 16)) {
        b = 1 - b;
    }
    if (ctl & (1u << 17)) {
        c = 1 - c;
    }
    switch ((ctl >> 18) & 7) {
    case 1:  r = a * b - c; break;              /* subtract */
    case 2:  r = a * b + c - 0.5f; break;       /* add signed */
    case 3:  r = b * c + a * (1 - c); break;    /* blend (interpolate) */
    default: r = a * b + c; break;              /* add */
    }
    r *= 1 << ((ctl >> 21) & 3);
    return r < 0 ? 0 : r > 1 ? 1 : r;
}

static void combine_stage(ATIFragment *f, ATITexture *t)
{
    uint32_t cb = t->cblend, ab = t->ablend;
    float out[4];

    if (((cb >> 18) & 7) >= 4) {                /* DOT3 */
        float dot = 0;

        for (int ch = 0; ch < 3; ch++) {
            dot += (color_arg(f, t, cb & 0x1f, ch) - 0.5f) *
                   (color_arg(f, t, (cb >> 5) & 0x1f, ch) - 0.5f);
        }
        dot = 4 * dot;
        dot = dot < 0 ? 0 : dot > 1 ? 1 : dot;
        out[0] = out[1] = out[2] = dot;
    } else {
        for (int ch = 0; ch < 3; ch++) {
            out[ch] = combine(cb, color_arg(f, t, cb & 0x1f, ch),
                              color_arg(f, t, (cb >> 5) & 0x1f, ch),
                              color_arg(f, t, (cb >> 10) & 0x1f, ch));
        }
    }
    out[3] = combine(ab, alpha_arg(f, t, ab & 0xf),
                     alpha_arg(f, t, (ab >> 4) & 0xf),
                     alpha_arg(f, t, (ab >> 8) & 0xf));
    memcpy(f->cur, out, sizeof(out));
}

/*
 * Rage 128 texture combiners (PRIM_/SEC_TEXTURE_COMBINE_CNTL_C), one per
 * texture: an input (the interpolated colour, the previous stage or the
 * constant colour), a factor (mostly the texel) and a function of the two:
 *   3:0   colour function (bit 8 extends it)    7:4   colour factor
 *   13:10 colour input                          17:14 alpha function
 *   21:18 alpha factor                          27:25 alpha input
 */
static float r128_cin(ATIDraw *d, ATIFragment *f, unsigned int sel, int ch)
{
    switch (sel) {
    case 2: return d->const_color[ch];
    case 3: return d->const_color[3];
    case 5: return f->diffuse[3];
    case 8: return f->cur[ch];
    case 9: return f->cur[3];
    default: return f->diffuse[ch];             /* 4: interpolated */
    }
}

static float r128_cfactor(ATIDraw *d, ATIFragment *f, const float *t,
                          unsigned int sel, int ch)
{
    switch (sel) {
    case 0: return d->const_color[ch];
    case 1: return 1 - d->const_color[ch];
    case 2: return d->const_color[3];
    case 3: return 1 - d->const_color[3];
    case 5: return 1 - t[ch];
    case 6: return t[3];
    case 7: return 1 - t[3];
    case 8: return f->cur[ch];
    default: return t[ch];                      /* 4: the texel */
    }
}

static float r128_ain(ATIDraw *d, ATIFragment *f, unsigned int sel)
{
    switch (sel) {
    case 1: return d->const_color[3];
    case 4: return f->cur[3];
    default: return f->diffuse[3];              /* 2: interpolated */
    }
}

static float r128_afactor(ATIDraw *d, const float *t, unsigned int sel)
{
    switch (sel) {
    case 0: case 2: return d->const_color[3];
    case 1: case 3: return 1 - d->const_color[3];
    case 7: return 1 - t[3];
    default: return t[3];                       /* 6: texel alpha */
    }
}

static float r128_fn(unsigned int fn, float in, float fac, float t_a,
                     float v_a, float c_a, float p_a, float cc)
{
    float r;

    switch (fn) {
    case 0: case 1: r = fac; break;             /* disable, copy */
    case 2: r = in; break;                      /* copy input */
    case 3: r = in * fac; break;                /* modulate */
    case 4: r = 2 * in * fac; break;
    case 5: r = 4 * in * fac; break;
    case 6: r = in + fac; break;                /* add */
    case 7: r = in + fac - 0.5f; break;         /* add signed */
    case 8: r = in * (1 - v_a) + fac * v_a; break;      /* blend vertex */
    case 9: r = in * (1 - t_a) + fac * t_a; break;      /* blend texture */
    case 10: r = in * (1 - c_a) + fac * c_a; break;     /* blend constant */
    case 11: r = in * (1 - t_a) + fac; break;           /* premultiplied */
    case 12: r = in * (1 - p_a) + fac * p_a; break;     /* blend previous */
    case 13: r = in * t_a + fac; break;
    case 14: r = 2 * (in + fac - 0.5f); break;          /* add signed 2x */
    case 15: case 20:           /* GL_BLEND: input towards the constant */
        r = in * (1 - fac) + cc * fac;
        break;
    default: r = in * fac; break;
    }
    return r < 0 ? 0 : r > 1 ? 1 : r;
}

static void r128_combine(ATIDraw *d, ATIFragment *f, int n)
{
    uint32_t c = d->r128_comb[n];
    const float *t = f->texel[n];
    unsigned int fn = (c & 0xf) | ((c >> 4) & 0x10);
    unsigned int afn = (c >> 14) & 0xf;
    float out[4];

    for (int ch = 0; ch < 3; ch++) {
        out[ch] = r128_fn(fn, r128_cin(d, f, (c >> 10) & 0xf, ch),
                          r128_cfactor(d, f, t, (c >> 4) & 0xf, ch),
                          t[3], f->diffuse[3], d->const_color[3],
                          f->cur[3], d->const_color[ch]);
    }
    out[3] = r128_fn(afn, r128_ain(d, f, (c >> 25) & 7),
                     r128_afactor(d, t, (c >> 18) & 0xf), t[3],
                     f->diffuse[3], d->const_color[3], f->cur[3],
                     d->const_color[3]);
    memcpy(f->cur, out, sizeof(out));
}

/* A ROP3 with the pattern left out, applied bitwise to source and dest. */
static inline uint32_t rop3_apply(uint32_t rop, uint32_t s, uint32_t dst)
{
    uint32_t r = 0;

    r |= (rop & 0x01) ? ~s & ~dst : 0;
    r |= (rop & 0x02) ? ~s & dst : 0;
    r |= (rop & 0x04) ? s & ~dst : 0;
    r |= (rop & 0x08) ? s & dst : 0;
    return r;
}

/* ---- tests, blending, pixel formats -------------------------------------- */

static inline bool compare(int func, uint32_t a, uint32_t b)
{
    switch (func) {
    case 0: return false;
    case 1: return a < b;
    case 2: return a <= b;
    case 3: return a == b;
    case 4: return a >= b;
    case 5: return a > b;
    case 6: return a != b;
    default: return true;
    }
}

static void unpack_color(int fmt, uint32_t v, float c[4])
{
    switch (fmt) {
    case 3:
        c[0] = ((v >> 10) & 0x1f) / 31.0f;
        c[1] = ((v >> 5) & 0x1f) / 31.0f;
        c[2] = (v & 0x1f) / 31.0f;
        c[3] = (v >> 15) & 1;
        break;
    case 4:
        c[0] = ((v >> 11) & 0x1f) / 31.0f;
        c[1] = ((v >> 5) & 0x3f) / 63.0f;
        c[2] = (v & 0x1f) / 31.0f;
        c[3] = 1;
        break;
    case 15:
        c[0] = ((v >> 8) & 0xf) / 15.0f;
        c[1] = ((v >> 4) & 0xf) / 15.0f;
        c[2] = (v & 0xf) / 15.0f;
        c[3] = ((v >> 12) & 0xf) / 15.0f;
        break;
    case 7:
        c[0] = ((v >> 5) & 7) / 7.0f;
        c[1] = ((v >> 2) & 7) / 7.0f;
        c[2] = (v & 3) / 3.0f;
        c[3] = 1;
        break;
    case 8:
        c[0] = c[1] = c[2] = (v & 0xff) / 255.0f;
        c[3] = 1;
        break;
    default:
        c[0] = ((v >> 16) & 0xff) / 255.0f;
        c[1] = ((v >> 8) & 0xff) / 255.0f;
        c[2] = (v & 0xff) / 255.0f;
        c[3] = (v >> 24) / 255.0f;
        break;
    }
}

static inline uint32_t q(float v, int max)
{
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    return (uint32_t)(v * max + 0.5f);
}

static uint32_t pack_color(int fmt, const float c[4])
{
    switch (fmt) {
    case 3:
        return q(c[3], 1) << 15 | q(c[0], 31) << 10 | q(c[1], 31) << 5 |
               q(c[2], 31);
    case 4:
        return q(c[0], 31) << 11 | q(c[1], 63) << 5 | q(c[2], 31);
    case 15:
        return q(c[3], 15) << 12 | q(c[0], 15) << 8 | q(c[1], 15) << 4 |
               q(c[2], 15);
    case 7:
        return q(c[0], 7) << 5 | q(c[1], 7) << 2 | q(c[2], 3);
    case 8:
        return q(c[0] * 0.299f + c[1] * 0.587f + c[2] * 0.114f, 255);
    default:
        return q(c[3], 255) << 24 | q(c[0], 255) << 16 | q(c[1], 255) << 8 |
               q(c[2], 255);
    }
}

static void blend_factor(int f, const float src[4], const float dst[4],
                         float out[4])
{
    switch (f) {
    case 32: out[0] = out[1] = out[2] = out[3] = 0; return;
    case 33: out[0] = out[1] = out[2] = out[3] = 1; return;
    case 34: memcpy(out, src, 16); return;
    case 35: for (int i = 0; i < 4; i++) { out[i] = 1 - src[i]; } return;
    case 36: memcpy(out, dst, 16); return;
    case 37: for (int i = 0; i < 4; i++) { out[i] = 1 - dst[i]; } return;
    case 38: out[0] = out[1] = out[2] = out[3] = src[3]; return;
    case 39: out[0] = out[1] = out[2] = out[3] = 1 - src[3]; return;
    case 40: out[0] = out[1] = out[2] = out[3] = dst[3]; return;
    case 41: out[0] = out[1] = out[2] = out[3] = 1 - dst[3]; return;
    case 42: {
        float m = MIN(src[3], 1 - dst[3]);

        out[0] = out[1] = out[2] = m;
        out[3] = 1;
        return;
    }
    default: out[0] = out[1] = out[2] = out[3] = 1; return;
    }
}

static uint32_t stencil_op(int op, uint32_t v, uint32_t ref)
{
    switch (op) {
    case 1: return 0;
    case 2: return ref;
    case 3: return v < 0xff ? v + 1 : v;
    case 4: return v > 0 ? v - 1 : v;
    case 5: return ~v & 0xff;
    case 6: return (v + 1) & 0xff;
    case 7: return (v - 1) & 0xff;
    default: return v;
    }
}

static inline uint32_t swap_val(uint32_t v, int bytes, uint32_t mode)
{
    if (bytes == 4 && mode == 2) {
        return bswap32(v);
    }
    if (bytes >= 2 && mode == 1) {
        return ((v & 0x00ff00ff) << 8) | ((v >> 8) & 0x00ff00ff);
    }
    return v;
}

/*
 * Colour buffer pixels. A buffer in system memory (render to the GART, as
 * Apple's driver does for glReadPixels) is staged for the draw; COLOR_ENDIAN
 * swapping applies to it, as bus-master writes are swapped.
 */
static inline uint32_t color_ld(ATIDraw *d, uint64_t off)
{
    if (d->color_stage) {
        return swap_val(ldn_le_p(d->color_stage + off, d->color_bpp),
                        d->color_bpp, d->color_swap);
    }
    return vram_ld(d->s, d->color_off + off, d->color_bpp);
}

static inline void color_st(ATIDraw *d, uint64_t off, uint32_t v)
{
    if (d->color_stage) {
        stn_le_p(d->color_stage + off, d->color_bpp,
                 swap_val(v, d->color_bpp, d->color_swap));
        return;
    }
    vram_st(d->s, d->color_off + off, d->color_bpp, v);
}

/* Shade, test and write one pixel. */
static void draw_pixel(ATIDraw *d, int x, int y, float z, ATIFragment *f,
                       const float tex_st[ATI_TEX_UNITS][2], float fog)
{
    ATIVGAState *s = d->s;
    uint64_t zoff = 0;
    uint32_t zval = 0, zcur = 0, sten = 0;
    bool zon = d->rb3d_cntl & Z_ENABLE;
    bool son = d->rb3d_cntl & STENCIL_ENABLE && d->depth_bpp == 4;

    /* depth and stencil */
    if (zon || son) {
        zoff = d->depth_off + ((uint64_t)y * d->depth_pitch + x) *
               d->depth_bpp;
        if (zoff + d->depth_bpp > s->vga.vram_size) {
            return;
        }
        z = z < 0 ? 0 : z > 1 ? 1 : z;
        if (d->depth_bpp == 2) {
            zval = (uint32_t)(z * 65535.0f + 0.5f);
            zcur = vram_ld(s, zoff, 2);
        } else {
            uint32_t raw = vram_ld(s, zoff, 4);

            zval = (uint32_t)(z * 16777215.0f + 0.5f);
            /* 24-bit depth in the low bits, stencil in the top byte */
            zcur = raw & 0xffffff;
            sten = raw >> 24;
        }
    }
    if (son) {
        uint32_t ref = d->stencil_ref & d->stencil_mask;

        uint32_t val = sten & d->stencil_mask;
        uint32_t nsten;

        if (!compare((d->zcntl >> 12) & 7, ref, val)) {
            nsten = stencil_op((d->zcntl >> 16) & 7, sten, d->stencil_ref);
            sten = (sten & ~d->stencil_wmask) | (nsten & d->stencil_wmask);
            vram_st(s, zoff, 4, (sten << 24) | zcur);
            return;
        }
        if (zon && !compare((d->zcntl >> 4) & 7, zval, zcur)) {
            nsten = stencil_op((d->zcntl >> 24) & 7, sten, d->stencil_ref);
            sten = (sten & ~d->stencil_wmask) | (nsten & d->stencil_wmask);
            vram_st(s, zoff, 4, (sten << 24) | zcur);
            return;
        }
        nsten = stencil_op((d->zcntl >> 20) & 7, sten, d->stencil_ref);
        sten = (sten & ~d->stencil_wmask) | (nsten & d->stencil_wmask);
    } else if (zon && !compare((d->zcntl >> 4) & 7, zval, zcur)) {
        stats.ztest_fail++;
        return;
    }

    /* textures and combiners */
    memcpy(f->cur, f->diffuse, sizeof(f->cur));
    for (int n = 0; n < ATI_TEX_UNITS; n++) {
        if (d->tex[n].enabled) {
            tex_sample(d, &d->tex[n], tex_st[n][0], tex_st[n][1],
                       f->texel[n]);
        } else {
            f->texel[n][0] = f->texel[n][1] = f->texel[n][2] = 0;
            f->texel[n][3] = 1;
        }
    }
    for (int n = 0; n < ATI_TEX_UNITS; n++) {
        if (d->r128) {
            if (n < 2 && d->tex[n].enabled) {
                r128_combine(d, f, n);
            }
        } else if (d->pp_cntl & TEX_BLEND_ENABLE(n)) {
            combine_stage(f, &d->tex[n]);
        }
    }
    if (d->pp_cntl & SPECULAR_ENABLE) {
        for (int ch = 0; ch < 3; ch++) {
            f->cur[ch] = MIN(f->cur[ch] + f->specular[ch], 1.0f);
        }
    }
    if (d->pp_cntl & FOG_ENABLE) {
        fog = fog < 0 ? 0 : fog > 1 ? 1 : fog;
        for (int ch = 0; ch < 3; ch++) {
            f->cur[ch] = fog * f->cur[ch] + (1 - fog) * d->fog_color[ch];
        }
    }

    /* alpha test */
    if (d->pp_cntl & ALPHA_TEST_ENABLE &&
        !compare((d->pp_misc >> 8) & 7, q(f->cur[3], 255),
                 d->pp_misc & 0xff)) {
        return;
    }

    /* depth/stencil write */
    if (son) {
        uint32_t nz = zon && (d->zcntl & Z_WRITE_ENABLE) ? zval : zcur;

        vram_st(s, zoff, 4, (sten << 24) | nz);
    } else if (zon && (d->zcntl & Z_WRITE_ENABLE)) {
        if (d->depth_bpp == 2) {
            vram_st(s, zoff, 2, zval);
        } else {
            vram_st(s, zoff, 4,
                    (vram_ld(s, zoff, 4) & 0xff000000) | zval);
        }
    }

    /* blend and write colour */
    uint64_t coff = ((uint64_t)y * d->color_pitch + x) * d->color_bpp;
    if (!d->color_stage && d->color_off + coff + d->color_bpp >
        s->vga.vram_size) {
        return;
    }
    uint32_t old = 0, out;
    bool need_old = d->rb3d_cntl & (ALPHA_BLEND_ENABLE | PLANE_MASK_ENABLE);

    if (need_old) {
        old = color_ld(d, coff);
    }
    if (d->rb3d_cntl & ALPHA_BLEND_ENABLE) {
        float dst[4], sf[4], df[4], res[4];
        int fcn = (d->blend >> 12) & 7;

        unpack_color(d->color_fmt, old, dst);
        blend_factor((d->blend >> 16) & 0x3f, f->cur, dst, sf);
        blend_factor((d->blend >> 24) & 0x3f, f->cur, dst, df);
        for (int ch = 0; ch < 4; ch++) {
            float a = f->cur[ch] * sf[ch], b = dst[ch] * df[ch];

            switch (fcn) {
            case 2: case 3: res[ch] = a - b; break;
            case 4: res[ch] = MIN(f->cur[ch], dst[ch]); break;
            case 5: res[ch] = MAX(f->cur[ch], dst[ch]); break;
            case 6: case 7: res[ch] = b - a; break;
            default: res[ch] = a + b; break;
            }
        }
        out = pack_color(d->color_fmt, res);
    } else {
        out = pack_color(d->color_fmt, f->cur);
    }
    if (d->rb3d_cntl & ROP_ENABLE && d->r128) {
        if (!need_old) {
            old = color_ld(d, coff);
        }
        out = rop3_apply(d->rop3, out, old);
        if (d->color_bpp < 4) {
            out &= (1u << (8 * d->color_bpp)) - 1;
        }
    } else if (d->rb3d_cntl & ROP_ENABLE) {
        if (!need_old) {
            old = color_ld(d, coff);
        }
        switch (d->rop) {
        case 0:  out = 0; break;
        case 1:  out = out & old; break;
        case 2:  out = out & ~old; break;
        case 3:  break;
        case 4:  out = ~out & old; break;
        case 5:  out = old; break;
        case 6:  out = out ^ old; break;
        case 7:  out = out | old; break;
        case 8:  out = ~(out | old); break;
        case 9:  out = ~(out ^ old); break;
        case 10: out = ~old; break;
        case 11: out = out | ~old; break;
        case 12: out = ~out; break;
        case 13: out = ~out | old; break;
        case 14: out = ~(out & old); break;
        default: out = ~0u; break;
        }
        if (d->color_bpp < 4) {
            out &= (1u << (8 * d->color_bpp)) - 1;
        }
    }
    if (d->rb3d_cntl & PLANE_MASK_ENABLE) {
        out = (out & d->planemask) | (old & ~d->planemask);
    }
    color_st(d, coff, out);
    stats.written++;
    if (y < d->dirty_y0) {
        d->dirty_y0 = y;
    }
    if (y > d->dirty_y1) {
        d->dirty_y1 = y;
    }
}

/* ---- rasterisation ------------------------------------------------------- */

static void fragment_from(ATIDraw *d, const ATIVertex *v, ATIFragment *f)
{
    memcpy(f->diffuse, v->c, sizeof(f->diffuse));
    memcpy(f->specular, v->sp, sizeof(f->specular));
}

/* Perspective-correct coordinates of texture unit n at barycentrics b. */
static inline float sq(float a)
{
    return a * a;
}

static void tex_coords_at(const ATIVertex *const v[3], int n,
                          const float b[3], float wsum, float st[2])
{
    float sn = 0, tn = 0, qn = 0;

    for (int k = 0; k < 3; k++) {
        sn += b[k] * v[k]->tc[n][0] * v[k]->w;
        tn += b[k] * v[k]->tc[n][1] * v[k]->w;
        qn += b[k] * v[k]->tc[n][2] * v[k]->w;
    }
    if (qn == 0) {
        qn = wsum;
    }
    st[0] = qn != 0 ? sn / qn : 0;
    st[1] = qn != 0 ? tn / qn : 0;
}

static void draw_triangle(ATIDraw *d, const ATIVertex *v0,
                          const ATIVertex *v1, const ATIVertex *v2,
                          const ATIVertex *flat)
{
    float area = (v1->x - v0->x) * (v2->y - v0->y) -
                 (v2->x - v0->x) * (v1->y - v0->y);
    /* window coordinates run downwards, so a negative area is CCW */
    bool ccw = area < 0;
    bool front = (d->se_cntl & 1) ? ccw : !ccw;
    int mode = front ? (d->se_cntl >> 3) & 3 : (d->se_cntl >> 1) & 3;
    int diffuse_shade = (d->se_cntl >> 8) & 3;
    int alpha_shade = (d->se_cntl >> 10) & 3;
    int spec_shade = (d->se_cntl >> 12) & 3;

    stats.tris++;
    if (area == 0 || mode == 0) {
        stats.culled++;
        return;                         /* degenerate or culled */
    }

    /* rasterise with positive area: swap two vertices if needed */
    if (area < 0) {
        const ATIVertex *t = v1;

        v1 = v2;
        v2 = t;
        area = -area;
    }

    int x0 = MAX((int)floorf(MIN(v0->x, MIN(v1->x, v2->x))), d->clip_x0);
    int x1 = MIN((int)ceilf(MAX(v0->x, MAX(v1->x, v2->x))), d->clip_x1);
    int y0 = MAX((int)floorf(MIN(v0->y, MIN(v1->y, v2->y))), d->clip_y0);
    int y1 = MIN((int)ceilf(MAX(v0->y, MAX(v1->y, v2->y))), d->clip_y1);
    float inv = 1.0f / area;
    const ATIVertex *v[3] = { v0, v1, v2 };
    /*
     * With y downwards and positive area, an edge a->b is a top edge if
     * horizontal with b to the right, a left edge if it goes up.
     */
#define TOP_LEFT(a, b) (((b)->y == (a)->y && (b)->x > (a)->x) || \
                        (b)->y < (a)->y)
    bool tl[3] = { TOP_LEFT(v1, v2), TOP_LEFT(v2, v0), TOP_LEFT(v0, v1) };
#undef TOP_LEFT

    /* level of detail per triangle: texels per pixel, by area */
    for (int n = 0; n < ATI_TEX_UNITS; n++) {
        ATITexture *t = &d->tex[n];
        float ds1, dt1, ds2, dt2, tarea;

        if (!t->enabled) {
            continue;
        }
        ds1 = v1->tc[n][0] - v0->tc[n][0];
        dt1 = v1->tc[n][1] - v0->tc[n][1];
        ds2 = v2->tc[n][0] - v0->tc[n][0];
        dt2 = v2->tc[n][1] - v0->tc[n][1];
        tarea = fabsf(ds1 * dt2 - ds2 * dt1);
        if (!t->nonparametric) {
            tarea *= (float)t->w * t->h;
        }
        t->lod = tarea > 0 ? 0.5f * log2f(tarea / area) : 0;
    }

    /* how the barycentrics change one pixel right and one pixel down */
    const float dbdx[3] = { -(v2->y - v1->y) * inv, -(v0->y - v2->y) * inv,
                            -(v1->y - v0->y) * inv };
    const float dbdy[3] = { (v2->x - v1->x) * inv, (v0->x - v2->x) * inv,
                            (v1->x - v0->x) * inv };

    for (int y = y0; y <= y1; y++) {
        float py = y + 0.5f;

        for (int x = x0; x <= x1; x++) {
            float px = x + 0.5f;
            float e0 = (v2->x - v1->x) * (py - v1->y) -
                       (v2->y - v1->y) * (px - v1->x);
            float e1 = (v0->x - v2->x) * (py - v2->y) -
                       (v0->y - v2->y) * (px - v2->x);
            float e2 = (v1->x - v0->x) * (py - v0->y) -
                       (v1->y - v0->y) * (px - v0->x);
            float b[3] = { e0 * inv, e1 * inv, e2 * inv };
            ATIFragment f;
            float st[ATI_TEX_UNITS][2] = { { 0 } };
            float z, fog, wsum;

            if (b[0] < 0 || b[1] < 0 || b[2] < 0) {
                continue;
            }
            /* shared edges: draw a pixel once (top-left rule) */
            if ((b[0] == 0 && !tl[0]) || (b[1] == 0 && !tl[1]) ||
                (b[2] == 0 && !tl[2])) {
                continue;
            }
            z = b[0] * v0->z + b[1] * v1->z + b[2] * v2->z;
            for (int ch = 0; ch < 4; ch++) {
                int shade = ch < 3 ? diffuse_shade : alpha_shade;

                f.diffuse[ch] = shade == 2 ?
                    b[0] * v0->c[ch] + b[1] * v1->c[ch] + b[2] * v2->c[ch] :
                    flat->c[ch];
                f.specular[ch] = spec_shade == 2 ?
                    b[0] * v0->sp[ch] + b[1] * v1->sp[ch] +
                    b[2] * v2->sp[ch] : flat->sp[ch];
            }
            fog = b[0] * v0->sp[3] + b[1] * v1->sp[3] + b[2] * v2->sp[3];
            /* perspective-correct texture coordinates (W0 is 1/w) */
            wsum = b[0] * v0->w + b[1] * v1->w + b[2] * v2->w;
            for (int n = 0; n < ATI_TEX_UNITS; n++) {
                ATITexture *t = &d->tex[n];

                if (!t->enabled) {
                    continue;
                }
                tex_coords_at(v, n, b, wsum, st[n]);
                if (t->levels > 1 || t->min_linear != t->mag_linear) {
                    /*
                     * Level of detail per pixel, from how far the texture
                     * coordinates move one pixel right and one down
                     * (projective coordinates included).
                     */
                    float bx[3], by[3], sx[2], sy[2], dx2, dy2;
                    float sw = t->nonparametric ? 1 : t->w;
                    float th = t->nonparametric ? 1 : t->h;

                    for (int k = 0; k < 3; k++) {
                        bx[k] = b[k] + dbdx[k];
                        by[k] = b[k] + dbdy[k];
                    }
                    tex_coords_at(v, n, bx,
                                  bx[0] * v0->w + bx[1] * v1->w +
                                  bx[2] * v2->w, sx);
                    tex_coords_at(v, n, by,
                                  by[0] * v0->w + by[1] * v1->w +
                                  by[2] * v2->w, sy);
                    dx2 = sq((sx[0] - st[n][0]) * sw) +
                          sq((sx[1] - st[n][1]) * th);
                    dy2 = sq((sy[0] - st[n][0]) * sw) +
                          sq((sy[1] - st[n][1]) * th);
                    t->lod = 0.5f * log2f(MAX(MAX(dx2, dy2), 1e-12f));
                }
            }
            stats.pixels++;
            draw_pixel(d, x, y, z, &f, st, fog);
        }
    }
}

/* Lines and points: thin, one pixel wide, interpolated along the line. */
static void draw_line(ATIDraw *d, const ATIVertex *a, const ATIVertex *b,
                      const ATIVertex *flat)
{
    float dx = b->x - a->x, dy = b->y - a->y;
    int steps = (int)ceilf(MAX(fabsf(dx), fabsf(dy)));
    int gouraud = ((d->se_cntl >> 8) & 3) == 2;

    for (int i = 0; i <= steps; i++) {
        float t = steps ? (float)i / steps : 0;
        int x = (int)floorf(a->x + dx * t), y = (int)floorf(a->y + dy * t);
        ATIFragment f;
        float st[ATI_TEX_UNITS][2];

        if (x < d->clip_x0 || x > d->clip_x1 || y < d->clip_y0 ||
            y > d->clip_y1 || (i == steps && steps)) {
            continue;
        }
        for (int ch = 0; ch < 4; ch++) {
            f.diffuse[ch] = gouraud ? a->c[ch] + (b->c[ch] - a->c[ch]) * t
                                    : flat->c[ch];
            f.specular[ch] = a->sp[ch] + (b->sp[ch] - a->sp[ch]) * t;
        }
        for (int n = 0; n < ATI_TEX_UNITS; n++) {
            st[n][0] = a->tc[n][0] + (b->tc[n][0] - a->tc[n][0]) * t;
            st[n][1] = a->tc[n][1] + (b->tc[n][1] - a->tc[n][1]) * t;
        }
        draw_pixel(d, x, y, a->z + (b->z - a->z) * t, &f, st,
                   f.specular[3]);
    }
}

static void draw_point(ATIDraw *d, const ATIVertex *v)
{
    ATIFragment f;
    float st[ATI_TEX_UNITS][2];
    int x = (int)floorf(v->x), y = (int)floorf(v->y);

    if (x < d->clip_x0 || x > d->clip_x1 || y < d->clip_y0 ||
        y > d->clip_y1) {
        return;
    }
    fragment_from(d, v, &f);
    for (int n = 0; n < ATI_TEX_UNITS; n++) {
        st[n][0] = v->tc[n][0];
        st[n][1] = v->tc[n][1];
    }
    draw_pixel(d, x, y, v->z, &f, st, v->sp[3]);
}

/* ---- draw setup ---------------------------------------------------------- */

static bool draw_begin(ATIVGAState *s, ATIDraw *d)
{
    uint32_t tl = reg(s, RE_TOP_LEFT), wh = reg(s, RE_WIDTH_HEIGHT);
    uint32_t fog = reg(s, PP_FOG_COLOR);
    uint32_t sref = reg(s, RB3D_STENCILREFMASK);

    memset(d, 0, sizeof(*d));
    d->s = s;
    d->rb3d_cntl = reg(s, RB3D_CNTL);
    d->pp_cntl = reg(s, PP_CNTL);
    d->se_cntl = reg(s, SE_CNTL);
    d->zcntl = reg(s, RB3D_ZSTENCILCNTL);
    d->blend = reg(s, RB3D_BLENDCNTL);
    d->pp_misc = reg(s, PP_MISC);
    d->planemask = reg(s, RB3D_PLANEMASK);
    d->rop = (reg(s, RB3D_ROPCNTL) >> 8) & 0xf;
    d->stencil_ref = sref & 0xff;
    /* RB3D_STENCILREFMASK: ref 7:0, mask 23:16, write mask 31:24 */
    d->stencil_mask = (sref >> 16) & 0xff;
    d->stencil_wmask = sref >> 24;
    d->fog_color[0] = ((fog >> 16) & 0xff) / 255.0f;
    d->fog_color[1] = ((fog >> 8) & 0xff) / 255.0f;
    d->fog_color[2] = (fog & 0xff) / 255.0f;

    d->color_fmt = (d->rb3d_cntl >> 10) & 0xf;
    switch (d->color_fmt) {
    case 3: case 4: case 15:
        d->color_bpp = 2;
        break;
    case 7: case 8:
        d->color_bpp = 1;
        break;
    default:
        d->color_bpp = 4;
        break;
    }
    d->color_pitch = reg(s, RB3D_COLORPITCH) & 0x1ff8;
    d->depth_pitch = reg(s, RB3D_DEPTHPITCH) & 0x1ff8;
    d->depth_bpp = (d->zcntl & 0xf) == 0 ? 2 : 4;
    d->coord_fmt = reg(s, SE_COORD_FMT);
    d->color_swap = (reg(s, RB3D_COLORPITCH) >> 18) & 3;
    if (!d->color_pitch) {
        return false;
    }
    if (d->rb3d_cntl & (Z_ENABLE | STENCIL_ENABLE) &&
        !ati_mc_vram_range(s, reg(s, RB3D_DEPTHOFFSET) & ~0xfu, 1,
                           &d->depth_off)) {
        d->rb3d_cntl &= ~(Z_ENABLE | STENCIL_ENABLE);
    }

    /* the scissor RE_TOP_LEFT to RE_WIDTH_HEIGHT is inclusive */
    d->clip_x0 = tl & 0x7ff;
    d->clip_y0 = (tl >> 16) & 0x7ff;
    d->clip_x1 = wh & 0x7ff;
    d->clip_y1 = (wh >> 16) & 0x7ff;
    d->clip_x1 = MIN(d->clip_x1, (int)d->color_pitch - 1);
    /* inclusive auxiliary scissors (RE_AUX_SCISSOR_CNTL SCISSOR_ENABLE_n) */
    uint32_t aux = reg(s, RE_AUX_SCISSOR_CNTL);
    for (int n = 0; n < 3; n++) {
        if ((aux & (1u << (28 + n))) && !(aux & (1u << (24 + n)))) {
            uint32_t stl = reg(s, RE_SCISSOR_TL_0 + n * 8);
            uint32_t sbr = reg(s, RE_SCISSOR_TL_0 + n * 8 + 4);

            d->clip_x0 = MAX(d->clip_x0, (int)(stl & 0x7ff));
            d->clip_y0 = MAX(d->clip_y0, (int)((stl >> 16) & 0x7ff));
            d->clip_x1 = MIN(d->clip_x1, (int)(sbr & 0x7ff));
            d->clip_y1 = MIN(d->clip_y1, (int)((sbr >> 16) & 0x7ff));
        }
    }
    d->dirty_y0 = INT_MAX;
    d->dirty_y1 = -1;
    if (d->clip_x0 > d->clip_x1 || d->clip_y0 > d->clip_y1) {
        return false;
    }

    /* the colour buffer: in video memory, or staged rows of the GART */
    uint32_t coffset = reg(s, RB3D_COLOROFFSET) & ~0xfu;
    uint64_t rowbytes = (uint64_t)d->color_pitch * d->color_bpp;
    uint64_t span = (uint64_t)(d->clip_y1 + 1) * rowbytes;

    {
        static int tl_on = -1;
        static uint32_t last;

        if (tl_on < 0) {
            tl_on = getenv("PPCOSX_R300_TARGETLOG") != NULL;
        }
        if (tl_on && coffset != last) {
            last = coffset;
            fprintf(stderr, "ati3d: target %08x pitch %u bpp %u clip %d,%d-%d,%d\n",
                    coffset, d->color_pitch, d->color_bpp, d->clip_x0,
                    d->clip_y0, d->clip_x1, d->clip_y1);
        }
    }
    if (!ati_mc_vram_range(s, coffset, span, &d->color_off)) {
        if (span > 64 * MiB) {
            return false;
        }
        d->color_mc = coffset;
        d->color_stage = g_malloc0(span);
        if (getenv("PPCOSX_ATI_3D_STATS")) {
            fprintf(stderr, "ati3d: draw to system memory 0x%08x pitch %u "
                    "swap %u clip %d,%d-%d,%d\n", coffset, d->color_pitch,
                    d->color_swap, d->clip_x0, d->clip_y0, d->clip_x1,
                    d->clip_y1);
        }
        if (!ati_mc_read(s, coffset + d->clip_y0 * rowbytes,
                         d->color_stage + d->clip_y0 * rowbytes,
                         (d->clip_y1 - d->clip_y0 + 1) * rowbytes)) {
            static bool warned;

            if (!warned) {
                warned = true;
                qemu_log_mask(LOG_GUEST_ERROR, "ati: 3D colour buffer at "
                              "0x%x is not mapped\n", coffset);
            }
            g_free(d->color_stage);
            d->color_stage = NULL;
            return false;
        }
    }

    for (int n = 0; n < ATI_TEX_UNITS; n++) {
        tex_setup(d, n);
    }
    return true;
}

/* ---- Rage 128 ------------------------------------------------------------ */

/*
 * The Rage 128's 3D engine draws with the 2D engine's destination
 * (DST_PITCH_OFFSET_C, DP_GUI_MASTER_CNTL_C's datatype and ROP3, the
 * SC_*_C scissor) and its own state below, mostly in "_C" registers.
 * Vertices arrive in window coordinates in 3D_RNDR_GEN_PRIM packets; the
 * vertex format bits are the ones the R100 kept. Encodings follow
 * xf86-video-r128's r128_reg.h and Mesa's r128 driver; Apple's 10.0
 * driver is what this was checked against.
 */
#define R128_PM4_VC_FPU_SETUP           0x071c
#define R128_SCALE_3D_CNTL              0x1a00
#define R128_SETUP_CNTL                 0x1bc4
#define R128_WINDOW_XY_OFFSET           0x1bcc
#define R128_Z_OFFSET_C                 0x1c90
#define R128_Z_PITCH_C                  0x1c94
#define R128_Z_STEN_CNTL_C              0x1c98
#define R128_TEX_CNTL_C                 0x1c9c
#define R128_MISC_3D_STATE_CNTL         0x1ca0
#define R128_FOG_COLOR_C                0x1cac
#define R128_PRIM_TEX_CNTL_C            0x1cb0
#define R128_PRIM_TEX_COMBINE_C         0x1cb4
#define R128_TEX_SIZE_PITCH_C           0x1cb8
#define R128_PRIM_TEX_0_OFFSET_C        0x1cbc  /* 11 levels */
#define R128_SEC_TEX_CNTL_C             0x1d00
#define R128_SEC_TEX_COMBINE_C          0x1d04
#define R128_SEC_TEX_0_OFFSET_C         0x1d08
#define R128_CONSTANT_COLOR_C           0x1d34
#define R128_PRIM_BORDER_COLOR_C        0x1d38
#define R128_SEC_BORDER_COLOR_C         0x1d3c
#define R128_STEN_REF_MASK_C            0x1d40
#define R128_PLANE_3D_MASK_C            0x1d44

/* TEX_CNTL_C */
#define R128_Z_ENABLE                   (1u << 0)
#define R128_Z_WRITE_ENABLE             (1u << 1)
#define R128_STENCIL_ENABLE             (1u << 3)
#define R128_TEXMAP_ENABLE              (1u << 4)
#define R128_SEC_TEXMAP_ENABLE          (1u << 5)
#define R128_FOG_ENABLE                 (1u << 7)
#define R128_ALPHA_ENABLE               (1u << 9)
#define R128_ALPHA_TEST_ENABLE          (1u << 10)
#define R128_SPEC_LIGHT_ENABLE          (1u << 11)

/* SETUP_CNTL: texture coordinates come multiplied by 1/w already */
#define R128_TEXTURE_ST_DIRECT          (1u << 9)
/* vertex format: the second texture's 1/w, after its coordinates */
#define R128_VTX_RHW2                   (1u << 9)

/* Rage 128 blend factor -> the R100 code blend_factor() takes */
static int r128_blend_factor(int f, bool dst)
{
    static const int map[] = {
        32, 33, 34, 35, 38, 39, 40, 41, 36, 37, 42,
    };

    if (f == 11) {                      /* both source alpha */
        return dst ? 39 : 38;
    }
    if (f == 12) {                      /* both inverse source alpha */
        return dst ? 38 : 39;
    }
    return f < ARRAY_SIZE(map) ? map[f] : 33;
}

/* Rage 128 texture datatype -> the R100 format tex_texel() reads */
static int r128_tex_format(int dt)
{
    switch (dt) {
    case 3: return 3;                   /* ARGB1555 */
    case 4: return 4;                   /* RGB565 */
    case 6: return 6;                   /* ARGB8888 */
    case 7: return 2;                   /* RGB332 */
    case 8: case 9: return 8;           /* Y8, RGB8 (grey) */
    case 11: return 10;                 /* YVYU422 */
    case 12: return 11;                 /* VYUY422 */
    case 15: return 5;                  /* ARGB4444 */
    default: return -1;                 /* palettes, VQ */
    }
}

/*
 * Texture n (0 primary, 1 secondary). TEX_SIZE_PITCH_C gives log2 of the
 * width (= pitch, in texels), of the larger side and of the height, and
 * of the smallest mipmap; level i of a texture with larger side 2^k sits
 * at TEX_(k - i)_OFFSET, each level packed at its own width.
 */
static void r128_tex_setup(ATIDraw *d, int n)
{
    ATIVGAState *s = d->s;
    ATITexture *t = &d->tex[n];
    uint32_t cntl = reg(s, n ? R128_SEC_TEX_CNTL_C : R128_PRIM_TEX_CNTL_C);
    uint32_t sp = reg(s, R128_TEX_SIZE_PITCH_C) >> (16 * n);
    uint32_t off0 = n ? R128_SEC_TEX_0_OFFSET_C : R128_PRIM_TEX_0_OFFSET_C;
    uint32_t border = reg(s, n ? R128_SEC_BORDER_COLOR_C
                                : R128_PRIM_BORDER_COLOR_C);
    int lw = sp & 0xf, lsize = (sp >> 4) & 0xf, lh = (sp >> 8) & 0xf;
    int lmin = (sp >> 12) & 0xf;
    int fmt = r128_tex_format((cntl >> 16) & 0xf);
    static const int min_map[8] = { 0, 1, 2, 3, 6, 7, 1, 1 };
    static const int wrap_map[4] = { 0, 1, 2, 4 };
    uint64_t lo = UINT64_MAX, hi = 0;

    memset(t, 0, sizeof(*t));
    if (fmt < 0) {
        static bool warned;

        if (!warned) {
            warned = true;
            qemu_log_mask(LOG_UNIMP, "ati: Rage 128 texture datatype %u "
                          "not implemented\n", (cntl >> 16) & 0xf);
        }
        return;
    }
    t->format = fmt;
    t->bpp = tex_bpp(fmt);
    t->w = 1 << lw;
    t->h = 1 << lh;
    t->pitch = t->w * t->bpp;
    t->mag_linear = (cntl >> 4) & 1;
    t->min_mode = min_map[(cntl >> 1) & 7];
    t->min_linear = t->min_mode == 1 || (t->min_mode & 4);
    t->wrap_s = wrap_map[(cntl >> 8) & 3];
    t->wrap_t = wrap_map[(cntl >> 11) & 3];
    t->perspective = true;
    t->border[0] = ((border >> 16) & 0xff) / 255.0f;
    t->border[1] = ((border >> 8) & 0xff) / 255.0f;
    t->border[2] = (border & 0xff) / 255.0f;
    t->border[3] = (border >> 24) / 255.0f;
    t->levels = 1;
    if (!(cntl & (1u << 7)) && t->min_mode >= 2) {      /* mipmapped */
        t->levels = MAX(MIN(lsize - lmin + 1, 12), 1);
    }
    for (int l = 0; l < t->levels; l++) {
        uint32_t o = reg(s, off0 + 4 * MAX(lsize - l, 0)) & ~0x1fu;

        t->lvl_w[l] = MAX(t->w >> l, 1);
        t->lvl_h[l] = MAX(t->h >> l, 1);
        t->lvl_pitch[l] = t->lvl_w[l] * t->bpp;
        t->lvl_off[l] = o;
        lo = MIN(lo, o);
        hi = MAX(hi, o + (uint64_t)t->lvl_pitch[l] * t->lvl_h[l]);
    }
    for (int l = 0; l < t->levels; l++) {
        t->lvl_off[l] -= lo;
    }
    if (ati_mc_vram_range(s, lo, hi - lo, &t->vram_off)) {
        t->vram = true;
    } else if (hi - lo <= 64 * MiB) {
        t->data = g_malloc(hi - lo);
        if (!ati_mc_read(s, lo, t->data, hi - lo)) {
            g_free(t->data);
            t->data = NULL;
            return;
        }
    } else {
        return;
    }
    t->enabled = true;
}

static bool r128_draw_begin(ATIVGAState *s, ATIDraw *d)
{
    uint32_t tex_cntl = reg(s, R128_TEX_CNTL_C);
    uint32_t misc = reg(s, R128_MISC_3D_STATE_CNTL);
    uint32_t fpu = reg(s, R128_PM4_VC_FPU_SETUP);
    uint32_t zsten = reg(s, R128_Z_STEN_CNTL_C);
    uint32_t fog = reg(s, R128_FOG_COLOR_C);
    uint32_t sref = reg(s, R128_STEN_REF_MASK_C);
    uint32_t cc = reg(s, R128_CONSTANT_COLOR_C);
    uint32_t tl = reg(s, SC_TOP_LEFT), br = reg(s, SC_BOTTOM_RIGHT);
    int color_mode = (fpu >> 5) & 3;
    uint64_t span, rowbytes;
    uint32_t coffset;

    memset(d, 0, sizeof(*d));
    d->s = s;
    d->r128 = true;

    /* the destination: the 2D engine's, in 8-pixel pitch units */
    switch (s->regs.dp_datatype & 0xf) {
    case 3: d->color_fmt = 3; d->color_bpp = 2; break;
    case 4: d->color_fmt = 4; d->color_bpp = 2; break;
    case 2: d->color_fmt = 8; d->color_bpp = 1; break;
    default: d->color_fmt = 6; d->color_bpp = 4; break;
    }
    d->color_pitch = s->regs.dst_pitch * 8;
    coffset = s->regs.dst_offset;
    d->rop3 = (s->regs.dp_mix >> 16) & 0xff;
    if (d->rop3 != 0xcc) {
        d->rb3d_cntl |= ROP_ENABLE;
    }
    d->planemask = reg(s, R128_PLANE_3D_MASK_C);
    if (d->planemask != 0xffffffff) {
        d->rb3d_cntl |= PLANE_MASK_ENABLE;
    }

    /* depth and stencil: the R100 layout, with Z writes in TEX_CNTL */
    d->zcntl = zsten & ~(1u << 30);
    if (tex_cntl & R128_Z_WRITE_ENABLE) {
        d->zcntl |= Z_WRITE_ENABLE;
    }
    if (tex_cntl & R128_Z_ENABLE) {
        d->rb3d_cntl |= Z_ENABLE;
    }
    if (tex_cntl & R128_STENCIL_ENABLE) {
        d->rb3d_cntl |= STENCIL_ENABLE;
    }
    d->depth_bpp = ((zsten >> 1) & 3) == 0 ? 2 : 4;
    d->depth_pitch = (reg(s, R128_Z_PITCH_C) & 0x3ff) * 8;
    if (d->rb3d_cntl & (Z_ENABLE | STENCIL_ENABLE) &&
        !ati_mc_vram_range(s, reg(s, R128_Z_OFFSET_C) & ~0x1fu, 1,
                           &d->depth_off)) {
        d->rb3d_cntl &= ~(Z_ENABLE | STENCIL_ENABLE);
    }
    d->stencil_ref = sref & 0xff;
    d->stencil_mask = (sref >> 16) & 0xff;
    d->stencil_wmask = sref >> 24;

    /* blending, alpha test, fog, specular */
    if (tex_cntl & R128_ALPHA_ENABLE) {
        int fcn = (misc >> 12) & 3;

        d->rb3d_cntl |= ALPHA_BLEND_ENABLE;
        d->blend = r128_blend_factor((misc >> 16) & 0xf, false) << 16 |
                   r128_blend_factor((misc >> 20) & 0xf, true) << 24 |
                   (fcn >= 2 ? 2 : 0) << 12;
    }
    if (tex_cntl & R128_ALPHA_TEST_ENABLE) {
        d->pp_cntl |= ALPHA_TEST_ENABLE;
        d->pp_misc = (misc & 0xff) | ((misc >> 24) & 7) << 8;
    }
    if (tex_cntl & R128_FOG_ENABLE) {
        d->pp_cntl |= FOG_ENABLE;
    }
    if (tex_cntl & R128_SPEC_LIGHT_ENABLE) {
        d->pp_cntl |= SPECULAR_ENABLE;
    }
    d->fog_color[0] = ((fog >> 16) & 0xff) / 255.0f;
    d->fog_color[1] = ((fog >> 8) & 0xff) / 255.0f;
    d->fog_color[2] = (fog & 0xff) / 255.0f;
    d->const_color[0] = ((cc >> 16) & 0xff) / 255.0f;
    d->const_color[1] = ((cc >> 8) & 0xff) / 255.0f;
    d->const_color[2] = (cc & 0xff) / 255.0f;
    d->const_color[3] = (cc >> 24) / 255.0f;

    /*
     * Culling and shading: PM4_VC_FPU_SETUP bits 4:0 are SE_CNTL's (front
     * direction, back and front face modes); colours are flat (from the
     * last vertex for OpenGL) or Gouraud.
     */
    d->se_cntl = fpu & 0x1f;
    if (color_mode >= 2) {
        d->se_cntl |= 2 << 8 | 2 << 10 | 2 << 12;
    }
    d->se_cntl |= ((fpu >> 14) & 1 ? 2 : 0) << 6;

    /* the scissor, inclusive, in the destination's coordinates */
    d->clip_x0 = tl & 0x3fff;
    d->clip_y0 = (tl >> 16) & 0x3fff;
    d->clip_x1 = MIN((int)(br & 0x3fff), (int)d->color_pitch - 1);
    d->clip_y1 = (br >> 16) & 0x3fff;
    d->dirty_y0 = INT_MAX;
    d->dirty_y1 = -1;
    if (!d->color_pitch || d->clip_x0 > d->clip_x1 ||
        d->clip_y0 > d->clip_y1) {
        return false;
    }
    rowbytes = (uint64_t)d->color_pitch * d->color_bpp;
    span = (uint64_t)(d->clip_y1 + 1) * rowbytes;
    if (!ati_mc_vram_range(s, coffset, span, &d->color_off)) {
        if (span > 64 * MiB) {
            return false;
        }
        d->color_mc = coffset;
        d->color_stage = g_malloc0(span);
        if (!ati_mc_read(s, coffset + d->clip_y0 * rowbytes,
                         d->color_stage + d->clip_y0 * rowbytes,
                         (d->clip_y1 - d->clip_y0 + 1) * rowbytes)) {
            g_free(d->color_stage);
            d->color_stage = NULL;
            return false;
        }
    }

    /* textures and their combiners */
    if (tex_cntl & R128_TEXMAP_ENABLE) {
        r128_tex_setup(d, 0);
        d->r128_comb[0] = reg(s, R128_PRIM_TEX_COMBINE_C);
        if (tex_cntl & R128_SEC_TEXMAP_ENABLE) {
            r128_tex_setup(d, 1);
            d->r128_comb[1] = reg(s, R128_SEC_TEX_COMBINE_C);
        }
    }
    return true;
}

static void draw_end(ATIDraw *d)
{
    ATIVGAState *s = d->s;


    tex_release(d);
    if (d->color_stage) {
        uint64_t rowbytes = (uint64_t)d->color_pitch * d->color_bpp;

        if (d->dirty_y1 >= d->dirty_y0) {
            ati_mc_write(s, d->color_mc + d->dirty_y0 * rowbytes,
                         d->color_stage + d->dirty_y0 * rowbytes,
                         (d->dirty_y1 - d->dirty_y0 + 1) * rowbytes);
        }
        g_free(d->color_stage);
        d->color_stage = NULL;
        return;
    }
    if (d->dirty_y1 >= d->dirty_y0) {
        uint64_t start = d->color_off +
                         (uint64_t)d->dirty_y0 * d->color_pitch * d->color_bpp;
        uint64_t len = (uint64_t)(d->dirty_y1 - d->dirty_y0 + 1) *
                       d->color_pitch * d->color_bpp;

        if (start < s->vga.vram_size) {
            memory_region_set_dirty(&s->vga.vram, start,
                                    MIN(len, s->vga.vram_size - start));
        }
    }
}

/* Size in words of one vertex in format `fmt`. */
static int vertex_words(uint32_t fmt)
{
    int n = 2;

    n += !!(fmt & VTX_Z) + !!(fmt & VTX_W0);
    n += (fmt & VTX_FPCOLOR) ? 3 : 0;
    n += !!(fmt & VTX_FPALPHA) + !!(fmt & VTX_PKCOLOR);
    n += (fmt & VTX_FPSPEC) ? 3 : 0;
    n += !!(fmt & VTX_FPFOG) + !!(fmt & VTX_PKSPEC);
    n += (fmt & VTX_ST0) ? 2 : 0;
    n += !!(fmt & VTX_Q0);
    n += (fmt & VTX_ST1) ? 2 : 0;
    n += !!(fmt & VTX_Q1);
    n += (fmt & VTX_ST2) ? 2 : 0;
    n += !!(fmt & VTX_Q2);
    n += (fmt & VTX_ST3) ? 2 : 0;
    n += !!(fmt & VTX_Q3);
    return n;
}

/*
 * Vertex colours come in BGRA order unless VF_CNTL's COLOR_ORDER bit asks
 * for RGBA: as four floats, or as bytes in a packed word.
 */
static void unpack_packed(uint32_t v, bool rgba, float c[4])
{
    if (rgba) {
        c[0] = (v & 0xff) / 255.0f;
        c[1] = ((v >> 8) & 0xff) / 255.0f;
        c[2] = ((v >> 16) & 0xff) / 255.0f;
    } else {
        c[0] = ((v >> 16) & 0xff) / 255.0f;
        c[1] = ((v >> 8) & 0xff) / 255.0f;
        c[2] = (v & 0xff) / 255.0f;
    }
    c[3] = (v >> 24) / 255.0f;
}

static void vertex_parse(ATIVGAState *s, uint32_t fmt, bool rgba,
                         const uint32_t *p, ATIVertex *v)
{
    uint32_t solid = reg(s, RE_SOLID_COLOR);

    memset(v, 0, sizeof(*v));
    v->x = u2f(*p++);
    v->y = u2f(*p++);
    v->z = fmt & VTX_Z ? u2f(*p++) : 0;
    v->w = fmt & VTX_W0 ? u2f(*p++) : 1;
    unpack_packed(solid, false, v->c);
    if (fmt & VTX_FPCOLOR) {
        v->c[rgba ? 0 : 2] = u2f(*p++);
        v->c[1] = u2f(*p++);
        v->c[rgba ? 2 : 0] = u2f(*p++);
        v->c[3] = 1;
    }
    if (fmt & VTX_FPALPHA) {
        v->c[3] = u2f(*p++);
    }
    if (fmt & VTX_PKCOLOR) {
        unpack_packed(*p++, rgba, v->c);
    }
    v->sp[3] = 1;
    if (fmt & VTX_FPSPEC) {
        v->sp[rgba ? 0 : 2] = u2f(*p++);
        v->sp[1] = u2f(*p++);
        v->sp[rgba ? 2 : 0] = u2f(*p++);
    }
    if (fmt & VTX_FPFOG) {
        v->sp[3] = u2f(*p++);
    }
    if (fmt & VTX_PKSPEC) {
        unpack_packed(*p++, rgba, v->sp);
    }
    static const uint32_t st[4] = { VTX_ST0, VTX_ST1, VTX_ST2, VTX_ST3 };
    static const uint32_t qb[4] = { VTX_Q0, VTX_Q1, VTX_Q2, VTX_Q3 };
    for (int n = 0; n < 4; n++) {
        float ts = 0, tt = 0, tq = 1;

        if (fmt & st[n]) {
            ts = u2f(*p++);
            tt = u2f(*p++);
        }
        if (fmt & qb[n]) {
            tq = u2f(*p++);
        }
        if (n < ATI_TEX_UNITS) {
            v->tc[n][0] = ts;
            v->tc[n][1] = tt;
            v->tc[n][2] = tq;
        }
    }
}

/* Draw `n` vertices as primitive type `prim` (VF_CNTL bits 3:0). */
static void draw_prims(ATIDraw *d, int prim, ATIVertex *v, uint32_t n)
{
    int flat_sel = (d->se_cntl >> 6) & 3;
#define FLAT(a, b, c) (flat_sel == 0 ? (a) : flat_sel == 1 ? (b) : (c))

    switch (prim) {
    case 1:                                             /* points */
    case 9:
        for (uint32_t i = 0; i < n; i++) {
            draw_point(d, &v[i]);
        }
        break;
    case 2:                                             /* lines */
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            draw_line(d, &v[i], &v[i + 1], &v[i + 1]);
        }
        break;
    case 3:                                             /* line strip */
        for (uint32_t i = 0; i + 1 < n; i++) {
            draw_line(d, &v[i], &v[i + 1], &v[i + 1]);
        }
        break;
    case 4:                                             /* triangles */
        for (uint32_t i = 0; i + 2 < n; i += 3) {
            draw_triangle(d, &v[i], &v[i + 1], &v[i + 2],
                          FLAT(&v[i], &v[i + 1], &v[i + 2]));
        }
        break;
    case 5:                                             /* fan */
        for (uint32_t i = 1; i + 1 < n; i++) {
            draw_triangle(d, &v[0], &v[i], &v[i + 1],
                          FLAT(&v[0], &v[i], &v[i + 1]));
        }
        break;
    case 6:                                             /* strip */
        for (uint32_t i = 0; i + 2 < n; i++) {
            if (i & 1) {
                draw_triangle(d, &v[i + 1], &v[i], &v[i + 2],
                              FLAT(&v[i + 1], &v[i], &v[i + 2]));
            } else {
                draw_triangle(d, &v[i], &v[i + 1], &v[i + 2],
                              FLAT(&v[i], &v[i + 1], &v[i + 2]));
            }
        }
        break;
    case 8:                                             /* rectangles */
        for (uint32_t i = 0; i + 2 < n; i += 3) {
            /* three corners in order; the fourth completes them */
            ATIVertex v3 = v[i + 2];

            v3.x = v[i].x + v[i + 2].x - v[i + 1].x;
            v3.y = v[i].y + v[i + 2].y - v[i + 1].y;
            for (int n2 = 0; n2 < ATI_TEX_UNITS; n2++) {
                for (int k = 0; k < 2; k++) {
                    v3.tc[n2][k] = v[i].tc[n2][k] + v[i + 2].tc[n2][k] -
                                   v[i + 1].tc[n2][k];
                }
            }
            /* both windings, so neither half is culled */
            uint32_t se = d->se_cntl;
            d->se_cntl |= (3 << 1) | (3 << 3);
            draw_triangle(d, &v[i], &v[i + 1], &v[i + 2], &v[i + 2]);
            draw_triangle(d, &v[i], &v[i + 2], &v3, &v[i + 2]);
            d->se_cntl = se;
        }
        break;
    case 10:                                            /* 3-vertex lines */
        for (uint32_t i = 0; i + 2 < n; i += 3) {
            draw_line(d, &v[i], &v[i + 1], &v[i + 1]);
            draw_line(d, &v[i + 1], &v[i + 2], &v[i + 2]);
        }
        break;
    default: {
        static bool warned[16];

        if (!warned[prim & 15]) {
            warned[prim & 15] = true;
            qemu_log_mask(LOG_UNIMP, "ati: 3D primitive type %d not "
                          "implemented\n", prim);
        }
        break;
    }
    }
#undef FLAT
}

void ati_3d_draw_immd(ATIVGAState *s, uint32_t vtx_fmt, uint32_t vf_cntl,
                      const uint32_t *data, uint32_t count)
{
    bool r128 = s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF;
    uint32_t n = vf_cntl >> 16;
    int words;
    bool rgba = !r128 && (vf_cntl & (1u << 6));
    g_autofree ATIVertex *v = NULL;
    ATIDraw d;

    if (r128) {
        /* 3D_RNDR_GEN_PRIM: z always; RHW2 where the R100 has Q1 */
        vtx_fmt |= VTX_Z;
    }
    words = vertex_words(vtx_fmt);

    if ((vf_cntl >> 4 & 3) != 3) {
        static bool warned;

        if (!warned) {
            warned = true;
            qemu_log_mask(LOG_UNIMP, "ati: 3D draw with vertex walk %u "
                          "not implemented\n", vf_cntl >> 4 & 3);
        }
        return;
    }
    n = MIN(n, count / words);
    if (!n || !(r128 ? r128_draw_begin(s, &d) : draw_begin(s, &d))) {
        return;
    }
    v = g_new(ATIVertex, n);
    for (uint32_t i = 0; i < n; i++) {
        vertex_parse(s, vtx_fmt, rgba, data + i * words, &v[i]);
    }
    if (r128) {
        uint32_t wxy = reg(s, R128_WINDOW_XY_OFFSET);
        bool direct = reg(s, R128_SETUP_CNTL) & R128_TEXTURE_ST_DIRECT;
        bool sec_st = reg(s, R128_SEC_TEX_CNTL_C) & 1;

        for (uint32_t i = 0; i < n; i++) {
            v[i].x += (wxy >> 20) & 0xfff;
            v[i].y += (wxy >> 4) & 0xfff;
            for (int t = 0; t < 2; t++) {
                /* the second texture's 1/w was read as its q */
                float w = t && (vtx_fmt & R128_VTX_RHW2) ? v[i].tc[1][2]
                                                         : v[i].w;

                v[i].tc[t][2] = 1;
                if (direct && w != 0) {
                    /* coordinates came multiplied by 1/w */
                    v[i].tc[t][0] /= w;
                    v[i].tc[t][1] /= w;
                }
            }
            if (!sec_st) {
                /* the secondary texture uses the primary coordinates */
                v[i].tc[1][0] = v[i].tc[0][0];
                v[i].tc[1][1] = v[i].tc[0][1];
            }
        }
    }
    draw_prims(&d, vf_cntl & 0xf, v, n);
    draw_end(&d);
    stats_tick(s);
}

/*
 * 3D_CLEAR_ZMASK: with Z compression every 4x4 tile of the depth buffer
 * has two bits in the Z mask; clearing them makes the tiles read as
 * RB3D_DEPTHCLEARVALUE. Without compression state, write the clear value
 * over the depth buffer: each mask word covers 256 pixels.
 */
void ati_3d_clear_zmask(ATIVGAState *s, const uint32_t *p, uint32_t count)
{
    uint32_t clear = reg(s, RB3D_DEPTHCLEARVALUE);
    uint32_t zcntl = reg(s, RB3D_ZSTENCILCNTL);
    int bpp = (zcntl & 0xf) == 0 ? 2 : 4;
    uint64_t off, pixels;

    if (count < 2 ||
        !ati_mc_vram_range(s, reg(s, RB3D_DEPTHOFFSET) & ~0xfu, 1, &off)) {
        return;
    }
    /* p[0]: byte offset into the mask, p[1]: mask words to clear */
    off += (uint64_t)(p[0] / 4) * 256 * bpp;
    if (off >= s->vga.vram_size) {
        return;
    }
    pixels = MIN((uint64_t)p[1] * 256, (s->vga.vram_size - off) / bpp);
    for (uint64_t i = 0; i < pixels; i++) {
        if (bpp == 2) {
            vram_st(s, off + i * 2, 2, clear);
        } else {
            /* the clear value holds both: stencil << 24 | depth */
            vram_st(s, off + i * 4, 4, clear);
        }
    }
}
