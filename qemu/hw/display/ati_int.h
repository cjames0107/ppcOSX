/*
 * QEMU ATI SVGA emulation
 *
 * Copyright (c) 2019 BALATON Zoltan
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#ifndef ATI_INT_H
#define ATI_INT_H

#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "qemu/thread.h"
#include "hw/pci/pci_device.h"
#include "hw/i2c/bitbang_i2c.h"
#include "vga_int.h"
#include "qom/object.h"

/*#define DEBUG_ATI*/

#ifdef DEBUG_ATI
#define DPRINTF(fmt, ...) printf("%s: " fmt, __func__, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do {} while (0)
#endif

#define PCI_VENDOR_ID_ATI 0x1002
/* Rage128 Pro GL */
#define PCI_DEVICE_ID_ATI_RAGE128_PF 0x5046
/* Radeon RV100 (VE) */
#define PCI_DEVICE_ID_ATI_RADEON_QY 0x5159
#define PCI_DEVICE_ID_ATI_RADEON_AP 0x4150 /* RV350, Radeon 9600 */

#define TYPE_ATI_VGA "ati-vga"
OBJECT_DECLARE_SIMPLE_TYPE(ATIVGAState, ATI_VGA)

typedef struct ATIVGARegs {
    uint32_t mm_index;
    uint32_t bios_scratch[8];
    uint32_t gen_int_cntl;
    uint32_t gen_int_status;
    uint32_t crtc_gen_cntl;
    uint32_t crtc_ext_cntl;
    uint32_t dac_cntl;
    uint32_t gpio_vga_ddc;
    uint32_t gpio_dvi_ddc;
    uint32_t gpio_monid;
    uint32_t config_cntl;
    uint32_t palette[256];
    uint32_t crtc_h_total_disp;
    uint32_t crtc_h_sync_strt_wid;
    uint32_t crtc_v_total_disp;
    uint32_t crtc_v_sync_strt_wid;
    uint32_t crtc_offset;
    uint32_t crtc_offset_cntl;
    uint32_t crtc_pitch;
    uint32_t cur_offset;
    uint32_t cur_hv_pos;
    uint32_t cur_hv_offs;
    uint32_t cur_color0;
    uint32_t cur_color1;
    uint32_t dst_offset;
    uint32_t dst_pitch;
    uint32_t dst_tile;
    uint32_t dst_width;
    uint32_t dst_height;
    uint32_t src_offset;
    uint32_t src_pitch;
    uint32_t src_tile;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t dst_x;
    uint32_t dst_y;
    uint32_t dp_gui_master_cntl;
    uint32_t dp_brush_bkgd_clr;
    uint32_t dp_brush_frgd_clr;
    uint32_t dp_src_frgd_clr;
    uint32_t dp_src_bkgd_clr;
    uint32_t dp_cntl;
    uint32_t dp_datatype;
    uint32_t dp_mix;
    uint32_t dp_write_mask;
    uint32_t default_offset;
    uint32_t default_pitch;
    uint32_t default_tile;
    uint32_t default_sc_bottom_right;
} ATIVGARegs;

#define ATI_SHADOW_SIZE 0x10000
#define ATI_CP_MAX_PACKET 0x4000

/* Command processor: the PM4 packet being assembled. */
typedef struct ATICPState {
    uint32_t hdr;
    uint32_t want;              /* payload words of this packet, 0: none */
    uint32_t have;
    uint32_t skip;              /* words of an oversized packet to drop */
    uint32_t buf[ATI_CP_MAX_PACKET];
} ATICPState;

/* A blit whose source comes through the HOST_DATA registers */
typedef struct ATIHostBlt {
    bool active;
    uint64_t base;              /* destination surface */
    unsigned int pitch, x, y, w, h, bypp;
    unsigned int cx0, cy0, cx1, cy1;    /* scissor, exclusive */
    uint8_t *buf;               /* w * h pixels, filled in order */
    size_t len, pos;
} ATIHostBlt;

