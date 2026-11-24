/*
 * QEMU ATI SVGA emulation
 * Radeon 9600 (RV350, R300 family) 3D engine.
 *
 * Unlike the R100 the R300 is programmable: vertices are transformed by a
 * vertex program (PVS) and pixels shaded by a fragment program (US). Both
 * are interpreted here, and triangles rasterised on the host straight
 * into video memory, through the registers the hardware uses:
 *
 *   VAP   vertex fetch (PROG_STREAM_CNTL), the vertex program and its
 *         constants (PVS_*), viewport transform (VTE, SE_VPORT_*)
 *   GA/SU/SC   setup: culling, shading, point size, scissor
 *   RS    rasteriser: interpolated colours and texture coordinates into
 *         fragment program temporaries
 *   US    fragment program: texture and ALU instructions, constants
 *   TX    texture units
 *   FG    alpha test
 *   ZB    depth and stencil
 *   RB3D  blending, colour mask, colour buffer
 *
 * Fragments are shaded in 2x2 quads, as on the hardware, so that texture
 * level of detail comes from real derivatives even for dependent reads.
 * Tiling bits are ignored throughout: every unit sees video memory as
 * linear, which keeps them consistent with each other and with the CPU.
 * Byte swapping (ENDIAN fields) applies to system memory only, as it is
 * done on the bus; video memory is in the card's order (see vram_xor).
 *
 * Register layouts follow the R300 register headers of the Linux kernel's
 * radeon driver; what those leave out was taken from what Apple's driver
 * sends.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qemu/thread.h"
#include "ati_int.h"
#include "ati_regs.h"
#include "ati_r300_gl.h"

/* ---- registers ----------------------------------------------------------- */

#define VAP_CNTL                0x2080
#define VAP_VF_CNTL             0x2084
#define VAP_OUTPUT_VTX_FMT_0    0x2090
#define VAP_OUTPUT_VTX_FMT_1    0x2094
#define VAP_VTE_CNTL            0x20b0
#define VAP_VTX_SIZE            0x20b4
#define VAP_INDEX_OFFSET        0x208c
#define VAP_CNTL_STATUS         0x2140
#define VAP_PROG_STREAM_CNTL_0  0x2150
#define VAP_PROG_STREAM_CNTL_EXT_0 0x21e0
#define VAP_PVS_UPLOAD_ADDRESS  0x2200
#define VAP_PVS_UPLOAD_DATA     0x2208
#define VAP_PVS_CODE_CNTL_0     0x22d0
#define VAP_PVS_CONST_CNTL      0x22d4
#define VAP_PVS_CODE_CNTL_1     0x22d8

#define SE_VPORT_XSCALE         0x1d98      /* then XOFFSET, Y..., Z... */

#define GB_VAP_RASTER_VTX_FMT_0 0x4000
#define GB_VAP_RASTER_VTX_FMT_1 0x4004
#define GB_ENABLE               0x4008
#define GB_SELECT               0x401c
#define GA_FOG_SCALE            0x4294
#define GA_FOG_OFFSET           0x4298
#define FG_FOG_COLOR_R          0x4bc8      /* then G, B: 10 bits each */
#define GA_POINT_S0             0x4200      /* then T0, S1, T1 */
#define GA_POINT_SIZE           0x421c
#define GA_COLOR_CONTROL        0x4278
#define SU_CULL_MODE            0x42b8
#define RS_COUNT                0x4300
#define RS_INST_COUNT           0x4304
#define RS_IP_0                 0x4310
#define RS_INST_0               0x4330
#define SC_SCISSOR0             0x43e0
#define SC_SCISSOR1             0x43e4
#define TX_ENABLE               0x4104
#define TX_FILTER0_0            0x4400
#define TX_FILTER1_0            0x4440
#define TX_FORMAT0_0            0x4480
#define TX_FORMAT1_0            0x44c0
#define TX_FORMAT2_0            0x4500
#define TX_OFFSET_0             0x4540
#define TX_BORDER_COLOR_0       0x45c0
#define US_CONFIG               0x4600
#define US_PIXSIZE              0x4604
#define US_CODE_OFFSET          0x4608
#define US_CODE_ADDR_0          0x4610
#define US_TEX_INST_0           0x4620
#define US_OUT_FMT_0            0x46a4
#define US_W_FMT                0x46b4
#define US_ALU_RGB_ADDR_0       0x46c0
#define US_ALU_ALPHA_ADDR_0     0x47c0
#define US_ALU_RGB_INST_0       0x48c0
#define US_ALU_ALPHA_INST_0     0x49c0
#define US_ALU_CONST_R_0        0x4c00      /* R, G, B, A; 16 bytes apart */
#define FG_FOG_BLEND            0x4bc0
#define FG_ALPHA_FUNC           0x4bd4
#define RB3D_CCTL               0x4e00
#define RB3D_CBLEND             0x4e04
#define RB3D_ABLEND             0x4e08
#define RB3D_COLOR_CHANNEL_MASK 0x4e0c
#define RB3D_BLEND_COLOR        0x4e10
#define RB3D_ROPCNTL            0x4e18
#define RB3D_COLOROFFSET0       0x4e28
#define RB3D_COLORPITCH0        0x4e38
#define ZB_CNTL                 0x4f00
#define ZB_ZSTENCILCNTL         0x4f04
#define ZB_STENCILREFMASK       0x4f08
#define ZB_FORMAT               0x4f10
#define ZB_DEPTHOFFSET          0x4f20
#define ZB_DEPTHPITCH           0x4f24
#define ZB_DEPTHCLEARVALUE      0x4f28

#define R300_TEX_UNITS          16
#define R300_TEMPS              32
#define R300_PVS_TEMPS          32
#define R300_PVS_IO             16

static inline uint32_t reg(ATIVGAState *s, uint32_t r)
{
    return s->shadow[r >> 2];
}

static inline float u2f(uint32_t v)
{
    union { uint32_t u; float f; } c = { .u = v };
    return c.f;
}

static inline float clamp01(float v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static float half_to_float(uint16_t h)
{
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float v;

    if (e == 0) {
        v = ldexpf(m, -24);
    } else if (e == 31) {
        v = m ? NAN : INFINITY;
    } else {
        v = ldexpf(m | 0x400, e - 25);
    }
    return s ? -v : v;
}

static uint16_t float_to_half(float f)
{
    union { float f; uint32_t u; } c = { .f = f };
    uint32_t s = (c.u >> 16) & 0x8000;
    int e = ((c.u >> 23) & 0xff) - 127 + 15;
    uint32_t m = c.u & 0x7fffff;

    if (((c.u >> 23) & 0xff) == 0xff) {
        return s | 0x7c00 | (m ? 0x200 : 0);
    }
    if (e >= 31) {
        return s | 0x7c00;
    }
    if (e <= 0) {
        if (e < -10) {
            return s;
        }
        m |= 0x800000;
        return s | ((m >> (14 - e)) + ((m >> (13 - e)) & 1));
    }
    return s | (e << 10) | ((m >> 13) + ((m >> 12) & 1));
}

/* US constants are 24-bit floats: sign 23, exponent 22:16 (bias 63). */
static float fp24_to_float(uint32_t v)
{
    uint32_t e = (v >> 16) & 0x7f, m = v & 0xffff;
    float f;

    if (!e && !m) {
        f = 0;
    } else if (e == 0x7f) {
        f = m ? NAN : INFINITY;
    } else {
        f = ldexpf(1.0f + m / 65536.0f, (int)e - 63);
    }
    return (v & 0x800000) ? -f : f;
}

/* Debugging: PPCOSX_ATI_3D_STATS prints counts every 1000 draws. */
static struct {
    uint64_t draws, tris, culled, quads, pixels, written, killed;
    int64_t ns;                 /* time spent drawing */
} stats;
static int stats_enabled = -1;

static bool stats_on(void)
{
    if (stats_enabled < 0) {
        stats_enabled = getenv("PPCOSX_ATI_3D_STATS") != NULL;
    }
    return stats_enabled;
}

/* ---- PVS memory upload ---------------------------------------------------- */

/*
 * VAP_PVS_UPLOAD_ADDRESS picks a vector (4 dwords) of the vertex program
 * memory: instructions from 0, constants from 0x200, state from 0x400.
 * VAP_PVS_UPLOAD_DATA writes it a dword at a time, moving on as it goes.
 */
void ati_r300_pvs_write(ATIVGAState *s, uint32_t addr, uint32_t val)
{
    if (addr == VAP_PVS_UPLOAD_ADDRESS) {
        s->pvs_index = (val & 0x7ff) * 4;
        return;
    }
    s->pvs[s->pvs_index % ATI_PVS_DWORDS] = val;
    s->pvs_index = (s->pvs_index + 1) % ATI_PVS_DWORDS;
}

/* ---- vertices ------------------------------------------------------------ */

typedef struct R300Vertex {
    float pos[4];               /* window x, y, z; 1/w */
    float clip[4];              /* the position the vertex program wrote */
    float col[4][4];
    float tc[32];               /* texture coordinate components, packed */
    float psize;
} R300Vertex;

typedef struct R300Stream {
    int type, skip, dst, sign, norm;
    uint32_t ext;
} R300Stream;

typedef struct R300Tex {
    bool enabled;
    int fmt;                    /* TX_FORMAT1 TXFORMAT */
    uint32_t fmt1;
    int bpp;                    /* bytes per texel, 0 for DXTn */
    int w, h;
    int levels;
    uint64_t lvl_off[12];
    int lvl_w[12], lvl_h[12];
    uint32_t lvl_pitch[12];     /* bytes */
    bool vram;
    uint64_t vram_off;
    uint8_t *data;              /* staged from system memory */
    int swap;
    int wrap_s, wrap_t;
    int mag, min, mip;          /* 1 nearest, 2 linear; mip 0 none */
    float border[4];
    int sel[4];                 /* SEL_RED, GREEN, BLUE, ALPHA */
    uint32_t sgn;               /* SIGNED_COMP */
} R300Tex;

typedef struct R300Alu {
    uint32_t rgb_addr, alpha_addr, rgb_inst, alpha_inst;
} R300Alu;

typedef struct R300AluDec {
    uint8_t rsrc[3], asrc[3];   /* addresses: bit 5 picks a constant */
    uint8_t rsel[3], rmod[3], asel[3], amod[3];
    uint8_t rpre, apre, rop, aop, romod, aomod;
    bool rclamp, aclamp;
    uint8_t rd, ad, rwmask, romask;
    bool awrite, aout, dout;
} R300AluDec;

typedef struct R300Node {
    int alu_start, alu_end;     /* inclusive */
    int tex_start, tex_end;     /* tex_end < tex_start: none */
} R300Node;

typedef struct R300Draw {
    ATIVGAState *s;
    /* vertex stage */
    R300Stream streams[16];
    int nstreams;
    bool bypass;
    uint32_t vte;
    float vport[6];
    uint32_t out_fmt0, out_fmt1;
    int tc_base[8], tc_count[8];
    int ncolors;
    int col_slot[4];
    int tc_slot[8];
    int psize_slot;
    int nslots;
    int pvs_first, pvs_last;
    int const_base;
    uint32_t vc_swap;
    /* setup */
    uint32_t cull, color_ctl, gb_enable;
    float point_w, point_h;
    float point_st[4];          /* GA_POINT_S0, T0, S1, T1 */
    int clip_x0, clip_y0, clip_x1, clip_y1;         /* inclusive */
    /* rasteriser */
    int rs_count;
    uint32_t rs_ip[8], rs_inst[8];
    /* fragment program */
    R300Node nodes[4];
    int nnodes;
    uint32_t tex_inst[32];
    R300Alu alu[64];
    R300AluDec dec[64];         /* the same, decoded */
    int ntemps;                 /* temporaries the program and RS use */
    float consts[32][4];
    bool writes_depth;
    bool has_kill;
    bool gpu;                   /* drawn on the host GPU (ati_r300_gl.c) */
    bool logged;                /* PPCOSX_R300_TARGETLOG matched */
    R300Tex tex[R300_TEX_UNITS];
    /* back end */
    uint32_t zb_cntl, zs_cntl, zb_format;
    uint32_t stencil_ref, stencil_mask, stencil_wmask;
    bool z_on, s_on;
    uint64_t depth_off;
    uint32_t depth_pitch;
    int depth_bpp;
    uint32_t cblend, ablend, blend_color, cmask, rop;
    uint32_t alpha_func;
    bool fog_on;
    int fog_sel;                /* GB_SELECT FOG_SELECT */
    float fog_scale, fog_offset, fog_color[3];
    int color_fmt, color_bpp;
    uint32_t color_pitch;       /* pixels */
    uint64_t color_off;
    uint8_t *color_stage;
    uint32_t color_mc;
    int color_swap;
    int dirty_y0, dirty_y1;
} R300Draw;

/* ---- textures ------------------------------------------------------------ */

static int tex_bpp(int fmt)
{
    switch (fmt) {
    case 0x0: case 0x2: case 0x5:               /* X8, Y4X4, Z3Y3X2 */
        return 1;
    case 0x1: case 0x3: case 0x6: case 0x7:     /* X16, Y8X8, 565, 655 */
    case 0xa: case 0xb: case 0x14: case 0x15:   /* 4444, 1555, YUV */
    case 0x18:                                  /* FL_I16 */
        return 2;
    case 0x4: case 0x8: case 0x9: case 0xc: case 0xd: case 0x12: case 0x13:
    case 0x19: case 0x1b:                       /* FL_I16A16, FL_I32 */
        return 4;
    case 0xe: case 0x1a: case 0x1c:             /* 16161616, FL_RGBA16 */
        return 8;
    case 0x1d:                                  /* FL_R32G32B32A32 */
        return 16;
    default:
        return 0;                               /* DXTn, ATI2N */
    }
}

static void tex_setup(R300Draw *d, int n)
{
    ATIVGAState *s = d->s;
    R300Tex *t = &d->tex[n];
    uint32_t f0 = reg(s, TX_FORMAT0_0 + 4 * n);
    uint32_t f1 = reg(s, TX_FORMAT1_0 + 4 * n);
    uint32_t f2 = reg(s, TX_FORMAT2_0 + 4 * n);
    uint32_t filt = reg(s, TX_FILTER0_0 + 4 * n);
    uint32_t off = reg(s, TX_OFFSET_0 + 4 * n);
    uint32_t border = reg(s, TX_BORDER_COLOR_0 + 4 * n);
    uint64_t len;
    int max_level;

    memset(t, 0, sizeof(*t));
    if (!(reg(s, TX_ENABLE) & (1u << n))) {
        return;
    }
    t->fmt1 = f1;
    t->fmt = f1 & 0x1f;
    t->bpp = tex_bpp(t->fmt);
    t->w = (f0 & 0x7ff) + 1;
    t->h = ((f0 >> 11) & 0x7ff) + 1;
    max_level = (f0 >> 26) & 0xf;
    t->sel[3] = (f1 >> 9) & 7;
    t->sel[0] = (f1 >> 12) & 7;
    t->sel[1] = (f1 >> 15) & 7;
    t->sel[2] = (f1 >> 18) & 7;
    t->sgn = (f1 >> 5) & 0xf;
    t->swap = off & 3;
    t->wrap_s = filt & 7;
    t->wrap_t = (filt >> 3) & 7;
    t->mag = (filt >> 9) & 3;
    t->min = (filt >> 11) & 3;
    t->mip = (filt >> 13) & 3;
    t->border[0] = ((border >> 16) & 0xff) / 255.0f;
    t->border[1] = ((border >> 8) & 0xff) / 255.0f;
    t->border[2] = (border & 0xff) / 255.0f;
    t->border[3] = (border >> 24) / 255.0f;
    if (!t->bpp) {
        static bool warned[32];

        if (!warned[t->fmt]) {
            warned[t->fmt] = true;
            qemu_log_mask(LOG_UNIMP, "ati: R300 texture format 0x%x not "
                          "implemented\n", t->fmt);
        }
        return;
    }
    /*
     * Level 0, then each mipmap level, every level starting on a 32-byte
     * boundary with rows a multiple of 32 bytes. TXPITCH_EN gives level
     * 0's pitch in texels explicitly.
     */
    t->levels = 1;
    t->lvl_w[0] = t->w;
    t->lvl_h[0] = t->h;
    if (f0 & (1u << 31)) {
        t->lvl_pitch[0] = ((f2 & 0x3fff) + 1) * t->bpp;
    } else {
        t->lvl_pitch[0] = ROUND_UP(t->w * t->bpp, 32);
    }
    len = (uint64_t)t->lvl_pitch[0] * t->h;
    if (t->mip) {
        while (t->levels <= max_level && t->levels < 12 &&
               (t->lvl_w[t->levels - 1] > 1 || t->lvl_h[t->levels - 1] > 1)) {
            int l = t->levels++;

            t->lvl_w[l] = MAX(t->lvl_w[l - 1] >> 1, 1);
            t->lvl_h[l] = MAX(t->lvl_h[l - 1] >> 1, 1);
            t->lvl_pitch[l] = ROUND_UP(t->lvl_w[l] * t->bpp, 32);
            t->lvl_off[l] = ROUND_UP(len, 32);
            len = t->lvl_off[l] + (uint64_t)t->lvl_pitch[l] * t->lvl_h[l];
        }
    }
    if (ati_mc_vram_range(s, off & ~0x1fu, len, &t->vram_off)) {
        t->vram = true;
    } else if (len <= 64 * MiB) {
        t->data = g_malloc(len);
        if (!ati_mc_read(s, off & ~0x1fu, t->data, len)) {
            g_free(t->data);
            t->data = NULL;
            return;
        }
    } else {
        return;
    }
    t->enabled = true;
    if (stats_on()) {
        static uint32_t seen[64][3];
        static int nseen;
        bool found = false;

        for (int i = 0; i < nseen; i++) {
            found |= seen[i][0] == off && seen[i][1] == f0 &&
                     seen[i][2] == f1;
        }
        if (!found && nseen < 64) {
            seen[nseen][0] = off;
            seen[nseen][1] = f0;
            seen[nseen++][2] = f1;
            fprintf(stderr, "r300: tex%d offset %08x format0 %08x format1 "
                    "%08x format2 %08x filter %08x -> %dx%d pitch %u vram "
                    "%d\n", n, off, f0, f1, f2, filt, t->w, t->h,
                    t->lvl_pitch[0], t->vram);
        }
    }
}

static void tex_release(R300Draw *d)
{
    for (int n = 0; n < R300_TEX_UNITS; n++) {
        g_free(d->tex[n].data);
        d->tex[n].data = NULL;
    }
}

/* Load `bytes` (1, 2, 4) of a texture: video memory as is, else swapped. */
static uint32_t tex_ld(R300Draw *d, R300Tex *t, uint64_t off, int bytes)
{
    uint32_t w;

    if (t->vram) {
        return ati_vram_ld(d->s, t->vram_off + off, bytes);
    }
    w = ldn_le_p(t->data + off, bytes);
    switch (t->swap) {
    case 1:                                     /* bytes in 16-bit words */
        return bytes >= 2 ? ((w & 0x00ff00ff) << 8) | ((w >> 8) & 0x00ff00ff)
                          : w;
    case 2:                                     /* 32-bit */
        return bytes == 4 ? bswap32(w) : bytes == 2 ? bswap16(w) : w;
    case 3:                                     /* 16-bit halves */
        return bytes == 4 ? (w << 16) | (w >> 16) : w;
    default:
        return w;
    }
}

static inline float unorm(uint32_t v, int bits)
{
    return v / (float)((1u << bits) - 1);
}

/* Fetch one texel as its X, Y, Z, W components. */
static void tex_texel(R300Draw *d, R300Tex *t, int l, int x, int y,
                      float c[4])
{
    uint64_t off = t->lvl_off[l] + (uint64_t)y * t->lvl_pitch[l] +
                   (uint64_t)x * t->bpp;
    uint32_t v, v2;

    c[0] = c[1] = c[2] = 0;
    c[3] = 1;
    switch (t->fmt) {
    case 0x0:                                   /* X8 */
        c[0] = unorm(tex_ld(d, t, off, 1), 8);
        break;
    case 0x1:                                   /* X16 */
        c[0] = unorm(tex_ld(d, t, off, 2), 16);
        break;
    case 0x2:                                   /* Y4X4 */
        v = tex_ld(d, t, off, 1);
        c[0] = unorm(v & 0xf, 4);
        c[1] = unorm(v >> 4, 4);
        break;
    case 0x3:                                   /* Y8X8 */
        v = tex_ld(d, t, off, 2);
        c[0] = unorm(v & 0xff, 8);
        c[1] = unorm(v >> 8, 8);
        break;
    case 0x4:                                   /* Y16X16 */
        v = tex_ld(d, t, off, 4);
        c[0] = unorm(v & 0xffff, 16);
        c[1] = unorm(v >> 16, 16);
        break;
    case 0x5:                                   /* Z3Y3X2 */
        v = tex_ld(d, t, off, 1);
        c[0] = unorm(v & 3, 2);
        c[1] = unorm((v >> 2) & 7, 3);
        c[2] = unorm(v >> 5, 3);
        break;
    case 0x6:                                   /* Z5Y6X5 */
        v = tex_ld(d, t, off, 2);
        c[0] = unorm(v & 0x1f, 5);
        c[1] = unorm((v >> 5) & 0x3f, 6);
        c[2] = unorm(v >> 11, 5);
        break;
    case 0x7:                                   /* Z6Y5X5 */
        v = tex_ld(d, t, off, 2);
        c[0] = unorm(v & 0x1f, 5);
        c[1] = unorm((v >> 5) & 0x1f, 5);
        c[2] = unorm(v >> 10, 6);
        break;
    case 0x8:                                   /* Z11Y11X10 */
        v = tex_ld(d, t, off, 4);
        c[0] = unorm(v & 0x3ff, 10);
        c[1] = unorm((v >> 10) & 0x7ff, 11);
        c[2] = unorm(v >> 21, 11);
        break;
    case 0x9:                                   /* Z10Y11X11 */
        v = tex_ld(d, t, off, 4);
        c[0] = unorm(v & 0x7ff, 11);
        c[1] = unorm((v >> 11) & 0x7ff, 11);
        c[2] = unorm(v >> 22, 10);
        break;
    case 0xa:                                   /* W4Z4Y4X4 */
        v = tex_ld(d, t, off, 2);
        for (int i = 0; i < 4; i++) {
            c[i] = unorm((v >> (4 * i)) & 0xf, 4);
        }
        break;
    case 0xb:                                   /* W1Z5Y5X5 */
        v = tex_ld(d, t, off, 2);
        c[0] = unorm(v & 0x1f, 5);
        c[1] = unorm((v >> 5) & 0x1f, 5);
        c[2] = unorm((v >> 10) & 0x1f, 5);
        c[3] = v >> 15;
        break;
    case 0xc: case 0x13:                        /* W8Z8Y8X8 */
        v = tex_ld(d, t, off, 4);
        for (int i = 0; i < 4; i++) {
            c[i] = ((v >> (8 * i)) & 0xff) / 255.0f;
        }
        break;
    case 0xd:                                   /* W2Z10Y10X10 */
        v = tex_ld(d, t, off, 4);
        c[0] = unorm(v & 0x3ff, 10);
        c[1] = unorm((v >> 10) & 0x3ff, 10);
        c[2] = unorm((v >> 20) & 0x3ff, 10);
        c[3] = unorm(v >> 30, 2);
        break;
    case 0xe:                                   /* W16Z16Y16X16 */
        v = tex_ld(d, t, off, 4);
        v2 = tex_ld(d, t, off + 4, 4);
        c[0] = unorm(v & 0xffff, 16);
        c[1] = unorm(v >> 16, 16);
        c[2] = unorm(v2 & 0xffff, 16);
        c[3] = unorm(v2 >> 16, 16);
        break;
    case 0x18:                                  /* FL_I16 */
        c[0] = half_to_float(tex_ld(d, t, off, 2));
        break;
    case 0x19:                                  /* FL_I16A16 */
        v = tex_ld(d, t, off, 4);
        c[0] = half_to_float(v & 0xffff);
        c[1] = half_to_float(v >> 16);
        break;
    case 0x1a:                                  /* FL_R16G16B16A16 */
        v = tex_ld(d, t, off, 4);
        v2 = tex_ld(d, t, off + 4, 4);
        c[0] = half_to_float(v & 0xffff);
        c[1] = half_to_float(v >> 16);
        c[2] = half_to_float(v2 & 0xffff);
        c[3] = half_to_float(v2 >> 16);
        break;
    case 0x1b:                                  /* FL_I32 */
        c[0] = u2f(tex_ld(d, t, off, 4));
        break;
    case 0x1c:                                  /* FL_I32A32 */
        c[0] = u2f(tex_ld(d, t, off, 4));
        c[1] = u2f(tex_ld(d, t, off + 4, 4));
        break;
    case 0x1d:                                  /* FL_R32G32B32A32 */
        for (int i = 0; i < 4; i++) {
            c[i] = u2f(tex_ld(d, t, off + 4 * i, 4));
        }
        break;
    default:
        break;
    }
}

/* Wrap a texel coordinate per CLAMP_*; -1 means the border colour. */
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
    case 3: case 5: case 7:                     /* mirror once */
        if (i < 0) {
            i = -1 - i;
        }
        if (mode == 7 && i >= size) {
            return -1;
        }
        return i >= size ? size - 1 : i;
    case 6:                                     /* clamp to border */
        return i < 0 || i >= size ? -1 : i;
    default:                                    /* clamp to edge */
        return i < 0 ? 0 : i >= size ? size - 1 : i;
    }
}

