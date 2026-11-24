/*
 * QEMU ATI SVGA emulation
 * 2D engine functions
 *
 * Copyright (c) 2019 BALATON Zoltan
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#include "qemu/osdep.h"
#include "ati_int.h"
#include "ati_regs.h"
#include "ati_r300_gl.h"
#include "qemu/log.h"
#include "ui/pixel_ops.h"
#include "ui/console.h"

/*
 * NOTE:
 * This is 2D _acceleration_ and supposed to be fast. Therefore, don't try to
 * reinvent the wheel (unlikely to get better with a naive implementation than
 * existing libraries) and avoid (poorly) reimplementing gfx primitives.
 * That is unnecessary and would become a performance problem. Instead, try to
 * map to and reuse existing optimised facilities (e.g. pixman) wherever
 * possible.
 */

static int ati_bpp_from_datatype(ATIVGAState *s)
{
    switch (s->regs.dp_datatype & 0xf) {
    case 2:
        return 8;
    case 3:
    case 4:
        return 16;
    case 5:
        return 24;
    case 6:
        return 32;
    default:
        qemu_log_mask(LOG_UNIMP, "Unknown dst datatype %d\n",
                      s->regs.dp_datatype & 0xf);
        return 0;
    }
}

#define DEFAULT_CNTL (s->regs.dp_gui_master_cntl & GMC_DST_PITCH_OFFSET_CNTL)
#define DEFAULT_SRC_CNTL (s->regs.dp_gui_master_cntl & GMC_SRC_PITCH_OFFSET_CNTL)

/*
 * A rectangle of a surface the 2D engine works on. Surfaces are given as
 * card addresses: in video memory the rectangle is used in place, elsewhere
 * (the PCI GART, i.e. system memory) it is staged through a temporary copy.
 */
typedef struct ATI2DRect {
    uint8_t *bits;          /* top-left pixel */
    unsigned int stride;    /* bytes from one row to the next */
    bool in_vram;
    bool swapped;           /* in video memory's 32-bit swapped order */
    uint64_t vram_off;      /* offset of bits in video memory */
    uint64_t span;          /* bytes from the first pixel to the last */
    uint32_t mc;            /* card address of the top-left pixel */
    unsigned int mc_stride;
    uint8_t *tmp;
} ATI2DRect;

static bool ati_2d_rect_get(ATIVGAState *s, uint64_t base, unsigned int pitch,
                            unsigned int x, unsigned int y, unsigned int w,
                            unsigned int h, unsigned int bypp, bool load,
                            bool copy, ATI2DRect *r)
{
    uint64_t first = base + (uint64_t)y * pitch + (uint64_t)x * bypp;
    uint64_t span = (uint64_t)(h - 1) * pitch + (uint64_t)w * bypp;

    memset(r, 0, sizeof(*r));
    r->span = span;
    /* what the host GPU drew here goes to video memory first */
    if (ati_mc_vram_range(s, first, span, &r->vram_off)) {
        ati_r300_gl_sync_range(s, r->vram_off, span);
    }
    if (copy && ati_mc_vram_range(s, first, span, &r->vram_off)) {
        /* a private copy in card order, e.g. to be byte swapped */
        r->in_vram = true;
        r->tmp = g_malloc((size_t)w * bypp * h);
        r->bits = r->tmp;
        r->stride = w * bypp;
        r->mc_stride = pitch;
        for (unsigned int i = 0; load && i < h; i++) {
            ati_vram_read(s, r->vram_off + (uint64_t)i * pitch,
                          r->tmp + (size_t)i * r->stride, r->stride);
        }
        return true;
    }
    if (ati_mc_vram_range(s, first, span, &r->vram_off)) {
        r->in_vram = true;
        r->bits = s->vga.vram_ptr + r->vram_off;
        r->stride = pitch;
        if (s->vram_xor) {
            /*
             * Only whole 32-bit pixels can be moved in place in a swapped
             * order; anything else goes through a copy in card order.
             */
            if (s->vram_xor == 3 && bypp == 4 && !(r->vram_off & 3) &&
                !(pitch & 3)) {
                r->swapped = true;
                return true;
            }
            r->tmp = g_malloc((size_t)w * bypp * h);
            r->bits = r->tmp;
            r->stride = w * bypp;
            for (unsigned int i = 0; load && i < h; i++) {
                ati_vram_read(s, r->vram_off + (uint64_t)i * pitch,
                              r->tmp + (size_t)i * r->stride, r->stride);
            }
            r->mc_stride = pitch;
        }
        return true;
    }
    if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF ||
        first + span > 0x100000000ULL) {
        qemu_log_mask(LOG_GUEST_ERROR, "ati: blt rectangle at 0x%" PRIx64
                      " outside card memory\n", first);
        return false;
    }
    r->mc = first;
    r->mc_stride = pitch;
    r->stride = w * bypp;
    r->tmp = g_malloc((size_t)r->stride * h);
    r->bits = r->tmp;
    for (unsigned int i = 0; load && i < h; i++) {
        if (!ati_mc_read(s, first + (uint64_t)i * pitch,
                         r->tmp + (size_t)i * r->stride, r->stride)) {
            g_free(r->tmp);
            r->tmp = NULL;
            return false;
        }
    }
    return true;
}

