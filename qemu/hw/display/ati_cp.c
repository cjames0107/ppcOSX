/*
 * QEMU ATI SVGA emulation
 * Radeon command processor (CP): PM4 packet streams.
 *
 * Drivers hand the CP a stream of PM4 packets, either written word by word
 * into the command queue aperture (PIO, 0x1000-0x13ff) or placed in a ring
 * buffer. This parses the stream and executes it as it arrives, so the
 * queues are always seen empty:
 *
 *   type 0  write consecutive registers (or one register repeatedly)
 *   type 1  write two registers
 *   type 2  padding
 *   type 3  an operation with a payload; unhandled ones are logged
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "ati_int.h"
#include "ati_regs.h"
#include "ati_r300_gl.h"
#include "trace.h"
#include "hw/pci/pci_device.h"
#include "qemu/bswap.h"
#include "hw/pci-host/uninorth.h"

#define PM4_TYPE(hdr)       ((hdr) >> 30)
#define PM4_COUNT(hdr)      ((((hdr) >> 16) & 0x3fff) + 1)
#define PM4_ONE_REG_WR      (1u << 15)

/*
 * Debugging: with PPCOSX_ATI_DUMP set to a file name, every packet is
 * written there as text once the file NAME.go exists, for 3 million
 * packets.
 */
static FILE *ati_cp_dump_file(void)
{
    static FILE *f;
    static const char *path;
    static bool tried, started;
    static uint64_t calls, written;
    static char go[4096];

    if (!tried) {
        tried = true;
        path = getenv("PPCOSX_ATI_DUMP");
        if (path) {
            snprintf(go, sizeof(go), "%s.go", path);
        }
    }
    if (!path) {
        return NULL;
    }
    /* Dumping starts once PATH.go exists (checked now and then). */
    if (!started) {
        if (++calls % 16 || access(go, F_OK)) {
            return NULL;
        }
        started = true;
        f = fopen(path, "w");
        if (f) {
            setvbuf(f, NULL, _IOLBF, 0);
        }
    }
    if (f && ++written > 3000000) {
        fclose(f);
        f = NULL;
        path = NULL;
    }
    return f;
}

static void ati_cp_dump(uint32_t hdr, const uint32_t *p, uint32_t count)
{
    FILE *f = ati_cp_dump_file();

    if (!f) {
        return;
    }
    if (PM4_TYPE(hdr) == 0) {
        uint32_t reg = (hdr & 0x1fff) << 2;

        for (uint32_t i = 0; i < count; i++) {
            fprintf(f, "R %04x %08x\n", reg, p[i]);
            if (!(hdr & PM4_ONE_REG_WR)) {
                reg += 4;
            }
        }
        return;
    }
    fprintf(f, "P%u %02x %u:", PM4_TYPE(hdr), (hdr >> 8) & 0xff, count);
    for (uint32_t i = 0; i < count; i++) {
        fprintf(f, " %08x", p[i]);
    }
    fputc('\n', f);
}

/*
 * PAINT_MULTI (0x9a) and BITBLT_MULTI (0x9b): a DP_GUI_MASTER_CNTL value,
 * the pitch/offsets, scissor and brush colours it asks for, then
 * rectangles, each (x << 16 | y) [source, destination] and (w << 16 | h).
 */
/*
 * The rectangles of PAINT_MULTI (destination, size) or BITBLT_MULTI
 * (source, destination, size), each word x << 16 | y.
 */
static void ati_cp_multi_rects(ATIVGAState *s, bool blit, const uint32_t *p,
                               uint32_t count)
{
    uint32_t i = 0;

    while (i + (blit ? 3 : 2) <= count) {
        if (blit) {
            s->regs.src_x = p[i] >> 16 & 0x3fff;
            s->regs.src_y = p[i++] & 0x3fff;
        }
        s->regs.dst_x = p[i] >> 16 & 0x3fff;
        s->regs.dst_y = p[i++] & 0x3fff;
        s->regs.dst_width = p[i] >> 16 & 0x3fff;
        s->regs.dst_height = p[i++] & 0x3fff;
        ati_2d_blt(s);
    }
}