static void tex_fetch(R300Draw *d, R300Tex *t, int l, int x, int y,
                      float c[4])
{
    int xx = tex_wrap(x, t->lvl_w[l], t->wrap_s);
    int yy = tex_wrap(y, t->lvl_h[l], t->wrap_t);

    if (xx < 0 || yy < 0) {
        /* the border is ARGB; return it in X=B, Y=G, Z=R, W=A order */
        c[0] = t->border[2];
        c[1] = t->border[1];
        c[2] = t->border[0];
        c[3] = t->border[3];
        return;
    }
    tex_texel(d, t, l, xx, yy, c);
}

static void tex_sample_level(R300Draw *d, R300Tex *t, int l, bool linear,
                             float s, float tt, float c[4])
{
    float u = s * t->lvl_w[l], v = tt * t->lvl_h[l];

    if (!linear) {
        tex_fetch(d, t, l, (int)floorf(u), (int)floorf(v), c);
        return;
    }
    u -= 0.5f;
    v -= 0.5f;
    int x0 = (int)floorf(u), y0 = (int)floorf(v);
    float fx = u - x0, fy = v - y0;
    float a[4], b[4], e[4], f[4];

    tex_fetch(d, t, l, x0, y0, a);
    tex_fetch(d, t, l, x0 + 1, y0, b);
    tex_fetch(d, t, l, x0, y0 + 1, e);
    tex_fetch(d, t, l, x0 + 1, y0 + 1, f);
    for (int i = 0; i < 4; i++) {
        c[i] = (a[i] * (1 - fx) + b[i] * fx) * (1 - fy) +
               (e[i] * (1 - fx) + f[i] * fx) * fy;
    }
}

/* Sample at (s, t) with level of detail `lod`; result in RGBA order. */
static void tex_sample(R300Draw *d, R300Tex *t, float s, float tt,
                       float lod, float out[4])
{
    float c[4], k[8];

    if (!isfinite(s) || !isfinite(tt)) {
        s = tt = 0;
    }
    if (lod <= 0 || t->levels == 1 || !t->mip) {
        bool linear = lod <= 0 ? t->mag == 2 : t->min == 2;
        int l = (lod > 0 && t->mip && t->levels > 1) ?
                MIN((int)(lod + 0.5f), t->levels - 1) : 0;

        tex_sample_level(d, t, l, linear, s, tt, c);
    } else {
        float lf = MIN(lod, (float)(t->levels - 1));
        int l0 = (int)lf;

        if (t->mip == 2 && l0 + 1 < t->levels) {
            float c1[4], fr = lf - l0;

            tex_sample_level(d, t, l0, t->min == 2, s, tt, c);
            tex_sample_level(d, t, l0 + 1, t->min == 2, s, tt, c1);
            for (int i = 0; i < 4; i++) {
                c[i] = c[i] * (1 - fr) + c1[i] * fr;
            }
        } else {
            tex_sample_level(d, t, MIN((int)(lf + 0.5f), t->levels - 1),
                             t->min == 2, s, tt, c);
        }
    }
    /* signed components: stored as two's complement fractions */
    for (int i = 0; i < 4; i++) {
        if (t->sgn & (1u << i)) {
            c[i] = c[i] > 0.5f ? (c[i] - 1.0f) * 2.0f : c[i] * 2.0f;
        }
    }
    memcpy(k, c, sizeof(c));
    k[4] = 0;
    k[5] = 1;
    k[6] = 0;
    k[7] = 1;
    for (int i = 0; i < 4; i++) {
        out[i] = k[t->sel[i]];
    }
}

/* ---- the fragment program ------------------------------------------------ */

typedef struct R300Quad {
    float t[4][R300_TEMPS][4];  /* temporaries, per pixel of the quad */
    float out[4][4];            /* colour output */
    float depth[4];
    float fog[4];               /* fog coordinate */
    bool live[4];               /* covered by the primitive */
    bool killed[4];
} R300Quad;

static float omod(float v, int m)
{
    switch (m) {
    case 1: return v * 2;
    case 2: return v * 4;
    case 3: return v * 8;
    case 4: return v * 0.5f;
    case 5: return v * 0.25f;
    case 6: return v * 0.125f;
    default: return v;
    }
}

static inline float modify(float v, int mod)
{
    if (mod & 2) {
        v = fabsf(v);
    }
    return (mod & 1) ? -v : v;
}

static float presub(int op, float s0, float s1)
{
    switch (op) {
    case 0: return 1 - 2 * s0;
    case 1: return s1 - s0;
    case 2: return s1 + s0;
    default: return 1 - s0;
    }
}

/*
 * An RGB argument: SEL picks source components: 0-11 srcN rgb/rrr/ggg/bbb,
 * 12-14 srcN alpha, 15-19 the pre-subtract srcp rgb/rrr/ggg/bbb/aaa,
 * 20 zero, 21 one, 22 half, then rotations.
 */
static void rgb_arg(int sel, const float rgb[3][4], const float alpha[3],
                    const float srcp[4], float out[3])
{
    static const int rot[3][3] = { { 1, 2, 0 }, { 2, 0, 1 }, { 3, 2, 1 } };

    if (sel < 12) {
        int src = sel / 4, sw = sel % 4;

        for (int i = 0; i < 3; i++) {
            out[i] = rgb[src][sw ? sw - 1 : i];
        }
    } else if (sel < 15) {
        out[0] = out[1] = out[2] = alpha[sel - 12];
    } else if (sel == 15) {
        memcpy(out, srcp, 3 * sizeof(float));
    } else if (sel <= 19) {
        out[0] = out[1] = out[2] = srcp[sel - 16];
    } else if (sel == 20) {
        out[0] = out[1] = out[2] = 0;
    } else if (sel == 21) {
        out[0] = out[1] = out[2] = 1;
    } else if (sel == 22) {
        out[0] = out[1] = out[2] = 0.5f;
    } else if (sel >= 23 && sel <= 28) {
        int src = (sel - 23) % 3, r = (sel - 23) / 3;

        for (int i = 0; i < 3; i++) {
            out[i] = rgb[src][rot[r][i]];
        }
    } else if (sel >= 29) {
        int src = sel - 29;

        out[0] = alpha[src];
        out[1] = rgb[src][2];
        out[2] = rgb[src][1];
    } else {
        out[0] = out[1] = out[2] = 0;
    }
}

/* An alpha argument: 0-8 srcN r/g/b, 9-11 srcN alpha, 12-15 srcp r/g/b/a. */
static float alpha_arg(int sel, const float rgb[3][4], const float alpha[3],
                       const float srcp[4])
{
    if (sel < 9) {
        return rgb[sel / 3][sel % 3];
    } else if (sel < 12) {
        return alpha[sel - 9];
    } else if (sel <= 15) {
        return srcp[sel - 12];
    } else if (sel == 16) {
        return 0;
    } else if (sel == 17) {
        return 1;
    } else if (sel == 18) {
        return 0.5f;
    }
    return 0;
}

/* One ALU instruction for one pixel. */
/*
 * An ALU instruction decoded once per draw (see alu_decode); fields as in
 * US_ALU_RGB_ADDR/INST and US_ALU_ALPHA_ADDR/INST.
 */
static void alu_decode(const R300Alu *a, R300AluDec *o)
{
    uint32_t ri = a->rgb_inst, ai = a->alpha_inst;

    for (int k = 0; k < 3; k++) {
        o->rsrc[k] = (a->rgb_addr >> (6 * k)) & 0x3f;
        o->asrc[k] = (a->alpha_addr >> (6 * k)) & 0x3f;
        o->rsel[k] = (ri >> (7 * k)) & 0x1f;
        o->rmod[k] = (ri >> (7 * k + 5)) & 3;
        o->asel[k] = (ai >> (7 * k)) & 0x1f;
        o->amod[k] = (ai >> (7 * k + 5)) & 3;
    }
    o->rpre = (ri >> 21) & 3;
    o->apre = (ai >> 21) & 3;
    o->rop = (ri >> 23) & 0xf;
    o->aop = (ai >> 23) & 0xf;
    o->romod = (ri >> 27) & 7;
    o->aomod = (ai >> 27) & 7;
    o->rclamp = ri & (1u << 30);
    o->aclamp = ai & (1u << 30);
    o->rd = (a->rgb_addr >> 18) & 0x1f;
    o->ad = (a->alpha_addr >> 18) & 0x1f;
    o->rwmask = (a->rgb_addr >> 23) & 7;
    o->romask = (a->rgb_addr >> 26) & 7;
    o->awrite = a->alpha_addr & (1u << 23);
    o->aout = a->alpha_addr & (1u << 24);
    o->dout = a->alpha_addr & (1u << 27);
}

