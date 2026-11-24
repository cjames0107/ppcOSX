/*
 * ppcosx-gpu: paravirtual graphics device for PowerPC Mac OS X guests.
 *
 * PCI plumbing, the register block, the command-batch transport and a
 * host-presented scanout. GL commands are handed to the host renderer in
 * ppcosx-gpu-gl.c when the host has one, and GL overlays (see
 * ppcosx-gpu.h) are composited onto the scanout as it is shown.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "hw/display/ppcosx-gpu.h"
#include "ppcosx-gpu-gl.h"
#include "ui/console.h"
#include "ui/qemu-pixman.h"
#include "qom/object.h"

typedef struct PPCOSXGpuOverlay {
    bool placed;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint8_t *frame;                 /* top row first, big-endian xRGB */
    uint32_t frame_width;
    uint32_t frame_height;
} PPCOSXGpuOverlay;

typedef struct PPCOSXGpuScanout {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t offset;
} PPCOSXGpuScanout;

struct PPCOSXGpuState {
    PCIDevice parent_obj;

    QemuConsole *con;
    MemoryRegion vram;
    MemoryRegion mmio;
    uint64_t vram_size;

    uint64_t cmd_addr;
    uint32_t cmd_size;
    uint32_t status;
    uint32_t irq_status;
    uint32_t irq_enable;
    uint32_t cmd_done;

    PPCOSXGpuScanout scanout;       /* requested by the guest */
    PPCOSXGpuScanout shown;         /* what the console surface maps */

    PPCOSXGL *gl;                   /* NULL: host has no GL renderer */

    PPCOSXGpuOverlay overlay[PPCOSX_GPU_GL_MAX_CONTEXTS + 1];
    bool composing;                 /* console shows `composite` */
    uint8_t *composite;             /* scanout with overlays applied */
    uint32_t dirty_y0, dirty_y1;    /* rows overlays changed */
};

#define TYPE_PPCOSX_GPU "ppcosx-gpu"
OBJECT_DECLARE_SIMPLE_TYPE(PPCOSXGpuState, PPCOSX_GPU)

static void ppcosx_gpu_update_irq(PPCOSXGpuState *s)
{
    pci_set_irq(&s->parent_obj, !!(s->irq_status & s->irq_enable));
}

static uint32_t ppcosx_gpu_set_scanout(PPCOSXGpuState *s, const uint8_t *p,
                                       uint32_t len)
{
    PPCOSXGpuScanout sc;
    uint64_t size;

    if (len != sizeof(struct ppcosx_gpu_cmd_set_scanout)) {
        return PPCOSX_GPU_ST_BAD_ARG;
    }
    sc.width = ldl_be_p(p + 8);
    sc.height = ldl_be_p(p + 12);
    sc.stride = ldl_be_p(p + 16);
    sc.offset = ldl_be_p(p + 20);

    if (sc.width == 0) {
        memset(&s->scanout, 0, sizeof(s->scanout));
        return PPCOSX_GPU_ST_OK;
    }
    if (sc.width > 8192 || sc.height == 0 || sc.height > 8192 ||
        sc.stride < sc.width * 4 || (sc.stride & 3) || (sc.offset & 3)) {
        return PPCOSX_GPU_ST_BAD_ARG;
    }
    size = (uint64_t)sc.stride * sc.height;
    if (sc.offset > s->vram_size || size > s->vram_size - sc.offset) {
        return PPCOSX_GPU_ST_BAD_ARG;
    }
    s->scanout = sc;
    return PPCOSX_GPU_ST_OK;
}

static uint32_t ppcosx_gpu_flush(PPCOSXGpuState *s, const uint8_t *p,
                                 uint32_t len)
{
    if (len != sizeof(struct ppcosx_gpu_cmd_flush)) {
        return PPCOSX_GPU_ST_BAD_ARG;
    }
    /* Dirty tracking in the refresh path already covers the scanout. */
    return PPCOSX_GPU_ST_OK;
}