/*
 * The header the 2D packets share: DP_GUI_MASTER_CNTL, then the pitch/
 * offsets, scissor and brush colours it asks for. Returns the words used.
 */
static uint32_t ati_cp_2d_header(ATIVGAState *s, const uint32_t *p,
                                 uint32_t count)
{
    uint32_t i = 0, gmc;

    if (count < 1) {
        return 0;
    }
    gmc = p[i++];
    ati_reg_write(s, DP_GUI_MASTER_CNTL, gmc);
    if ((gmc & GMC_SRC_PITCH_OFFSET_CNTL) && i < count) {
        ati_reg_write(s, SRC_PITCH_OFFSET, p[i++]);
    }
    if ((gmc & GMC_DST_PITCH_OFFSET_CNTL) && i < count) {
        ati_reg_write(s, DST_PITCH_OFFSET, p[i++]);
    }
    if ((gmc & GMC_SRC_CLIPPING) && i + 1 < count) {
        i += 2;     /* source scissor: not modelled */
    }
    if ((gmc & GMC_DST_CLIPPING) && i + 1 < count) {
        ati_reg_write(s, SC_TOP_LEFT, p[i++]);
        ati_reg_write(s, SC_BOTTOM_RIGHT, p[i++]);
    }
    switch ((gmc >> 4) & 0xf) {
    case 0: case 1: case 2: case 3:     /* 8x8 mono patterns: colours, bits */
        i += 4;
        break;
    case 0xc:                           /* solid, with background colour */
        i += 2;
        break;
    case 0xd:                           /* solid colour */
        if (i < count) {
            ati_reg_write(s, DP_BRUSH_FRGD_CLR, p[i++]);
        }
        break;
    default:                            /* no brush */
        break;
    }
    return MIN(i, count);
}

static void ati_cp_multi(ATIVGAState *s, bool blit, const uint32_t *p,
                         uint32_t count)
{
    uint32_t i = ati_cp_2d_header(s, p, count);

    if (i) {
        ati_cp_multi_rects(s, blit, p + i, count - i);
    }
}

/* CNTL_BITBLT (0x92): the 2D header, then SRC_Y_X, DST_Y_X, HEIGHT_WIDTH. */
static void ati_cp_bitblt(ATIVGAState *s, const uint32_t *p, uint32_t count)
{
    uint32_t i = ati_cp_2d_header(s, p, count);

    if (i && i + 3 <= count) {
        s->regs.src_y = p[i] >> 16 & 0x3fff;
        s->regs.src_x = p[i++] & 0x3fff;
        s->regs.dst_y = p[i] >> 16 & 0x3fff;
        s->regs.dst_x = p[i++] & 0x3fff;
        s->regs.dst_height = p[i] >> 16 & 0x3fff;
        s->regs.dst_width = p[i] & 0x3fff;
        ati_2d_blt(s);
    }
}

/*
 * CNTL_HOSTDATA_BLT (0x94): the 2D header, the two source colours (sent
 * for colour sources too), DST_Y_X, HEIGHT_WIDTH, the number of data words
 * and then the pixels, as written to HOST_DATA. Apple's 10.1 RV100 GL
 * driver draws glDrawPixels this way.
 */