/* One ALU instruction for one pixel. */
static void alu_exec(R300Draw *d, R300Quad *q, int p, const R300AluDec *a)
{
    float (*t)[4] = q->t[p];
    float rgb[3][4], alpha[3], srcp[4];
    float A[3], B[3], C[3], Aa, Ba, Ca;
    float r[3], ra, dot = 0;
    int rop = a->rop, aop = a->aop;

    /* sources: RGB per US_ALU_RGB_ADDR, alpha per US_ALU_ALPHA_ADDR */
    for (int k = 0; k < 3; k++) {
        int rs = a->rsrc[k], as = a->asrc[k];
        const float *rv = (rs & 0x20) ? d->consts[rs & 0x1f] : t[rs & 0x1f];

        memcpy(rgb[k], rv, 4 * sizeof(float));
        alpha[k] = (as & 0x20) ? d->consts[as & 0x1f][3] : t[as & 0x1f][3];
    }
    for (int i = 0; i < 3; i++) {
        srcp[i] = presub(a->rpre, rgb[0][i], rgb[1][i]);
    }
    srcp[3] = presub(a->apre, alpha[0], alpha[1]);

    rgb_arg(a->rsel[0], rgb, alpha, srcp, A);
    rgb_arg(a->rsel[1], rgb, alpha, srcp, B);
    rgb_arg(a->rsel[2], rgb, alpha, srcp, C);
    if (a->rmod[0] | a->rmod[1] | a->rmod[2]) {
        for (int i = 0; i < 3; i++) {
            A[i] = modify(A[i], a->rmod[0]);
            B[i] = modify(B[i], a->rmod[1]);
            C[i] = modify(C[i], a->rmod[2]);
        }
    }
    Aa = modify(alpha_arg(a->asel[0], rgb, alpha, srcp), a->amod[0]);
    Ba = modify(alpha_arg(a->asel[1], rgb, alpha, srcp), a->amod[1]);
    Ca = modify(alpha_arg(a->asel[2], rgb, alpha, srcp), a->amod[2]);

    /* alpha unit first: SOP results feed the RGB unit */
    switch (aop) {
    case 0: ra = Aa * Ba + Ca; break;                   /* MAD */
    case 1: ra = 0; break;                              /* DP: below */
    case 2: ra = MIN(Aa, Ba); break;
    case 3: ra = MAX(Aa, Ba); break;
    case 5: ra = Ca > 0.5f ? Aa : Ba; break;            /* CND */
    case 6: ra = Ca >= 0 ? Aa : Ba; break;              /* CMP */
    case 7: ra = Aa - floorf(Aa); break;                /* FRC */
    case 8: ra = exp2f(Aa); break;                      /* EX2 */
    case 9: ra = Aa == 0 ? -INFINITY : log2f(fabsf(Aa)); break; /* LN2 */
    case 10: ra = 1.0f / Aa; break;                     /* RCP */
    case 11: ra = 1.0f / sqrtf(fabsf(Aa)); break;       /* RSQ */
    default: {
        static bool warned[16];

        if (!warned[aop]) {
            warned[aop] = true;
            qemu_log_mask(LOG_UNIMP, "ati: R300 US alpha op %d\n", aop);
        }
        ra = 0;
    }
    }

    switch (rop) {
    case 0:                                             /* MAD */
        for (int i = 0; i < 3; i++) {
            r[i] = A[i] * B[i] + C[i];
        }
        break;
    case 1:                                             /* DP3 */
        dot = A[0] * B[0] + A[1] * B[1] + A[2] * B[2];
        r[0] = r[1] = r[2] = dot;
        break;
    case 2:                                             /* DP4 */
        dot = A[0] * B[0] + A[1] * B[1] + A[2] * B[2] + Aa * Ba;
        r[0] = r[1] = r[2] = dot;
        break;
    case 3:                                             /* D2A */
        dot = A[0] * B[0] + A[1] * B[1] + C[2];
        r[0] = r[1] = r[2] = dot;
        break;
    case 4:
        for (int i = 0; i < 3; i++) {
            r[i] = MIN(A[i], B[i]);
        }
        break;
    case 5:
        for (int i = 0; i < 3; i++) {
            r[i] = MAX(A[i], B[i]);
        }
        break;
    case 7:                                             /* CND */
        for (int i = 0; i < 3; i++) {
            r[i] = C[i] > 0.5f ? A[i] : B[i];
        }
        break;
    case 8:                                             /* CMP */
        for (int i = 0; i < 3; i++) {
            r[i] = C[i] >= 0 ? A[i] : B[i];
        }
        break;
    case 9:                                             /* FRC */
        for (int i = 0; i < 3; i++) {
            r[i] = A[i] - floorf(A[i]);
        }
        break;
    case 10:                                            /* SOP */
        r[0] = r[1] = r[2] = ra;
        break;
    default: {
        static bool warned[16];

        if (!warned[rop]) {
            warned[rop] = true;
            qemu_log_mask(LOG_UNIMP, "ati: R300 US rgb op %d\n", rop);
        }
        r[0] = r[1] = r[2] = 0;
    }
    }
    if (aop == 1) {
        ra = dot;
    }

    /* output modifier and clamp */
    if (a->romod || a->rclamp) {
        for (int i = 0; i < 3; i++) {
            r[i] = omod(r[i], a->romod);
            if (a->rclamp) {
                r[i] = clamp01(r[i]);
            }
        }
    }
    ra = omod(ra, a->aomod);
    if (a->aclamp) {
        ra = clamp01(ra);
    }

    /* writes: temporaries, the colour output, depth */
    for (int i = 0; i < 3; i++) {
        if (a->rwmask & (1u << i)) {
            t[a->rd][i] = r[i];
        }
        if (a->romask & (1u << i)) {
            q->out[p][i] = r[i];
        }
    }
    if (a->awrite) {
        t[a->ad][3] = ra;
    }
    if (a->aout) {
        q->out[p][3] = ra;
    }
    if (a->dout) {
        q->depth[p] = ra;
    }
}

/* Level of detail from the quad's texture coordinates. */
static float quad_lod(R300Tex *t, const float s[4], const float tt[4])
{
    float dsx = (s[1] - s[0]) * t->w, dtx = (tt[1] - tt[0]) * t->h;
    float dsy = (s[2] - s[0]) * t->w, dty = (tt[2] - tt[0]) * t->h;
    float rho = MAX(dsx * dsx + dtx * dtx, dsy * dsy + dty * dty);

    if (!(rho > 0) || !isfinite(rho)) {
        return 0;
    }
    return 0.5f * log2f(rho);
}

static void tex_exec(R300Draw *d, R300Quad *q, uint32_t inst)
{
    int src = inst & 0x1f, dst = (inst >> 6) & 0x1f;
    int unit = (inst >> 11) & 0xf, op = (inst >> 15) & 7;
    R300Tex *t = &d->tex[unit];
    float s[4], tt[4], bias[4] = { 0 };

    if (op == 2) {                                      /* TEXKILL */
        for (int p = 0; p < 4; p++) {
            const float *c = q->t[p][src];

            if (c[0] < 0 || c[1] < 0 || c[2] < 0 || c[3] < 0) {
                q->killed[p] = true;
            }
        }
        return;
    }
    for (int p = 0; p < 4; p++) {
        const float *c = q->t[p][src];

        s[p] = c[0];
        tt[p] = c[1];
        if (op == 3 && c[3] != 0) {                     /* TXP */
            s[p] /= c[3];
            tt[p] /= c[3];
        }
        if (op == 4) {                                  /* TXB */
            bias[p] = c[3];
        }
    }
    if (!t->enabled) {
        for (int p = 0; p < 4; p++) {
            q->t[p][dst][0] = q->t[p][dst][1] = q->t[p][dst][2] = 0;
            q->t[p][dst][3] = 1;
        }
        return;
    }
    float lod = quad_lod(t, s, tt);

    for (int p = 0; p < 4; p++) {
        if (q->live[p] || p == 0) {
            tex_sample(d, t, s[p], tt[p], lod + bias[p], q->t[p][dst]);
        } else {
            memcpy(q->t[p][dst], q->t[0][dst], 4 * sizeof(float));
        }
    }
}

static void program_run(R300Draw *d, R300Quad *q)
{
    for (int n = 0; n < d->nnodes; n++) {
        const R300Node *node = &d->nodes[n];

        for (int i = node->tex_start; i <= node->tex_end; i++) {
            tex_exec(d, q, d->tex_inst[i & 31]);
        }
        for (int i = node->alu_start; i <= node->alu_end; i++) {
            for (int p = 0; p < 4; p++) {
                if (q->live[p]) {
                    alu_exec(d, q, p, &d->dec[i & 63]);
                }
            }
        }
    }
}

/* ---- pixels -------------------------------------------------------------- */

static inline uint32_t swap_val(uint32_t v, int bytes, int mode)
{
    if (bytes == 4 && mode == 2) {
        return bswap32(v);
    }
    if (bytes == 4 && mode == 3) {
        return (v << 16) | (v >> 16);
    }
    if (bytes >= 2 && mode == 1) {
        return ((v & 0x00ff00ff) << 8) | ((v >> 8) & 0x00ff00ff);
    }
    return v;
}

/* RB3D_COLORPITCH COLORFORMAT (bits 24:21) */
static int color_bpp(int fmt)
{
    switch (fmt) {
    case 9: case 14:                    /* I8, I10 (stored in 8 bits) */
        return 1;
    case 3: case 4: case 11: case 12: case 13: case 15:
        return 2;
    case 10:                            /* ARGB16161616 */
        return 8;
    case 7:                             /* ARGB32323232 */
        return 16;
    default:
        return 4;
    }
}

static inline uint32_t qz(float v, int max)
{
    v = clamp01(v);
    return (uint32_t)(v * max + 0.5f);
}

static uint64_t color_off(R300Draw *d, int x, int y)
{
    return ((uint64_t)y * d->color_pitch + x) * d->color_bpp;
}

static uint32_t color_ld32(R300Draw *d, uint64_t off, int bytes)
{
    if (d->color_stage) {
        return swap_val(ldn_le_p(d->color_stage + off, bytes), bytes,
                        d->color_swap);
    }
    return ati_vram_ld(d->s, d->color_off + off, bytes);
}

static void color_st32(R300Draw *d, uint64_t off, int bytes, uint32_t v)
{
    if (d->color_stage) {
        stn_le_p(d->color_stage + off, bytes, swap_val(v, bytes,
                                                       d->color_swap));
        return;
    }
    ati_vram_st(d->s, d->color_off + off, bytes, v);
}

/* Read a colour buffer pixel as RGBA. */
static void color_read(R300Draw *d, uint64_t off, float c[4])
{
    uint32_t v, v2;

    switch (d->color_fmt) {
    case 3:                                             /* ARGB1555 */
        v = color_ld32(d, off, 2);
        c[0] = unorm((v >> 10) & 0x1f, 5);
        c[1] = unorm((v >> 5) & 0x1f, 5);
        c[2] = unorm(v & 0x1f, 5);
        c[3] = v >> 15;
        break;
    case 4:                                             /* RGB565 */
        v = color_ld32(d, off, 2);
        c[0] = unorm(v >> 11, 5);
        c[1] = unorm((v >> 5) & 0x3f, 6);
        c[2] = unorm(v & 0x1f, 5);
        c[3] = 1;
        break;
    case 15:                                            /* ARGB4444 */
        v = color_ld32(d, off, 2);
        c[0] = unorm((v >> 8) & 0xf, 4);
        c[1] = unorm((v >> 4) & 0xf, 4);
        c[2] = unorm(v & 0xf, 4);
        c[3] = unorm(v >> 12, 4);
        break;
    case 9:                                             /* I8 */
        v = color_ld32(d, off, 1);
        c[0] = c[1] = c[2] = c[3] = unorm(v, 8);
        break;
    case 13:                                            /* UV88 */
        v = color_ld32(d, off, 2);
        c[0] = unorm(v & 0xff, 8);
        c[1] = unorm(v >> 8, 8);
        c[2] = 0;
        c[3] = 1;
        break;
    case 5:                                             /* ARGB2101010 */
        v = color_ld32(d, off, 4);
        c[0] = unorm((v >> 20) & 0x3ff, 10);
        c[1] = unorm((v >> 10) & 0x3ff, 10);
        c[2] = unorm(v & 0x3ff, 10);
        c[3] = unorm(v >> 30, 2);
        break;
    case 10:                                            /* ARGB16161616 */
        v = color_ld32(d, off, 4);
        v2 = color_ld32(d, off + 4, 4);
        /* half floats, R G B A in memory order */
        c[0] = half_to_float(v & 0xffff);
        c[1] = half_to_float(v >> 16);
        c[2] = half_to_float(v2 & 0xffff);
        c[3] = half_to_float(v2 >> 16);
        break;
    case 7:                                             /* ARGB32323232 */
        for (int i = 0; i < 4; i++) {
            c[i] = u2f(color_ld32(d, off + 4 * i, 4));
        }
        break;
    default:                                            /* ARGB8888 */
        v = color_ld32(d, off, 4);
        c[0] = ((v >> 16) & 0xff) / 255.0f;
        c[1] = ((v >> 8) & 0xff) / 255.0f;
        c[2] = (v & 0xff) / 255.0f;
        c[3] = (v >> 24) / 255.0f;
        break;
    }
}

/* Write RGBA to a colour buffer pixel, channels per RB3D_COLOR_CHANNEL_MASK. */
static void color_write(R300Draw *d, uint64_t off, const float c[4],
                        uint32_t mask)
{
    uint32_t v, keep = 0;
    /* channel mask bits: B 0, G 1, R 2, A 3 */
    bool wr = mask & 4, wg = mask & 2, wb = mask & 1, wa = mask & 8;

    switch (d->color_fmt) {
    case 10: {
        uint32_t lo = color_ld32(d, off, 4), hi = color_ld32(d, off + 4, 4);

        if (wr) {
            lo = (lo & 0xffff0000) | float_to_half(c[0]);
        }
        if (wg) {
            lo = (lo & 0xffff) | (uint32_t)float_to_half(c[1]) << 16;
        }
        if (wb) {
            hi = (hi & 0xffff0000) | float_to_half(c[2]);
        }
        if (wa) {
            hi = (hi & 0xffff) | (uint32_t)float_to_half(c[3]) << 16;
        }
        color_st32(d, off, 4, lo);
        color_st32(d, off + 4, 4, hi);
        return;
    }
    case 7: {
        bool w[4] = { wr, wg, wb, wa };

        for (int i = 0; i < 4; i++) {
            if (w[i]) {
                union { float f; uint32_t u; } cv = { .f = c[i] };

                color_st32(d, off + 4 * i, 4, cv.u);
            }
        }
        return;
    }
    case 3:
        v = qz(c[3], 1) << 15 | qz(c[0], 31) << 10 | qz(c[1], 31) << 5 |
            qz(c[2], 31);
        keep = (wr ? 0 : 0x7c00) | (wg ? 0 : 0x3e0) | (wb ? 0 : 0x1f) |
               (wa ? 0 : 0x8000);
        break;
    case 4:
        v = qz(c[0], 31) << 11 | qz(c[1], 63) << 5 | qz(c[2], 31);
        keep = (wr ? 0 : 0xf800) | (wg ? 0 : 0x7e0) | (wb ? 0 : 0x1f);
        break;
    case 15:
        v = qz(c[3], 15) << 12 | qz(c[0], 15) << 8 | qz(c[1], 15) << 4 |
            qz(c[2], 15);
        keep = (wr ? 0 : 0xf00) | (wg ? 0 : 0xf0) | (wb ? 0 : 0xf) |
               (wa ? 0 : 0xf000);
        break;
    case 9:
        v = qz(c[0], 255);
        keep = wr ? 0 : 0xff;
        break;
    case 13:
        v = qz(c[0], 255) | qz(c[1], 255) << 8;
        keep = (wr ? 0 : 0xff) | (wg ? 0 : 0xff00);
        break;
    case 5:
        v = qz(c[3], 3) << 30 | qz(c[0], 1023) << 20 | qz(c[1], 1023) << 10 |
            qz(c[2], 1023);
        keep = (wr ? 0 : 0x3ff00000) | (wg ? 0 : 0xffc00) |
               (wb ? 0 : 0x3ff) | (wa ? 0 : 0xc0000000);
        break;
    default:
        v = qz(c[3], 255) << 24 | qz(c[0], 255) << 16 | qz(c[1], 255) << 8 |
            qz(c[2], 255);
        keep = (wr ? 0 : 0xff0000) | (wg ? 0 : 0xff00) | (wb ? 0 : 0xff) |
               (wa ? 0 : 0xff000000);
        break;
    }
    if (keep) {
        v = (v & ~keep) | (color_ld32(d, off, d->color_bpp) & keep);
    }
    color_st32(d, off, d->color_bpp, v);
}

static void blend_factor(R300Draw *d, int f, const float src[4],
                         const float dst[4], float out[4])
{
    float k[4] = {
        ((d->blend_color >> 16) & 0xff) / 255.0f,
        ((d->blend_color >> 8) & 0xff) / 255.0f,
        (d->blend_color & 0xff) / 255.0f,
        (d->blend_color >> 24) / 255.0f,
    };

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
    case 43: memcpy(out, k, 16); return;
    case 44: for (int i = 0; i < 4; i++) { out[i] = 1 - k[i]; } return;
    case 45: out[0] = out[1] = out[2] = out[3] = k[3]; return;
    case 46: out[0] = out[1] = out[2] = out[3] = 1 - k[3]; return;
    default: out[0] = out[1] = out[2] = out[3] = 1; return;
    }
}

static float combine(int fcn, float s, float sf, float dd, float df)
{
    switch (fcn) {
    case 2: case 3: return s * sf - dd * df;
    case 4: return MIN(s, dd);
    case 5: return MAX(s, dd);
    case 6: case 7: return dd * df - s * sf;
    default: return s * sf + dd * df;
    }
}

/* R300 compare functions as a mask: bit 0 less, 1 equal, 2 greater. */
static inline bool fg_compare(int func, float a, float ref)
{
    return ((func & 1) && a < ref) || ((func & 2) && a == ref) ||
           ((func & 4) && a > ref);
}

/* ZB functions, GL order: never, less, lequal, equal, gequal, ... */
static inline bool zb_compare(int func, uint32_t a, uint32_t b)
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

/*
 * Depth and stencil for one pixel; false if it is discarded. 24-bit depth
 * is in bits 31:8 and stencil in 7:0.
 */
static bool depth_stencil(R300Draw *d, int x, int y, float z, bool front)
{
    ATIVGAState *s = d->s;
    uint64_t off = d->depth_off + ((uint64_t)y * d->depth_pitch + x) *
                   d->depth_bpp;
    uint32_t raw, zcur, zval, sten = 0;
    int shift = front ? 0 : 12;
    int sfunc = (d->zs_cntl >> (3 + shift)) & 7;
    int sfail = (d->zs_cntl >> (6 + shift)) & 7;
    int zpass = (d->zs_cntl >> (9 + shift)) & 7;
    int zfail = (d->zs_cntl >> (12 + shift)) & 7;
    bool zok = true;

    if (!(d->zb_cntl & (1u << 4))) {            /* STENCIL_FRONT_BACK off */
        sfunc = (d->zs_cntl >> 3) & 7;
        sfail = (d->zs_cntl >> 6) & 7;
        zpass = (d->zs_cntl >> 9) & 7;
        zfail = (d->zs_cntl >> 12) & 7;
    }
    if (off + d->depth_bpp > s->vga.vram_size) {
        return true;
    }
    z = clamp01(z);
    if (d->depth_bpp == 2) {
        raw = ati_vram_ld(s, off, 2);
        zcur = raw;
        zval = (uint32_t)(z * 65535.0f + 0.5f);
    } else {
        raw = ati_vram_ld(s, off, 4);
        zcur = raw >> 8;
        sten = raw & 0xff;
        zval = (uint32_t)(z * 16777215.0f + 0.5f);
    }
    if (d->z_on) {
        zok = zb_compare(d->zs_cntl & 7, zval, zcur);
    }
    if (d->s_on && d->depth_bpp == 4) {
        uint32_t ref = d->stencil_ref & d->stencil_mask;
        uint32_t val = sten & d->stencil_mask;
        int op;
        bool pass = zb_compare(sfunc, ref, val);

        op = !pass ? sfail : zok ? zpass : zfail;
        uint32_t ns = stencil_op(op, sten, d->stencil_ref);

        sten = (sten & ~d->stencil_wmask) | (ns & d->stencil_wmask);
        if (!pass || !zok) {
            ati_vram_st(s, off, 4, (zcur << 8) | sten);
            return false;
        }
        if (d->z_on && (d->zb_cntl & 4)) {
            zcur = zval;
        }
        ati_vram_st(s, off, 4, (zcur << 8) | sten);
        return true;
    }
    if (!zok) {
        return false;
    }
    if (d->z_on && (d->zb_cntl & 4)) {
        if (d->depth_bpp == 2) {
            ati_vram_st(s, off, 2, zval);
        } else {
            ati_vram_st(s, off, 4, (zval << 8) | sten);
        }
    }
    return true;
}

