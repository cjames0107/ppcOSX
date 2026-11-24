/*
 * QEMU ATI SVGA emulation
 *
 * Copyright (c) 2019 BALATON Zoltan
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

/*
 * WARNING:
 * This is very incomplete and only enough for Linux console and some
 * unaccelerated X output at the moment.
 * Currently it's little more than a frame buffer with minimal functions,
 * other more advanced features of the hardware are yet to be implemented.
 * We only aim for Rage 128 Pro (and some RV100) and 2D only at first,
 * No 3D at all yet (maybe after 2D works, but feel free to improve it)
 */

#include "qemu/osdep.h"
#include "ati_int.h"
#include "ati_regs.h"
#include "ati_r300_gl.h"
#include "vga-access.h"
#include "hw/qdev-properties.h"
#include "vga_regs.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "hw/display/i2c-ddc.h"
#include "trace.h"

#define ATI_DEBUG_HW_CURSOR 0

#ifdef CONFIG_PIXMAN
#define DEFAULT_X_PIXMAN 3
#else
#define DEFAULT_X_PIXMAN 0
#endif

static const struct {
    const char *name;
    uint16_t dev_id;
} ati_model_aliases[] = {
    { "rage128p", PCI_DEVICE_ID_ATI_RAGE128_PF },
    { "rv100", PCI_DEVICE_ID_ATI_RADEON_QY },
    { "rv350", PCI_DEVICE_ID_ATI_RADEON_AP },
};

enum { VGA_MODE, EXT_MODE };

/* ---- video memory byte order (Radeon apertures) ----------------------- */

void ati_vram_read(ATIVGAState *s, uint64_t off, uint8_t *buf, uint32_t len)
{
    if (!s->vram_xor) {
        memcpy(buf, s->vga.vram_ptr + off, len);
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = s->vga.vram_ptr[(off + i) ^ s->vram_xor];
    }
}

void ati_vram_write(ATIVGAState *s, uint64_t off, const uint8_t *buf,
                    uint32_t len)
{
    if (!s->vram_xor) {
        memcpy(s->vga.vram_ptr + off, buf, len);
    } else {
        for (uint32_t i = 0; i < len; i++) {
            s->vga.vram_ptr[(off + i) ^ s->vram_xor] = buf[i];
        }
    }
    memory_region_set_dirty(&s->vga.vram, off & ~3ULL, len + 4);
}

static unsigned int ati_swap_xor(uint32_t cntl, uint32_t sw16, uint32_t sw32)
{
    return cntl & sw32 ? 3 : cntl & sw16 ? 1 : 0;
}

/* Reorder all of video memory from byte order `from` to `to`. */
static void ati_vram_reorder(ATIVGAState *s, unsigned int from,
                             unsigned int to)
{
    uint8_t *p = s->vga.vram_ptr;
    uint64_t n = s->vga.vram_size;

    if (from == to) {
        return;
    }
    if (from == 3) {
        for (uint64_t i = 0; i < n; i += 4) {
            stl_he_p(p + i, bswap32(ldl_he_p(p + i)));
        }
    } else if (from == 1) {
        for (uint64_t i = 0; i < n; i += 2) {
            stw_he_p(p + i, bswap16(lduw_he_p(p + i)));
        }
    }
    if (to == 3) {
        for (uint64_t i = 0; i < n; i += 4) {
            stl_he_p(p + i, bswap32(ldl_he_p(p + i)));
        }
    } else if (to == 1) {
        for (uint64_t i = 0; i < n; i += 2) {
            stw_he_p(p + i, bswap16(lduw_he_p(p + i)));
        }
    }
    memory_region_set_dirty(&s->vga.vram, 0, n);
}

static void ati_update_surfaces(ATIVGAState *s);

/* SURFACE_CNTL changed: follow aperture 1's byte order. */
static void ati_update_apertures(ATIVGAState *s)
{
    uint32_t cntl = s->shadow[SURFACE_CNTL >> 2];
    unsigned int ap0, ap1;

    if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
        return;
    }
    ap0 = ati_swap_xor(cntl, NONSURF_AP0_SWP_16BPP, NONSURF_AP0_SWP_32BPP);
    ap1 = ati_swap_xor(cntl, NONSURF_AP1_SWP_16BPP, NONSURF_AP1_SWP_32BPP);
    memory_region_transaction_begin();
    ati_vram_reorder(s, s->vram_xor, ap1);
    s->vram_xor = ap1;
    s->aper0_xor = ap0 ^ ap1;
    memory_region_set_enabled(&s->vram_aper0, !s->aper0_xor);
    memory_region_transaction_commit();
    s->vga.big_endian_fb = s->vram_xor != 0;
    ati_update_surfaces(s);
}

/* Aperture 0 when its byte order differs from aperture 1's. */
static uint64_t ati_aper0_read(void *opaque, hwaddr addr, unsigned int size)
{
    ATIVGAState *s = opaque;
    uint64_t val = 0;

    for (unsigned int i = 0; i < size; i++) {
        val |= (uint64_t)s->vga.vram_ptr[(addr + i) ^ s->aper0_xor] << (8 * i);
    }
    return val;
}

static void ati_aper0_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned int size)
{
    ATIVGAState *s = opaque;

    for (unsigned int i = 0; i < size; i++) {
        s->vga.vram_ptr[(addr + i) ^ s->aper0_xor] = val >> (8 * i);
    }
    memory_region_set_dirty(&s->vga.vram, addr & ~3ULL, 8);
}

static const MemoryRegionOps ati_aper0_ops = {
    .read = ati_aper0_read,
    .write = ati_aper0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

/* A SURFACEn range seen through an aperture in a byte order of its own. */
static uint64_t ati_surf_read(void *opaque, hwaddr addr, unsigned int size)
{
    struct ATISurfaceWindow *w = opaque;
    uint8_t *p = w->s->vga.vram_ptr;
    uint64_t val = 0;

    addr += w->base;
    for (unsigned int i = 0; i < size; i++) {
        val |= (uint64_t)p[(addr + i) ^ w->xor] << (8 * i);
    }
    return val;
}

static void ati_surf_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned int size)
{
    struct ATISurfaceWindow *w = opaque;
    uint8_t *p = w->s->vga.vram_ptr;

    addr += w->base;
    for (unsigned int i = 0; i < size; i++) {
        p[(addr + i) ^ w->xor] = val >> (8 * i);
    }
    memory_region_set_dirty(&w->s->vga.vram, addr & ~3ULL, 8);
}

static const MemoryRegionOps ati_surf_ops = {
    .read = ati_surf_read,
    .write = ati_surf_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

/*
 * SURFACEn_LOWER_BOUND/UPPER_BOUND/INFO (0xb04 + 16n): CPU accesses
 * through either aperture inside [lower, upper] use the surface's byte
 * order (INFO bits 20-23) instead of SURFACE_CNTL's. Apple's 10.1 RV100
 * GL driver reads its 32-bit back buffer this way with a 16-bit desktop.
 */
static void ati_update_surfaces(ATIVGAState *s)
{
    uint32_t cntl = s->shadow[SURFACE_CNTL >> 2];

    if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
        return;
    }
    memory_region_transaction_begin();
    for (int a = 0; a < 2; a++) {
        unsigned int global = a ?
            ati_swap_xor(cntl, NONSURF_AP1_SWP_16BPP, NONSURF_AP1_SWP_32BPP) :
            ati_swap_xor(cntl, NONSURF_AP0_SWP_16BPP, NONSURF_AP0_SWP_32BPP);

        for (int n = 0; n < 8; n++) {
            struct ATISurfaceWindow *w = &s->surf_win[a][n];
            uint32_t lower = s->shadow[(0xb04 + n * 16) >> 2];
            uint32_t upper = s->shadow[(0xb08 + n * 16) >> 2];
            uint32_t lo = lower & ~0xfffu, hi = upper | 0xfffu;
            uint32_t info = s->shadow[(0xb0c + n * 16) >> 2];
            unsigned int order = a ?
                ati_swap_xor(info, 1u << 22, 1u << 23) :
                ati_swap_xor(info, 1u << 20, 1u << 21);
            /*
             * Bounds both zero is an unused surface, not one over the first
             * page: Mac OS X 10.4/10.5's Radeon drivers leave all eight so
             * and write the hardware cursor at offset 0 through aperture 0,
             * whose byte swapping must apply there too.
             */
            bool on = (lower | upper) && hi > lo && lo < s->vga.vram_size &&
                      order != global;

            if (on) {
                hi = MIN(hi, s->vga.vram_size - 1);
                w->base = lo;
                w->xor = order ^ s->vram_xor;
                memory_region_set_size(&w->mr, (uint64_t)hi - lo + 1);
                memory_region_set_address(&w->mr,
                                          a * s->vga.vram_size + lo);
            }
            memory_region_set_enabled(&w->mr, on);
        }
    }
    memory_region_transaction_commit();
}