/* Mark the scanout rows an overlay covers for recompositing. */
static void ppcosx_gpu_overlay_damage(PPCOSXGpuState *s, PPCOSXGpuOverlay *o)
{
    int64_t y0, y1;

    if (!o->placed || !o->frame) {
        return;
    }
    y0 = MAX((int64_t)o->y, 0);
    y1 = MIN((int64_t)o->y + o->height, s->scanout.height);
    if (y0 >= y1) {
        return;
    }
    if (s->dirty_y0 == s->dirty_y1) {
        s->dirty_y0 = y0;
        s->dirty_y1 = y1;
    } else {
        s->dirty_y0 = MIN(s->dirty_y0, y0);
        s->dirty_y1 = MAX(s->dirty_y1, y1);
    }
}

static void ppcosx_gpu_overlay_remove(PPCOSXGpuState *s, uint32_t ctx)
{
    PPCOSXGpuOverlay *o = &s->overlay[ctx];

    ppcosx_gpu_overlay_damage(s, o);
    g_free(o->frame);
    memset(o, 0, sizeof(*o));
}

static uint32_t ppcosx_gpu_overlay_set(PPCOSXGpuState *s, const uint8_t *args,
                                       uint32_t len)
{
    uint32_t ctx, width, height, flags;
    PPCOSXGpuOverlay *o;

    if (len != sizeof(struct ppcosx_gpu_cmd_gl_overlay_set)) {
        return PPCOSX_GPU_ST_BAD_ARG;
    }
    ctx = ldl_be_p(args);
    width = ldl_be_p(args + 12);
    height = ldl_be_p(args + 16);
    flags = ldl_be_p(args + 20);
    if (ctx < 1 || ctx > PPCOSX_GPU_GL_MAX_CONTEXTS ||
        width > PPCOSX_GPU_GL_MAX_SIZE || height > PPCOSX_GPU_GL_MAX_SIZE) {
        return PPCOSX_GPU_ST_BAD_ARG;
    }
    o = &s->overlay[ctx];
    ppcosx_gpu_overlay_damage(s, o);    /* where it was */
    o->placed = (flags & PPCOSX_GPU_OVERLAY_VISIBLE) && width && height;
    o->x = (int32_t)ldl_be_p(args + 4);
    o->y = (int32_t)ldl_be_p(args + 8);
    o->width = width;
    o->height = height;
    ppcosx_gpu_overlay_damage(s, o);    /* where it is now */
    return PPCOSX_GPU_ST_OK;
}

static uint32_t ppcosx_gpu_overlay_frame(PPCOSXGpuState *s)
{
    uint32_t ctx = ppcosx_gl_bound_id(s->gl);
    uint32_t width, height;
    PPCOSXGpuOverlay *o;

    if (!ctx || !ppcosx_gl_frame_size(s->gl, &width, &height)) {
        return PPCOSX_GPU_ST_BAD_ARG;
    }
    o = &s->overlay[ctx];
    if (!o->frame || o->frame_width != width || o->frame_height != height) {
        g_free(o->frame);
        o->frame = g_malloc((size_t)width * height * 4);
        o->frame_width = width;
        o->frame_height = height;
    }
    ppcosx_gl_read_frame(s->gl, o->frame);
    ppcosx_gpu_overlay_damage(s, o);
    return PPCOSX_GPU_ST_OK;
}