/* Alpha test, blend and write one shaded pixel. */
static void pixel_out(R300Draw *d, int x, int y, const float src[4],
                      float fog)
{
    uint64_t off = color_off(d, x, y);
    float c[4];

    if (d->alpha_func & (1u << 11)) {
        float ref = (d->alpha_func & 0xff) / 255.0f;

        if (!fg_compare((d->alpha_func >> 8) & 7, clamp01(src[3]), ref)) {
            return;
        }
    }
    if (!d->color_stage && d->color_off + off + d->color_bpp >
        d->s->vga.vram_size) {
        return;
    }
    if (!d->cmask) {
        return;
    }
    memcpy(c, src, sizeof(c));
    if (d->fog_on) {
        /* FG: the colour towards FG_FOG_COLOR by the GA-scaled amount */
        float a = clamp01(fog * d->fog_scale + d->fog_offset);

        for (int i = 0; i < 3; i++) {
            c[i] = c[i] * (1 - a) + d->fog_color[i] * a;
        }
    }
    if (d->cblend & 1) {
        float dst[4], sf[4], df[4], asf[4], adf[4];
        uint32_t ab = (d->cblend & 2) ? d->ablend : d->cblend;
        int fcn = (d->cblend >> 12) & 7, afcn = (ab >> 12) & 7;
        bool clamp = d->color_fmt != 10 && d->color_fmt != 7;

        if (clamp) {
            for (int i = 0; i < 4; i++) {
                c[i] = clamp01(c[i]);
            }
        }
        color_read(d, off, dst);
        blend_factor(d, (d->cblend >> 16) & 0x3f, c, dst, sf);
        blend_factor(d, (d->cblend >> 24) & 0x3f, c, dst, df);
        blend_factor(d, (ab >> 16) & 0x3f, c, dst, asf);
        blend_factor(d, (ab >> 24) & 0x3f, c, dst, adf);
        float res[4];

        for (int i = 0; i < 3; i++) {
            res[i] = combine(fcn, c[i], sf[i], dst[i], df[i]);
        }
        res[3] = combine(afcn, c[3], asf[3], dst[3], adf[3]);
        memcpy(c, res, sizeof(c));
    }
    if ((d->rop & 4) && d->color_bpp <= 4 && d->color_fmt != 10 &&
        d->color_fmt != 7) {
        /* write the colour, then combine it with what was there */
        uint32_t old = color_ld32(d, off, d->color_bpp);

        color_write(d, off, c, 0xf);
        uint32_t src = color_ld32(d, off, d->color_bpp), out;

        switch ((d->rop >> 8) & 0xf) {
        case 0:  out = 0; break;
        case 1:  out = src & old; break;
        case 2:  out = src & ~old; break;
        case 3:  out = src; break;
        case 4:  out = ~src & old; break;
        case 5:  out = old; break;
        case 6:  out = src ^ old; break;
        case 7:  out = src | old; break;
        case 8:  out = ~(src | old); break;
        case 9:  out = ~(src ^ old); break;
        case 10: out = ~old; break;
        case 11: out = src | ~old; break;
        case 12: out = ~src; break;
        case 13: out = ~src | old; break;
        case 14: out = ~(src & old); break;
        default: out = ~0u; break;
        }
        if (d->color_bpp < 4) {
            out &= (1u << (8 * d->color_bpp)) - 1;
        }
        if (d->cmask != 0xf && d->color_bpp == 4 && d->color_fmt == 6) {
            uint32_t m = (d->cmask & 1 ? 0xff : 0) |
                         (d->cmask & 2 ? 0xff00 : 0) |
                         (d->cmask & 4 ? 0xff0000 : 0) |
                         (d->cmask & 8 ? 0xff000000 : 0);
            out = (out & m) | (old & ~m);
        }
        color_st32(d, off, d->color_bpp, out);
    } else {
        color_write(d, off, c, d->cmask);
    }
    stats.written++;
}

/* ---- rasterisation ------------------------------------------------------- */

/* Interpolate vertex data with perspective-correct weights b[3]. */
static void rs_load(R300Draw *d, const R300Vertex *const v[3],
                    const float b[3],
                    const R300Vertex *flat, float (*t)[4], float *fog)
{
    float pw = b[0] * v[0]->pos[3] + b[1] * v[1]->pos[3] +
               b[2] * v[2]->pos[3];
    float w[3];
    bool smooth = (d->color_ctl & 3) != 1;

    for (int k = 0; k < 3; k++) {
        w[k] = pw != 0 ? b[k] * v[k]->pos[3] / pw : b[k];
    }
    if (d->fog_on) {
        /* GB_SELECT FOG_SELECT: colour n alpha, 1/(1/w), or z */
        switch (d->fog_sel) {
        case 0: case 1: case 2: case 3:
            *fog = w[0] * v[0]->col[d->fog_sel][3] +
                   w[1] * v[1]->col[d->fog_sel][3] +
                   w[2] * v[2]->col[d->fog_sel][3];
            break;
        case 4:
            *fog = pw != 0 ? 1.0f / pw : 0;
            break;
        default:
            *fog = b[0] * v[0]->pos[2] + b[1] * v[1]->pos[2] +
                   b[2] * v[2]->pos[2];
            break;
        }
    }
    for (int i = 0; i < d->rs_count; i++) {
        uint32_t inst = d->rs_inst[i];

        if (inst & (1u << 3)) {                         /* TEX_CN_WRITE */
            uint32_t ip = d->rs_ip[inst & 7];
            int ptr = ip & 0x3f, dst = (inst >> 6) & 0x1f;

            for (int c = 0; c < 4; c++) {
                int sel = (ip >> (13 + 3 * c)) & 7;
                float val;

                if (sel < 4) {
                    int k = (ptr + sel) & 31;

                    val = w[0] * v[0]->tc[k] + w[1] * v[1]->tc[k] +
                          w[2] * v[2]->tc[k];
                } else {
                    val = sel == 5 ? 1.0f : 0.0f;
                }
                t[dst][c] = val;
            }
        }
        if (inst & (3u << 14)) {                        /* COL_CN_WRITE */
            uint32_t ip = d->rs_ip[(inst >> 11) & 7];
            int ptr = (ip >> 6) & 3, fmt = (ip >> 9) & 0xf;
            int dst = (inst >> 17) & 0x1f;
            float c[4];

            for (int ch = 0; ch < 4; ch++) {
                c[ch] = smooth ? w[0] * v[0]->col[ptr][ch] +
                                 w[1] * v[1]->col[ptr][ch] +
                                 w[2] * v[2]->col[ptr][ch]
                               : flat->col[ptr][ch];
            }
            switch (fmt) {
            case 1: c[3] = 0; break;                    /* RGB0 */
            case 2: c[3] = 1; break;                    /* RGB1 */
            case 4: c[0] = c[1] = c[2] = 0; break;      /* 000A */
            case 5: c[0] = c[1] = c[2] = c[3] = 0; break;
            case 6: c[0] = c[1] = c[2] = 0; c[3] = 1; break;
            case 8: c[0] = c[1] = c[2] = 1; break;      /* 111A */
            case 9: c[0] = c[1] = c[2] = 1; c[3] = 0; break;
            case 10: c[0] = c[1] = c[2] = c[3] = 1; break;
            default: break;
            }
            memcpy(t[dst], c, sizeof(c));
        }
    }
}

/* PPCOSX_R300_WATCH=x,y: log every shaded write to that pixel. */
static int watch_x = -2, watch_y;
static uint32_t watch_color = ~0u;

static void watch_pixel(R300Draw *d, R300Quad *q, int p, int x, int y)
{
    ATIVGAState *s = d->s;
    R300Tex *t = &d->tex[0];

    if (watch_x == -2) {
        const char *w = getenv("PPCOSX_R300_WATCH");

        watch_x = -1;
        if (w && sscanf(w, "%d,%d,%x", &watch_x, &watch_y,
                        &watch_color) < 2) {
            watch_x = -1;
        }
    }
    if (x != watch_x || y != watch_y ||
        (watch_color != ~0u &&
         (reg(s, RB3D_COLOROFFSET0) & ~0xfu) != watch_color)) {
        return;
    }
    fprintf(stderr, "r300 watch: draw %" PRIu64 " color %08x tex0 %d off %"
            PRIx64 " %dx%d pitch %u fmt0 %08x fmt1 %08x | t0 %.4f %.4f %.4f "
            "%.4f t1 %.4f %.4f %.4f %.4f | out %.3f %.3f %.3f %.3f | "
            "us %08x/%08x vte %08x bypass %d gb %08x\n", stats.draws,
            reg(s, RB3D_COLOROFFSET0), t->enabled, t->vram_off, t->w, t->h,
            t->lvl_pitch[0], reg(s, TX_FORMAT0_0), reg(s, TX_FORMAT1_0),
            q->t[p][0][0], q->t[p][0][1], q->t[p][0][2], q->t[p][0][3],
            q->t[p][1][0], q->t[p][1][1], q->t[p][1][2], q->t[p][1][3],
            q->out[p][0], q->out[p][1], q->out[p][2], q->out[p][3],
            reg(s, US_CONFIG), reg(s, US_CODE_OFFSET), d->vte, d->bypass,
            d->gb_enable);
}

static void shade_quad(R300Draw *d, R300Quad *q, int qx, int qy,
                       bool front)
{
    static int traced;
    bool trace = stats_on() && traced < 400 && q->live[0] &&
                 stats.quads % 4096 == 0;
    float t0[4], t1[4];

    if (trace) {
        memcpy(t0, q->t[0][0], sizeof(t0));
        memcpy(t1, q->t[0][1], sizeof(t1));
    }
    stats.quads++;
    if (watch_x >= 0 && (watch_x >> 1) == (qx >> 1) &&
        (watch_y >> 1) == (qy >> 1)) {
        /* keep the inputs: copy them where the trace prints from */
        int p = (watch_x & 1) | ((watch_y & 1) << 1);

        fprintf(stderr, "r300 watch in: t0 %.4f %.4f %.4f %.4f t1 %.4f %.4f "
                "%.4f %.4f\n", q->t[p][0][0], q->t[p][0][1], q->t[p][0][2],
                q->t[p][0][3], q->t[p][1][0], q->t[p][1][1], q->t[p][1][2],
                q->t[p][1][3]);
    }
    program_run(d, q);
    if (trace) {
        traced++;
        fprintf(stderr, "r300: draw %" PRIu64 " tex %d %dx%d fmt %d vram %d "
                "off %" PRIx64 " | ", stats.draws, d->tex[0].enabled,
                d->tex[0].w, d->tex[0].h, d->tex[0].fmt, d->tex[0].vram,
                d->tex[0].vram_off);
        fprintf(stderr, "px %d,%d in t0 %.3f %.3f %.3f %.3f t1 %.3f "
                "%.3f %.3f %.3f -> t1 %.3f %.3f %.3f %.3f out %.3f %.3f %.3f "
                "%.3f\n", qx, qy, t0[0], t0[1], t0[2], t0[3], t1[0], t1[1],
                t1[2], t1[3], q->t[0][1][0], q->t[0][1][1], q->t[0][1][2],
                q->t[0][1][3], q->out[0][0], q->out[0][1], q->out[0][2],
                q->out[0][3]);
    }
    for (int p = 0; p < 4; p++) {
        int x = qx + (p & 1), y = qy + (p >> 1);

        if (!q->live[p] || q->killed[p]) {
            continue;
        }
        stats.pixels++;
        if (watch_x != -1) {
            watch_pixel(d, q, p, x, y);
        }
        if ((d->z_on || d->s_on) &&
            !depth_stencil(d, x, y, q->depth[p], front)) {
            continue;
        }
        pixel_out(d, x, y, q->out[p], q->fog[p]);
    }
}

/*
 * For the host GPU: one vertex as window position, 1/w, the rasteriser's
 * outputs (in RS instruction order, a texture coordinate write then a
 * colour write per instruction) and the fog coordinate, which the GPU
 * then interpolates as rs_load does.
 */
static void gpu_vertex(R300Draw *d, const R300Vertex *v,
                       const R300Vertex *flat, float *out)
{
    bool smooth = (d->color_ctl & 3) != 1;
    int k = 4;

    out[0] = v->pos[0];
    out[1] = v->pos[1];
    out[2] = v->pos[2];
    out[3] = v->pos[3];
    for (int i = 0; i < d->rs_count; i++) {
        uint32_t inst = d->rs_inst[i];

        if (inst & (1u << 3)) {
            uint32_t ip = d->rs_ip[inst & 7];
            int ptr = ip & 0x3f;

            for (int c = 0; c < 4; c++) {
                int sel = (ip >> (13 + 3 * c)) & 7;

                out[k++] = sel < 4 ? v->tc[(ptr + sel) & 31] :
                           sel == 5 ? 1.0f : 0.0f;
            }
        }
        if (inst & (3u << 14)) {
            uint32_t ip = d->rs_ip[(inst >> 11) & 7];
            int ptr = (ip >> 6) & 3, fmt = (ip >> 9) & 0xf;
            const R300Vertex *cv = smooth ? v : flat;
            float c[4];

            memcpy(c, cv->col[ptr], sizeof(c));
            switch (fmt) {
            case 1: c[3] = 0; break;
            case 2: c[3] = 1; break;
            case 4: c[0] = c[1] = c[2] = 0; break;
            case 5: c[0] = c[1] = c[2] = c[3] = 0; break;
            case 6: c[0] = c[1] = c[2] = 0; c[3] = 1; break;
            case 8: c[0] = c[1] = c[2] = 1; break;
            case 9: c[0] = c[1] = c[2] = 1; c[3] = 0; break;
            case 10: c[0] = c[1] = c[2] = c[3] = 1; break;
            default: break;
            }
            memcpy(out + k, c, sizeof(c));
            k += 4;
        }
    }
    switch (d->fog_sel) {
    case 0: case 1: case 2: case 3:
        out[k] = v->col[d->fog_sel][3];
        break;
    case 4:
        out[k] = v->pos[3] != 0 ? 1.0f / v->pos[3] : 0;
        break;
    default:
        out[k] = v->pos[2];
        break;
    }
}

/* A triangle set up for rasterisation (positive area, top-left rule). */
typedef struct R300Tri {
    R300Draw *d;
    const R300Vertex *v[3];
    const R300Vertex *flat;
    bool front;
    bool tl[3];
    float inv;
    int x0, y0, x1, y1;
} R300Tri;

/* Shade quad rows band, band + bands, ... of the triangle. */
static void tri_rows(const R300Tri *tri, int band, int bands)
{
    R300Draw *d = tri->d;
    const R300Vertex *v0 = tri->v[0], *v1 = tri->v[1], *v2 = tri->v[2];

    for (int qy = tri->y0 + 2 * band; qy <= tri->y1; qy += 2 * bands) {
        for (int qx = tri->x0; qx <= tri->x1; qx += 2) {
            R300Quad q;
            bool any = false;

            for (int p = 0; p < 4; p++) {
                int x = qx + (p & 1), y = qy + (p >> 1);
                float px = x + 0.5f, py = y + 0.5f;
                float e0 = (v2->pos[0] - v1->pos[0]) * (py - v1->pos[1]) -
                           (v2->pos[1] - v1->pos[1]) * (px - v1->pos[0]);
                float e1 = (v0->pos[0] - v2->pos[0]) * (py - v2->pos[1]) -
                           (v0->pos[1] - v2->pos[1]) * (px - v2->pos[0]);
                float e2 = (v1->pos[0] - v0->pos[0]) * (py - v0->pos[1]) -
                           (v1->pos[1] - v0->pos[1]) * (px - v0->pos[0]);
                float b[3] = { e0 * tri->inv, e1 * tri->inv, e2 * tri->inv };

                q.live[p] = b[0] >= 0 && b[1] >= 0 && b[2] >= 0 &&
                            !(b[0] == 0 && !tri->tl[0]) &&
                            !(b[1] == 0 && !tri->tl[1]) &&
                            !(b[2] == 0 && !tri->tl[2]) &&
                            x >= d->clip_x0 && x <= d->clip_x1 &&
                            y >= d->clip_y0 && y <= d->clip_y1;
                any |= q.live[p];
                q.killed[p] = false;
                memset(q.t[p], 0, sizeof(q.t[p][0]) * d->ntemps);
                q.out[p][0] = q.out[p][1] = q.out[p][2] = 0;
                q.out[p][3] = 1;
                q.depth[p] = b[0] * v0->pos[2] + b[1] * v1->pos[2] +
                             b[2] * v2->pos[2];
                q.fog[p] = 0;
                rs_load(d, tri->v, b, tri->flat, q.t[p], &q.fog[p]);
            }
            if (any) {
                shade_quad(d, &q, qx, qy, tri->front);
            }
        }
    }
}

/*
 * Host threads for big triangles. Each takes interleaved bands of quad
 * rows; rows are disjoint, and a triangle is finished before the next
 * starts, so the result is the same as on one thread. PPCOSX_R300_THREADS
 * sets the number of threads (1: none).
 */
#define R300_PARALLEL_PIXELS 16384

static struct {
    bool started;
    int workers;
    QemuThread *threads;
    QemuMutex lock;
    QemuCond go, done;
    uint64_t gen;
    int pending;
    const R300Tri *tri;
} pool;

static void *pool_worker(void *arg)
{
    int band = (intptr_t)arg;
    uint64_t seen = 0;

    for (;;) {
        const R300Tri *tri;

        qemu_mutex_lock(&pool.lock);
        while (pool.gen == seen) {
            qemu_cond_wait(&pool.go, &pool.lock);
        }
        seen = pool.gen;
        tri = pool.tri;
        qemu_mutex_unlock(&pool.lock);

        tri_rows(tri, band, pool.workers + 1);

        qemu_mutex_lock(&pool.lock);
        if (--pool.pending == 0) {
            qemu_cond_signal(&pool.done);
        }
        qemu_mutex_unlock(&pool.lock);
    }
    return NULL;
}

static bool pool_start(void)
{
    if (!pool.started) {
        const char *e = getenv("PPCOSX_R300_THREADS");
        int n = e ? atoi(e) : MIN((int)g_get_num_processors() - 1, 8);

        pool.started = true;
        pool.workers = MAX(n, 1) - 1;
        if (pool.workers) {
            qemu_mutex_init(&pool.lock);
            qemu_cond_init(&pool.go);
            qemu_cond_init(&pool.done);
            pool.threads = g_new0(QemuThread, pool.workers);
            for (int i = 0; i < pool.workers; i++) {
                qemu_thread_create(&pool.threads[i], "ati-r300",
                                   pool_worker, (void *)(intptr_t)(i + 1),
                                   QEMU_THREAD_DETACHED);
            }
        }
    }
    return pool.workers > 0;
}