/*
 * Byte swapping done by the bus interface on what the 2D engine reads from
 * (SRC_ENDIAN) or writes to (DST_ENDIAN) system memory: 1 swaps bytes in
 * 16-bit units, 2 in 32-bit units, 3 swaps 16-bit halves.
 */
static void ati_2d_swap(uint8_t *buf, size_t n, unsigned int mode)
{
    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint32_t v = ldl_he_p(buf + i);

        switch (mode) {
        case 1:
            v = ((v & 0x00ff00ff) << 8) | ((v >> 8) & 0x00ff00ff);
            break;
        case 2:
            v = bswap32(v);
            break;
        default:
            v = (v << 16) | (v >> 16);
            break;
        }
        stl_he_p(buf + i, v);
    }
}

/* Done with a rectangle; `store` if it was drawn to. */
static void ati_2d_rect_put(ATIVGAState *s, ATI2DRect *r, unsigned int w,
                            unsigned int h, unsigned int bypp, bool store)
{
    if (r->in_vram && store) {
        /* host copies of this memory are out of date */
        ati_r300_gl_changed(s, r->vram_off, r->span);
    }
    if (r->in_vram && r->tmp) {
        for (unsigned int i = 0; store && i < h; i++) {
            ati_vram_write(s, r->vram_off + (uint64_t)i * r->mc_stride,
                           r->tmp + (size_t)i * r->stride, r->stride);
        }
        g_free(r->tmp);
        r->tmp = NULL;
        return;
    }
    if (r->in_vram) {
        if (store) {
            memory_region_set_dirty(&s->vga.vram, r->vram_off,
                                    (uint64_t)(h - 1) * r->stride + w * bypp);
        }
        return;
    }
    static int stats = -1;

    if (stats < 0) {
        stats = getenv("PPCOSX_ATI_3D_STATS") != NULL;
    }
    if (store && stats) {
        fprintf(stderr, "ati2d: blt to system memory 0x%08x %ux%u swap %u\n",
                r->mc, w, h, s->shadow[DST_ENDIAN >> 2]);
    }
    if (store && s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF &&
        (s->shadow[DST_ENDIAN >> 2] & 3)) {
        ati_2d_swap(r->tmp, (size_t)r->stride * h,
                    s->shadow[DST_ENDIAN >> 2] & 3);
    }
    for (unsigned int i = 0; store && i < h; i++) {
        ati_mc_write(s, r->mc + i * r->mc_stride,
                     r->tmp + (size_t)i * r->stride, r->stride);
    }
    g_free(r->tmp);
    r->tmp = NULL;
}

static void ati_2d_blt_do(ATIVGAState *s);

int64_t ati_2d_ns;              /* time spent in 2D operations */

void ati_2d_blt(ATIVGAState *s)
{
    int64_t t0 = get_clock();

    ati_2d_blt_do(s);
    ati_2d_ns += get_clock() - t0;
}