static void ati_vga_switch_mode(ATIVGAState *s)
{
    DPRINTF("%d -> %d\n",
            s->mode, !!(s->regs.crtc_gen_cntl & CRTC2_EXT_DISP_EN));
    if (s->regs.crtc_gen_cntl & CRTC2_EXT_DISP_EN) {
        /* Extended mode enabled */
        s->mode = EXT_MODE;
        if (s->regs.crtc_gen_cntl & CRTC2_EN) {
            /* CRT controller enabled, use CRTC values */
            /* FIXME Should these be the same as VGA CRTC regs? */
            uint32_t offs = s->regs.crtc_offset & 0x07ffffff;
            int stride = (s->regs.crtc_pitch & 0x7ff) * 8;
            int bpp = 0;
            int h, v;

            if (s->regs.crtc_h_total_disp == 0) {
                s->regs.crtc_h_total_disp = ((640 / 8) - 1) << 16;
            }
            if (s->regs.crtc_v_total_disp == 0) {
                s->regs.crtc_v_total_disp = (480 - 1) << 16;
            }
            h = ((s->regs.crtc_h_total_disp >> 16) + 1) * 8;
            v = (s->regs.crtc_v_total_disp >> 16) + 1;
            switch (s->regs.crtc_gen_cntl & CRTC_PIX_WIDTH_MASK) {
            case CRTC_PIX_WIDTH_4BPP:
                bpp = 4;
                break;
            case CRTC_PIX_WIDTH_8BPP:
                bpp = 8;
                break;
            case CRTC_PIX_WIDTH_15BPP:
                bpp = 15;
                break;
            case CRTC_PIX_WIDTH_16BPP:
                bpp = 16;
                break;
            case CRTC_PIX_WIDTH_24BPP:
                bpp = 24;
                break;
            case CRTC_PIX_WIDTH_32BPP:
                bpp = 32;
                break;
            default:
                qemu_log_mask(LOG_UNIMP, "Unsupported bpp value\n");
                return;
            }
            DPRINTF("Switching to %dx%d %d %d @ %x\n", h, v, stride, bpp, offs);
            vbe_ioport_write_index(&s->vga, 0, VBE_DISPI_INDEX_ENABLE);
            vbe_ioport_write_data(&s->vga, 0, VBE_DISPI_DISABLED);
            if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
                s->vga.big_endian_fb = (s->regs.config_cntl & APER_0_ENDIAN ||
                                        s->regs.config_cntl & APER_1_ENDIAN ?
                                        true : false);
            } else {
                s->vga.big_endian_fb = s->vram_xor != 0;
            }
            /* reset VBE regs then set up mode */
            s->vga.vbe_regs[VBE_DISPI_INDEX_XRES] = h;
            s->vga.vbe_regs[VBE_DISPI_INDEX_YRES] = v;
            s->vga.vbe_regs[VBE_DISPI_INDEX_BPP] = bpp;
            /* enable mode via ioport so it updates vga regs */
            vbe_ioport_write_index(&s->vga, 0, VBE_DISPI_INDEX_ENABLE);
            vbe_ioport_write_data(&s->vga, 0, VBE_DISPI_ENABLED |
                VBE_DISPI_LFB_ENABLED | VBE_DISPI_NOCLEARMEM |
                (s->regs.dac_cntl & DAC_8BIT_EN ? VBE_DISPI_8BIT_DAC : 0));
            /* now set offset and stride after enable as that resets these */
            if (stride) {
                int bypp = DIV_ROUND_UP(bpp, BITS_PER_BYTE);

                vbe_ioport_write_index(&s->vga, 0, VBE_DISPI_INDEX_VIRT_WIDTH);
                vbe_ioport_write_data(&s->vga, 0, stride);
                stride *= bypp;
                if (offs % stride) {
                    DPRINTF("CRTC offset is not multiple of pitch\n");
                    vbe_ioport_write_index(&s->vga, 0,
                                           VBE_DISPI_INDEX_X_OFFSET);
                    vbe_ioport_write_data(&s->vga, 0, offs % stride / bypp);
                }
                vbe_ioport_write_index(&s->vga, 0, VBE_DISPI_INDEX_Y_OFFSET);
                vbe_ioport_write_data(&s->vga, 0, offs / stride);
                DPRINTF("VBE offset (%d,%d), vbe_start_addr=%x\n",
                        s->vga.vbe_regs[VBE_DISPI_INDEX_X_OFFSET],
                        s->vga.vbe_regs[VBE_DISPI_INDEX_Y_OFFSET],
                        s->vga.vbe_start_addr);
            }
        }
    } else {
        /* VGA mode enabled */
        s->mode = VGA_MODE;
        vbe_ioport_write_index(&s->vga, 0, VBE_DISPI_INDEX_ENABLE);
        vbe_ioport_write_data(&s->vga, 0, VBE_DISPI_DISABLED);
    }
}

/*
 * CRTC_OFFSET changed in an extended mode: move the scanout there at once
 * (page flips, as Quartz Extreme's compositor does between its buffers).
 */
static void ati_vga_update_offset(ATIVGAState *s)
{
    uint32_t offs = s->regs.crtc_offset & 0x07ffffff;
    int bypp = DIV_ROUND_UP(s->vga.vbe_regs[VBE_DISPI_INDEX_BPP],
                            BITS_PER_BYTE);
    int stride = (s->regs.crtc_pitch & 0x7ff) * 8 * bypp;

    if (s->mode != EXT_MODE || !bypp || !stride) {
        return;
    }
    vbe_ioport_write_index(&s->vga, 0, VBE_DISPI_INDEX_X_OFFSET);
    vbe_ioport_write_data(&s->vga, 0, offs % stride / bypp);
    vbe_ioport_write_index(&s->vga, 0, VBE_DISPI_INDEX_Y_OFFSET);
    vbe_ioport_write_data(&s->vga, 0, offs / stride);
}

/* CRTC_GEN_CNTL cursor mode: 0 is 2bpp mono, others are 32bpp colour */
static unsigned ati_cursor_mode(ATIVGAState *s)
{
    return s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF ? 0 :
           (s->regs.crtc_gen_cntl >> 20) & 7;
}

/*
 * 32bpp colour cursor (Radeon CRTC_CUR_MODE 1-3): 64x64 pixels, 256 bytes a
 * row, each a little-endian ARGB word with straight alpha, which is what
 * QEMUCursor holds (mode 1 has no alpha). Rows start at CUR_OFFSET; the
 * image is placed at CUR_HORZ_VERT_POSN less CUR_HORZ_VERT_OFF (see
 * ati_cursor_update_pos). Drivers may rewrite the image in place, so the
 * refresh path calls this with force false to pick up changes.
 */