static void pool_run(const R300Tri *tri)
{
    qemu_mutex_lock(&pool.lock);
    pool.tri = tri;
    pool.pending = pool.workers;
    pool.gen++;
    qemu_cond_broadcast(&pool.go);
    qemu_mutex_unlock(&pool.lock);

    tri_rows(tri, 0, pool.workers + 1);

    qemu_mutex_lock(&pool.lock);
    while (pool.pending) {
        qemu_cond_wait(&pool.done, &pool.lock);
    }
    qemu_mutex_unlock(&pool.lock);
}

static void draw_triangle(R300Draw *d, const R300Vertex *v0,
                          const R300Vertex *v1, const R300Vertex *v2,
                          const R300Vertex *flat)
{
    float area = (v1->pos[0] - v0->pos[0]) * (v2->pos[1] - v0->pos[1]) -
                 (v2->pos[0] - v0->pos[0]) * (v1->pos[1] - v0->pos[1]);
    /* window coordinates run downwards, so a negative area is CCW */
    bool ccw = area < 0;
    bool front = (d->cull & 4) ? !ccw : ccw;

    stats.tris++;
    if (area == 0 || !isfinite(area) ||
        ((d->cull & 1) && front) || ((d->cull & 2) && !front)) {
        stats.culled++;
        return;
    }
    if (d->gpu) {
        float a[R300GL_VERTEX_FLOATS(R300GL_MAX_VARY)];
        float b[R300GL_VERTEX_FLOATS(R300GL_MAX_VARY)];
        float c[R300GL_VERTEX_FLOATS(R300GL_MAX_VARY)];

        gpu_vertex(d, v0, flat, a);
        gpu_vertex(d, v1, flat, b);
        gpu_vertex(d, v2, flat, c);
        ati_r300_gl_triangle(d->s, a, b, c);
        return;
    }
    if (area < 0) {
        const R300Vertex *t = v1;

        v1 = v2;
        v2 = t;
        area = -area;
    }

    R300Tri tri = { .d = d, .v = { v0, v1, v2 }, .flat = flat,
                    .front = front, .inv = 1.0f / area };
    float minx = MIN(v0->pos[0], MIN(v1->pos[0], v2->pos[0]));
    float maxx = MAX(v0->pos[0], MAX(v1->pos[0], v2->pos[0]));
    float miny = MIN(v0->pos[1], MIN(v1->pos[1], v2->pos[1]));
    float maxy = MAX(v0->pos[1], MAX(v1->pos[1], v2->pos[1]));

    tri.x0 = MAX((int)floorf(minx), d->clip_x0) & ~1;
    tri.x1 = MIN((int)ceilf(maxx), d->clip_x1);
    tri.y0 = MAX((int)floorf(miny), d->clip_y0) & ~1;
    tri.y1 = MIN((int)ceilf(maxy), d->clip_y1);
#define TOP_LEFT(a, b) (((b)->pos[1] == (a)->pos[1] && \
                         (b)->pos[0] > (a)->pos[0]) || \
                        (b)->pos[1] < (a)->pos[1])
    tri.tl[0] = TOP_LEFT(v1, v2);
    tri.tl[1] = TOP_LEFT(v2, v0);
    tri.tl[2] = TOP_LEFT(v0, v1);
#undef TOP_LEFT

    if (tri.x0 > tri.x1 || tri.y0 > tri.y1) {
        return;
    }
    /* big triangles are shaded by several host threads, in row bands */
    if ((int64_t)(tri.x1 - tri.x0 + 1) * (tri.y1 - tri.y0 + 1) >=
        R300_PARALLEL_PIXELS && pool_start()) {
        pool_run(&tri);
    } else {
        tri_rows(&tri, 0, 1);
    }
    d->dirty_y0 = MIN(d->dirty_y0, tri.y0);
    d->dirty_y1 = MAX(d->dirty_y1, MIN(tri.y1 + 1, d->clip_y1));
}

/*
 * Fast path for the compositor's window blits: a point sprite whose
 * stuffed texture coordinates map texels one to one onto pixels, a
 * fragment program that only passes the texel through (texture unit 0 into
 * temporary 0, then out = t0), an ARGB8888 texture and colour buffer in
 * video memory and nothing else on (blending, logic ops, masks, depth,
 * stencil, fog, alpha test): rows are copied. Anything else is shaded.
 */
static bool blit_point(R300Draw *d, const R300Vertex *v)
{
    ATIVGAState *s = d->s;
    R300Tex *t = &d->tex[0];
    const R300Node *n = &d->nodes[0];
    uint32_t f1 = reg(s, TX_FORMAT1_0);
    float hw = d->point_w, hh = d->point_h;
    float sx, sy, tx0, ty0;
    int left, top, w, h;
    bool force_alpha;

    if (d->nnodes != 1 || n->tex_start != n->tex_end ||
        n->alu_start != n->alu_end ||
        d->tex_inst[n->tex_start & 31] != 0x00008000 ||
        d->alu[n->alu_start & 63].rgb_addr != 0x1c000000 ||
        d->alu[n->alu_start & 63].rgb_inst != 0x00050a80 ||
        d->alu[n->alu_start & 63].alpha_addr != 0x01000000 ||
        d->alu[n->alu_start & 63].alpha_inst != 0x00040889 ||
        d->rs_count != 1 || d->rs_inst[0] != 0x00000008 ||
        !(d->gb_enable & 1) || ((d->gb_enable >> 16) & 3) != 1 ||
        d->tc_base[0] != 0) {
        return false;
    }
    if (!t->enabled || !t->vram || t->fmt != 0xc || t->levels != 1 ||
        d->color_stage || d->color_fmt != 6 || d->cmask != 0xf ||
        (d->cblend & 1) || (d->rop & 4) || d->z_on || d->s_on ||
        d->fog_on || (d->alpha_func & (1u << 11))) {
        return false;
    }
    /* RGB from Z, Y, X; alpha from W, or forced to one */
    if (((f1 >> 12) & 7) != 2 || ((f1 >> 15) & 7) != 1 ||
        ((f1 >> 18) & 7) != 0) {
        return false;
    }
    if (((f1 >> 9) & 7) == 3) {
        force_alpha = false;
    } else if (((f1 >> 9) & 7) == 5) {
        force_alpha = true;
    } else {
        return false;
    }
    left = (int)(v->pos[0] - hw);
    top = (int)(v->pos[1] - hh);
    w = (int)(2 * hw);
    h = (int)(2 * hh);
    if (left != v->pos[0] - hw || top != v->pos[1] - hh || w != 2 * hw ||
        h != 2 * hh || w <= 0 || h <= 0) {
        return false;
    }
    /* s from S0 at the left, t from T1 at the top (see draw_point) */
    sx = (d->point_st[2] - d->point_st[0]) * t->w;
    sy = (d->point_st[1] - d->point_st[3]) * t->h;
    tx0 = d->point_st[0] * t->w;
    ty0 = d->point_st[3] * t->h;
    if (sx != w || sy != h || tx0 != floorf(tx0) || ty0 != floorf(ty0) ||
        tx0 < 0 || ty0 < 0 || tx0 + w > t->w || ty0 + h > t->h) {
        return false;
    }
    int x0 = MAX(left, d->clip_x0), x1 = MIN(left + w - 1, d->clip_x1);
    int y0 = MAX(top, d->clip_y0), y1 = MIN(top + h - 1, d->clip_y1);

    if (x0 > x1 || y0 > y1) {
        return true;
    }
    uint8_t *row = g_malloc((x1 - x0 + 1) * 4);

    for (int y = y0; y <= y1; y++) {
        uint64_t src = t->vram_off + (uint64_t)((int)ty0 + y - top) *
                       t->lvl_pitch[0] + (uint64_t)((int)tx0 + x0 - left) * 4;
        uint64_t dst = d->color_off + ((uint64_t)y * d->color_pitch + x0) * 4;
        uint32_t n4 = (x1 - x0 + 1) * 4;

        if (src + n4 > s->vga.vram_size || dst + n4 > s->vga.vram_size) {
            break;
        }
        ati_vram_read(s, src, row, n4);
        if (force_alpha) {
            for (uint32_t i = 3; i < n4; i += 4) {
                row[i] = 0xff;
            }
        }
        ati_vram_write(s, dst, row, n4);
    }
    g_free(row);
    d->dirty_y0 = MIN(d->dirty_y0, y0);
    d->dirty_y1 = MAX(d->dirty_y1, y1);
    stats.written += (uint64_t)(x1 - x0 + 1) * (y1 - y0 + 1);
    return true;
}

/*
 * Points as screen-aligned rectangles. With GB_ENABLE's POINT_STUFF_ENABLE,
 * texture coordinate sets whose GB_TEXn_SOURCE is ST or STR are replaced
 * by GA_POINT_S0..S1 left to right and GA_POINT_T0..T1 bottom to top;
 * Apple's driver blits sub-rectangles of windows this way.
 */
static void draw_point(R300Draw *d, const R300Vertex *v)
{
    float hw = MAX(d->point_w, 0.5f), hh = MAX(d->point_h, 0.5f);
    R300Vertex c[4];

    if (!d->gpu && blit_point(d, v)) {
        return;
    }

    for (int i = 0; i < 4; i++) {
        c[i] = *v;
        c[i].pos[0] = v->pos[0] + ((i & 1) ? hw : -hw);
        c[i].pos[1] = v->pos[1] + ((i & 2) ? hh : -hh);
        if (!(d->gb_enable & 1)) {
            continue;
        }
        for (int t = 0; t < 8; t++) {
            int src = (d->gb_enable >> (16 + 2 * t)) & 3;
            int b = d->tc_base[t];

            if (src == 1 || src == 2) {
                c[i].tc[b & 31] = (i & 1) ? d->point_st[2] : d->point_st[0];
                c[i].tc[(b + 1) & 31] = (i & 2) ? d->point_st[1]
                                                : d->point_st[3];
                if (src == 2) {
                    c[i].tc[(b + 2) & 31] = 0.0f;
                }
                c[i].tc[(b + 3) & 31] = 1.0f;
            }
        }
    }
    draw_triangle(d, &c[0], &c[1], &c[2], v);
    draw_triangle(d, &c[1], &c[3], &c[2], v);
}

/* Lines as thin quads one pixel wide. */
static void draw_line(R300Draw *d, const R300Vertex *a, const R300Vertex *b)
{
    float dx = b->pos[0] - a->pos[0], dy = b->pos[1] - a->pos[1];
    float len = sqrtf(dx * dx + dy * dy);
    R300Vertex c[4];

    if (len == 0) {
        return;
    }
    float nx = -dy / len * 0.5f, ny = dx / len * 0.5f;

    c[0] = *a;
    c[1] = *a;
    c[2] = *b;
    c[3] = *b;
    c[0].pos[0] += nx;
    c[0].pos[1] += ny;
    c[1].pos[0] -= nx;
    c[1].pos[1] -= ny;
    c[2].pos[0] += nx;
    c[2].pos[1] += ny;
    c[3].pos[0] -= nx;
    c[3].pos[1] -= ny;
    draw_triangle(d, &c[0], &c[1], &c[2], b);
    draw_triangle(d, &c[1], &c[3], &c[2], b);
}

/* ---- the vertex program -------------------------------------------------- */

typedef struct PVSState {
    float in[R300_PVS_IO][4];
    float out[R300_PVS_IO][4];
    float t[R300_PVS_TEMPS][4];
    float alt[R300_PVS_TEMPS][4];   /* alternate temporaries */
    int a0[4];
} PVSState;

static void pvs_src(R300Draw *d, PVSState *v, uint32_t src, float out[4])
{
    int type = src & 3, idx = (src >> 5) & 0xff;
    const float *r;
    float k[4];
    static const float zero[4];

    if (src & (1u << 4)) {                      /* relative addressing */
        idx += v->a0[(src >> 29) & 3];
    }
    switch (type) {
    case 0:                                     /* temporary */
        r = v->t[idx & (R300_PVS_TEMPS - 1)];
        break;
    case 3:                                     /* alternate temporary */
        r = v->alt[idx & (R300_PVS_TEMPS - 1)];
        break;
    case 1:                                     /* input */
        r = v->in[idx & (R300_PVS_IO - 1)];
        break;
    default: {                                  /* constant */
        int ci = d->const_base + idx;

        if (ci < 0 || ci >= 256) {
            r = zero;
            break;
        }
        for (int i = 0; i < 4; i++) {
            k[i] = u2f(d->s->pvs[(0x200 + ci) * 4 + i]);
        }
        r = k;
        break;
    }
    }
    for (int i = 0; i < 4; i++) {
        int sw = (src >> (13 + 3 * i)) & 7;
        float val = sw < 4 ? r[sw] : sw == 5 ? 1.0f : 0.0f;

        if (src & (1u << 3)) {                  /* ABS */
            val = fabsf(val);
        }
        if (src & (1u << (25 + i))) {
            val = -val;
        }
        out[i] = val;
    }
}

/* A scalar math op of the dual math path: x is a[0]. */
static void pvs_math(int op, const float a[4], const float b[4],
                     const float c[4], float r[4])
{
    float x = a[0], res;

    switch (op) {
    case 1: case 11: res = exp2f(x); break;
    case 2: case 12: case 18: res = x == 0 ? -INFINITY : log2f(fabsf(x)); break;
    case 3: res = expf(x); break;
    case 5: case 13: case 14: res = powf(x, c[0]); break;
    case 15: res = clamp01(powf(x, c[0])); break;
    case 6: case 7: case 19: res = 1.0f / x; break;
    case 8: case 9: case 20: res = 1.0f / sqrtf(fabsf(x)); break;
    case 10: res = x * b[0]; break;
    case 16: res = sinf(x); break;
    case 17: res = cosf(x); break;
    default: {
        static bool warned[64];

        if (!warned[op & 63]) {
            warned[op & 63] = true;
            qemu_log_mask(LOG_UNIMP, "ati: R300 PVS dual math op %d\n", op);
        }
        res = 0;
    }
    }
    r[0] = r[1] = r[2] = r[3] = res;
}

static void pvs_run(R300Draw *d, PVSState *v)
{
    const uint32_t *code = d->s->pvs;

    for (int pc = d->pvs_first; pc <= d->pvs_last && pc < 512; pc++) {
        uint32_t dst = code[pc * 4];
        float a[4], b[4], c[4], r[4] = { 0 };
        int op = dst & 0x3f;
        bool math = dst & (1u << 6), macro = dst & (1u << 7);

        float dual = 0;
        uint32_t dw = code[pc * 4 + 3];
        bool has_dual = (dst & (1u << 28)) && !math && !macro;

        pvs_src(d, v, code[pc * 4 + 1], a);
        pvs_src(d, v, code[pc * 4 + 2], b);
        if (has_dual) {
            /*
             * DUAL_MATH_OP: the third source word holds a scalar math op
             * done alongside: operand register (type 1:0, offset 12:5,
             * component 15:13), op 24:21, destination alternate temporary
             * 4:2, component 28:27.
             */
            int comp = (dw >> 13) & 7;
            float m[4], z[4] = { 0 };

            pvs_src(d, v, (dw & 0x1fe3) | comp << 13 | comp << 16 |
                    comp << 19 | comp << 22, m);
            pvs_math((dw >> 21) & 0xf, m, z, z, m);
            dual = m[0];
            memset(c, 0, sizeof(c));
        } else {
            pvs_src(d, v, dw, c);
        }
        if (macro) {
            for (int i = 0; i < 4; i++) {
                r[i] = op == 1 ? 2 * a[i] * b[i] + c[i] : a[i] * b[i] + c[i];
            }
        } else if (math) {
            float x = a[0], res = 0;

            switch (op) {
            case 1: {                           /* EXP_BASE2_DX */
                float fl = floorf(x);

                r[0] = exp2f(fl);
                r[1] = x - fl;
                r[2] = exp2f(x);
                r[3] = 1;
                goto write;
            }
            case 2: {                           /* LOG_BASE2_DX */
                float ax = fabsf(x);
                float lg = ax == 0 ? -INFINITY : log2f(ax);
                float fl = floorf(lg);

                r[0] = fl;
                r[1] = ax / exp2f(fl);
                r[2] = lg;
                r[3] = 1;
                goto write;
            }
            case 3: res = expf(x); break;       /* EXP_BASEE_FF */
            case 4: {                           /* LIGHT_COEFF_DX */
                /*
                 * As Apple's driver uses it: N.H in the first source,
                 * N.L in the second, the exponent in the third.
                 */
                float nh = a[0], nl = b[0], sp = c[0];

                sp = sp < -128 ? -128 : sp > 128 ? 128 : sp;
                r[0] = 1;
                r[1] = MAX(nl, 0);
                r[2] = nl > 0 ? powf(MAX(nh, 0), sp) : 0;
                r[3] = 1;
                goto write;
            }
            case 5: case 13: case 14:           /* POWER_FUNC */
                res = powf(x, c[0]);
                break;
            case 15:                            /* POWER_FUNC_FF_CLAMP_01 */
                res = clamp01(powf(x, c[0]));
                break;
            case 6: case 7: case 19:            /* RECIP */
                res = 1.0f / x;
                break;
            case 8: case 9: case 20:            /* RECIP_SQRT */
                res = 1.0f / sqrtf(fabsf(x));
                break;
            case 10:                            /* MULTIPLY */
                res = x * b[0];
                break;
            case 11:                            /* EXP_BASE2_FULL_DX */
                res = exp2f(x);
                break;
            case 12: case 18:                   /* LOG_BASE2_FULL_DX */
                res = x == 0 ? -INFINITY : log2f(fabsf(x));
                break;
            case 16: res = sinf(x); break;
            case 17: res = cosf(x); break;
            default: {
                static bool warned[64];

                if (!warned[op]) {
                    warned[op] = true;
                    qemu_log_mask(LOG_UNIMP, "ati: R300 PVS math op %d\n",
                                  op);
                }
            }
            }
            r[0] = r[1] = r[2] = r[3] = res;
        } else {
            switch (op) {
            case 0: break;                      /* NOP */
            case 1: {                           /* DOT_PRODUCT */
                float dp = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] +
                           a[3] * b[3];
                r[0] = r[1] = r[2] = r[3] = dp;
                break;
            }
            case 2:
                for (int i = 0; i < 4; i++) { r[i] = a[i] * b[i]; }
                break;
            case 3:
                for (int i = 0; i < 4; i++) { r[i] = a[i] + b[i]; }
                break;
            case 4:
                for (int i = 0; i < 4; i++) { r[i] = a[i] * b[i] + c[i]; }
                break;
            case 5:                             /* DISTANCE_VECTOR */
                r[0] = 1;
                r[1] = a[1] * b[1];
                r[2] = a[2];
                r[3] = b[3];
                break;
            case 6:
                for (int i = 0; i < 4; i++) { r[i] = a[i] - floorf(a[i]); }
                break;
            case 7:
                for (int i = 0; i < 4; i++) { r[i] = MAX(a[i], b[i]); }
                break;
            case 8:
                for (int i = 0; i < 4; i++) { r[i] = MIN(a[i], b[i]); }
                break;
            case 9:
                for (int i = 0; i < 4; i++) { r[i] = a[i] >= b[i]; }
                break;
            case 10:
                for (int i = 0; i < 4; i++) { r[i] = a[i] < b[i]; }
                break;
            case 11:                            /* MULTIPLYX2_ADD */
                for (int i = 0; i < 4; i++) { r[i] = 2 * a[i] * b[i] + c[i]; }
                break;
            case 12:                            /* MULTIPLY_CLAMP */
                for (int i = 0; i < 4; i++) { r[i] = clamp01(a[i] * b[i]); }
                break;
            case 13: case 14:                   /* FLT2FIX (ARL) */
                for (int i = 0; i < 4; i++) {
                    r[i] = op == 13 ? floorf(a[i]) : roundf(a[i]);
                }
                break;
            default: {
                static bool warned[64];

                if (!warned[op]) {
                    warned[op] = true;
                    qemu_log_mask(LOG_UNIMP, "ati: R300 PVS vector op %d\n",
                                  op);
                }
            }
            }
        }
write:
        if (has_dual) {
            v->alt[(dw >> 2) & 7][(dw >> 27) & 3] = dual;
        }
        {
            int type = (dst >> 8) & 0xf, idx = (dst >> 13) & 0x7f;

            for (int i = 0; i < 4; i++) {
                if (!(dst & (1u << (20 + i)))) {
                    continue;
                }
                switch (type) {
                case 0: case 4:
                    v->t[idx & (R300_PVS_TEMPS - 1)][i] = r[i];
                    break;
                case 1:
                    v->a0[i] = (int)floorf(r[i]);
                    break;
                case 2:
                    v->out[idx & (R300_PVS_IO - 1)][i] = r[i];
                    break;
                case 3:
                    v->out[idx & (R300_PVS_IO - 1)][i] = r[0];
                    break;
                default:
                    break;
                }
            }
        }
    }
}