static void ati_2d_blt_do(ATIVGAState *s)
{
    bool r128 = s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF;
    unsigned int w = s->regs.dst_width, h = s->regs.dst_height;
    /*
     * The Rage 128 takes the right/bottom edge as the start when drawing
     * right to left or bottom to top. Apple's Radeon driver always gives the
     * top-left corner and leaves DP_CNTL at 0, so there coordinates are
     * taken as top-left and the direction only matters for overlap, which
     * the copy below handles itself.
     */
    bool ltr = !r128 || (s->regs.dp_cntl & DST_X_LEFT_TO_RIGHT);
    bool ttb = !r128 || (s->regs.dp_cntl & DST_Y_TOP_TO_BOTTOM);
    unsigned int dst_x = ltr ? s->regs.dst_x : s->regs.dst_x + 1 - w;
    unsigned int dst_y = ttb ? s->regs.dst_y : s->regs.dst_y + 1 - h;
    int bpp = ati_bpp_from_datatype(s);
    unsigned int bypp = bpp / 8;
    ATI2DRect dst;

    if (!bpp) {
        qemu_log_mask(LOG_GUEST_ERROR, "Invalid bpp\n");
        return;
    }
    if (!w || !h) {
        return;
    }
    unsigned int dst_pitch = DEFAULT_CNTL ? s->regs.dst_pitch
                                          : s->regs.default_pitch;
    uint64_t dst_base = DEFAULT_CNTL ? s->regs.dst_offset
                                     : s->regs.default_offset;
    if (!dst_pitch) {
        qemu_log_mask(LOG_GUEST_ERROR, "Zero dest pitch\n");
        return;
    }
    if (r128) {
        /* pitch in units of 8 pixels; offsets are absolute card addresses */
        dst_pitch *= bpp;
    }
    if (dst_x > 0x3fff || dst_y > 0x3fff) {
        qemu_log_mask(LOG_GUEST_ERROR, "ati: blt start outside surface\n");
        return;
    }

    /*
     * The source is host data: the pixels follow through the HOST_DATA
     * registers (ati_2d_host_data), so only the destination is set up now.
     */
    if (((s->regs.dp_mix >> 8) & 7) == (DP_SRC_HOST >> 8) ||
        ((s->regs.dp_mix >> 8) & 7) == (DP_SRC_HOST_BYTEALIGN >> 8)) {
        ATIHostBlt *hb = &s->host_blt;

        g_free(hb->buf);
        memset(hb, 0, sizeof(*hb));
        if ((s->regs.dp_mix & GMC_ROP3_MASK) != ROP3_SRCCOPY ||
            (s->regs.dp_datatype & 0x30000) != 0x30000) {
            static bool warned;

            if (!warned) {
                warned = true;
                qemu_log_mask(LOG_UNIMP, "ati: host data blit with rop %x "
                              "datatype %08x not implemented\n",
                              (s->regs.dp_mix & GMC_ROP3_MASK) >> 16,
                              s->regs.dp_datatype);
            }
            return;
        }
        hb->base = dst_base;
        hb->pitch = dst_pitch;
        hb->x = dst_x;
        hb->y = dst_y;
        hb->w = w;
        hb->h = h;
        hb->bypp = bypp;
        hb->cx0 = 0;
        hb->cy0 = 0;
        hb->cx1 = hb->cy1 = 0x4000;
        if (s->regs.dp_gui_master_cntl & GMC_DST_CLIPPING) {
            uint32_t tl = s->shadow[SC_TOP_LEFT >> 2];
            uint32_t br = s->shadow[SC_BOTTOM_RIGHT >> 2];

            hb->cx0 = tl & 0x3fff;
            hb->cy0 = (tl >> 16) & 0x3fff;
            hb->cx1 = (br & 0x3fff) + r128;     /* Rage 128: inclusive */
            hb->cy1 = ((br >> 16) & 0x3fff) + r128;
        }
        hb->len = (size_t)w * h * bypp;
        hb->buf = g_malloc0(hb->len);
        hb->active = true;
        return;
    }

    /*
     * Clip to the scissor (SC_TOP_LEFT to SC_BOTTOM_RIGHT; exclusive on the
     * Radeon, inclusive on the Rage 128) when asked; the source moves with
     * the destination.
     */
    unsigned int clip_dx = 0, clip_dy = 0;
    if (s->regs.dp_gui_master_cntl & GMC_DST_CLIPPING) {
        uint32_t tl = s->shadow[SC_TOP_LEFT >> 2];
        uint32_t br = s->shadow[SC_BOTTOM_RIGHT >> 2];
        unsigned int cx0 = tl & 0x3fff, cy0 = (tl >> 16) & 0x3fff;
        unsigned int cx1 = (br & 0x3fff) + r128;
        unsigned int cy1 = ((br >> 16) & 0x3fff) + r128;
        unsigned int x1 = MIN(dst_x + w, cx1), y1 = MIN(dst_y + h, cy1);

        if (dst_x < cx0) {
            clip_dx = cx0 - dst_x;
            dst_x = cx0;
        }
        if (dst_y < cy0) {
            clip_dy = cy0 - dst_y;
            dst_y = cy0;
        }
        if (x1 <= dst_x || y1 <= dst_y) {
            return;
        }
        w = x1 - dst_x;
        h = y1 - dst_y;
    }

    static int tl_on = -1;
    static uint32_t tl_lo, tl_hi;

    if (tl_on < 0) {
        const char *e = getenv("PPCOSX_R300_TARGETLOG");

        tl_on = e && sscanf(e, "%x-%x", &tl_lo, &tl_hi) == 2;
    }
    if (tl_on) {
        if (dst_base >= tl_lo && dst_base < tl_hi) {
            fprintf(stderr, "ati2d: into %08" PRIx64 " pitch %u at %u,%u %ux%u"
                    " rop %02x src %08" PRIx64 " pitch %u at %u,%u gmc %08x"
                    " datatype %08x bpp %d\n",
                    dst_base, dst_pitch, dst_x, dst_y, w, h,
                    (s->regs.dp_mix & GMC_ROP3_MASK) >> 16,
                    DEFAULT_SRC_CNTL ? (uint64_t)s->regs.src_offset
                                     : (uint64_t)s->regs.default_offset,
                    DEFAULT_SRC_CNTL ? s->regs.src_pitch
                                     : s->regs.default_pitch,
                    s->regs.src_x, s->regs.src_y,
                    s->regs.dp_gui_master_cntl, s->regs.dp_datatype, bpp);
        }
    }

    switch (s->regs.dp_mix & GMC_ROP3_MASK) {
    case ROP3_SRCCOPY:
    {
        unsigned int src_x = (ltr ? s->regs.src_x : s->regs.src_x + 1 - w) +
                             clip_dx;
        unsigned int src_y = (ttb ? s->regs.src_y : s->regs.src_y + 1 - h) +
                             clip_dy;
        unsigned int src_pitch = DEFAULT_SRC_CNTL ? s->regs.src_pitch
                                                  : s->regs.default_pitch;
        uint64_t src_base = DEFAULT_SRC_CNTL ? s->regs.src_offset
                                             : s->regs.default_offset;
        ATI2DRect src;

        if (!src_pitch) {
            qemu_log_mask(LOG_GUEST_ERROR, "Zero source pitch\n");
            return;
        }
        if (r128) {
            src_pitch *= bpp;
        }
        if (src_x > 0x3fff || src_y > 0x3fff) {
            qemu_log_mask(LOG_GUEST_ERROR, "ati: blt source outside "
                          "surface\n");
            return;
        }
        /*
         * SRC_ENDIAN: byte swapping of what is read over the bus, i.e. a
         * source in system memory (the PCI GART), as for texture uploads.
         */
        unsigned int src_swap = r128 ? 0 : s->shadow[SRC_ENDIAN >> 2] & 3;

        if (!ati_2d_rect_get(s, src_base, src_pitch, src_x, src_y, w, h,
                             bypp, true, false, &src)) {
            return;
        }
        if (src_swap && !src.in_vram) {
            ati_2d_swap(src.bits, (size_t)src.stride * h, src_swap);
        }
        if (!ati_2d_rect_get(s, dst_base, dst_pitch, dst_x, dst_y, w, h,
                             bypp, false, false, &dst)) {
            ati_2d_rect_put(s, &src, w, h, bypp, false);
            return;
        }
        /* Overlapping copies within video memory: go against the overlap. */
        bool up = dst.bits > src.bits;
        for (unsigned int i = 0; i < h; i++) {
            unsigned int row = up ? h - 1 - i : i;
            uint8_t *d = dst.bits + (size_t)row * dst.stride;
            uint8_t *sp = src.bits + (size_t)row * src.stride;

            if (src.swapped == dst.swapped) {
                memmove(d, sp, w * bypp);
            } else {
                /* 32-bit pixels between swapped and card order */
                for (unsigned int x = 0; x < w; x++) {
                    stl_he_p(d + x * 4, bswap32(ldl_he_p(sp + x * 4)));
                }
            }
        }
        ati_2d_rect_put(s, &src, w, h, bypp, false);
        ati_2d_rect_put(s, &dst, w, h, bypp, true);
        s->regs.dst_x = ltr ? dst_x + w : dst_x;
        s->regs.dst_y = ttb ? dst_y + h : dst_y;
        break;
    }
    case ROP3_PATCOPY:
    case ROP3_BLACKNESS:
    case ROP3_WHITENESS:
    {
        uint32_t filler = 0;

        switch (s->regs.dp_mix & GMC_ROP3_MASK) {
        case ROP3_PATCOPY:
            filler = s->regs.dp_brush_frgd_clr;
            break;
        case ROP3_BLACKNESS:
            filler = 0xffUL << 24 | rgb_to_pixel32(s->vga.palette[0],
                     s->vga.palette[1], s->vga.palette[2]);
            break;
        case ROP3_WHITENESS:
            filler = 0xffUL << 24 | rgb_to_pixel32(s->vga.palette[3],
                     s->vga.palette[4], s->vga.palette[5]);
            break;
        }
        if (!ati_2d_rect_get(s, dst_base, dst_pitch, dst_x, dst_y, w, h,
                             bypp, false, false, &dst)) {
            return;
        }
        for (unsigned int x = 0; x < w; x++) {
            if (dst.swapped) {
                stl_be_p(dst.bits + x * 4, filler);
            } else {
                stn_le_p(dst.bits + x * bypp, bypp, filler);
            }
        }
        for (unsigned int y = 1; y < h; y++) {
            memcpy(dst.bits + (size_t)y * dst.stride, dst.bits, w * bypp);
        }
        ati_2d_rect_put(s, &dst, w, h, bypp, true);
        s->regs.dst_y = ttb ? dst_y + h : dst_y;
        break;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "Unimplemented ati_2d blt op %x\n",
                      (s->regs.dp_mix & GMC_ROP3_MASK) >> 16);
    }
}