static uint32_t ppcosx_gpu_gl_cmd(PPCOSXGpuState *s, uint32_t opcode,
                                  const uint8_t *p, uint32_t len)
{
    const uint8_t *args = p + sizeof(struct ppcosx_gpu_cmd_hdr);

    if (!s->gl) {
        return PPCOSX_GPU_ST_UNSUPPORTED;
    }

    switch (opcode) {
    case PPCOSX_GPU_CMD_GL_CTX_CREATE:
        if (len != sizeof(struct ppcosx_gpu_cmd_gl_ctx_create)) {
            return PPCOSX_GPU_ST_BAD_ARG;
        }
        return ppcosx_gl_ctx_create(s->gl, ldl_be_p(args), ldl_be_p(args + 4),
                                    ldl_be_p(args + 8), ldl_be_p(args + 12));
    case PPCOSX_GPU_CMD_GL_CTX_RESIZE:
        if (len != sizeof(struct ppcosx_gpu_cmd_gl_ctx_create)) {
            return PPCOSX_GPU_ST_BAD_ARG;
        }
        return ppcosx_gl_ctx_resize(s->gl, ldl_be_p(args), ldl_be_p(args + 4),
                                    ldl_be_p(args + 8), ldl_be_p(args + 12));
    case PPCOSX_GPU_CMD_GL_READBACK: {
        uint32_t offset;

        if (len != sizeof(struct ppcosx_gpu_cmd_gl_readback)) {
            return PPCOSX_GPU_ST_BAD_ARG;
        }
        offset = ldl_be_p(args);
        if (offset > s->vram_size || (offset & 3)) {
            return PPCOSX_GPU_ST_BAD_ARG;
        }
        return ppcosx_gl_readback(s->gl,
                                  (uint8_t *)memory_region_get_ram_ptr(&s->vram)
                                  + offset, s->vram_size - offset);
    }
    case PPCOSX_GPU_CMD_GL_CTX_DESTROY: {
        uint32_t ctx, st;

        if (len != sizeof(struct ppcosx_gpu_cmd_gl_ctx)) {
            return PPCOSX_GPU_ST_BAD_ARG;
        }
        ctx = ldl_be_p(args);
        st = ppcosx_gl_ctx_destroy(s->gl, ctx);
        if (st == PPCOSX_GPU_ST_OK) {
            ppcosx_gpu_overlay_remove(s, ctx);
        }
        return st;
    }
    case PPCOSX_GPU_CMD_GL_READPIXELS: {
        uint32_t offset;

        if (len != sizeof(struct ppcosx_gpu_cmd_gl_readpixels)) {
            return PPCOSX_GPU_ST_BAD_ARG;
        }
        offset = ldl_be_p(args);
        if (offset > s->vram_size || (offset & 3)) {
            return PPCOSX_GPU_ST_BAD_ARG;
        }
        return ppcosx_gl_read_pixels(s->gl, (int32_t)ldl_be_p(args + 4),
                                     (int32_t)ldl_be_p(args + 8),
                                     ldl_be_p(args + 12), ldl_be_p(args + 16),
                                     ldl_be_p(args + 20), ldl_be_p(args + 24),
                                     (uint8_t *)memory_region_get_ram_ptr(
                                         &s->vram) + offset,
                                     s->vram_size - offset);
    }
    case PPCOSX_GPU_CMD_GL_OVERLAY_SET:
        return ppcosx_gpu_overlay_set(s, args, len);
    case PPCOSX_GPU_CMD_GL_OVERLAY_FRAME:
        if (len != sizeof(struct ppcosx_gpu_cmd_hdr)) {
            return PPCOSX_GPU_ST_BAD_ARG;
        }
        return ppcosx_gpu_overlay_frame(s);
    case PPCOSX_GPU_CMD_GL_CTX_BIND:
        if (len != sizeof(struct ppcosx_gpu_cmd_gl_ctx)) {
            return PPCOSX_GPU_ST_BAD_ARG;
        }
        return ppcosx_gl_ctx_bind(s->gl, ldl_be_p(args));
    case PPCOSX_GPU_CMD_GL_PRESENT: {
        PPCOSXGpuScanout *sc = &s->scanout;
        PPCOSXGLRect damage;
        uint32_t st;

        if (len != sizeof(struct ppcosx_gpu_cmd_gl_present)) {
            return PPCOSX_GPU_ST_BAD_ARG;
        }
        if (sc->width == 0) {
            return PPCOSX_GPU_ST_OK;
        }
        st = ppcosx_gl_present(s->gl, (int32_t)ldl_be_p(args),
                               (int32_t)ldl_be_p(args + 4),
                               (uint8_t *)memory_region_get_ram_ptr(&s->vram)
                               + sc->offset,
                               sc->width, sc->height, sc->stride, &damage);
        if (st == PPCOSX_GPU_ST_OK && damage.height) {
            memory_region_set_dirty(&s->vram,
                                    sc->offset +
                                    (uint64_t)damage.y * sc->stride,
                                    (uint64_t)damage.height * sc->stride);
        }
        return st;
    }
    default:
        return ppcosx_gl_call(s->gl, opcode, args,
                              (len - sizeof(struct ppcosx_gpu_cmd_hdr)) / 4);
    }
}