/* Window position from the clip-space one: VTE and the viewport. */
static void vertex_window(R300Draw *d, R300Vertex *v)
{
    float x = v->clip[0], y = v->clip[1], z = v->clip[2], w = v->clip[3];
    float rw;

    /* VTE: W0_FMT means w is not yet 1/w; XY_FMT/Z_FMT: already divided */
    rw = (d->vte & (1u << 10)) ? (w != 0 ? 1.0f / w : 0) : w;
    if (!(d->vte & (1u << 8))) {
        x *= rw;
        y *= rw;
    }
    if (!(d->vte & (1u << 9))) {
        z *= rw;
    }
    if (d->vte & 1) {
        x *= d->vport[0];
    }
    if (d->vte & 2) {
        x += d->vport[1];
    }
    if (d->vte & 4) {
        y *= d->vport[2];
    }
    if (d->vte & 8) {
        y += d->vport[3];
    }
    if (d->vte & 0x10) {
        z *= d->vport[4];
    }
    if (d->vte & 0x20) {
        z += d->vport[5];
    }
    v->pos[0] = x;
    v->pos[1] = y;
    v->pos[2] = z;
    v->pos[3] = (d->vte & (1u << 10)) ? rw : 1.0f;
}

/* Vertex outputs, in VAP_OUTPUT_VTX_FMT order, into a raster vertex. */
static void vertex_finish(R300Draw *d, const float (*out)[4], R300Vertex *v)
{
    memset(v, 0, sizeof(*v));
    memcpy(v->clip, out[0], sizeof(v->clip));
    vertex_window(d, v);
    for (int c = 0; c < 4; c++) {
        if (d->col_slot[c] >= 0) {
            memcpy(v->col[c], out[d->col_slot[c]], 4 * sizeof(float));
        }
    }
    for (int t = 0; t < 8; t++) {
        if (d->tc_slot[t] >= 0) {
            for (int i = 0; i < d->tc_count[t]; i++) {
                v->tc[(d->tc_base[t] + i) & 31] = out[d->tc_slot[t]][i];
            }
        }
    }
    if (d->psize_slot >= 0) {
        v->psize = out[d->psize_slot][0];
    }
}

/* ---- vertex fetch -------------------------------------------------------- */

/* Element of one stream from `src` dwords; returns the dwords used. */
static int stream_element(const R300Stream *st, const uint32_t *src,
                          int avail, float out[4])
{
    float raw[4] = { 0, 0, 0, 1 };
    int n = 0, used;

    switch (st->type) {
    case 0: case 1: case 2: case 3:             /* FLOAT_1..4 */
        n = st->type + 1;
        used = n;
        for (int i = 0; i < n && i < avail; i++) {
            raw[i] = u2f(src[i]);
        }
        break;
    case 4: case 5: {                           /* BYTE, D3DCOLOR */
        uint32_t w = avail > 0 ? src[0] : 0;

        used = 1;
        for (int i = 0; i < 4; i++) {
            int b = (w >> (8 * i)) & 0xff;

            if (st->sign) {
                raw[i] = st->norm ? (int8_t)b / 127.0f : (int8_t)b;
            } else {
                raw[i] = st->norm ? b / 255.0f : b;
            }
        }
        if (st->type == 5) {                    /* BGRA in memory */
            float t = raw[0];

            raw[0] = raw[2];
            raw[2] = t;
        }
        break;
    }
    case 6: case 7: {                           /* SHORT_2, SHORT_4 */
        n = st->type == 6 ? 2 : 4;
        used = n / 2;
        for (int i = 0; i < n; i++) {
            uint32_t w = (i / 2) < avail ? src[i / 2] : 0;
            int v = (w >> (16 * (i & 1))) & 0xffff;

            if (st->sign) {
                raw[i] = st->norm ? (int16_t)v / 32767.0f : (int16_t)v;
            } else {
                raw[i] = st->norm ? v / 65535.0f : v;
            }
        }
        break;
    }
    default:
        used = 1;
        break;
    }
    /* PROG_STREAM_CNTL_EXT: SWIZZLE_SELECT and WRITE_ENA */
    for (int i = 0; i < 4; i++) {
        int sel = (st->ext >> (3 * i)) & 7;

        if (st->ext & (1u << (12 + i))) {
            out[i] = sel < 4 ? raw[sel] : sel == 5 ? 1.0f : 0.0f;
        }
    }
    return used + st->skip;
}

typedef struct R300Fetch {
    const uint32_t *imm;        /* immediate vertex data, or */
    uint32_t nimm;
    struct {
        uint32_t addr, stride, size;
    } aos[16];
    bool arrays;
} R300Fetch;

static bool fetch_vertex(R300Draw *d, R300Fetch *f, uint32_t index,
                         PVSState *pv)
{
    for (int i = 0; i < R300_PVS_IO; i++) {
        pv->in[i][0] = pv->in[i][1] = pv->in[i][2] = 0;
        pv->in[i][3] = 1;
    }
    if (!f->arrays) {
        uint32_t size = reg(d->s, VAP_VTX_SIZE) & 0x7f;
        const uint32_t *p;
        int avail;

        if (!size) {
            return false;
        }
        if ((uint64_t)(index + 1) * size > f->nimm) {
            return false;
        }
        p = f->imm + index * size;
        avail = size;
        for (int i = 0; i < d->nstreams; i++) {
            int used = stream_element(&d->streams[i], p, avail,
                                      pv->in[d->streams[i].dst & 15]);
            p += used;
            avail -= used;
        }
        return true;
    }
    for (int i = 0; i < d->nstreams; i++) {
        uint32_t buf[16];
        uint32_t n = MIN(f->aos[i].size, 16u);
        uint32_t addr = f->aos[i].addr + index * f->aos[i].stride * 4;

        if (!n) {
            n = 1;
        }
        uint64_t voff;
        /* VC_SWAP is done on the bus: arrays in video memory are as is */
        int swap = ati_mc_vram_range(d->s, addr, n * 4, &voff) ? 0
                                                              : d->vc_swap;

        if (!ati_mc_read(d->s, addr, (uint8_t *)buf, n * 4)) {
            return false;
        }
        for (uint32_t k = 0; k < n; k++) {
            buf[k] = swap == 2 ? ldl_be_p(&buf[k]) :
                     swap == 1 ? ((ldl_le_p(&buf[k]) & 0x00ff00ff) << 8 |
                                  ((ldl_le_p(&buf[k]) >> 8) & 0x00ff00ff))
                               : ldl_le_p(&buf[k]);
        }
        stream_element(&d->streams[i], buf, n,
                       pv->in[d->streams[i].dst & 15]);
    }
    return true;
}

static bool make_vertex(R300Draw *d, R300Fetch *f, uint32_t index,
                        R300Vertex *v)
{
    PVSState pv;

    if (!fetch_vertex(d, f, index, &pv)) {
        return false;
    }
    if (d->bypass) {
        memcpy(pv.out, pv.in, sizeof(pv.out));
    } else {
        memset(pv.t, 0, sizeof(pv.t));
        memset(pv.alt, 0, sizeof(pv.alt));
        memset(pv.out, 0, sizeof(pv.out));
        memset(pv.a0, 0, sizeof(pv.a0));
        pvs_run(d, &pv);
    }
    vertex_finish(d, (const float (*)[4])pv.out, v);
    return true;
}

/* ---- draw setup ---------------------------------------------------------- */

static void zclear_apply(ATIVGAState *s);

/* Hand the draw to the host GPU if it can do it. */
static bool gpu_try(ATIVGAState *s, R300Draw *d, uint32_t coffset)
{
    R300GLDesc g;
    int n = 0;

    memset(&g, 0, sizeof(g));
    g.color_off = d->color_off;
    g.color_pitch = d->color_pitch;
    g.color_fmt = d->color_fmt;
    g.color_bpp = d->color_bpp;
    g.z_on = d->z_on;
    g.s_on = d->s_on;
    g.depth_off = d->depth_off;
    g.depth_pitch = d->depth_pitch;
    g.depth_bpp = d->depth_bpp;
    g.zb_cntl = d->zb_cntl;
    g.zs_cntl = d->zs_cntl;
    g.stencil_ref = d->stencil_ref;
    g.stencil_mask = d->stencil_mask;
    g.stencil_wmask = d->stencil_wmask;
    g.cblend = d->cblend;
    g.ablend = d->ablend;
    g.blend_color = d->blend_color;
    g.cmask = d->cmask;
    g.rop = d->rop;
    g.alpha_func = d->alpha_func;
    g.fog_on = d->fog_on;
    g.fog_scale = d->fog_scale;
    g.fog_offset = d->fog_offset;
    memcpy(g.fog_color, d->fog_color, sizeof(g.fog_color));
    g.clip_x0 = d->clip_x0;
    g.clip_y0 = d->clip_y0;
    g.clip_x1 = d->clip_x1;
    g.clip_y1 = d->clip_y1;
    g.cull = d->cull;
    g.nnodes = d->nnodes;
    for (int i = 0; i < d->nnodes; i++) {
        g.nodes[i].alu_start = d->nodes[i].alu_start;
        g.nodes[i].alu_end = d->nodes[i].alu_end;
        g.nodes[i].tex_start = d->nodes[i].tex_start;
        g.nodes[i].tex_end = d->nodes[i].tex_end;
    }
    memcpy(g.tex_inst, d->tex_inst, sizeof(g.tex_inst));
    for (int i = 0; i < 64; i++) {
        g.alu[i][0] = d->alu[i].rgb_addr;
        g.alu[i][1] = d->alu[i].rgb_inst;
        g.alu[i][2] = d->alu[i].alpha_addr;
        g.alu[i][3] = d->alu[i].alpha_inst;
    }
    memcpy(g.consts, d->consts, sizeof(g.consts));
    for (int i = 0; i < d->rs_count; i++) {
        uint32_t inst = d->rs_inst[i];

        if ((inst & (1u << 3)) && n < R300GL_MAX_VARY) {
            g.vary_dst[n++] = (inst >> 6) & 0x1f;
        }
        if ((inst & (3u << 14)) && n < R300GL_MAX_VARY) {
            g.vary_dst[n++] = (inst >> 17) & 0x1f;
        }
    }
    g.nvary = n;
    (void)coffset;
    return ati_r300_gl_begin(s, &g);
}