/* PPCOSX_ATI_CURSOR_DUMP=file.ppm: the last cursor defined, over grey */
static void ati_cursor_dump(QEMUCursor *c)
{
    const char *path = getenv("PPCOSX_ATI_CURSOR_DUMP");
    FILE *f;
    int i;

    if (!path || !(f = fopen(path, "wb"))) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", c->width, c->height);
    for (i = 0; i < c->width * c->height; i++) {
        uint32_t v = c->data[i], a = v >> 24;
        uint8_t px[3];
        int k;
        for (k = 0; k < 3; k++) {
            uint32_t ch = (v >> (16 - 8 * k)) & 0xff;
            px[k] = (ch * a + 0x80 * (255 - a)) / 255;
        }
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

static void ati_cursor_define_argb(ATIVGAState *s, bool force)
{
    uint32_t base = s->regs.cur_offset & 0x07fffff0;
    unsigned offs_x = 0;
    unsigned mode = ati_cursor_mode(s);
    uint32_t sum = mode;
    uint8_t row[256];
    int x, y;

    if (!s->cursor) {
        s->cursor = cursor_alloc(64, 64);
    }
    for (y = 0; y < 64; y++) {
        ati_vram_read(s, (base + y * 256) & (s->vga.vram_size - 1), row, 256);
        for (x = 0; x < 64; x++) {
            uint32_t v = x + offs_x < 64 ? ldl_le_p(row + (x + offs_x) * 4)
                                         : 0;
            if (mode == 1) {
                v |= 0xff000000;
            }
            sum = (sum * 31) ^ v;
            if (force) {
                s->cursor->data[y * 64 + x] = v;
            } else if (s->cursor->data[y * 64 + x] != v) {
                force = true;
                s->cursor->data[y * 64 + x] = v;
            }
        }
    }
    if (force || sum != s->cursor_sum) {
        s->cursor_sum = sum;
        s->cursor->hot_x = 0;
        s->cursor->hot_y = 0;
        dpy_cursor_define(s->vga.con, s->cursor);
        ati_cursor_dump(s->cursor);
    }
}

/*
 * Tell the UI where the host-drawn cursor is and whether it shows. Drivers
 * move it with the update lock (bit 31) held and release the lock through
 * any of the cursor registers, so this runs on every refresh and sends
 * changes, rather than on particular register writes.
 */
static void ati_cursor_update_pos(ATIVGAState *s)
{
    bool on = (s->regs.crtc_gen_cntl & CRTC2_CUR_EN) &&
              !(s->regs.cur_offset & BIT(31));
    int x = (int)(s->regs.cur_hv_pos >> 16) - (int)(s->regs.cur_hv_offs >> 16);
    int y = (int)(s->regs.cur_hv_pos & 0xffff) -
            (int)(s->regs.cur_hv_offs & 0xffff);

    if (s->regs.cur_offset & BIT(31)) {
        return;                 /* locked: keep what was shown */
    }
    if (on == s->cursor_shown && x == s->cursor_x && y == s->cursor_y) {
        return;
    }
    s->cursor_shown = on;
    s->cursor_x = x;
    s->cursor_y = y;
    static int log_on = -1;

    if (log_on < 0) {
        log_on = getenv("PPCOSX_ATI_CURSOR_DUMP") != NULL;
    }
    if (log_on) {
        fprintf(stderr, "ati: cursor %s at %d,%d\n", on ? "on" : "off", x, y);
    }
    dpy_mouse_set(s->vga.con, x, y, on);
}

static void ati_gfx_update(void *opaque)
{
    ATIVGAState *s = container_of(opaque, ATIVGAState, vga);

    if (!s->cursor_guest_mode) {
        if ((s->regs.crtc_gen_cntl & CRTC2_CUR_EN) &&
            !(s->regs.cur_offset & BIT(31)) && ati_cursor_mode(s)) {
            ati_cursor_define_argb(s, false);
        }
        ati_cursor_update_pos(s);
    }
    s->vga_hw_ops->gfx_update(opaque);
}

/*
 * A byte of the monochrome cursor image. The Rage 128's display driver
 * writes it a byte at a time through the CPU's view of video memory, so on
 * that card it is read in the CPU's byte order, not the engines' (see
 * vram_xor); swapping would shuffle each row's 8-pixel groups.
 */
static uint8_t ati_cursor_byte(ATIVGAState *s, uint64_t off)
{
    if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
        return s->vga.vram_ptr[off];
    }
    return ati_vram_byte(s, off);
}

/* Used by host side hardware cursor */
static void ati_cursor_define(ATIVGAState *s)
{
    uint8_t data[1024];
    uint32_t srcoff;
    int i, j, idx = 0;

    if ((s->regs.cur_offset & BIT(31)) || s->cursor_guest_mode) {
        return; /* Do not update cursor if locked or rendered by guest */
    }
    if (ati_cursor_mode(s)) {
        ati_cursor_define_argb(s, true);
        return;
    }
    /* FIXME handle cur_hv_offs correctly */
    srcoff = s->regs.cur_offset -
        (s->regs.cur_hv_offs >> 16) - (s->regs.cur_hv_offs & 0xffff) * 16;
    for (i = 0; i < 64; i++) {
        for (j = 0; j < 8; j++, idx++) {
            data[idx] = ati_cursor_byte(s, (srcoff + i * 16 + j) &
                                      (s->vga.vram_size - 1));
            data[512 + idx] = ati_cursor_byte(s, (srcoff + i * 16 + j + 8) &
                                            (s->vga.vram_size - 1));
        }
    }
    if (!s->cursor) {
        s->cursor = cursor_alloc(64, 64);
    }
    cursor_set_mono(s->cursor, s->regs.cur_color1, s->regs.cur_color0,
                    &data[512], 1, &data[0]);
    dpy_cursor_define(s->vga.con, s->cursor);
}

/* Alternatively support guest rendered hardware cursor */
static void ati_cursor_invalidate(VGACommonState *vga)
{
    ATIVGAState *s = container_of(vga, ATIVGAState, vga);
    int size = (s->regs.crtc_gen_cntl & CRTC2_CUR_EN) ? 64 : 0;

    if (s->regs.cur_offset & BIT(31)) {
        return; /* Do not update cursor if locked */
    }
    if (s->cursor_size != size ||
        vga->hw_cursor_x != s->regs.cur_hv_pos >> 16 ||
        vga->hw_cursor_y != (s->regs.cur_hv_pos & 0xffff) ||
        s->cursor_offset != s->regs.cur_offset - (s->regs.cur_hv_offs >> 16) -
        (s->regs.cur_hv_offs & 0xffff) * 16) {
        /* Remove old cursor then update and show new one if needed */
        vga_invalidate_scanlines(vga, vga->hw_cursor_y, vga->hw_cursor_y + 63);
        vga->hw_cursor_x = s->regs.cur_hv_pos >> 16;
        vga->hw_cursor_y = s->regs.cur_hv_pos & 0xffff;
        s->cursor_offset = s->regs.cur_offset - (s->regs.cur_hv_offs >> 16) -
                           (s->regs.cur_hv_offs & 0xffff) * 16;
        s->cursor_size = size;
        if (size) {
            vga_invalidate_scanlines(vga,
                                     vga->hw_cursor_y, vga->hw_cursor_y + 63);
        }
    }
}

static void ati_cursor_draw_line(VGACommonState *vga, uint8_t *d, int scr_y)
{
    ATIVGAState *s = container_of(vga, ATIVGAState, vga);
    uint32_t srcoff;
    uint32_t *dp = (uint32_t *)d;
    int i, j, h;

    if (!(s->regs.crtc_gen_cntl & CRTC2_CUR_EN) ||
        scr_y < vga->hw_cursor_y || scr_y >= vga->hw_cursor_y + 64 ||
        scr_y > s->regs.crtc_v_total_disp >> 16) {
        return;
    }
    /* FIXME handle cur_hv_offs correctly */
    srcoff = s->cursor_offset + (scr_y - vga->hw_cursor_y) * 16;
    dp = &dp[vga->hw_cursor_x];
    h = ((s->regs.crtc_h_total_disp >> 16) + 1) * 8;
    for (i = 0; i < 8; i++) {
        uint32_t color;
        uint8_t abits = ati_cursor_byte(s, (srcoff + i) & (vga->vram_size - 1));
        uint8_t xbits = ati_cursor_byte(s, (srcoff + i + 8) &
                                      (vga->vram_size - 1));
        for (j = 0; j < 8; j++, abits <<= 1, xbits <<= 1) {
            if (abits & BIT(7)) {
                if (xbits & BIT(7)) {
                    color = dp[i * 8 + j] ^ 0xffffffff; /* complement */
                } else {
                    continue; /* transparent, no change */
                }
            } else {
                color = (xbits & BIT(7) ? s->regs.cur_color1 :
                                          s->regs.cur_color0) | 0xff000000;
            }
            if (vga->hw_cursor_x + i * 8 + j >= h) {
                return; /* end of screen, don't span to next line */
            }
            dp[i * 8 + j] = color;
        }
    }
}

static uint64_t ati_i2c(bitbang_i2c_interface *i2c, uint64_t data, int base)
{
    bool c = (data & BIT(base + 17) ? !!(data & BIT(base + 1)) : 1);
    bool d = (data & BIT(base + 16) ? !!(data & BIT(base)) : 1);

    bitbang_i2c_set(i2c, BITBANG_I2C_SCL, c);
    d = bitbang_i2c_set(i2c, BITBANG_I2C_SDA, d);

    data &= ~0xf00ULL;
    if (c) {
        data |= BIT(base + 9);
    }
    if (d) {
        data |= BIT(base + 8);
    }
    return data;
}

static void ati_vga_update_irq(ATIVGAState *s)
{
    pci_set_irq(&s->dev, !!(s->regs.gen_int_status & s->regs.gen_int_cntl));
}

static void ati_vga_vblank_irq(void *opaque)
{
    ATIVGAState *s = opaque;

    timer_mod(&s->vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 60);
    s->regs.gen_int_status |= CRTC_VBLANK_INT;
    ati_vga_update_irq(s);
}

static inline uint64_t ati_reg_read_offs(uint32_t reg, int offs,
                                         unsigned int size)
{
    if (offs == 0 && size == 4) {
        return reg;
    } else {
        return extract32(reg, offs * BITS_PER_BYTE, size * BITS_PER_BYTE);
    }
}

/* PLL registers (CLOCK_CNTL_DATA with CLOCK_CNTL_INDEX). */
static uint32_t ati_pll_read(ATIVGAState *s, unsigned int idx)
{
    return s->pll[idx & 0x3f];
}

static void ati_pll_write(ATIVGAState *s, unsigned int idx, uint32_t val)
{
    switch (idx & 0x3f) {
    case PPLL_REF_DIV:
        /*
         * Bit 15 written is PPLL_ATOMIC_UPDATE_W, read is
         * PPLL_ATOMIC_UPDATE_R: the new dividers are applied at once, so
         * the update is never seen pending.
         */
        val &= ~BIT(15);
        break;
    }
    s->pll[idx & 0x3f] = val;
}

static uint64_t ati_mm_read(void *opaque, hwaddr addr, unsigned int size)
{
    ATIVGAState *s = opaque;
    uint64_t val = 0;

    switch (addr) {
    case MM_INDEX:
        val = s->regs.mm_index;
        break;
    case MM_DATA ... MM_DATA + 3:
        /* indexed access to regs or memory */
        if (s->regs.mm_index & BIT(31)) {
            uint32_t idx = s->regs.mm_index & ~BIT(31);
            if (idx <= s->vga.vram_size - size) {
                uint8_t b[8];

                ati_vram_read(s, idx, b, size);
                val = ldn_le_p(b, size);
            }
        } else if (s->regs.mm_index > MM_DATA + 3) {
            val = ati_mm_read(s, s->regs.mm_index + addr - MM_DATA, size);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                "ati_mm_read: mm_index too small: %u\n", s->regs.mm_index);
        }
        break;
    case BIOS_0_SCRATCH ... BUS_CNTL - 1:
    {
        int i = (addr - BIOS_0_SCRATCH) / 4;
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF && i > 3) {
            break;
        }
        val = ati_reg_read_offs(s->regs.bios_scratch[i],
                                addr - (BIOS_0_SCRATCH + i * 4), size);
        break;
    }
    case GEN_INT_CNTL:
        val = s->regs.gen_int_cntl;
        break;
    case GEN_INT_STATUS:
        val = s->regs.gen_int_status;
        break;
    case CRTC_GEN_CNTL ... CRTC_GEN_CNTL + 3:
        val = ati_reg_read_offs(s->regs.crtc_gen_cntl,
                                addr - CRTC_GEN_CNTL, size);
        break;
    case CRTC_EXT_CNTL ... CRTC_EXT_CNTL + 3:
        val = ati_reg_read_offs(s->regs.crtc_ext_cntl,
                                addr - CRTC_EXT_CNTL, size);
        break;
    case DAC_CNTL:
        val = s->regs.dac_cntl;
        break;
    case GPIO_VGA_DDC ... GPIO_VGA_DDC + 3:
        val = ati_reg_read_offs(s->regs.gpio_vga_ddc,
                                addr - GPIO_VGA_DDC, size);
        break;
    case GPIO_DVI_DDC ... GPIO_DVI_DDC + 3:
        val = ati_reg_read_offs(s->regs.gpio_dvi_ddc,
                                addr - GPIO_DVI_DDC, size);
        break;
    case GPIO_MONID ... GPIO_MONID + 3:
        val = ati_reg_read_offs(s->regs.gpio_monid,
                                addr - GPIO_MONID, size);
        break;
    case PALETTE_INDEX:
        /* FIXME unaligned access */
        val = vga_ioport_read(&s->vga, VGA_PEL_IR) << 16;
        val |= vga_ioport_read(&s->vga, VGA_PEL_IW) & 0xff;
        break;
    case PALETTE_DATA:
        val = vga_ioport_read(&s->vga, VGA_PEL_D);
        break;
    case PALETTE_30_DATA:
        val = s->regs.palette[vga_ioport_read(&s->vga, VGA_PEL_IR)];
        break;
    case CNFG_CNTL:
        val = s->regs.config_cntl;
        break;
    case CNFG_MEMSIZE:
        val = s->vga.vram_size;
        break;
    case CONFIG_APER_0_BASE:
    case CONFIG_APER_1_BASE:
        val = pci_default_read_config(&s->dev,
                                      PCI_BASE_ADDRESS_0, size) & 0xfffffff0;
        if (addr == CONFIG_APER_1_BASE &&
            s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF) {
            val += s->vga.vram_size;    /* right after aperture 0 */
        }
        break;
    case CONFIG_APER_SIZE:
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            val = s->vga.vram_size / 2;
        } else {
            val = s->vga.vram_size;     /* each aperture covers all VRAM */
        }
        break;
    case CONFIG_REG_1_BASE:
        val = pci_default_read_config(&s->dev,
                                      PCI_BASE_ADDRESS_2, size) & 0xfffffff0;
        break;
    case CONFIG_REG_APER_SIZE:
        val = memory_region_size(&s->mm) / 2;
        break;
    case HOST_PATH_CNTL:
        val = BIT(23); /* Radeon HDP_APER_CNTL */
        break;
    case CLOCK_CNTL_INDEX ... CLOCK_CNTL_INDEX + 3:
        val = ati_reg_read_offs(s->clock_cntl_index,
                                addr - CLOCK_CNTL_INDEX, size);
        break;
    case CLOCK_CNTL_DATA ... CLOCK_CNTL_DATA + 3:
        val = ati_reg_read_offs(ati_pll_read(s, s->clock_cntl_index),
                                addr - CLOCK_CNTL_DATA, size);
        break;
    case CP_CSQ_CNTL ... CP_CSQ_CNTL + 3:
        /*
         * Packets are executed as they arrive, so the queues are always
         * empty: CSQ_CNT_PRIMARY (bits 7:0) and CSQ_CNT_INDIRECT (15:8)
         * report every entry free.
         */
        val = ati_reg_read_offs((s->shadow[CP_CSQ_CNTL >> 2] & ~0xffff) |
                                0xc0c0, addr - CP_CSQ_CNTL, size);
        break;
    case MC_STATUS:
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            val = 5;
        } else {
            /* MEM_PWRUP_COMPL_A | MEM_PWRUP_COMPL_B | MC_IDLE */
            val = BIT(0) | BIT(1) | BIT(2);
        }
        break;
    case MEM_SDRAM_MODE_REG:
        if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF) {
            val = BIT(28) | BIT(20);
        }
        break;
    case 0x7b8:                         /* Rage 128 PM4_STAT */
        if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF) {
            val = s->shadow[addr >> 2];
            break;
        }
        /*
         * 192 free command FIFO entries, idle: 10.0's window server waits
         * for room in the FIFO here before each 2D operation.
         */
        val = 192;
        if (ati_cp_busy(s)) {
            val |= BIT(16) | BIT(31);       /* PM4_BUSY, GUI_ACTIVE */
        }
        break;
    case RBBM_STATUS:
    case GUI_STAT:
        val = 64; /* free CMDFIFO entries */
        if (ati_cp_busy(s)) {
            val |= BIT(31);     /* GUI_ACTIVE */
        }
        break;
    case CRTC_H_TOTAL_DISP:
        val = s->regs.crtc_h_total_disp;
        break;
    case CRTC_H_SYNC_STRT_WID:
        val = s->regs.crtc_h_sync_strt_wid;
        break;
    case CRTC_V_TOTAL_DISP:
        val = s->regs.crtc_v_total_disp;
        break;
    case CRTC_V_SYNC_STRT_WID:
        val = s->regs.crtc_v_sync_strt_wid;
        break;
    case CRTC_OFFSET:
        val = s->regs.crtc_offset;
        break;
    case CRTC_OFFSET_CNTL:
        val = s->regs.crtc_offset_cntl;
        break;
    case CRTC_PITCH:
        val = s->regs.crtc_pitch;
        break;
    case 0xf00 ... 0xfff:
        val = pci_default_read_config(&s->dev, addr - 0xf00, size);
        break;
    case CUR_OFFSET ... CUR_OFFSET + 3:
        val = ati_reg_read_offs(s->regs.cur_offset, addr - CUR_OFFSET, size);
        break;
    case CUR_HORZ_VERT_POSN ... CUR_HORZ_VERT_POSN + 3:
        val = ati_reg_read_offs(s->regs.cur_hv_pos,
                                addr - CUR_HORZ_VERT_POSN, size);
        if (addr + size > CUR_HORZ_VERT_POSN + 3) {
            val |= (s->regs.cur_offset & BIT(31)) >> (4 - size);
        }
        break;
    case CUR_HORZ_VERT_OFF ... CUR_HORZ_VERT_OFF + 3:
        val = ati_reg_read_offs(s->regs.cur_hv_offs,
                                addr - CUR_HORZ_VERT_OFF, size);
        if (addr + size > CUR_HORZ_VERT_OFF + 3) {
            val |= (s->regs.cur_offset & BIT(31)) >> (4 - size);
        }
        break;
    case CUR_CLR0 ... CUR_CLR0 + 3:
        val = ati_reg_read_offs(s->regs.cur_color0, addr - CUR_CLR0, size);
        break;
    case CUR_CLR1 ... CUR_CLR1 + 3:
        val = ati_reg_read_offs(s->regs.cur_color1, addr - CUR_CLR1, size);
        break;
    case DST_OFFSET:
        val = s->regs.dst_offset;
        break;
    case DST_PITCH:
        val = s->regs.dst_pitch;
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            val &= s->regs.dst_tile << 16;
        }
        break;
    case DST_WIDTH:
        val = s->regs.dst_width;
        break;
    case DST_HEIGHT:
        val = s->regs.dst_height;
        break;
    case SRC_X:
        val = s->regs.src_x;
        break;
    case SRC_Y:
        val = s->regs.src_y;
        break;
    case DST_X:
        val = s->regs.dst_x;
        break;
    case DST_Y:
        val = s->regs.dst_y;
        break;
    case DP_GUI_MASTER_CNTL:
        val = s->regs.dp_gui_master_cntl;
        break;
    case SRC_OFFSET:
        val = s->regs.src_offset;
        break;
    case SRC_PITCH:
        val = s->regs.src_pitch;
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            val &= s->regs.src_tile << 16;
        }
        break;
    case DP_BRUSH_BKGD_CLR:
        val = s->regs.dp_brush_bkgd_clr;
        break;
    case DP_BRUSH_FRGD_CLR:
        val = s->regs.dp_brush_frgd_clr;
        break;
    case DP_SRC_FRGD_CLR:
        val = s->regs.dp_src_frgd_clr;
        break;
    case DP_SRC_BKGD_CLR:
        val = s->regs.dp_src_bkgd_clr;
        break;
    case DP_CNTL:
        val = s->regs.dp_cntl;
        break;
    case DP_DATATYPE:
        val = s->regs.dp_datatype;
        break;
    case DP_MIX:
        val = s->regs.dp_mix;
        break;
    case DP_WRITE_MASK:
        val = s->regs.dp_write_mask;
        break;
    case DEFAULT_OFFSET:
        val = s->regs.default_offset;
        if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF) {
            val >>= 10;
            val |= s->regs.default_pitch << 16;
            val |= s->regs.default_tile << 30;
        }
        break;
    case DEFAULT_PITCH:
        val = s->regs.default_pitch;
        val |= s->regs.default_tile << 16;
        break;
    case DEFAULT_SC_BOTTOM_RIGHT:
        val = s->regs.default_sc_bottom_right;
        break;
    default:
        if (addr < ATI_SHADOW_SIZE) {
            val = ati_reg_read_offs(s->shadow[addr >> 2], addr & 3, size);
        }
        break;
    }
    if (addr < CUR_OFFSET || addr > CUR_CLR1 || ATI_DEBUG_HW_CURSOR) {
        trace_ati_mm_read(size, addr, ati_reg_name(addr & ~3ULL), val);
    }
    return val;
}