struct ATIVGAState {
    PCIDevice dev;
    VGACommonState vga;
    char *model;
    bool r300; /* R300 family (RV350): R300 3D engine */
    uint16_t dev_id;
    uint8_t mode;
    uint8_t use_pixman;
    bool agp;                       /* on the AGP bus, with its capability */
    bool cursor_guest_mode;
    uint16_t cursor_size;
    uint32_t cursor_offset;
    QEMUCursor *cursor;
    uint32_t cursor_sum;
    bool cursor_shown;              /* last sent to the UI */
    int cursor_x, cursor_y;
    const GraphicHwOps *vga_hw_ops;
    GraphicHwOps hw_ops;
    QEMUTimer vblank_timer;
    bitbang_i2c_interface bbi2c;
    MemoryRegion io;
    MemoryRegion mm;
    MemoryRegion vram_bar;          /* Radeon: aperture 0 and 1 */
    MemoryRegion vram_aper1;
    MemoryRegion vram_aper0;        /* aperture 0 as plain memory... */
    MemoryRegion vram_aper0_io;     /* ...or with its own byte order */
    /*
     * Video memory is kept in aperture 1's byte order, as big-endian hosts
     * write it, so that aperture can be plain memory: the byte the card
     * sees at offset a is stored at a ^ vram_xor (3 when aperture 1 swaps
     * 32-bit words, 1 for 16-bit ones). aper0_xor maps aperture 0 likewise.
     */
    uint8_t vram_xor;
    uint8_t aper0_xor;
    /*
     * The eight SURFACEn ranges (Radeon): a byte order of their own for
     * each aperture, applied by windows over the BAR where it differs
     * from the stored order.
     */
    struct ATISurfaceWindow {
        struct ATIVGAState *s;
        MemoryRegion mr;
        hwaddr base;                /* video memory offset of the window */
        uint8_t xor;
    } surf_win[2][8];
    ATIVGARegs regs;

    /*
     * Plain storage behind registers that are not otherwise modelled, so
     * drivers read back what they wrote (read-modify-write sequences), and
     * the PLL registers behind CLOCK_CNTL_INDEX / CLOCK_CNTL_DATA.
     */
    uint32_t shadow[ATI_SHADOW_SIZE / 4];
    uint32_t clock_cntl_index;
    uint32_t pll[64];
    ATICPState cp;
    ATICPState cp_ib;               /* the indirect buffer being run */
    ATIHostBlt host_blt;
    bool in_ib;
    bool in_ring;
    /* R300: vertex program memory (VAP_PVS_UPLOAD_*), vertex arrays */
#define ATI_PVS_DWORDS (0x800 * 4)
    uint32_t pvs[ATI_PVS_DWORDS];
    uint32_t pvs_index;
    struct {                        /* 3D_CLEAR_ZMASK still to do */
        bool pending;
        uint32_t offset, pitch;
        int bpp, x0, y0, x1, y1;
    } r300_zclear;
    void *r300_gl;                  /* host GPU state (ati_r300_gl.c) */
    /* the command processor thread (ati_cp.c) */
    QemuThread cp_thread;
    QemuMutex cp_lock;
    QemuCond cp_cond;
    bool cp_threaded, cp_kick, cp_busy;
};

const char *ati_reg_name(int num);

/* Copy bytes between video memory, in the card's byte order, and buf. */
void ati_vram_read(ATIVGAState *s, uint64_t off, uint8_t *buf, uint32_t len);
void ati_vram_write(ATIVGAState *s, uint64_t off, const uint8_t *buf,
                    uint32_t len);
static inline uint8_t ati_vram_byte(ATIVGAState *s, uint64_t off)
{
    return s->vga.vram_ptr[off ^ s->vram_xor];
}

void ati_2d_blt(ATIVGAState *s);
void ati_2d_host_data(ATIVGAState *s, uint32_t data, bool last);