static bool draw_begin(ATIVGAState *s, R300Draw *d)
{
    uint32_t pitch = reg(s, RB3D_COLORPITCH0);
    uint32_t sref = reg(s, ZB_STENCILREFMASK);
    uint32_t fmt0 = reg(s, VAP_OUTPUT_VTX_FMT_0);
    uint32_t fmt1 = reg(s, VAP_OUTPUT_VTX_FMT_1);
    uint32_t code0 = reg(s, VAP_PVS_CODE_CNTL_0);
    uint32_t sc0 = reg(s, SC_SCISSOR0), sc1 = reg(s, SC_SCISSOR1);
    int slot = 0;

    memset(d, 0, sizeof(*d));
    d->s = s;

    /* vertex streams */
    for (int i = 0; i < 16; i++) {
        uint32_t c = reg(s, VAP_PROG_STREAM_CNTL_0 + 4 * (i / 2)) >>
                     (16 * (i & 1));
        uint32_t e = reg(s, VAP_PROG_STREAM_CNTL_EXT_0 + 4 * (i / 2)) >>
                     (16 * (i & 1));
        R300Stream *st = &d->streams[i];

        st->type = c & 0xf;
        st->skip = (c >> 4) & 0xf;
        st->dst = (c >> 8) & 0x1f;
        st->sign = (c >> 14) & 1;
        st->norm = (c >> 15) & 1;
        st->ext = e & 0xffff;
        d->nstreams = i + 1;
        if (c & (1u << 13)) {                   /* LAST_VEC */
            break;
        }
    }
    d->bypass = reg(s, VAP_CNTL_STATUS) & (1u << 8);
    d->vc_swap = reg(s, VAP_CNTL_STATUS) & 3;
    d->vte = reg(s, VAP_VTE_CNTL);
    for (int i = 0; i < 6; i++) {
        d->vport[i] = u2f(reg(s, SE_VPORT_XSCALE + 4 * i));
    }
    d->pvs_first = code0 & 0x3ff;
    d->pvs_last = (code0 >> 20) & 0x3ff;
    d->const_base = reg(s, VAP_PVS_CONST_CNTL) & 0xff;

    /* the order of vertex outputs: position, point size, colours, tex */
    d->out_fmt0 = fmt0;
    d->out_fmt1 = fmt1;
    slot = 1;                                   /* position is always 0 */
    d->psize_slot = -1;
    if (fmt0 & (1u << 16)) {
        d->psize_slot = slot++;
    }
    for (int c = 0; c < 4; c++) {
        d->col_slot[c] = (fmt0 & (1u << (1 + c))) ? slot++ : -1;
    }
    int base = 0;

    for (int t = 0; t < 8; t++) {
        int n = (fmt1 >> (3 * t)) & 7;

        d->tc_slot[t] = n ? slot++ : -1;
        d->tc_count[t] = MIN(n, 4);
        d->tc_base[t] = base;
        base += MIN(n, 4);
    }
    /*
     * With the vertex program bypassed Apple's driver leaves the output
     * format at "position only"; the vectors after the position then feed
     * the rasteriser as they are: RS_COUNT's colours, then its texture
     * coordinate components, four to a vector.
     */
    if (d->bypass && !(fmt0 & 0x1e) && !fmt1) {
        uint32_t rc = reg(s, RS_COUNT);
        int ncol = (rc >> 7) & 0xf, ntc = rc & 0x7f;

        slot = 1;
        for (int c = 0; c < 4; c++) {
            d->col_slot[c] = c < ncol ? slot++ : -1;
        }
        for (int t = 0; t < 8; t++) {
            int n = MIN(ntc - 4 * t, 4);

            d->tc_slot[t] = n > 0 ? slot++ : -1;
            d->tc_count[t] = n > 0 ? 4 : 0;
            d->tc_base[t] = 4 * t;
        }
    }
    d->nslots = slot;

    /* setup */
    d->cull = reg(s, SU_CULL_MODE);
    d->color_ctl = reg(s, GA_COLOR_CONTROL);
    d->gb_enable = reg(s, GB_ENABLE);
    d->point_w = ((reg(s, GA_POINT_SIZE) >> 16) & 0xffff) / 12.0f;
    d->point_h = (reg(s, GA_POINT_SIZE) & 0xffff) / 12.0f;
    for (int i = 0; i < 4; i++) {
        d->point_st[i] = u2f(reg(s, GA_POINT_S0 + 4 * i));
    }

    /* scissor, inclusive, offset by 1440 on R300 */
    d->clip_x0 = MAX((int)(sc0 & 0x1fff) - 1440, 0);
    d->clip_y0 = MAX((int)((sc0 >> 13) & 0x1fff) - 1440, 0);
    d->clip_x1 = (int)(sc1 & 0x1fff) - 1440;
    d->clip_y1 = (int)((sc1 >> 13) & 0x1fff) - 1440;

    /* rasteriser */
    d->rs_count = (reg(s, RS_INST_COUNT) & 0xf) + 1;
    for (int i = 0; i < 8; i++) {
        d->rs_ip[i] = reg(s, RS_IP_0 + 4 * i);
        d->rs_inst[i] = reg(s, RS_INST_0 + 4 * i);
    }

    /* fragment program: active nodes are the last (NLEVEL + 1) */
    /*
     * US_CODE_OFFSET: ALU_OFFSET 5:0, ALU_SIZE 12:6, TEX_OFFSET 17:13,
     * TEX_SIZE 22:18. Node addresses are relative to the offsets.
     */
    uint32_t config = reg(s, US_CONFIG), code_off = reg(s, US_CODE_OFFSET);
    int alu_off = code_off & 0x3f, tex_off = (code_off >> 13) & 0x1f;

    d->nnodes = (config & 3) + 1;
    for (int n = 0; n < d->nnodes; n++) {
        uint32_t a = reg(s, US_CODE_ADDR_0 + 4 * (4 - d->nnodes + n));
        R300Node *node = &d->nodes[n];

        node->alu_start = alu_off + (a & 0x3f);
        node->alu_end = node->alu_start + ((a >> 6) & 0x3f);
        node->tex_start = tex_off + ((a >> 12) & 0x1f);
        node->tex_end = node->tex_start + ((a >> 17) & 0x1f);
        if (n == 0 && !(config & (1u << 3))) {
            node->tex_end = node->tex_start - 1;        /* no textures */
        }
    }
    for (int i = 0; i < 32; i++) {
        d->tex_inst[i] = reg(s, US_TEX_INST_0 + 4 * i);
        for (int c = 0; c < 4; c++) {
            d->consts[i][c] = fp24_to_float(reg(s, US_ALU_CONST_R_0 +
                                                16 * i + 4 * c));
        }
    }
    for (int i = 0; i < 64; i++) {
        d->alu[i].rgb_addr = reg(s, US_ALU_RGB_ADDR_0 + 4 * i);
        d->alu[i].alpha_addr = reg(s, US_ALU_ALPHA_ADDR_0 + 4 * i);
        d->alu[i].rgb_inst = reg(s, US_ALU_RGB_INST_0 + 4 * i);
        d->alu[i].alpha_inst = reg(s, US_ALU_ALPHA_INST_0 + 4 * i);
        alu_decode(&d->alu[i], &d->dec[i]);
    }
    /* temporaries used: cleared per pixel, so count them */
    d->ntemps = 1;
    for (int n = 0; n < d->nnodes; n++) {
        for (int i = d->nodes[n].tex_start; i <= d->nodes[n].tex_end; i++) {
            uint32_t ti = d->tex_inst[i & 31];

            d->ntemps = MAX(d->ntemps, (int)(ti & 0x1f) + 1);
            d->ntemps = MAX(d->ntemps, (int)((ti >> 6) & 0x1f) + 1);
        }
        for (int i = d->nodes[n].alu_start; i <= d->nodes[n].alu_end; i++) {
            const R300AluDec *a = &d->dec[i & 63];

            for (int k = 0; k < 3; k++) {
                if (!(a->rsrc[k] & 0x20)) {
                    d->ntemps = MAX(d->ntemps, (a->rsrc[k] & 0x1f) + 1);
                }
                if (!(a->asrc[k] & 0x20)) {
                    d->ntemps = MAX(d->ntemps, (a->asrc[k] & 0x1f) + 1);
                }
            }
            d->ntemps = MAX(d->ntemps, MAX(a->rd, a->ad) + 1);
        }
    }
    for (int i = 0; i < d->rs_count; i++) {
        d->ntemps = MAX(d->ntemps, (int)((d->rs_inst[i] >> 6) & 0x1f) + 1);
        d->ntemps = MAX(d->ntemps, (int)((d->rs_inst[i] >> 17) & 0x1f) + 1);
    }

    /* depth and stencil */
    d->zb_cntl = reg(s, ZB_CNTL);
    d->zs_cntl = reg(s, ZB_ZSTENCILCNTL);
    d->zb_format = reg(s, ZB_FORMAT);
    d->z_on = d->zb_cntl & 2;
    d->s_on = d->zb_cntl & 1;
    d->stencil_ref = sref & 0xff;
    d->stencil_mask = (sref >> 8) & 0xff;
    d->stencil_wmask = (sref >> 16) & 0xff;
    d->depth_bpp = (d->zb_format & 0xf) == 2 ? 4 : 2;
    d->depth_pitch = reg(s, ZB_DEPTHPITCH) & 0x3ffc;
    if ((d->z_on || d->s_on) &&
        (!d->depth_pitch ||
         !ati_mc_vram_range(s, reg(s, ZB_DEPTHOFFSET) & ~0x1fu, 1,
                            &d->depth_off))) {
        d->z_on = d->s_on = false;
    }

    if (s->r300_zclear.pending && (d->z_on || d->s_on)) {
        ati_r300_gl_flush(s);
        zclear_apply(s);
        ati_r300_gl_touch(s);
    }

    /* colour */
    d->cblend = reg(s, RB3D_CBLEND);
    d->ablend = reg(s, RB3D_ABLEND);
    d->blend_color = reg(s, RB3D_BLEND_COLOR);
    d->cmask = reg(s, RB3D_COLOR_CHANNEL_MASK) & 0xf;
    d->rop = reg(s, RB3D_ROPCNTL);
    d->alpha_func = reg(s, FG_ALPHA_FUNC);
    d->fog_on = reg(s, FG_FOG_BLEND) & 1;
    d->fog_sel = reg(s, GB_SELECT) & 7;
    d->fog_scale = u2f(reg(s, GA_FOG_SCALE));
    d->fog_offset = u2f(reg(s, GA_FOG_OFFSET));
    for (int i = 0; i < 3; i++) {
        d->fog_color[i] = (reg(s, FG_FOG_COLOR_R + 4 * i) & 0x3ff) / 1023.0f;
    }
    if (d->fog_on && (reg(s, FG_FOG_BLEND) & 6)) {
        static bool warned;

        if (!warned) {
            warned = true;
            qemu_log_mask(LOG_UNIMP, "ati: R300 fog function %d treated as "
                          "linear\n", (reg(s, FG_FOG_BLEND) >> 1) & 3);
        }
    }
    d->color_fmt = (pitch >> 21) & 0xf;
    d->color_bpp = color_bpp(d->color_fmt);
    d->color_pitch = pitch & 0x1ffe;
    d->color_swap = (pitch >> 19) & 3;
    if (!d->color_pitch) {
        return false;
    }
    d->clip_x1 = MIN(d->clip_x1, (int)d->color_pitch - 1);
    d->dirty_y0 = INT_MAX;
    d->dirty_y1 = -1;
    if (d->clip_x0 > d->clip_x1 || d->clip_y0 > d->clip_y1) {
        return false;
    }

    uint32_t coffset = reg(s, RB3D_COLOROFFSET0) & ~0xfu;
    uint64_t rowbytes = (uint64_t)d->color_pitch * d->color_bpp;
    uint64_t span = (uint64_t)(d->clip_y1 + 1) * rowbytes;

    if (!ati_mc_vram_range(s, coffset, span, &d->color_off)) {
        if (span > 64 * MiB) {
            return false;
        }
        d->color_mc = coffset;
        d->color_stage = g_malloc0(span);
        if (!ati_mc_read(s, coffset + d->clip_y0 * rowbytes,
                         d->color_stage + d->clip_y0 * rowbytes,
                         (d->clip_y1 - d->clip_y0 + 1) * rowbytes)) {
            static bool warned;

            if (!warned) {
                warned = true;
                qemu_log_mask(LOG_GUEST_ERROR, "ati: R300 colour buffer at "
                              "0x%x is not mapped\n", coffset);
            }
            g_free(d->color_stage);
            d->color_stage = NULL;
            return false;
        }
    }
    if (!d->color_stage && gpu_try(s, d, coffset)) {
        d->gpu = true;
    } else {
        /* drawn in software: video memory must be up to date */
        ati_r300_gl_flush(s);
        for (int n = 0; n < R300_TEX_UNITS; n++) {
            tex_setup(d, n);
        }
    }
    static int tl_on = -1;
    static uint32_t tl_lo, tl_hi;

    if (tl_on < 0) {
        const char *e = getenv("PPCOSX_R300_TARGETLOG");

        tl_on = e && sscanf(e, "%x-%x", &tl_lo, &tl_hi) == 2;
    }
    if (tl_on) {
        uint32_t lo = tl_lo, hi = tl_hi;

        if (coffset >= lo && coffset < hi) {
            d->logged = true;
            fprintf(stderr, "r300: draw %" PRIu64 " into %08x pitch %u fmt %d "
                    "swap %d clip %d,%d-%d,%d tex0 %08x %08x/%08x\n",
                    stats.draws, coffset, d->color_pitch, d->color_fmt,
                    d->color_swap, d->clip_x0, d->clip_y0, d->clip_x1,
                    d->clip_y1, reg(s, TX_OFFSET_0), reg(s, TX_FORMAT0_0),
                    reg(s, TX_FORMAT2_0));
        }
    }
    if (stats_on()) {
        static uint32_t seen[64][2];
        static int nseen;
        bool found = false;

        for (int i = 0; i < nseen; i++) {
            found |= seen[i][0] == coffset && seen[i][1] == pitch;
        }
        if (!found && nseen < 64) {
            seen[nseen][0] = coffset;
            seen[nseen++][1] = pitch;
            fprintf(stderr, "r300: target %08x pitch %08x (%u px, fmt %d) "
                    "clip %d,%d-%d,%d\n", coffset, pitch, d->color_pitch,
                    d->color_fmt, d->clip_x0, d->clip_y0, d->clip_x1,
                    d->clip_y1);
        }
    }
    return true;
}

static void draw_end(R300Draw *d)
{
    ATIVGAState *s = d->s;
    uint64_t rowbytes = (uint64_t)d->color_pitch * d->color_bpp;

    tex_release(d);
    if (d->gpu) {
        ati_r300_gl_end(s);
        return;
    }
    /* drawn in software: host copies must be checked again */
    ati_r300_gl_touch(s);
    if (d->color_stage) {
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
        uint64_t start = d->color_off + d->dirty_y0 * rowbytes;
        uint64_t len = (d->dirty_y1 - d->dirty_y0 + 1) * rowbytes;

        if (start < s->vga.vram_size) {
            memory_region_set_dirty(&s->vga.vram, start,
                                    MIN(len, s->vga.vram_size - start));
        }
        /* depth and stencil written pixel by pixel: log them too */
        if ((d->z_on && (d->zb_cntl & 4)) || (d->s_on && d->stencil_wmask)) {
            uint64_t zrow = (uint64_t)d->depth_pitch * d->depth_bpp;

            start = d->depth_off + d->dirty_y0 * zrow;
            len = (d->dirty_y1 - d->dirty_y0 + 1) * zrow;
            if (start < s->vga.vram_size) {
                memory_region_set_dirty(&s->vga.vram, start,
                                        MIN(len, s->vga.vram_size - start));
            }
        }
    }
}

static void stats_report(R300Draw *d)
{
    ATIVGAState *s = d->s;

    stats.draws++;
    if (!stats_on() || stats.draws % 1000) {
        return;
    }
    fprintf(stderr, "r300: %" PRIu64 " draws took %.1f ms in all; command "
            "processor %.1f ms, 2D %.1f ms\n", stats.draws, stats.ns / 1e6,
            ati_cp_ns / 1e6, ati_2d_ns / 1e6);
    fprintf(stderr, "r300: draws %" PRIu64 " tris %" PRIu64 " culled %"
            PRIu64 " quads %" PRIu64 " pixels %" PRIu64 " written %" PRIu64
            " | color %08x/%08x blend %08x zb %08x us %08x/%08x vte %08x "
            "pvs %08x bypass %d\n", stats.draws, stats.tris, stats.culled,
            stats.quads, stats.pixels, stats.written,
            reg(s, RB3D_COLOROFFSET0), reg(s, RB3D_COLORPITCH0),
            d->cblend, d->zb_cntl, reg(s, US_CONFIG), reg(s, US_CODE_OFFSET),
            d->vte, reg(s, VAP_PVS_CODE_CNTL_0), d->bypass);
}

/*
 * Clipping. With perspective division to do (VTE W0_FMT without XY_FMT),
 * triangles are clipped in clip space against the near plane z >= -w and
 * against w > 0, so nothing behind the eye is projected; the rest falls
 * to the scissor. Attributes are linear in clip space.
 */
static bool needs_clip(R300Draw *d)
{
    return (d->vte & (1u << 10)) && !(d->vte & (1u << 8));
}

static void vertex_lerp(R300Draw *d, const R300Vertex *a, const R300Vertex *b,
                        float t, R300Vertex *o)
{
    *o = *a;
    for (int i = 0; i < 4; i++) {
        o->clip[i] = a->clip[i] + (b->clip[i] - a->clip[i]) * t;
    }
    for (int c = 0; c < 4; c++) {
        for (int i = 0; i < 4; i++) {
            o->col[c][i] = a->col[c][i] + (b->col[c][i] - a->col[c][i]) * t;
        }
    }
    for (int i = 0; i < 32; i++) {
        o->tc[i] = a->tc[i] + (b->tc[i] - a->tc[i]) * t;
    }
    o->psize = a->psize + (b->psize - a->psize) * t;
    vertex_window(d, o);
}

/* Clip polygon `in` (n vertices) to dist >= 0; returns the new count. */
static int clip_plane(R300Draw *d, const R300Vertex *in, int n,
                      R300Vertex *out, int plane)
{
    int m = 0;

    for (int i = 0; i < n; i++) {
        const R300Vertex *a = &in[i], *b = &in[(i + 1) % n];
        float da = plane ? a->clip[3] - 1e-6f : a->clip[2] + a->clip[3];
        float db = plane ? b->clip[3] - 1e-6f : b->clip[2] + b->clip[3];

        if (da >= 0) {
            out[m++] = *a;
        }
        if ((da >= 0) != (db >= 0)) {
            vertex_lerp(d, a, b, da / (da - db), &out[m++]);
        }
    }
    return m;
}

static void emit_triangle(R300Draw *d, const R300Vertex *v0,
                          const R300Vertex *v1, const R300Vertex *v2,
                          const R300Vertex *flat)
{
    const R300Vertex *v[3] = { v0, v1, v2 };
    R300Vertex a[8], b[8];
    int n = 3;
    bool inside = true;

    if (!needs_clip(d)) {
        draw_triangle(d, v0, v1, v2, flat);
        return;
    }
    for (int i = 0; i < 3; i++) {
        if (v[i]->clip[2] + v[i]->clip[3] < 0 || v[i]->clip[3] <= 1e-6f) {
            inside = false;
        }
    }
    if (inside) {
        draw_triangle(d, v0, v1, v2, flat);
        return;
    }
    a[0] = *v0;
    a[1] = *v1;
    a[2] = *v2;
    n = clip_plane(d, a, n, b, 0);
    n = clip_plane(d, b, n, a, 1);
    for (int i = 1; i + 1 < n; i++) {
        draw_triangle(d, &a[0], &a[i], &a[i + 1], flat);
    }
}

/* Debugging: in stats mode, describe each new fragment program once. */
static void dump_program(R300Draw *d)
{
    ATIVGAState *s = d->s;
    static uint32_t seen[128];
    static int nseen;
    uint32_t h = reg(s, US_CONFIG) * 31 + reg(s, US_CODE_OFFSET);

    if (!stats_on()) {
        return;
    }
    for (int i = 0; i < 64; i++) {
        h = h * 33 + d->alu[i].rgb_addr * 7 + d->alu[i].alpha_addr * 3 +
            d->alu[i].rgb_inst * 5 + d->alu[i].alpha_inst;
    }
    for (int i = 0; i < 8; i++) {
        h = h * 33 + d->rs_ip[i] * 3 + d->rs_inst[i] + d->tex_inst[i] * 7;
    }
    for (int i = 0; i < nseen; i++) {
        if (seen[i] == h) {
            return;
        }
    }
    if (nseen < 128) {
        seen[nseen++] = h;
    }
    fprintf(stderr, "r300: program config %08x code_offset %08x pixsize %08x"
            " out_fmt %08x w_fmt %08x rs_count %08x/%08x vtx_fmt %08x/%08x "
            "gb %08x/%08x vte %08x bypass %d pvs %08x/%08x streams %d\n",
            reg(s, US_CONFIG), reg(s, US_CODE_OFFSET), reg(s, US_PIXSIZE),
            reg(s, US_OUT_FMT_0), reg(s, US_W_FMT), reg(s, RS_COUNT),
            reg(s, RS_INST_COUNT), d->out_fmt0, d->out_fmt1,
            reg(s, GB_VAP_RASTER_VTX_FMT_0), reg(s, GB_VAP_RASTER_VTX_FMT_1),
            d->vte, d->bypass, reg(s, VAP_PVS_CODE_CNTL_0),
            reg(s, VAP_PVS_CONST_CNTL), d->nstreams);
    fprintf(stderr, "r300:   fg %08x %08x %08x %08x %08x %08x %08x %08x\n",
            reg(s, 0x4bc0), reg(s, 0x4bc4), reg(s, 0x4bc8), reg(s, 0x4bcc),
            reg(s, 0x4bd0), reg(s, 0x4bd4), reg(s, 0x4bd8), reg(s, 0x4bdc));
    for (int i = 0; i < d->rs_count; i++) {
        fprintf(stderr, "r300:   rs %d ip %08x inst %08x\n", i,
                d->rs_ip[d->rs_inst[i] & 7], d->rs_inst[i]);
    }
    for (int n = 0; n < d->nnodes; n++) {
        R300Node *node = &d->nodes[n];

        fprintf(stderr, "r300:   node %d alu %d-%d tex %d-%d\n", n,
                node->alu_start, node->alu_end, node->tex_start,
                node->tex_end);
        for (int i = node->tex_start; i <= node->tex_end; i++) {
            fprintf(stderr, "r300:     tex %08x\n", d->tex_inst[i & 31]);
        }
        for (int i = node->alu_start; i <= node->alu_end; i++) {
            R300Alu *a = &d->alu[i & 63];

            fprintf(stderr, "r300:     alu %08x %08x %08x %08x\n",
                    a->rgb_addr, a->rgb_inst, a->alpha_addr, a->alpha_inst);
        }
    }
    for (int i = 0; i < 4; i++) {
        fprintf(stderr, "r300:   const %d %f %f %f %f\n", i,
                d->consts[i][0], d->consts[i][1], d->consts[i][2],
                d->consts[i][3]);
    }
    if (!d->bypass) {
        for (int pc = d->pvs_first; pc <= d->pvs_last && pc < 512; pc++) {
            fprintf(stderr, "r300:   pvs %3d %08x %08x %08x %08x\n", pc,
                    s->pvs[pc * 4], s->pvs[pc * 4 + 1], s->pvs[pc * 4 + 2],
                    s->pvs[pc * 4 + 3]);
        }
    }
}

/*
 * RB3D_AARESOLVE_CTL's RESOLVE_MODE: the primitive drawn copies the colour
 * buffer under it to the resolve buffer (RB3D_AARESOLVE_OFFSET/PITCH),
 * averaging samples; the primitive's own colour is not written. Apple's
 * driver swaps GL buffers this way. Multisampling is not modelled (one
 * sample per pixel), so resolving is a copy.
 */
#define RB3D_AARESOLVE_OFFSET   0x4e80
#define RB3D_AARESOLVE_PITCH    0x4e84
#define RB3D_AARESOLVE_CTL      0x4e88