static void ati_cp_hostdata_blt(ATIVGAState *s, const uint32_t *p,
                                uint32_t count)
{
    uint32_t i = ati_cp_2d_header(s, p, count), n;

    if (!i || i + 5 > count) {
        return;
    }
    ati_reg_write(s, DP_SRC_FRGD_CLR, p[i++]);
    ati_reg_write(s, DP_SRC_BKGD_CLR, p[i++]);
    s->regs.dst_y = p[i] >> 16 & 0x3fff;
    s->regs.dst_x = p[i++] & 0x3fff;
    s->regs.dst_height = p[i] >> 16 & 0x3fff;
    s->regs.dst_width = p[i++] & 0x3fff;
    n = MIN(p[i++], count - i);
    ati_2d_blt(s);
    for (uint32_t k = 0; k < n; k++) {
        ati_2d_host_data(s, p[i + k], k + 1 == n);
    }
}

/* A complete packet has arrived: header plus `count` payload words. */
static void ati_cp_packet(ATIVGAState *s, uint32_t hdr, const uint32_t *p,
                          uint32_t count)
{
    ati_cp_dump(hdr, p, count);
    switch (PM4_TYPE(hdr)) {
    case 0: {
        uint32_t reg = (hdr & 0x1fff) << 2;

        for (uint32_t i = 0; i < count; i++) {
            ati_reg_write(s, reg, p[i]);
            if (!(hdr & PM4_ONE_REG_WR)) {
                reg += 4;
            }
        }
        break;
    }
    case 1:
        ati_reg_write(s, (hdr & 0x7ff) << 2, p[0]);
        ati_reg_write(s, ((hdr >> 11) & 0x7ff) << 2, p[1]);
        break;
    case 3: {
        uint32_t op = (hdr >> 8) & 0xff;

        trace_ati_cp_packet3(op, count);
        switch (op) {
        case 0x10:      /* NOP */
        case 0x26:      /* WAIT_FOR_IDLE: always idle */
            break;
        /*
         * 2D packets with bit 7 clear carry no DP_GUI_MASTER_CNTL header
         * and use the engine's current state: Apple's R300 driver sends a
         * BITBLT_MULTI, then more rectangles this way.
         */
        case 0x1a:      /* PAINT_MULTI, no header */
            ati_cp_multi_rects(s, false, p, count);
            break;
        case 0x1b:      /* BITBLT_MULTI, no header */
            ati_cp_multi_rects(s, true, p, count);
            break;
        case 0x92:      /* CNTL_BITBLT */
            ati_cp_bitblt(s, p, count);
            break;
        case 0x94:      /* CNTL_HOSTDATA_BLT */
            ati_cp_hostdata_blt(s, p, count);
            break;
        case 0x9a:      /* PAINT_MULTI */
            ati_cp_multi(s, false, p, count);
            break;
        case 0x9b:      /* BITBLT_MULTI */
            ati_cp_multi(s, true, p, count);
            break;
        case 0x2f:      /* R300 3D_LOAD_VBPNTR */
            if (s->r300) {
                ati_r300_load_vbpntr(s, p, count);
            }
            break;
        case 0x34:      /* R300 3D_DRAW_VBUF_2 */
            if (s->r300) {
                ati_r300_draw_vbuf(s, p, count);
            }
            break;
        case 0x35:      /* R300 3D_DRAW_IMMD_2 */
            if (s->r300) {
                ati_r300_draw_immd(s, p, count);
            }
            break;
        case 0x36:      /* R300 3D_DRAW_INDX_2 */
            if (s->r300) {
                ati_r300_draw_indx(s, p, count);
            }
            break;
        case 0x25:      /* 3D_DRAW_IMMD-like: format, VF_CNTL, vertices */
        case 0x29:      /* 3D_DRAW_IMMD */
            ati_3d_draw_immd(s, p[0], count > 1 ? p[1] : 0, p + 2,
                             count > 2 ? count - 2 : 0);
            break;
        case 0x32:      /* 3D_CLEAR_ZMASK */
            if (s->r300) {
                ati_r300_clear_zmask(s, p, count);
            } else {
                ati_3d_clear_zmask(s, p, count);
            }
            break;
        case 0x37:      /* R300 3D_CLEAR_HIZ: no hierarchical Z here */
        case 0x38:      /* R300 3D_CLEAR_CMASK: no colour compression */
            break;
        default: {
            static bool warned[256];

            if (!warned[op]) {
                warned[op] = true;
                qemu_log_mask(LOG_UNIMP, "ati: CP packet3 opcode 0x%02x "
                              "(%u words) not implemented\n", op, count);
            }
            break;
        }
        }
        break;
    }
    }
}