/* Write a finished (or abandoned) host data blit to its destination. */
static void ati_2d_host_flush(ATIVGAState *s)
{
    ATIHostBlt *hb = &s->host_blt;
    unsigned int x0 = MAX(hb->x, hb->cx0), y0 = MAX(hb->y, hb->cy0);
    unsigned int x1 = MIN(hb->x + hb->w, hb->cx1);
    unsigned int y1 = MIN(hb->y + hb->h, hb->cy1);
    ATI2DRect dst;

    hb->active = false;
    if (x0 < x1 && y0 < y1 &&
        ati_2d_rect_get(s, hb->base, hb->pitch, x0, y0, x1 - x0, y1 - y0,
                        hb->bypp, false, false, &dst)) {
        for (unsigned int y = y0; y < y1; y++) {
            const uint8_t *sp = hb->buf +
                ((size_t)(y - hb->y) * hb->w + (x0 - hb->x)) * hb->bypp;
            uint8_t *d = dst.bits + (size_t)(y - y0) * dst.stride;

            for (unsigned int x = 0; x < x1 - x0; x++) {
                uint32_t v = ldn_le_p(sp + x * hb->bypp, hb->bypp);

                if (dst.swapped) {
                    stl_be_p(d + x * 4, v);
                } else {
                    stn_le_p(d + x * hb->bypp, hb->bypp, v);
                }
            }
        }
        ati_2d_rect_put(s, &dst, x1 - x0, y1 - y0, hb->bypp, true);
    }
    g_free(hb->buf);
    hb->buf = NULL;
}