static inline void ati_reg_write_offs(uint32_t *reg, int offs,
                                      uint64_t data, unsigned int size)
{
    if (offs == 0 && size == 4) {
        *reg = data;
    } else {
        *reg = deposit32(*reg, offs * BITS_PER_BYTE, size * BITS_PER_BYTE,
                         data);
    }
}

static void ati_mm_write(void *opaque, hwaddr addr,
                           uint64_t data, unsigned int size);

void ati_reg_write(ATIVGAState *s, hwaddr addr, uint32_t val)
{
    ati_mm_write(s, addr, val, 4);
}

static void ati_mm_write(void *opaque, hwaddr addr,
                           uint64_t data, unsigned int size)
{
    ATIVGAState *s = opaque;

    if (addr < CUR_OFFSET || addr > CUR_CLR1 || ATI_DEBUG_HW_CURSOR) {
        trace_ati_mm_write(size, addr, ati_reg_name(addr & ~3ULL), data);
    }
    switch (addr) {
    case CP_IB_BUFSZ:
        s->shadow[CP_IB_BUFSZ >> 2] = data;
        ati_cp_run_ib(s, data);
        break;
    case 0x780:                         /* DMA_GUI_TABLE_ADDR */
    case 0x7a0:                         /* DMA_VID_TABLE_ADDR */
        s->shadow[addr >> 2] = data;
        if (size == 4) {
            ati_dma_run(s, data);
        }
        break;
    case 0x2200:                        /* R300 VAP_PVS_UPLOAD_ADDRESS */
    case 0x2204:                        /* the same data port: Panther's */
    case 0x2208:                        /* R300 VAP_PVS_UPLOAD_DATA */
        s->shadow[addr >> 2] = data;
        if (s->r300 && size == 4) {
            ati_r300_pvs_write(s, addr, data);
        }
        break;
    case CP_RB_CNTL:
        s->shadow[CP_RB_CNTL >> 2] = data;
        if (data & BIT(31)) {           /* RB_RPTR_WR_ENA */
            s->shadow[CP_RB_RPTR >> 2] = s->shadow[CP_RB_RPTR_WR >> 2];
        }
        break;
    case CP_RB_RPTR_WR:
        s->shadow[CP_RB_RPTR_WR >> 2] = data;
        if (s->shadow[CP_RB_CNTL >> 2] & BIT(31)) {
            s->shadow[CP_RB_RPTR >> 2] = data;
        }
        break;
    case CP_RB_RPTR:
        break;                          /* read only */
    case CP_RB_WPTR:
        s->shadow[CP_RB_WPTR >> 2] = data;
        ati_cp_ring_run(s);
        break;
    case CP_CSQ_CNTL:
        s->shadow[CP_CSQ_CNTL >> 2] = data;
        ati_cp_ring_run(s);
        break;
    case SCRATCH_REG0 ... SCRATCH_REG5 + 3:
        ati_reg_write_offs(&s->shadow[addr >> 2], addr & 3, data, size);
        ati_cp_scratch_written(s, (addr - SCRATCH_REG0) >> 2,
                               s->shadow[addr >> 2]);
        break;
    case CP_CSQ_APER_PRIMARY ... CP_CSQ_APER_END - 1:
        /* PIO command queue: words of the PM4 stream, in order. */
        if (size == 4) {
            ati_cp_stream_word(s, data);
        }
        break;
    case CLOCK_CNTL_INDEX ... CLOCK_CNTL_INDEX + 3:
        ati_reg_write_offs(&s->clock_cntl_index, addr - CLOCK_CNTL_INDEX,
                           data, size);
        break;
    case CLOCK_CNTL_DATA ... CLOCK_CNTL_DATA + 3:
        if (s->clock_cntl_index & PLL_WR_EN) {
            uint32_t v = ati_pll_read(s, s->clock_cntl_index);

            ati_reg_write_offs(&v, addr - CLOCK_CNTL_DATA, data, size);
            ati_pll_write(s, s->clock_cntl_index, v);
        }
        break;
    case MM_INDEX:
        s->regs.mm_index = data & ~3;
        break;
    case MM_DATA ... MM_DATA + 3:
        /* indexed access to regs or memory */
        if (s->regs.mm_index & BIT(31)) {
            uint32_t idx = s->regs.mm_index & ~BIT(31);
            if (idx <= s->vga.vram_size - size) {
                uint8_t b[8];

                stn_le_p(b, size, data);
                ati_vram_write(s, idx, b, size);
            }
        } else if (s->regs.mm_index > MM_DATA + 3) {
            ati_mm_write(s, s->regs.mm_index + addr - MM_DATA, data, size);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                "ati_mm_write: mm_index too small: %u\n", s->regs.mm_index);
        }
        break;
    case BIOS_0_SCRATCH ... BUS_CNTL - 1:
    {
        int i = (addr - BIOS_0_SCRATCH) / 4;
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF && i > 3) {
            break;
        }
        ati_reg_write_offs(&s->regs.bios_scratch[i],
                           addr - (BIOS_0_SCRATCH + i * 4), data, size);
        break;
    }
    case GEN_INT_CNTL:
        s->regs.gen_int_cntl = data;
        if (data & CRTC_VBLANK_INT) {
            ati_vga_vblank_irq(s);
        } else {
            timer_del(&s->vblank_timer);
            ati_vga_update_irq(s);
        }
        break;
    case GEN_INT_STATUS:
    {
        /* Radeon SW_INT_FIRE (bit 26) raises SW_INT (bit 25): fences */
        bool fire = s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF &&
                    (data & BIT(26));

        data &= (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF ?
                 0x000f040fUL : 0xfc080effUL);
        s->regs.gen_int_status &= ~data;
        if (fire) {
            /* a fence too: drawn pixels to video memory first */
            ati_r300_gl_flush(s);
            ati_r300_gl_touch(s);
            s->regs.gen_int_status |= BIT(25);
        }
        ati_vga_update_irq(s);
        break;
    }
    case CRTC_GEN_CNTL ... CRTC_GEN_CNTL + 3:
    {
        uint32_t val = s->regs.crtc_gen_cntl;
        ati_reg_write_offs(&s->regs.crtc_gen_cntl,
                           addr - CRTC_GEN_CNTL, data, size);
        if ((val & CRTC2_CUR_EN) != (s->regs.crtc_gen_cntl & CRTC2_CUR_EN)) {
            if (s->cursor_guest_mode) {
                s->vga.force_shadow = !!(s->regs.crtc_gen_cntl & CRTC2_CUR_EN);
            } else {
                if (s->regs.crtc_gen_cntl & CRTC2_CUR_EN) {
                    ati_cursor_define(s);
                }
                ati_cursor_update_pos(s);
            }
        }
        /*
         * A new mode, or a new depth with the CRTC left running: Jaguar's
         * ATI driver goes from 16 to 32 bits per pixel that way.
         */
        if ((val ^ s->regs.crtc_gen_cntl) &
            (CRTC2_EXT_DISP_EN | CRTC2_EN | CRTC_PIX_WIDTH_MASK)) {
            ati_vga_switch_mode(s);
        }
        break;
    }
    case CRTC_EXT_CNTL ... CRTC_EXT_CNTL + 3:
    {
        uint32_t val = s->regs.crtc_ext_cntl;
        ati_reg_write_offs(&s->regs.crtc_ext_cntl,
                           addr - CRTC_EXT_CNTL, data, size);
        if (s->regs.crtc_ext_cntl & CRT_CRTC_DISPLAY_DIS) {
            DPRINTF("Display disabled\n");
            s->vga.ar_index &= ~BIT(5);
        } else {
            DPRINTF("Display enabled\n");
            s->vga.ar_index |= BIT(5);
            ati_vga_switch_mode(s);
        }
        if ((val & CRT_CRTC_DISPLAY_DIS) !=
            (s->regs.crtc_ext_cntl & CRT_CRTC_DISPLAY_DIS)) {
            ati_vga_switch_mode(s);
        }
        break;
    }
    case DAC_CNTL:
        s->regs.dac_cntl = data & 0xffffe3ff;
        s->vga.dac_8bit = !!(data & DAC_8BIT_EN);
        break;
    /*
     * GPIO regs for DDC access. Because some drivers access these via
     * multiple byte writes we have to be careful when we send bits to
     * avoid spurious changes in bitbang_i2c state. Only do it when either
     * the enable bits are changed or output bits changed while enabled.
     */
    case GPIO_VGA_DDC ... GPIO_VGA_DDC + 3:
        if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF) {
            /* FIXME: Maybe add a property to select VGA or DVI port? */
        }
        break;
    case GPIO_DVI_DDC ... GPIO_DVI_DDC + 3:
        if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF) {
            ati_reg_write_offs(&s->regs.gpio_dvi_ddc,
                               addr - GPIO_DVI_DDC, data, size);
            if ((addr <= GPIO_DVI_DDC + 2 && addr + size > GPIO_DVI_DDC + 2) ||
                (addr == GPIO_DVI_DDC && (s->regs.gpio_dvi_ddc & 0x30000))) {
                s->regs.gpio_dvi_ddc = ati_i2c(&s->bbi2c,
                                               s->regs.gpio_dvi_ddc, 0);
            }
        }
        break;
    case GPIO_MONID ... GPIO_MONID + 3:
        /* FIXME What does Radeon have here? */
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            /* Rage128p accesses DDC via MONID(1-2) with additional mask bit */
            ati_reg_write_offs(&s->regs.gpio_monid,
                               addr - GPIO_MONID, data, size);
            if ((s->regs.gpio_monid & BIT(25)) &&
                ((addr <= GPIO_MONID + 2 && addr + size > GPIO_MONID + 2) ||
                 (addr == GPIO_MONID && (s->regs.gpio_monid & 0x60000)))) {
                s->regs.gpio_monid = ati_i2c(&s->bbi2c, s->regs.gpio_monid, 1);
            }
        }
        break;
    case PALETTE_INDEX ... PALETTE_INDEX + 3:
        if (size == 4) {
            vga_ioport_write(&s->vga, VGA_PEL_IR, (data >> 16) & 0xff);
            vga_ioport_write(&s->vga, VGA_PEL_IW, data & 0xff);
        } else {
            if (addr == PALETTE_INDEX) {
                vga_ioport_write(&s->vga, VGA_PEL_IW, data & 0xff);
            } else {
                vga_ioport_write(&s->vga, VGA_PEL_IR, data & 0xff);
            }
        }
        break;
    case PALETTE_DATA ... PALETTE_DATA + 3:
        data <<= addr - PALETTE_DATA;
        data = bswap32(data) >> 8;
        vga_ioport_write(&s->vga, VGA_PEL_D, data & 0xff);
        data >>= 8;
        vga_ioport_write(&s->vga, VGA_PEL_D, data & 0xff);
        data >>= 8;
        vga_ioport_write(&s->vga, VGA_PEL_D, data & 0xff);
        break;
    case PALETTE_30_DATA:
        s->regs.palette[vga_ioport_read(&s->vga, VGA_PEL_IW)] = data;
        vga_ioport_write(&s->vga, VGA_PEL_D, (data >> 22) & 0xff);
        vga_ioport_write(&s->vga, VGA_PEL_D, (data >> 12) & 0xff);
        vga_ioport_write(&s->vga, VGA_PEL_D, (data >> 2) & 0xff);
        break;
    case CNFG_CNTL ... CNFG_CNTL + 3:
        ati_reg_write_offs(&s->regs.config_cntl, addr - CNFG_CNTL, data, size);
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            /*
             * The byte swapping the CPU's aperture does (APER_1_ENDIAN, or
             * APER_0_ENDIAN: 1 swaps 16-bit units, 2 32-bit ones). Video
             * memory is kept in the CPU's order, as for the Radeons, and
             * the 2D and 3D engines swap (vram_xor): Mac OS X 10.0's
             * drivers read back with the CPU what the engines drew.
             */
            uint32_t c = s->regs.config_cntl;
            unsigned int sw = (c & APER_1_ENDIAN) ? (c >> 2) & 3 : c & 3;
            unsigned int x = sw == 2 ? 3 : sw == 1 ? 1 : 0;

            ati_vram_reorder(s, s->vram_xor, x);
            s->vram_xor = x;
            s->vga.big_endian_fb = x != 0;
        }
        break;
    case CRTC_H_TOTAL_DISP ... CRTC_H_TOTAL_DISP + 3:
    {
        /* Mac OS X 10.0's Rage 128 driver writes these in halves */
        uint32_t v = s->regs.crtc_h_total_disp;

        ati_reg_write_offs(&v, addr - CRTC_H_TOTAL_DISP, data, size);
        data = v;
    }
        if (s->regs.crtc_h_total_disp != (data & 0x07ff07ff)) {
            s->regs.crtc_h_total_disp = data & 0x07ff07ff;
            if (s->mode == EXT_MODE) {
                ati_vga_switch_mode(s);        /* resolution change */
            }
        }
        break;
    case CRTC_H_SYNC_STRT_WID:
        s->regs.crtc_h_sync_strt_wid = data & 0x17bf1fff;
        break;
    case CRTC_V_TOTAL_DISP ... CRTC_V_TOTAL_DISP + 3:
    {
        uint32_t v = s->regs.crtc_v_total_disp;

        ati_reg_write_offs(&v, addr - CRTC_V_TOTAL_DISP, data, size);
        data = v;
    }
        if (s->regs.crtc_v_total_disp != (data & 0x0fff0fff)) {
            s->regs.crtc_v_total_disp = data & 0x0fff0fff;
            if (s->mode == EXT_MODE) {
                ati_vga_switch_mode(s);        /* resolution change */
            }
        }
        break;
    case CRTC_V_SYNC_STRT_WID:
        s->regs.crtc_v_sync_strt_wid = data & 0x9f0fff;
        break;
    case CRTC_OFFSET:
        s->regs.crtc_offset = data & 0xc7ffffff;
        if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF) {
            ati_vga_update_offset(s);
        }
        break;
    case CRTC_OFFSET_CNTL:
        s->regs.crtc_offset_cntl = data; /* FIXME */
        break;
    case CRTC_PITCH ... CRTC_PITCH + 3:
    {
        uint32_t v = s->regs.crtc_pitch;

        ati_reg_write_offs(&v, addr - CRTC_PITCH, data, size);
        data = v;
    }
        if (s->regs.crtc_pitch != (data & 0x07ff07ff)) {
            s->regs.crtc_pitch = data & 0x07ff07ff;
            if (s->mode == EXT_MODE) {
                ati_vga_switch_mode(s);
            }
        }
        break;
    case 0xf00 ... 0xfff:
        /* read-only copy of PCI config space so ignore writes */
        break;
    case CUR_OFFSET ... CUR_OFFSET + 3:
    {
        uint32_t t = s->regs.cur_offset;

        ati_reg_write_offs(&t, addr - CUR_OFFSET, data, size);
        t &= 0x87fffff0;
        if (s->regs.cur_offset != t) {
            s->regs.cur_offset = t;
            ati_cursor_define(s);
        }
        if (!s->cursor_guest_mode) {
            ati_cursor_update_pos(s);
        }
        break;
    }
    case CUR_HORZ_VERT_POSN ... CUR_HORZ_VERT_POSN + 3:
    {
        uint32_t t = s->regs.cur_hv_pos | (s->regs.cur_offset & BIT(31));

        ati_reg_write_offs(&t, addr - CUR_HORZ_VERT_POSN, data, size);
        s->regs.cur_hv_pos = t & 0x3fff0fff;
        if (t & BIT(31)) {
            s->regs.cur_offset |= t & BIT(31);
        } else if (s->regs.cur_offset & BIT(31)) {
            s->regs.cur_offset &= ~BIT(31);
            ati_cursor_define(s);
        }
        if (!s->cursor_guest_mode) {
            ati_cursor_update_pos(s);
        }
        break;
    }
    case CUR_HORZ_VERT_OFF:
    {
        uint32_t t = s->regs.cur_hv_offs | (s->regs.cur_offset & BIT(31));

        ati_reg_write_offs(&t, addr - CUR_HORZ_VERT_OFF, data, size);
        s->regs.cur_hv_offs = t & 0x3f003f;
        if (t & BIT(31)) {
            s->regs.cur_offset |= t & BIT(31);
        } else if (s->regs.cur_offset & BIT(31)) {
            s->regs.cur_offset &= ~BIT(31);
            ati_cursor_define(s);
        }
        if (!s->cursor_guest_mode) {
            ati_cursor_update_pos(s);
        }
        break;
    }
    case CUR_CLR0 ... CUR_CLR0 + 3:
    {
        uint32_t t = s->regs.cur_color0;

        ati_reg_write_offs(&t, addr - CUR_CLR0, data, size);
        t &= 0xffffff;
        if (s->regs.cur_color0 != t) {
            s->regs.cur_color0 = t;
            ati_cursor_define(s);
        }
        break;
    }
    case CUR_CLR1 ... CUR_CLR1 + 3:
        /*
         * Update cursor unconditionally here because some clients set up
         * other registers before actually writing cursor data to memory at
         * offset so we would miss cursor change unless always updating here
         */
        ati_reg_write_offs(&s->regs.cur_color1, addr - CUR_CLR1, data, size);
        s->regs.cur_color1 &= 0xffffff;
        ati_cursor_define(s);
        break;
    case DST_OFFSET:
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.dst_offset = data & 0xfffffff0;
        } else {
            /* byte address (Apple's driver blits from 32-byte offsets) */
            s->regs.dst_offset = data & ~3u;
        }
        break;
    case DST_PITCH:
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.dst_pitch = data & 0x3fff;
            s->regs.dst_tile = (data >> 16) & 1;
        } else {
            s->regs.dst_pitch = data & 0x3ff0;
        }
        break;
    case DST_TILE:
        if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.dst_tile = data & 3;
        }
        break;
    case DST_WIDTH:
        s->regs.dst_width = data & 0x3fff;
        ati_2d_blt(s);
        break;
    case DST_HEIGHT:
        s->regs.dst_height = data & 0x3fff;
        break;
    case SRC_X:
        s->regs.src_x = data & 0x3fff;
        break;
    case SRC_Y:
        s->regs.src_y = data & 0x3fff;
        break;
    case DST_X:
        s->regs.dst_x = data & 0x3fff;
        break;
    case DST_Y:
        s->regs.dst_y = data & 0x3fff;
        break;
    case SRC_PITCH_OFFSET:
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.src_offset = (data & 0x1fffff) << 5;
            s->regs.src_pitch = (data & 0x7fe00000) >> 21;
            s->regs.src_tile = data >> 31;
        } else {
            s->regs.src_offset = (data & 0x3fffff) << 10;
            s->regs.src_pitch = (data & 0x3fc00000) >> 16;
            s->regs.src_tile = (data >> 30) & 1;
        }
        break;
    case DST_PITCH_OFFSET:
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.dst_offset = (data & 0x1fffff) << 5;
            s->regs.dst_pitch = (data & 0x7fe00000) >> 21;
            s->regs.dst_tile = data >> 31;
        } else {
            s->regs.dst_offset = (data & 0x3fffff) << 10;
            s->regs.dst_pitch = (data & 0x3fc00000) >> 16;
            s->regs.dst_tile = data >> 30;
        }
        break;
    case SRC_Y_X:
        s->regs.src_x = data & 0x3fff;
        s->regs.src_y = (data >> 16) & 0x3fff;
        break;
    case DST_Y_X:
        s->regs.dst_x = data & 0x3fff;
        s->regs.dst_y = (data >> 16) & 0x3fff;
        break;
    case DST_HEIGHT_WIDTH:
        s->regs.dst_width = data & 0x3fff;
        s->regs.dst_height = (data >> 16) & 0x3fff;
        ati_2d_blt(s);
        break;
    case 0x17c0 ... 0x17e0:     /* HOST_DATA0-7, HOST_DATA_LAST */
        ati_2d_host_data(s, data, addr == 0x17e0);
        break;
    case DP_GUI_MASTER_CNTL:
        s->regs.dp_gui_master_cntl = data & 0xf800000f;
        s->regs.dp_datatype = (data & 0x0f00) >> 8 | (data & 0x30f0) << 4 |
                              (data & 0x4000) << 16 |
                              /* Rage 128: HOST_BIG_ENDIAN_EN stays */
                              (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF ?
                               s->regs.dp_datatype & BIT(29) : 0);
        s->regs.dp_mix = (data & GMC_ROP3_MASK) | (data & 0x7000000) >> 16;
        break;
    case DST_WIDTH_X:
        s->regs.dst_x = data & 0x3fff;
        s->regs.dst_width = (data >> 16) & 0x3fff;
        ati_2d_blt(s);
        break;
    case SRC_X_Y:
        s->regs.src_y = data & 0x3fff;
        s->regs.src_x = (data >> 16) & 0x3fff;
        break;
    case DST_X_Y:
        s->regs.dst_y = data & 0x3fff;
        s->regs.dst_x = (data >> 16) & 0x3fff;
        break;
    case DST_WIDTH_HEIGHT:
        s->regs.dst_height = data & 0x3fff;
        s->regs.dst_width = (data >> 16) & 0x3fff;
        ati_2d_blt(s);
        break;
    case DST_HEIGHT_Y:
        s->regs.dst_y = data & 0x3fff;
        s->regs.dst_height = (data >> 16) & 0x3fff;
        break;
    case SRC_OFFSET:
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.src_offset = data & 0xfffffff0;
        } else {
            s->regs.src_offset = data & ~3u;
        }
        break;
    case SRC_PITCH:
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.src_pitch = data & 0x3fff;
            s->regs.src_tile = (data >> 16) & 1;
        } else {
            s->regs.src_pitch = data & 0x3ff0;
        }
        break;
    case DP_BRUSH_BKGD_CLR:
        s->regs.dp_brush_bkgd_clr = data;
        break;
    case DP_BRUSH_FRGD_CLR:
        s->regs.dp_brush_frgd_clr = data;
        break;
    case DP_CNTL:
        s->regs.dp_cntl = data;
        break;
    case DP_DATATYPE:
        s->regs.dp_datatype = data & 0xe0070f0f;
        break;
    case DP_MIX:
        s->regs.dp_mix = data & 0x00ff0700;
        break;
    case DP_WRITE_MASK:
        s->regs.dp_write_mask = data;
        break;
    case DEFAULT_OFFSET:
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.default_offset = data & 0xfffffff0;
        } else {
            /* Radeon has DEFAULT_PITCH_OFFSET here like DST_PITCH_OFFSET */
            s->regs.default_offset = (data & 0x3fffff) << 10;
            s->regs.default_pitch = (data & 0x3fc00000) >> 16;
            s->regs.default_tile = data >> 30;
        }
        break;
    case DEFAULT_PITCH:
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.default_pitch = data & 0x3fff;
            s->regs.default_tile = (data >> 16) & 1;
        }
        break;
    case SURFACE_CNTL ... SURFACE_CNTL + 3:
        ati_reg_write_offs(&s->shadow[addr >> 2], addr & 3, data, size);
        ati_update_apertures(s);
        break;
    case 0xb04 ... 0xb7f:               /* SURFACE0-7 bounds and INFO */
        ati_reg_write_offs(&s->shadow[addr >> 2], addr & 3, data, size);
        ati_update_surfaces(s);
        break;
    case DEFAULT_SC_BOTTOM_RIGHT:
        s->regs.default_sc_bottom_right = data & 0x3fff3fff;
        break;
    case DST_PITCH_OFFSET_C ... SC_BOTTOM_RIGHT_C + 3:
        if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF || size != 4) {
            ati_reg_write_offs(&s->shadow[addr >> 2], addr & 3, data, size);
            break;
        }
        /*
         * Rage 128 "context" registers: the same state as the plain ones.
         * Writing DP_GUI_MASTER_CNTL_C also puts the engine back to drawing
         * left to right, top to bottom: 10.0's ATIRage128GA never sets
         * DP_CNTL itself.
         */
        switch (addr) {
        case DST_PITCH_OFFSET_C:
            ati_mm_write(s, DST_PITCH_OFFSET, data, 4);
            break;
        case DP_GUI_MASTER_CNTL_C:
            ati_mm_write(s, DP_GUI_MASTER_CNTL, data, 4);
            s->regs.dp_cntl = DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM;
            break;
        case SC_TOP_LEFT_C:
            s->shadow[SC_TOP_LEFT >> 2] = data;
            break;
        case SC_BOTTOM_RIGHT_C:
            s->shadow[SC_BOTTOM_RIGHT >> 2] = data;
            break;
        }
        break;
    default:
        if (addr < ATI_SHADOW_SIZE) {
            ati_reg_write_offs(&s->shadow[addr >> 2], addr & 3, data, size);
        }
        break;
    }
}