/* Feed one word of a PM4 stream into parser state `cp`. */
static void ati_cp_feed(ATIVGAState *s, ATICPState *cp, uint32_t word)
{
    if (cp->skip) {
        cp->skip--;
        return;
    }
    if (!cp->want) {
        cp->hdr = word;
        cp->have = 0;
        switch (PM4_TYPE(word)) {
        case 0:
        case 3:
            cp->want = PM4_COUNT(word);
            break;
        case 1:
            cp->want = 2;
            break;
        case 2:
            return;             /* padding: no payload */
        }
        if (cp->want > ATI_CP_MAX_PACKET) {
            qemu_log_mask(LOG_GUEST_ERROR, "ati: CP packet of %u words "
                          "too long, dropped\n", cp->want);
            cp->skip = cp->want;
            cp->want = 0;
        }
        return;
    }
    cp->buf[cp->have++] = word;
    if (cp->have == cp->want) {
        cp->want = 0;
        ati_cp_packet(s, cp->hdr, cp->buf, cp->have);
    }
}

/* One word of the primary (PIO) stream. */
void ati_cp_stream_word(ATIVGAState *s, uint32_t word)
{
    ati_cp_feed(s, &s->cp, word);
}

/*
 * The ring buffer: CP_RB_BASE (a card address) holds 2^(RB_BUFSZ + 1)
 * dwords, RB_BUFSZ being CP_RB_CNTL bits 5:0. The driver writes packets
 * and moves CP_RB_WPTR; the CP runs them up to it, then reports its read
 * pointer in CP_RB_RPTR and, unless RB_NO_UPDATE (bit 27) is set, writes
 * it to CP_RB_RPTR_ADDR, in the byte order bits 1:0 there give. Ring words
 * are in the byte order of CP_RB_CNTL's swap field, like indirect buffers.
 */
static uint32_t ati_reg(ATIVGAState *s, uint32_t reg);

int64_t ati_cp_ns;              /* time spent running ring batches */

/*
 * Runs the ring up to CP_RB_WPTR. With the command processor thread
 * (ati_cp_thread), on that thread, with the BQL held but given up every
 * so often so the guest can reach the card's registers meanwhile.
 */
static void ati_cp_ring_do(ATIVGAState *s, bool threaded)
{
    int64_t t0 = get_clock();
    uint32_t since_yield = 0;
    uint32_t cntl = ati_reg(s, CP_RB_CNTL);
    uint32_t mode = ati_reg(s, CP_CSQ_CNTL) >> 28;
    uint32_t size = 2u << (cntl & 0x3f);
    uint32_t base = ati_reg(s, CP_RB_BASE) & ~3u;
    uint32_t rptr = ati_reg(s, CP_RB_RPTR) & (size - 1);
    uint32_t wptr = ati_reg(s, CP_RB_WPTR) & (size - 1);
    bool swap32 = ((cntl >> 16) & 3) == 2;
    uint32_t budget = size;

    /* CSQ modes with a bus-master primary queue: PRIBM_INDDIS, PRIBM_INDBM */
    if ((mode != 2 && mode != 4) || s->in_ring || (cntl & 0x3f) > 22) {
        return;
    }
    s->in_ring = true;
    while (rptr != wptr && budget--) {
        uint8_t b[4];

        if (!ati_mc_read(s, base + rptr * 4, b, 4)) {
            break;
        }
        rptr = (rptr + 1) & (size - 1);
        s->shadow[CP_RB_RPTR >> 2] = rptr;
        ati_cp_feed(s, &s->cp, swap32 ? ldl_be_p(b) : ldl_le_p(b));
        if (threaded && ++since_yield >= 256 && !s->cp.want) {
            /* between packets: let the vCPU at the registers */
            since_yield = 0;
            bql_unlock();
            bql_lock();
        }
        /* a packet may have moved the write pointer or reset the ring */
        wptr = ati_reg(s, CP_RB_WPTR) & (size - 1);
        rptr = ati_reg(s, CP_RB_RPTR) & (size - 1);
    }
    s->in_ring = false;
    /* the batch is done: what the host GPU drew goes to video memory */
    ati_r300_gl_flush(s);
    ati_r300_gl_touch(s);
    ati_cp_ns += get_clock() - t0;
    if (!(cntl & BIT(27))) {
        /* RB_RPTR_SWAP, bits 1:0 of the address: 2 is 32-bit swapping */
        uint32_t addr = ati_reg(s, CP_RB_RPTR_ADDR);
        ati_mc_write32(s, addr & ~3u, rptr, (addr & 3) == 2);
    }
}