/* 3D engine (ati_3d.c) */
void ati_3d_draw_immd(ATIVGAState *s, uint32_t vtx_fmt, uint32_t vf_cntl,
                      const uint32_t *data, uint32_t count);
void ati_3d_clear_zmask(ATIVGAState *s, const uint32_t *p, uint32_t count);

/* R300 3D engine (ati_r300.c) */
void ati_r300_pvs_write(ATIVGAState *s, uint32_t addr, uint32_t val);
void ati_r300_draw_immd(ATIVGAState *s, const uint32_t *p, uint32_t count);
void ati_r300_draw_vbuf(ATIVGAState *s, const uint32_t *p, uint32_t count);
void ati_r300_draw_indx(ATIVGAState *s, const uint32_t *p, uint32_t count);
void ati_r300_clear_zmask(ATIVGAState *s, const uint32_t *p, uint32_t count);
void ati_r300_load_vbpntr(ATIVGAState *s, const uint32_t *p, uint32_t count);

/* A full register write, as from the CPU, for the command processor. */
void ati_reg_write(ATIVGAState *s, hwaddr addr, uint32_t val);
void ati_cp_ring_run(ATIVGAState *s);
void ati_cp_start_thread(ATIVGAState *s);
bool ati_cp_busy(ATIVGAState *s);
extern int64_t ati_cp_ns;
extern int64_t ati_2d_ns;
void ati_cp_stream_word(ATIVGAState *s, uint32_t word);
void ati_cp_reset(ATIVGAState *s);
/* Write a 32-bit word at a card (memory controller) address. */
bool ati_mc_write32(ATIVGAState *s, uint32_t mc, uint32_t val, bool big_endian);
/* Copy bytes to or from card addresses (video memory or the PCI GART). */
bool ati_mc_read(ATIVGAState *s, uint32_t mc, uint8_t *buf, uint32_t len);
void ati_dma_run(ATIVGAState *s, uint32_t table);
bool ati_mc_write(ATIVGAState *s, uint32_t mc, const uint8_t *buf,
                  uint32_t len);
bool ati_mc_vram_range(ATIVGAState *s, uint64_t mc, uint64_t len,
                       uint64_t *off);
void ati_cp_scratch_written(ATIVGAState *s, unsigned int n, uint32_t val);
void ati_cp_run_ib(ATIVGAState *s, uint32_t dwords);

/* Load `bytes` (1, 2 or 4) at a video memory offset, in the card's order. */
static inline uint32_t ati_vram_ld(ATIVGAState *s, uint64_t off, int bytes)
{
    const uint8_t *p = s->vga.vram_ptr;

    if (s->vram_xor == 3 && !(off & (bytes - 1))) {
        switch (bytes) {
        case 4:
            return ldl_be_p(p + off);
        case 2:
            return lduw_be_p(p + (off ^ 2));
        default:
            return p[off ^ 3];
        }
    }
    if (!s->vram_xor) {
        return ldn_le_p(p + off, bytes);
    }
    uint32_t v = 0;
    for (int i = 0; i < bytes; i++) {
        v |= (uint32_t)p[(off + i) ^ s->vram_xor] << (8 * i);
    }
    return v;
}

static inline void ati_vram_st(ATIVGAState *s, uint64_t off, int bytes,
                           uint32_t v)
{
    uint8_t *p = s->vga.vram_ptr;

    if (s->vram_xor == 3 && !(off & (bytes - 1))) {
        switch (bytes) {
        case 4:
            stl_be_p(p + off, v);
            return;
        case 2:
            stw_be_p(p + (off ^ 2), v);
            return;
        default:
            p[off ^ 3] = v;
            return;
        }
    }
    if (!s->vram_xor) {
        stn_le_p(p + off, bytes, v);
        return;
    }
    for (int i = 0; i < bytes; i++) {
        p[(off + i) ^ s->vram_xor] = v >> (8 * i);
    }
}


#endif /* ATI_INT_H */