static const MemoryRegionOps ati_mm_ops = {
    .read = ati_mm_read,
    .write = ati_mm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void ati_vga_realize(PCIDevice *dev, Error **errp)
{
    ATIVGAState *s = ATI_VGA(dev);
    VGACommonState *vga = &s->vga;

#ifndef CONFIG_PIXMAN
    if (s->use_pixman != 0) {
        warn_report("x-pixman != 0, not effective without PIXMAN");
    }
#endif

    if (s->model) {
        int i;
        for (i = 0; i < ARRAY_SIZE(ati_model_aliases); i++) {
            if (!strcmp(s->model, ati_model_aliases[i].name)) {
                s->dev_id = ati_model_aliases[i].dev_id;
                break;
            }
        }
        if (i >= ARRAY_SIZE(ati_model_aliases)) {
            warn_report("Unknown ATI VGA model name, "
                        "using default rage128p");
        }
    }
    if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF &&
        s->dev_id != PCI_DEVICE_ID_ATI_RADEON_QY &&
        s->dev_id != PCI_DEVICE_ID_ATI_RADEON_AP) {
        error_setg(errp, "Unknown ATI VGA device id, "
                   "only 0x5046, 0x5159 and 0x4150 are supported");
        return;
    }
    s->r300 = s->dev_id == PCI_DEVICE_ID_ATI_RADEON_AP;
    pci_set_word(dev->config + PCI_DEVICE_ID, s->dev_id);

    if (s->dev_id == PCI_DEVICE_ID_ATI_RADEON_QY &&
        s->vga.vram_size_mb < 16) {
        warn_report("Too small video memory for device id");
        s->vga.vram_size_mb = 16;
    }
    if (s->r300 && s->vga.vram_size_mb < 64) {
        warn_report("Too small video memory for device id");
        s->vga.vram_size_mb = 64;
    }

    /* init vga bits */
    if (!vga_common_init(vga, OBJECT(s), errp)) {
        return;
    }
    vga_init(vga, OBJECT(s), pci_address_space(dev),
             pci_address_space_io(dev), true);
    s->vga_hw_ops = s->vga.hw_ops;
    s->hw_ops = *s->vga.hw_ops;
    s->hw_ops.gfx_update = ati_gfx_update;
    vga->con = graphic_console_init(DEVICE(s), 0, &s->hw_ops, vga);
    ati_cp_start_thread(s);
    if (s->cursor_guest_mode) {
        vga->cursor_invalidate = ati_cursor_invalidate;
        vga->cursor_draw_line = ati_cursor_draw_line;
    }

    /* ddc, edid */
    I2CBus *i2cbus = i2c_init_bus(DEVICE(s), "ati-vga.ddc");
    bitbang_i2c_init(&s->bbi2c, i2cbus);
    I2CSlave *i2cddc = I2C_SLAVE(qdev_new(TYPE_I2CDDC));
    i2c_slave_set_address(i2cddc, 0x50);
    qdev_realize_and_unref(DEVICE(i2cddc), BUS(i2cbus), &error_abort);

    /* mmio register space */
    memory_region_init_io(&s->mm, OBJECT(s), &ati_mm_ops, s,
                          "ati.mmregs", s->r300 ? 0x10000 : 0x4000);
    /* io space is alias to beginning of mmregs */
    memory_region_init_alias(&s->io, OBJECT(s), "ati.io", &s->mm, 0, 0x100);

    if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
        pci_register_bar(dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &vga->vram);
    } else {
        /*
         * Radeon: the BAR holds two apertures onto the same video memory,
         * each with its own byte-swapping setting; big-endian hosts such
         * as Macs write pixels through aperture 1. Video memory is kept
         * in aperture 1's byte order (see vram_xor), so it is plain
         * memory; aperture 0 is too unless its order differs.
         */
        memory_region_init(&s->vram_bar, OBJECT(s), "ati.vram-bar",
                           2 * vga->vram_size);
        memory_region_init_alias(&s->vram_aper0, OBJECT(s), "ati.aper0",
                                 &vga->vram, 0, vga->vram_size);
        memory_region_init_io(&s->vram_aper0_io, OBJECT(s), &ati_aper0_ops,
                              s, "ati.aper0-swapped", vga->vram_size);
        memory_region_add_subregion_overlap(&s->vram_bar, 0, &s->vram_aper0,
                                            1);
        memory_region_add_subregion_overlap(&s->vram_bar, 0,
                                            &s->vram_aper0_io, 0);
        memory_region_init_alias(&s->vram_aper1, OBJECT(s), "ati.aper1",
                                 &vga->vram, 0, vga->vram_size);
        memory_region_add_subregion(&s->vram_bar, vga->vram_size,
                                    &s->vram_aper1);
        for (int a = 0; a < 2; a++) {
            for (int n = 0; n < 8; n++) {
                struct ATISurfaceWindow *w = &s->surf_win[a][n];

                w->s = s;
                memory_region_init_io(&w->mr, OBJECT(s), &ati_surf_ops, w,
                                      "ati.surface", 0x1000);
                memory_region_set_enabled(&w->mr, false);
                memory_region_add_subregion_overlap(&s->vram_bar,
                                                    a * vga->vram_size,
                                                    &w->mr, 2 + n);
            }
        }
        pci_register_bar(dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH,
                         &s->vram_bar);
    }
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mm);

    if (s->agp) {
        /* AGP 2.0 capability, as on AGP Radeons: 1x/2x/4x, sideband */
        int cap = pci_add_capability(dev, PCI_CAP_ID_AGP, 0x58, 12, errp);

        if (cap < 0) {
            return;
        }
        pci_set_byte(dev->config + cap + 2, 0x20);
        pci_set_long(dev->config + cap + 4, 0x1f000207);
        pci_set_long(dev->wmask + cap + 8, 0xff000307);
    }

    /* most interrupts are not yet emulated but MacOS needs at least VBlank */
    dev->config[PCI_INTERRUPT_PIN] = 1;
    timer_init_ns(&s->vblank_timer, QEMU_CLOCK_VIRTUAL, ati_vga_vblank_irq, s);
}