static void *ati_cp_thread(void *opaque)
{
    ATIVGAState *s = opaque;

    qemu_mutex_lock(&s->cp_lock);
    for (;;) {
        while (!s->cp_kick) {
            qemu_cond_wait(&s->cp_cond, &s->cp_lock);
        }
        s->cp_kick = false;
        qemu_mutex_unlock(&s->cp_lock);

        bql_lock();
        s->cp_busy = true;
        ati_cp_ring_do(s, true);
        s->cp_busy = false;
        bql_unlock();

        qemu_mutex_lock(&s->cp_lock);
    }
    return NULL;
}

/*
 * CP_RB_WPTR (or the queue mode) was written. With the command processor
 * thread the guest goes on at once and the batch runs there; completion
 * shows as on the hardware, through the read pointer, fences and
 * interrupts, each written only once what was drawn before it is in video
 * memory. PPCOSX_ATI_CP_THREAD=0 runs batches synchronously instead.
 */
void ati_cp_ring_run(ATIVGAState *s)
{
    if (s->cp_threaded) {
        qemu_mutex_lock(&s->cp_lock);
        s->cp_kick = true;
        qemu_cond_signal(&s->cp_cond);
        qemu_mutex_unlock(&s->cp_lock);
        return;
    }
    ati_cp_ring_do(s, false);
}

/*
 * Off unless PPCOSX_ATI_CP_THREAD=1: Apple's driver waits on each batch,
 * so the thread overlaps little with the guest and measured slower than
 * running the ring in the register write that kicks it.
 */
void ati_cp_start_thread(ATIVGAState *s)
{
    const char *e = getenv("PPCOSX_ATI_CP_THREAD");

    if (!s->r300 || !e || strcmp(e, "1")) {
        return;
    }
    qemu_mutex_init(&s->cp_lock);
    qemu_cond_init(&s->cp_cond);
    s->cp_threaded = true;
    qemu_thread_create(&s->cp_thread, "ati-cp", ati_cp_thread, s,
                       QEMU_THREAD_DETACHED);
}

/* Queued or running command processor work: RBBM_STATUS GUI_ACTIVE. */
bool ati_cp_busy(ATIVGAState *s)
{
    return s->cp_busy ||
           (s->cp_threaded &&
            (ati_reg(s, CP_RB_RPTR) != ati_reg(s, CP_RB_WPTR)));
}

void ati_cp_reset(ATIVGAState *s)
{
    memset(&s->cp, 0, sizeof(s->cp));
    memset(&s->cp_ib, 0, sizeof(s->cp_ib));
    s->in_ib = false;
    s->in_ring = false;
}

/* ---- the card's view of memory ------------------------------------------ */

/*
 * The UniNorth AGP bridge (hw/pci-host/uninorth.c) translates AGP
 * addresses; machines without one pass them through unchanged.
 */