/*
 * One HOST_DATA register write of a host data blit: the source pixels,
 * packed from one row to the next with no padding, the first pixel in the
 * word's most significant bits (Apple's driver uploads Quartz GL's glyphs
 * this way, 8-bit, into its glyph cache). The blit is written out when
 * the rectangle is full or at HOST_DATA_LAST.
 */
void ati_2d_host_data(ATIVGAState *s, uint32_t data, bool last)
{
    ATIHostBlt *hb = &s->host_blt;

    if (!hb->active) {
        return;
    }
    if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF &&
        (s->regs.dp_datatype & BIT(29))) {
        /*
         * Rage 128 HOST_BIG_ENDIAN_EN: the words arrive in the host's
         * (big-endian) byte order. Mac OS X 10.0 uploads textures so.
         */
        data = bswap32(data);
    }
    if (hb->bypp == 4) {
        if (hb->pos + 4 <= hb->len) {
            stl_le_p(hb->buf + hb->pos, data);
            hb->pos += 4;
        }
    } else {
        for (int i = 0; i < 4 / hb->bypp && hb->pos < hb->len; i++) {
            uint32_t v = data >> (32 - 8 * hb->bypp * (i + 1));

            stn_le_p(hb->buf + hb->pos, hb->bypp, v);
            hb->pos += hb->bypp;
        }
    }
    if (last || hb->pos >= hb->len) {
        ati_2d_host_flush(s);
    }
}