static void resolve_rect(R300Draw *d, int x0, int y0, int x1, int y1)
{
    ATIVGAState *s = d->s;
    uint64_t dst;
    uint32_t dpitch = reg(s, RB3D_AARESOLVE_PITCH) & 0x1ffe;
    int bpp = d->color_bpp;

    x0 = MAX(x0, d->clip_x0);
    y0 = MAX(y0, d->clip_y0);
    x1 = MIN(x1, MIN(d->clip_x1, (int)dpitch - 1));
    y1 = MIN(y1, d->clip_y1);
    if (x0 > x1 || y0 > y1 || !dpitch || d->color_stage ||
        !ati_mc_vram_range(s, reg(s, RB3D_AARESOLVE_OFFSET) & ~0x1fu, 1,
                           &dst)) {
        return;
    }
    uint32_t n = (x1 - x0 + 1) * bpp;
    uint8_t *row = g_malloc(n);

    /* what the host GPU drew goes to video memory first */
    ati_r300_gl_sync_range(s, d->color_off,
                           (uint64_t)(y1 + 1) * d->color_pitch * bpp);
    for (int y = y0; y <= y1; y++) {
        uint64_t src = d->color_off + ((uint64_t)y * d->color_pitch + x0) * bpp;
        uint64_t to = dst + ((uint64_t)y * dpitch + x0) * bpp;

        if (src + n > s->vga.vram_size || to + n > s->vga.vram_size) {
            break;
        }
        ati_vram_read(s, src, row, n);
        ati_vram_write(s, to, row, n);
    }
    g_free(row);
    ati_r300_gl_changed(s, dst + (uint64_t)y0 * dpitch * bpp,
                        (uint64_t)(y1 - y0 + 1) * dpitch * bpp);
}

/* The window rectangle a resolve primitive covers. */
static void resolve_prims(R300Draw *d, int prim, const R300Vertex *v,
                          uint32_t nv)
{
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    float hw = prim == 1 ? d->point_w : 0, hh = prim == 1 ? d->point_h : 0;

    for (uint32_t i = 0; i < nv; i++) {
        x0 = MIN(x0, v[i].pos[0] - hw);
        y0 = MIN(y0, v[i].pos[1] - hh);
        x1 = MAX(x1, v[i].pos[0] + hw);
        y1 = MAX(y1, v[i].pos[1] + hh);
    }
    if (nv && isfinite(x0) && isfinite(y0) && isfinite(x1) && isfinite(y1)) {
        resolve_rect(d, (int)floorf(x0 + 0.5f), (int)floorf(y0 + 0.5f),
                     (int)ceilf(x1 - 0.5f), (int)ceilf(y1 - 0.5f));
    }
}

/* Walk VF_CNTL's primitive over vertices 0..n-1, fetched by `index`. */
static void draw_prims(R300Draw *d, uint32_t vf_cntl, R300Fetch *f,
                       const uint32_t *indices, bool idx32, uint32_t n)
{
    int prim = vf_cntl & 0xf;
    uint32_t cap = MIN(n, 65536u);
    R300Vertex *v = g_new(R300Vertex, cap ? cap : 1);
    uint32_t nv = 0;

    for (uint32_t i = 0; i < cap; i++) {
        uint32_t index = i;

        if (indices) {
            index = idx32 ? indices[i]
                          : (indices[i / 2] >> (16 * (i & 1))) & 0xffff;
            index += reg(d->s, VAP_INDEX_OFFSET);
        }
        if (!make_vertex(d, f, index, &v[nv])) {
            break;
        }
        nv++;
    }
    if (d->logged) {
        static uint32_t seen[64];
        static int nseen;
        uint32_t h = d->pvs_first * 7 + d->pvs_last;
        bool found = false;

        for (int pc = d->pvs_first; pc <= d->pvs_last && pc < 512; pc++) {
            for (int k = 0; k < 4; k++) {
                h = h * 31 + d->s->pvs[pc * 4 + k];
            }
        }
        for (int i = 0; i < nseen; i++) {
            found |= seen[i] == h;
        }
        if (!found && nseen < 64) {
            seen[nseen++] = h;
            fprintf(stderr, "r300 log: pvs %d-%d bypass %d vte %08x fmt "
                    "%08x/%08x const_base %d prim %d n %u streams:",
                    d->pvs_first, d->pvs_last, d->bypass, d->vte,
                    d->out_fmt0, d->out_fmt1, d->const_base, prim, nv);
            for (int i = 0; i < d->nstreams; i++) {
                fprintf(stderr, " [t%d d%d s%d n%d sk%d e%04x]",
                        d->streams[i].type, d->streams[i].dst,
                        d->streams[i].sign, d->streams[i].norm,
                        d->streams[i].skip, d->streams[i].ext);
            }
            fprintf(stderr, "\n");
            for (int pc = d->pvs_first; pc <= d->pvs_last && pc < 512; pc++) {
                fprintf(stderr, "r300 log:   pvs %3d %08x %08x %08x %08x\n",
                        pc, d->s->pvs[pc * 4], d->s->pvs[pc * 4 + 1],
                        d->s->pvs[pc * 4 + 2], d->s->pvs[pc * 4 + 3]);
            }
            for (int i = 0; f->arrays && i < d->nstreams; i++) {
                uint32_t raw[4] = { 0 };

                ati_mc_read(d->s, f->aos[i].addr, (uint8_t *)raw, 16);
                fprintf(stderr, "r300 log:   aos %d addr %08x stride %u size "
                        "%u raw %08x %08x %08x %08x idx %d\n", i,
                        f->aos[i].addr, f->aos[i].stride, f->aos[i].size,
                        raw[0], raw[1], raw[2], raw[3], indices != NULL);
            }
            if (indices) {
                fprintf(stderr, "r300 log:   indices %08x %08x %08x idx32 %d "
                        "offset %u\n", indices[0], indices[1], indices[2],
                        idx32, reg(d->s, VAP_INDEX_OFFSET));
            }
            for (uint32_t i = 0; i < MIN(nv, 3u); i++) {
                fprintf(stderr, "r300 log:   v%u clip %.3f %.3f %.3f %.3f "
                        "win %.2f %.2f %.4f %.4f\n", i, v[i].clip[0],
                        v[i].clip[1], v[i].clip[2], v[i].clip[3],
                        v[i].pos[0], v[i].pos[1], v[i].pos[2], v[i].pos[3]);
            }
        }
    }
    if (reg(d->s, RB3D_AARESOLVE_CTL) & 1) {
        resolve_prims(d, prim, v, nv);
        g_free(v);
        return;
    }
    dump_program(d);
    if (stats_on() && f->arrays) {
        static int logged;

        if (logged++ < 30) {
            fprintf(stderr, "r300: arrays draw prim %d n %u idx %d vtx_size "
                    "%u streams %d:", prim, n, indices != NULL,
                    reg(d->s, VAP_VTX_SIZE), d->nstreams);
            for (int i = 0; i < d->nstreams; i++) {
                fprintf(stderr, " [%08x stride %u size %u type %d dst %d "
                        "ext %04x]", f->aos[i].addr, f->aos[i].stride,
                        f->aos[i].size, d->streams[i].type,
                        d->streams[i].dst, d->streams[i].ext);
            }
            fprintf(stderr, " swap %u\n", d->vc_swap);
            for (uint32_t i = 0; i < MIN(nv, 3u); i++) {
                fprintf(stderr, "r300:   av%u pos %.2f %.2f %.3f %.3f col "
                        "%.2f %.2f %.2f %.2f\n", i, v[i].pos[0], v[i].pos[1],
                        v[i].pos[2], v[i].pos[3], v[i].col[0][0],
                        v[i].col[0][1], v[i].col[0][2], v[i].col[0][3]);
            }
        }
    }
    if (stats_on() && stats.draws < 40) {
        for (uint32_t i = 0; i < MIN(nv, 4u); i++) {
            fprintf(stderr, "r300:   v%u pos %.2f %.2f %.3f %.3f col %.2f %.2f"
                    " %.2f %.2f tc %.3f %.3f %.3f %.3f\n", i, v[i].pos[0],
                    v[i].pos[1], v[i].pos[2], v[i].pos[3], v[i].col[0][0],
                    v[i].col[0][1], v[i].col[0][2], v[i].col[0][3],
                    v[i].tc[0], v[i].tc[1], v[i].tc[2], v[i].tc[3]);
        }
    }
    switch (prim) {
    case 1:                                             /* points */
        if (stats_on()) {
            static int logged;

            if (d->tex[0].w >= 32 && logged++ < 80) {
                fprintf(stderr, "r300: point at %.2f,%.2f size %08x tex0 "
                        "%dx%d color %08x\n", v[0].pos[0], v[0].pos[1],
                        reg(d->s, GA_POINT_SIZE), d->tex[0].w, d->tex[0].h,
                        reg(d->s, RB3D_COLOROFFSET0));
            }
        }
        for (uint32_t i = 0; i < nv; i++) {
            draw_point(d, &v[i]);
        }
        break;
    case 2:                                             /* lines */
        for (uint32_t i = 0; i + 1 < nv; i += 2) {
            draw_line(d, &v[i], &v[i + 1]);
        }
        break;
    case 3: case 12:                                    /* strip, loop */
        for (uint32_t i = 0; i + 1 < nv; i++) {
            draw_line(d, &v[i], &v[i + 1]);
        }
        if (prim == 12 && nv > 2) {
            draw_line(d, &v[nv - 1], &v[0]);
        }
        break;
    case 4:                                             /* triangles */
        for (uint32_t i = 0; i + 2 < nv; i += 3) {
            emit_triangle(d, &v[i], &v[i + 1], &v[i + 2], &v[i + 2]);
        }
        break;
    case 5: case 15:                                    /* fan, polygon */
        for (uint32_t i = 1; i + 1 < nv; i++) {
            emit_triangle(d, &v[0], &v[i], &v[i + 1], &v[i + 1]);
        }
        break;
    case 6:                                             /* strip */
        for (uint32_t i = 0; i + 2 < nv; i++) {
            if (i & 1) {
                emit_triangle(d, &v[i + 1], &v[i], &v[i + 2], &v[i + 2]);
            } else {
                emit_triangle(d, &v[i], &v[i + 1], &v[i + 2], &v[i + 2]);
            }
        }
        break;
    case 8:                                             /* rectangles */
        for (uint32_t i = 0; i + 2 < nv; i += 3) {
            R300Vertex v3 = v[i + 2];

            for (int k = 0; k < 3; k++) {
                v3.pos[k] = v[i].pos[k] + v[i + 2].pos[k] - v[i + 1].pos[k];
            }
            for (int k = 0; k < 32; k++) {
                v3.tc[k] = v[i].tc[k] + v[i + 2].tc[k] - v[i + 1].tc[k];
            }
            emit_triangle(d, &v[i], &v[i + 1], &v[i + 2], &v[i + 2]);
            draw_triangle(d, &v[i], &v[i + 2], &v3, &v[i + 2]);
        }
        break;
    case 13:                                            /* quads */
        for (uint32_t i = 0; i + 3 < nv; i += 4) {
            emit_triangle(d, &v[i], &v[i + 1], &v[i + 2], &v[i + 3]);
            emit_triangle(d, &v[i], &v[i + 2], &v[i + 3], &v[i + 3]);
        }
        break;
    case 14:                                            /* quad strip */
        for (uint32_t i = 0; i + 3 < nv; i += 2) {
            emit_triangle(d, &v[i], &v[i + 1], &v[i + 3], &v[i + 3]);
            emit_triangle(d, &v[i], &v[i + 3], &v[i + 2], &v[i + 3]);
        }
        break;
    default: {
        static bool warned[16];

        if (!warned[prim]) {
            warned[prim] = true;
            qemu_log_mask(LOG_UNIMP, "ati: R300 primitive %d\n", prim);
        }
    }
    }
    g_free(v);
}

/* ---- packets ------------------------------------------------------------- */

/*
 * 3D_CLEAR_ZMASK with HyperZ on (ZB_BW_CNTL fast fill and compression):
 * the Z tiles are marked cleared and read back as ZB_DEPTHCLEARVALUE,
 * which drivers write after the packet. Depth is kept uncompressed here,
 * so the buffer, as bounded by the scissor, is filled with the clear
 * value before the next draw that uses depth or stencil.
 */
void ati_r300_clear_zmask(ATIVGAState *s, const uint32_t *p, uint32_t count)
{
    uint32_t sc0 = reg(s, SC_SCISSOR0), sc1 = reg(s, SC_SCISSOR1);

    s->r300_zclear.pending = true;
    s->r300_zclear.offset = reg(s, ZB_DEPTHOFFSET) & ~0x1fu;
    s->r300_zclear.pitch = reg(s, ZB_DEPTHPITCH) & 0x3ffc;
    s->r300_zclear.bpp = (reg(s, ZB_FORMAT) & 0xf) == 2 ? 4 : 2;
    s->r300_zclear.x0 = MAX((int)(sc0 & 0x1fff) - 1440, 0);
    s->r300_zclear.y0 = MAX((int)((sc0 >> 13) & 0x1fff) - 1440, 0);
    s->r300_zclear.x1 = (int)(sc1 & 0x1fff) - 1440;
    s->r300_zclear.y1 = (int)((sc1 >> 13) & 0x1fff) - 1440;
}

static void zclear_apply(ATIVGAState *s)
{
    uint32_t v = reg(s, ZB_DEPTHCLEARVALUE);
    uint64_t base;
    int bpp = s->r300_zclear.bpp;
    int x1 = MIN(s->r300_zclear.x1, (int)s->r300_zclear.pitch - 1);

    s->r300_zclear.pending = false;
    if (!s->r300_zclear.pitch || x1 < s->r300_zclear.x0 ||
        s->r300_zclear.y1 < s->r300_zclear.y0 ||
        !ati_mc_vram_range(s, s->r300_zclear.offset, 1, &base)) {
        return;
    }
    if (bpp == 2) {
        v >>= 16;       /* 16-bit depth: the top of the clear value */
    }
    for (int y = s->r300_zclear.y0; y <= s->r300_zclear.y1; y++) {
        uint64_t row = base + (uint64_t)y * s->r300_zclear.pitch * bpp;

        if (row + (uint64_t)(x1 + 1) * bpp > s->vga.vram_size) {
            break;
        }
        for (int x = s->r300_zclear.x0; x <= x1; x++) {
            ati_vram_st(s, row + (uint64_t)x * bpp, bpp, v);
        }
        memory_region_set_dirty(&s->vga.vram, row,
                                (uint64_t)(x1 + 1) * bpp);
    }
}

/* 3D_DRAW_IMMD_2: VF_CNTL, then the vertices. */
void ati_r300_draw_immd(ATIVGAState *s, const uint32_t *p, uint32_t count)
{
    R300Draw d;
    R300Fetch f = { 0 };
    uint32_t vf_cntl;

    if (count < 1) {
        return;
    }
    vf_cntl = p[0];
    s->shadow[VAP_VF_CNTL >> 2] = vf_cntl;
    f.imm = p + 1;
    f.nimm = count - 1;
    if (!draw_begin(s, &d)) {
        tex_release(&d);
        return;
    }
    int64_t t0 = stats_on() ? get_clock() : 0;

    draw_prims(&d, vf_cntl, &f, NULL, false, vf_cntl >> 16);
    draw_end(&d);
    if (t0) {
        stats.ns += get_clock() - t0;
    }
    stats_report(&d);
}

/*
 * The vertex arrays: VAP_VTX_NUM_ARRAYS (0x20c0), then for each pair of
 * arrays VAP_VTX_AOS_ATTR (size in dwords 6:0 and stride 14:8 of each,
 * the second in the top half) and the two addresses. 3D_LOAD_VBPNTR
 * carries the same words; drivers may also write the registers.
 */
#define VAP_VTX_NUM_ARRAYS      0x20c0
#define VAP_VTX_AOS_ATTR01      0x20c4

void ati_r300_load_vbpntr(ATIVGAState *s, const uint32_t *p, uint32_t count)
{
    for (uint32_t i = 0; i < count && i < 1 + 8 * 3; i++) {
        s->shadow[(VAP_VTX_NUM_ARRAYS >> 2) + i] = p[i];
    }
}

static void fetch_arrays(ATIVGAState *s, R300Fetch *f)
{
    uint32_t n = MIN(reg(s, VAP_VTX_NUM_ARRAYS) & 0x1f, 16u);

    f->arrays = true;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t base = VAP_VTX_AOS_ATTR01 + 12 * (i / 2);
        uint32_t attr = reg(s, base) >> (16 * (i & 1));

        f->aos[i].addr = reg(s, base + 4 + 4 * (i & 1));
        f->aos[i].size = attr & 0x7f;
        f->aos[i].stride = (attr >> 8) & 0x7f;
    }
}

/* 3D_DRAW_VBUF_2: VF_CNTL; vertices 0..n-1 of the arrays. */
void ati_r300_draw_vbuf(ATIVGAState *s, const uint32_t *p, uint32_t count)
{
    R300Draw d;
    R300Fetch f = { 0 };

    if (count < 1) {
        return;
    }
    s->shadow[VAP_VF_CNTL >> 2] = p[0];
    fetch_arrays(s, &f);
    if (!draw_begin(s, &d)) {
        tex_release(&d);
        return;
    }
    int64_t t0 = stats_on() ? get_clock() : 0;

    draw_prims(&d, p[0], &f, NULL, false, p[0] >> 16);
    draw_end(&d);
    if (t0) {
        stats.ns += get_clock() - t0;
    }
    stats_report(&d);
}

/* 3D_DRAW_INDX_2: VF_CNTL, then the indices (16-bit pairs or 32-bit). */
void ati_r300_draw_indx(ATIVGAState *s, const uint32_t *p, uint32_t count)
{
    R300Draw d;
    R300Fetch f = { 0 };
    bool idx32;
    uint32_t n;

    if (count < 1) {
        return;
    }
    s->shadow[VAP_VF_CNTL >> 2] = p[0];
    idx32 = p[0] & (1u << 11);
    n = p[0] >> 16;
    n = MIN(n, (count - 1) * (idx32 ? 1 : 2));
    fetch_arrays(s, &f);
    if (!draw_begin(s, &d)) {
        tex_release(&d);
        return;
    }
    int64_t t0 = stats_on() ? get_clock() : 0;

    draw_prims(&d, p[0], &f, p + 1, idx32, n);
    draw_end(&d);
    if (t0) {
        stats.ns += get_clock() - t0;
    }
    stats_report(&d);
}