bool __attribute__((weak)) unin_agp_translate(PCIBus *bus, uint64_t addr,
                                              uint64_t *out)
{
    *out = addr;
    return true;
}

static uint32_t ati_reg(ATIVGAState *s, uint32_t reg)
{
    return s->shadow[reg >> 2];
}

/*
 * Translate a memory-controller address. The framebuffer range
 * (MC_FB_LOCATION) is video memory; the PCI GART window (AIC_LO_ADDR to
 * AIC_HI_ADDR) maps 4 KB pages through a table of 32-bit little-endian bus
 * addresses at AIC_PT_BASE. Returns false if the address maps nowhere.
 */
static bool ati_mc_translate(ATIVGAState *s, uint32_t mc, bool *vram,
                             uint64_t *addr)
{
    uint32_t fb = ati_reg(s, MC_FB_LOCATION);
    uint32_t fb_start = (fb & 0xffff) << 16;
    uint32_t fb_end = (fb & 0xffff0000) | 0xffff;

    if (fb && mc >= fb_start && mc <= fb_end) {
        *vram = true;
        *addr = mc - fb_start;
        return *addr < s->vga.vram_size;
    }
    if ((ati_reg(s, AIC_CNTL) & 1) && mc >= ati_reg(s, AIC_LO_ADDR) &&
        mc <= ati_reg(s, AIC_HI_ADDR)) {
        uint32_t idx = (mc - ati_reg(s, AIC_LO_ADDR)) >> 12;
        uint32_t entry;
        /* the R300 keeps the table's bus address in 0xab0 (high in 0xab4) */
        uint64_t table = s->r300 ?
            (ati_reg(s, 0xab0) & ~0xfffu) | (uint64_t)ati_reg(s, 0xab4) << 32 :
            ati_reg(s, AIC_PT_BASE);

        if (pci_dma_read(&s->dev, table + idx * 4, &entry, 4) != MEMTX_OK) {
            return false;
        }
        *vram = false;
        *addr = (le32_to_cpu(entry) & ~0xfffu) | (mc & 0xfff);
        return true;
    }
    /*
     * AGP: MC_AGP_LOCATION maps to bus addresses from AGP_BASE, which the
     * UniNorth bridge passes through its GART.
     */
    uint32_t agp = ati_reg(s, MC_AGP_LOCATION);
    uint32_t agp_start = (agp & 0xffff) << 16;
    uint32_t agp_end = (agp & 0xffff0000) | 0xffff;

    if (s->agp && agp_start <= agp_end && mc >= agp_start && mc <= agp_end) {
        *vram = false;
        return unin_agp_translate(pci_get_bus(&s->dev),
                                  (uint64_t)ati_reg(s, AGP_BASE) +
                                  (mc - agp_start), addr);
    }
    return false;
}

bool ati_mc_write32(ATIVGAState *s, uint32_t mc, uint32_t val, bool big_endian)
{
    uint8_t bytes[4];
    uint64_t addr;
    bool vram;

    if (!ati_mc_translate(s, mc, &vram, &addr)) {
        qemu_log_mask(LOG_GUEST_ERROR, "ati: write to unmapped card "
                      "address 0x%08x\n", mc);
        return false;
    }
    if (big_endian) {
        stl_be_p(bytes, val);
    } else {
        stl_le_p(bytes, val);
    }
    if (vram) {
        if (addr + 4 > s->vga.vram_size) {
            return false;
        }
        ati_vram_write(s, addr, bytes, 4);
        return true;
    }
    return pci_dma_write(&s->dev, addr, bytes, 4) == MEMTX_OK;
}

/*
 * SCRATCH_REGn was written. With its SCRATCH_UMSK bit set the CP copies the
 * value to SCRATCH_ADDR + 4n, which is how drivers learn how far the CP has
 * got (fences). CP_RB_CNTL's swap field (bits 17:16) picks the byte order.
 */