static uint32_t ppcosx_gpu_run_cmds(PPCOSXGpuState *s, const uint8_t *buf);

static uint32_t ppcosx_gpu_run_batch(PPCOSXGpuState *s)
{
    g_autofree uint8_t *buf = NULL;
    uint32_t st;

    if (s->cmd_size < sizeof(struct ppcosx_gpu_cmd_hdr) ||
        s->cmd_size > PPCOSX_GPU_MAX_BATCH || (s->cmd_size & 3)) {
        return PPCOSX_GPU_ST_BAD_BATCH;
    }
    buf = g_malloc(s->cmd_size);
    if (pci_dma_read(&s->parent_obj, s->cmd_addr, buf, s->cmd_size)
        != MEMTX_OK) {
        return PPCOSX_GPU_ST_BAD_BATCH;
    }

    st = ppcosx_gpu_run_cmds(s, buf);
    if (s->gl) {
        ppcosx_gl_unbind(s->gl);    /* bindings never outlive a batch */
    }
    return st;
}

static uint32_t ppcosx_gpu_run_cmds(PPCOSXGpuState *s, const uint8_t *buf)
{
    uint32_t pos = 0;

    while (pos < s->cmd_size) {
        uint32_t opcode, len, st;

        if (s->cmd_size - pos < sizeof(struct ppcosx_gpu_cmd_hdr)) {
            return PPCOSX_GPU_ST_BAD_BATCH;
        }
        opcode = ldl_be_p(buf + pos);
        len = ldl_be_p(buf + pos + 4);
        if (len < sizeof(struct ppcosx_gpu_cmd_hdr) || (len & 3) ||
            len > s->cmd_size - pos) {
            return PPCOSX_GPU_ST_BAD_BATCH;
        }

        switch (opcode) {
        case PPCOSX_GPU_CMD_NOP:
            st = PPCOSX_GPU_ST_OK;
            break;
        case PPCOSX_GPU_CMD_SET_SCANOUT:
            st = ppcosx_gpu_set_scanout(s, buf + pos, len);
            break;
        case PPCOSX_GPU_CMD_FLUSH:
            st = ppcosx_gpu_flush(s, buf + pos, len);
            break;
        case PPCOSX_GPU_CMD_GL_CTX_CREATE ... PPCOSX_GPU_CMD_GL_READPIXELS:
        case PPCOSX_GPU_CMD_GL_FIRST ... PPCOSX_GPU_CMD_GL_LAST:
            st = ppcosx_gpu_gl_cmd(s, opcode, buf + pos, len);
            break;
        default:
            qemu_log_mask(LOG_UNIMP, "ppcosx-gpu: unimplemented opcode 0x%x\n",
                          opcode);
            st = PPCOSX_GPU_ST_UNSUPPORTED;
            break;
        }
        if (st != PPCOSX_GPU_ST_OK) {
            return st;
        }
        s->cmd_done++;
        pos += len;
    }
    return PPCOSX_GPU_ST_OK;
}

static uint64_t ppcosx_gpu_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    PPCOSXGpuState *s = opaque;

    switch (addr) {
    case PPCOSX_GPU_REG_MAGIC:
        return PPCOSX_GPU_MAGIC;
    case PPCOSX_GPU_REG_VERSION:
        return PPCOSX_GPU_VERSION;
    case PPCOSX_GPU_REG_FEATURES:
        return PPCOSX_GPU_F_SCANOUT | (s->gl ? PPCOSX_GPU_F_GL : 0);
    case PPCOSX_GPU_REG_VRAM_SIZE:
        return s->vram_size;
    case PPCOSX_GPU_REG_CMD_ADDR_LO:
        return (uint32_t)s->cmd_addr;
    case PPCOSX_GPU_REG_CMD_ADDR_HI:
        return s->cmd_addr >> 32;
    case PPCOSX_GPU_REG_CMD_SIZE:
        return s->cmd_size;
    case PPCOSX_GPU_REG_STATUS:
        return s->status;
    case PPCOSX_GPU_REG_IRQ_STATUS:
        return s->irq_status;
    case PPCOSX_GPU_REG_IRQ_ENABLE:
        return s->irq_enable;
    case PPCOSX_GPU_REG_CMD_DONE:
        return s->cmd_done;
    default:
        return 0;
    }
}