static void ati_vga_reset(DeviceState *dev)
{
    ATIVGAState *s = ATI_VGA(dev);

    timer_del(&s->vblank_timer);
    ati_vga_update_irq(s);

    /* reset vga */
    vga_common_reset(&s->vga);
    s->mode = VGA_MODE;
    memset(s->shadow, 0, sizeof(s->shadow));
    memset(s->pll, 0, sizeof(s->pll));
    s->clock_cntl_index = 0;
    ati_cp_reset(s);
    s->vram_xor = 0;
    ati_update_apertures(s);
}

static void ati_vga_exit(PCIDevice *dev)
{
    ATIVGAState *s = ATI_VGA(dev);

    timer_del(&s->vblank_timer);
    graphic_console_close(s->vga.con);
}

static const Property ati_vga_properties[] = {
    DEFINE_PROP_UINT32("vgamem_mb", ATIVGAState, vga.vram_size_mb, 16),
    DEFINE_PROP_STRING("model", ATIVGAState, model),
    DEFINE_PROP_UINT16("x-device-id", ATIVGAState, dev_id,
                       PCI_DEVICE_ID_ATI_RAGE128_PF),
    DEFINE_PROP_BOOL("guest_hwcursor", ATIVGAState, cursor_guest_mode, false),
    DEFINE_PROP_BOOL("agp", ATIVGAState, agp, false),
    /* this is a debug option, prefer PROP_UINT over PROP_BIT for simplicity */
    DEFINE_PROP_UINT8("x-pixman", ATIVGAState, use_pixman, DEFAULT_X_PIXMAN),
};

static void ati_vga_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, ati_vga_reset);
    device_class_set_props(dc, ati_vga_properties);
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);

    k->class_id = PCI_CLASS_DISPLAY_VGA;
    k->vendor_id = PCI_VENDOR_ID_ATI;
    k->device_id = PCI_DEVICE_ID_ATI_RAGE128_PF;
    k->romfile = "vgabios-ati.bin";
    k->realize = ati_vga_realize;
    k->exit = ati_vga_exit;
}

static void ati_vga_init(Object *o)
{
    object_property_set_description(o, "x-pixman", "Use pixman for: "
                                    "1: fill, 2: blit");
}

static const TypeInfo ati_vga_info = {
    .name = TYPE_ATI_VGA,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(ATIVGAState),
    .class_init = ati_vga_class_init,
    .instance_init = ati_vga_init,
    .interfaces = (const InterfaceInfo[]) {
          { INTERFACE_CONVENTIONAL_PCI_DEVICE },
          { },
    },
};

static void ati_vga_register_types(void)
{
    type_register_static(&ati_vga_info);
}

type_init(ati_vga_register_types)