void ati_cp_scratch_written(ATIVGAState *s, unsigned int n, uint32_t val)
{
    uint32_t addr = ati_reg(s, SCRATCH_ADDR);
    /*
     * Apple's R300 driver reads the copies little-endian with the ring
     * swapped, so there the swap is SCRATCH_ADDR's bits 1:0, as with
     * CP_RB_RPTR_ADDR; its RV100 driver expects them in the ring's order.
     */
    bool swap32 = s->r300 ? (addr & 3) == 2
                          : ((ati_reg(s, CP_RB_CNTL) >> 16) & 3) == 2;

    trace_ati_cp_scratch(n, val);
    /* a fence: what was drawn before it goes to video memory first */
    ati_r300_gl_flush(s);
    ati_r300_gl_touch(s);
    if (ati_reg(s, SCRATCH_UMSK) & (1u << n)) {
        ati_mc_write32(s, (addr & ~3u) + n * 4, val, swap32);
    }
}

/* Read `len` bytes at a card address, a page at a time. */
bool ati_mc_read(ATIVGAState *s, uint32_t mc, uint8_t *buf,
                        uint32_t len)
{
    while (len) {
        uint32_t chunk = MIN(len, 0x1000 - (mc & 0xfff));
        uint64_t addr;
        bool vram;

        if (!ati_mc_translate(s, mc, &vram, &addr)) {
            qemu_log_mask(LOG_GUEST_ERROR, "ati: read of unmapped card "
                          "address 0x%08x\n", mc);
            return false;
        }
        if (vram) {
            if (addr + chunk > s->vga.vram_size) {
                return false;
            }
            ati_vram_read(s, addr, buf, chunk);
        } else if (pci_dma_read(&s->dev, addr, buf, chunk) != MEMTX_OK) {
            return false;
        }
        mc += chunk;
        buf += chunk;
        len -= chunk;
    }
    return true;
}

/* Write `len` bytes at a card address, a page at a time. */
bool ati_mc_write(ATIVGAState *s, uint32_t mc, const uint8_t *buf,
                  uint32_t len)
{
    while (len) {
        uint32_t chunk = MIN(len, 0x1000 - (mc & 0xfff));
        uint64_t addr;
        bool vram;

        if (!ati_mc_translate(s, mc, &vram, &addr)) {
            qemu_log_mask(LOG_GUEST_ERROR, "ati: write to unmapped card "
                          "address 0x%08x\n", mc);
            return false;
        }
        if (vram) {
            if (addr + chunk > s->vga.vram_size) {
                return false;
            }
            ati_vram_write(s, addr, buf, chunk);
        } else if (pci_dma_write(&s->dev, addr, buf, chunk) != MEMTX_OK) {
            return false;
        }
        mc += chunk;
        buf += chunk;
        len -= chunk;
    }
    return true;
}