static void ppcosx_gpu_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    PPCOSXGpuState *s = opaque;

    switch (addr) {
    case PPCOSX_GPU_REG_CMD_ADDR_LO:
        s->cmd_addr = deposit64(s->cmd_addr, 0, 32, val);
        break;
    case PPCOSX_GPU_REG_CMD_ADDR_HI:
        s->cmd_addr = deposit64(s->cmd_addr, 32, 32, val);
        break;
    case PPCOSX_GPU_REG_CMD_SIZE:
        s->cmd_size = val;
        break;
    case PPCOSX_GPU_REG_CMD_KICK:
        s->status = ppcosx_gpu_run_batch(s);
        s->irq_status |= PPCOSX_GPU_IRQ_BATCH_DONE;
        ppcosx_gpu_update_irq(s);
        break;
    case PPCOSX_GPU_REG_IRQ_STATUS:
        s->irq_status &= ~val;
        ppcosx_gpu_update_irq(s);
        break;
    case PPCOSX_GPU_REG_IRQ_ENABLE:
        s->irq_enable = val & PPCOSX_GPU_IRQ_BATCH_DONE;
        ppcosx_gpu_update_irq(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps ppcosx_gpu_mmio_ops = {
    .read = ppcosx_gpu_mmio_read,
    .write = ppcosx_gpu_mmio_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

static bool ppcosx_gpu_any_overlay(PPCOSXGpuState *s)
{
    for (int i = 1; i <= PPCOSX_GPU_GL_MAX_CONTEXTS; i++) {
        if (s->overlay[i].placed && s->overlay[i].frame) {
            return true;
        }
    }
    return false;
}

/*
 * One composited pixel. `pixel` is what the guest shows there, inside the
 * overlay of context `ctx`; `frame` is the overlay frame's pixel (NULL if
 * the frame does not reach this far). Returns false to keep `pixel`.
 *
 * Besides the exact key colour this handles the key seen through a grey
 * translucent layer: window shadows, the Dock's backdrop, menus,
 * Dashboard's dimming. For a layer of grey d at opacity a the guest shows
 * p = a*d + (1-a)*key in every channel, and since the key is magenta
 * (255, ctx, 255), red minus green gives (1-a) and green then gives a*d,
 * so the same blend can be redone over the frame instead. Pure greys come
 * out unchanged (no key shows through), and other colours are left alone.
 */
static bool ppcosx_gpu_compose_pixel(uint32_t pixel, uint32_t ctx,
                                     const uint8_t *frame, uint8_t out[4])
{
    int r = (pixel >> 16) & 0xff, g = (pixel >> 8) & 0xff, b = pixel & 0xff;
    int k = ctx & 0xff, cover, base;

    if ((pixel & 0x00ffffff) == PPCOSX_GPU_OVERLAY_KEY(ctx)) {
        if (frame) {
            memcpy(out, frame, 4);
        } else {
            memset(out, 0, 4);      /* frame not resized yet */
        }
        return true;
    }
    if (abs(r - b) > 1 || r <= g) {
        return false;               /* not the key under a grey layer */
    }
    cover = (r - g) * 256 / (255 - k);          /* (1-a), in 1/256 */
    if (cover > 256) {
        cover = 256;
    }
    base = g - cover * k / 256;                 /* a*d */
    if (base < 0) {
        base = 0;
    }
    if (base > (256 - cover) * 255 / 256 + 2) {
        return false;               /* too bright to be such a blend */
    }
    out[0] = 0;
    for (int c = 1; c < 4; c++) {
        int v = base + (frame ? cover * frame[c] / 256 : 0);

        out[c] = MIN(v, 255);
    }
    return true;
}

/* Build one row of the composite: the scanout row, with overlay frames
 * wherever the row shows their key colour. */
static void ppcosx_gpu_compose_row(PPCOSXGpuState *s, uint32_t y)
{
    PPCOSXGpuScanout *sc = &s->scanout;
    const uint8_t *src = (const uint8_t *)memory_region_get_ram_ptr(&s->vram)
                         + sc->offset + (size_t)y * sc->stride;
    uint8_t *dst = s->composite + (size_t)y * sc->stride;

    memcpy(dst, src, (size_t)sc->width * 4);
    for (uint32_t ctx = 1; ctx <= PPCOSX_GPU_GL_MAX_CONTEXTS; ctx++) {
        PPCOSXGpuOverlay *o = &s->overlay[ctx];
        uint32_t key = PPCOSX_GPU_OVERLAY_KEY(ctx);
        int64_t x0, x1, fy;

        if (!o->placed || !o->frame || (int64_t)y < o->y ||
            (int64_t)y >= (int64_t)o->y + o->height) {
            continue;
        }
        x0 = MAX((int64_t)o->x, 0);
        x1 = MIN((int64_t)o->x + o->width, sc->width);
        fy = (int64_t)y - o->y;
        for (int64_t x = x0; x < x1; x++) {
            int64_t fx = x - o->x;
            uint32_t pixel = ldl_be_p(src + x * 4);
            const uint8_t *f = NULL;

            /* Quick reject: no key shows through unless red is high. */
            if ((pixel & 0x00ffffff) != key && ((pixel >> 16) & 0xff) <=
                ((pixel >> 8) & 0xff)) {
                continue;
            }
            if (fx < o->frame_width && fy < o->frame_height) {
                f = o->frame + ((size_t)fy * o->frame_width + fx) * 4;
            }
            ppcosx_gpu_compose_pixel(pixel, ctx, f, dst + x * 4);
        }
    }
}

static void ppcosx_gpu_gfx_update(void *opaque)
{
    PPCOSXGpuState *s = opaque;
    PPCOSXGpuScanout *sc = &s->scanout;
    DirtyBitmapSnapshot *snap;
    bool want = ppcosx_gpu_any_overlay(s);
    int y, ys = -1;

    if (sc->width == 0) {
        return;
    }

    if (memcmp(&s->shown, sc, sizeof(*sc)) != 0 || want != s->composing) {
        uint8_t *ptr = (uint8_t *)memory_region_get_ram_ptr(&s->vram)
                       + sc->offset;
        DisplaySurface *ds;

        s->shown = *sc;
        s->composing = want;
        g_free(s->composite);
        s->composite = NULL;
        if (want) {
            s->composite = g_malloc((size_t)sc->stride * sc->height);
            for (y = 0; y < sc->height; y++) {
                ppcosx_gpu_compose_row(s, y);
            }
            ptr = s->composite;
        }
        ds = qemu_create_displaysurface_from(sc->width, sc->height,
                                             PIXMAN_BE_x8r8g8b8, sc->stride,
                                             ptr);
        dpy_gfx_replace_surface(s->con, ds);
        dpy_gfx_update_full(s->con);
        s->dirty_y0 = s->dirty_y1 = 0;
        memory_region_snapshot_and_clear_dirty(&s->vram, sc->offset,
                                               (uint64_t)sc->stride *
                                               sc->height, DIRTY_MEMORY_VGA);
        return;
    }

    snap = memory_region_snapshot_and_clear_dirty(&s->vram, sc->offset,
                                                  (uint64_t)sc->stride *
                                                  sc->height,
                                                  DIRTY_MEMORY_VGA);
    for (y = 0; y < sc->height; y++) {
        bool dirty = memory_region_snapshot_get_dirty(&s->vram, snap,
                                                      sc->offset +
                                                      (uint64_t)sc->stride * y,
                                                      sc->stride);

        if (s->composing) {
            dirty = dirty || (y >= s->dirty_y0 && y < s->dirty_y1);
            if (dirty) {
                ppcosx_gpu_compose_row(s, y);
            }
        }
        if (dirty && ys < 0) {
            ys = y;
        }
        if (!dirty && ys >= 0) {
            dpy_gfx_update(s->con, 0, ys, sc->width, y - ys);
            ys = -1;
        }
    }
    if (ys >= 0) {
        dpy_gfx_update(s->con, 0, ys, sc->width, y - ys);
    }
    s->dirty_y0 = s->dirty_y1 = 0;
    g_free(snap);
}

static const GraphicHwOps ppcosx_gpu_gfx_ops = {
    .gfx_update = ppcosx_gpu_gfx_update,
};

static void ppcosx_gpu_reset(DeviceState *dev)
{
    PPCOSXGpuState *s = PPCOSX_GPU(dev);

    s->cmd_addr = 0;
    s->cmd_size = 0;
    s->status = 0;
    s->irq_status = 0;
    s->irq_enable = 0;
    s->cmd_done = 0;
    memset(&s->scanout, 0, sizeof(s->scanout));
    memset(&s->shown, 0, sizeof(s->shown));
    for (int i = 1; i <= PPCOSX_GPU_GL_MAX_CONTEXTS; i++) {
        ppcosx_gpu_overlay_remove(s, i);
    }
    s->composing = false;
    g_free(s->composite);
    s->composite = NULL;
    s->dirty_y0 = s->dirty_y1 = 0;
    if (s->gl) {
        ppcosx_gl_reset(s->gl);
    }
}

static void ppcosx_gpu_realize(PCIDevice *dev, Error **errp)
{
    PPCOSXGpuState *s = PPCOSX_GPU(dev);
    Object *obj = OBJECT(dev);

    if (s->vram_size < 16 * MiB || s->vram_size > 512 * MiB) {
        error_setg(errp, "ppcosx-gpu: vram must be between 16M and 512M");
        return;
    }
    s->vram_size = pow2ceil(s->vram_size);

    if (!memory_region_init_ram(&s->vram, obj, "ppcosx-gpu-vram",
                                s->vram_size, errp)) {
        return;
    }
    memory_region_init_io(&s->mmio, obj, &ppcosx_gpu_mmio_ops, s,
                          "ppcosx-gpu-mmio", PPCOSX_GPU_MMIO_SIZE);

    pci_config_set_interrupt_pin(dev->config, 1);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &s->vram);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
    memory_region_set_log(&s->vram, true, DIRTY_MEMORY_VGA);

    s->con = graphic_console_init(DEVICE(dev), 0, &ppcosx_gpu_gfx_ops, s);
    s->gl = ppcosx_gl_init();
}

static void ppcosx_gpu_exit(PCIDevice *dev)
{
    PPCOSXGpuState *s = PPCOSX_GPU(dev);

    graphic_console_close(s->con);
    for (int i = 1; i <= PPCOSX_GPU_GL_MAX_CONTEXTS; i++) {
        g_free(s->overlay[i].frame);
    }
    g_free(s->composite);
}

static const Property ppcosx_gpu_properties[] = {
    DEFINE_PROP_SIZE("vram", PPCOSXGpuState, vram_size, 64 * MiB),
};

static void ppcosx_gpu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->vendor_id = PPCOSX_GPU_PCI_VENDOR_ID;
    k->device_id = PPCOSX_GPU_PCI_DEVICE_ID;
    k->class_id = PCI_CLASS_DISPLAY_OTHER;
    k->realize = ppcosx_gpu_realize;
    k->exit = ppcosx_gpu_exit;
    dc->desc = "Paravirtual GPU for PowerPC Mac OS X guests";
    device_class_set_legacy_reset(dc, ppcosx_gpu_reset);
    device_class_set_props(dc, ppcosx_gpu_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo ppcosx_gpu_type_info = {
    .name = TYPE_PPCOSX_GPU,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PPCOSXGpuState),
    .class_init = ppcosx_gpu_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void ppcosx_gpu_register_types(void)
{
    type_register_static(&ppcosx_gpu_type_info);
}

type_init(ppcosx_gpu_register_types)