/* A DMA byte swap: 1 swaps bytes in 16-bit units, 2 in 32-bit, 3 halves */
static void ati_dma_swap(uint8_t *buf, size_t n, unsigned int mode)
{
    for (size_t i = 0; mode && i + 4 <= n; i += 4) {
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

/*
 * The bus-master DMA engines (DMA_GUI_TABLE_ADDR, DMA_VID_TABLE_ADDR):
 * writing the table's card address starts a walk of 16-byte little-endian
 * descriptors: source, destination and command, then a reserved word. The
 * command has the byte count in bits 20:0, the source and destination byte
 * swaps in 23:22 and 25:24, "hold the address" bits 28 (source) and 29
 * (destination), and the end of the list in bit 31. Apple's RV100 GL driver
 * reads pixels back this way, video memory to GART pages. The copy is done
 * at once, so the engines are never busy.
 */
void ati_dma_run(ATIVGAState *s, uint32_t table)
{
    static int log = -1;

    if (log < 0) {
        log = getenv("PPCOSX_ATI_DMA_LOG") != NULL;
    }
    for (unsigned int n = 0; n < 65536; n++) {
        uint32_t d[4], len;
        uint64_t off;
        g_autofree uint8_t *buf = NULL;

        if (!ati_mc_read(s, (table & ~0xfu) + n * 16, (uint8_t *)d, 16)) {
            return;
        }
        for (int i = 0; i < 4; i++) {
            d[i] = le32_to_cpu(d[i]);
        }
        if (log) {
            fprintf(stderr, "ati dma: table %08x [%u] %08x %08x %08x\n",
                    table, n, d[0], d[1], d[2]);
        }
        len = d[2] & 0x1fffff;
        if (len && !(d[2] & (3u << 28))) {
            buf = g_malloc(len);
            if (ati_mc_vram_range(s, d[0], len, &off)) {
                /* what the host GPU drew here goes to video memory first */
                ati_r300_gl_sync_range(s, off, len);
            }
            if (ati_mc_read(s, d[0], buf, len)) {
                ati_dma_swap(buf, len, (d[2] >> 22) & 3);
                ati_dma_swap(buf, len, (d[2] >> 24) & 3);
                if (ati_mc_vram_range(s, d[1], len, &off)) {
                    ati_r300_gl_changed(s, off, len);
                }
                ati_mc_write(s, d[1], buf, len);
            }
        } else if (len) {
            qemu_log_mask(LOG_UNIMP, "ati: DMA with a held address\n");
        }
        if (d[2] & 0x80000000u) {
            return;
        }
    }
}

/*
 * If card addresses [mc, mc + len) are all video memory, return the offset
 * of the first in *off. The Rage 128 has no MC_FB_LOCATION: its addresses
 * are offsets.
 */
bool ati_mc_vram_range(ATIVGAState *s, uint64_t mc, uint64_t len,
                       uint64_t *off)
{
    uint64_t start = 0, end = s->vga.vram_size;

    if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF) {
        uint32_t fb = ati_reg(s, MC_FB_LOCATION);

        if (fb) {
            start = (uint64_t)(fb & 0xffff) << 16;
            end = MIN(start + s->vga.vram_size,
                      ((uint64_t)(fb & 0xffff0000) | 0xffff) + 1);
        }
    }
    if (mc < start || mc + len > end) {
        return false;
    }
    *off = mc - start;
    return true;
}

/*
 * CP_IB_BUFSZ was written: run the indirect buffer of `dwords` words at
 * CP_IB_BASE. Indirect buffers hold complete packets and do not nest.
 */
void ati_cp_run_ib(ATIVGAState *s, uint32_t dwords)
{
    bool swap32 = ((ati_reg(s, CP_RB_CNTL) >> 16) & 3) == 2;
    uint32_t base = ati_reg(s, CP_IB_BASE) & ~3u;
    g_autofree uint8_t *buf = NULL;

    dwords &= 0x7fffff;
    trace_ati_cp_ib(base, dwords);
    if (!dwords || s->in_ib) {
        return;
    }
    buf = g_malloc((size_t)dwords * 4);
    if (!ati_mc_read(s, base, buf, dwords * 4)) {
        return;
    }
    s->in_ib = true;
    memset(&s->cp_ib, 0, sizeof(s->cp_ib));
    for (uint32_t i = 0; i < dwords; i++) {
        ati_cp_feed(s, &s->cp_ib, swap32 ? ldl_be_p(buf + i * 4)
                                         : ldl_le_p(buf + i * 4));
    }
    if (s->cp_ib.want) {
        qemu_log_mask(LOG_GUEST_ERROR, "ati: indirect buffer ends inside a "
                      "packet\n");
    }
    s->in_ib = false;
    if (!s->in_ring) {
        ati_r300_gl_flush(s);
        ati_r300_gl_touch(s);
    }
}
