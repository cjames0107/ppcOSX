/*
 * ppcosx-gpu: paravirtual graphics device for PowerPC Mac OS X guests.
 *
 * This header is the guest/host contract. It is written so that it can be
 * included unchanged by the guest-side driver (a PowerPC kext), so it uses
 * only fixed-width types and no QEMU definitions.
 *
 * All registers and all command-buffer fields are BIG-ENDIAN, the guest's
 * native byte order.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef HW_DISPLAY_PPCOSX_GPU_H
#define HW_DISPLAY_PPCOSX_GPU_H

#define PPCOSX_GPU_PCI_VENDOR_ID    0x1234      /* QEMU, experimental */
#define PPCOSX_GPU_PCI_DEVICE_ID    0x7078

/* BAR0: scanout memory (prefetchable). BAR2: registers below. */
#define PPCOSX_GPU_MMIO_SIZE        0x1000

#define PPCOSX_GPU_REG_MAGIC        0x00    /* RO  'PXGP' */
#define PPCOSX_GPU_REG_VERSION      0x04    /* RO  major << 16 | minor */
#define PPCOSX_GPU_REG_FEATURES     0x08    /* RO  PPCOSX_GPU_F_* */
#define PPCOSX_GPU_REG_VRAM_SIZE    0x0c    /* RO  bytes in BAR0 */
#define PPCOSX_GPU_REG_CMD_ADDR_LO  0x10    /* RW  guest-physical batch address */
#define PPCOSX_GPU_REG_CMD_ADDR_HI  0x14    /* RW */
#define PPCOSX_GPU_REG_CMD_SIZE     0x18    /* RW  batch length in bytes */
#define PPCOSX_GPU_REG_CMD_KICK     0x1c    /* WO  any write runs the batch */
#define PPCOSX_GPU_REG_STATUS       0x20    /* RO  PPCOSX_GPU_ST_* of last batch */
#define PPCOSX_GPU_REG_IRQ_STATUS   0x24    /* RW  write 1 to clear */
#define PPCOSX_GPU_REG_IRQ_ENABLE   0x28    /* RW */
#define PPCOSX_GPU_REG_CMD_DONE     0x2c    /* RO  commands completed, wraps */

#define PPCOSX_GPU_MAGIC            0x50584750
#define PPCOSX_GPU_VERSION          0x00000008  /* 0.8 */

#define PPCOSX_GPU_F_SCANOUT        (1u << 0)   /* host-presented framebuffer */
#define PPCOSX_GPU_F_GL             (1u << 1)   /* GL command stream */

#define PPCOSX_GPU_IRQ_BATCH_DONE   (1u << 0)

#define PPCOSX_GPU_ST_OK            0
#define PPCOSX_GPU_ST_BAD_BATCH     1   /* unreadable or malformed batch */
#define PPCOSX_GPU_ST_BAD_ARG       2   /* command had an invalid argument */
#define PPCOSX_GPU_ST_UNSUPPORTED   3   /* opcode not implemented by host */

#define PPCOSX_GPU_MAX_BATCH        (1u << 20)

/*
 * A batch is a sequence of commands. Each starts with this header; `length`
 * is the size of the whole command including the header, a multiple of 4.
 */
struct ppcosx_gpu_cmd_hdr {
    uint32_t opcode;
    uint32_t length;
};

#define PPCOSX_GPU_CMD_NOP          0x0000
#define PPCOSX_GPU_CMD_SET_SCANOUT  0x0001
#define PPCOSX_GPU_CMD_FLUSH        0x0002

/*
 * GL command stream (PPCOSX_GPU_F_GL).
 *
 * A GL context is an offscreen colour + depth + stencil surface on the host
 * with its own GL state. Context commands (CREATE, DESTROY, BIND, RESIZE)
 * are privileged: the guest kernel driver issues them and must not let
 * applications submit them, and it must set the offset in every READBACK.
 * A batch has no bound context when it starts.
 *
 * 0x1000-0x1fff are GL calls executed against the bound context: generated
 * ones from 0x1000 (ppcosx-gl-ops.h, each argument one or two big-endian
 * 32-bit words) and hand-written ones that carry pixel data from 0x1800
 * (ppcosx-gl-image.h).
 */
#define PPCOSX_GPU_CMD_GL_CTX_CREATE    0x0010
#define PPCOSX_GPU_CMD_GL_CTX_DESTROY   0x0011
#define PPCOSX_GPU_CMD_GL_CTX_BIND      0x0012
#define PPCOSX_GPU_CMD_GL_PRESENT       0x0013
#define PPCOSX_GPU_CMD_GL_CTX_RESIZE    0x0014
#define PPCOSX_GPU_CMD_GL_READBACK      0x0015
#define PPCOSX_GPU_CMD_GL_OVERLAY_SET   0x0016  /* privileged */
#define PPCOSX_GPU_CMD_GL_OVERLAY_FRAME 0x0017
#define PPCOSX_GPU_CMD_GL_READPIXELS    0x0018
#define PPCOSX_GPU_CMD_GL_FIRST         0x1000
#define PPCOSX_GPU_CMD_GL_LAST          0x1fff

#define PPCOSX_GPU_GL_MAX_CONTEXTS      16      /* ids 1..16 */
#define PPCOSX_GPU_GL_MAX_SIZE          4096    /* per dimension */

/* Pixels are 32-bit big-endian xRGB. width == 0 disables the scanout. */
struct ppcosx_gpu_cmd_set_scanout {
    struct ppcosx_gpu_cmd_hdr hdr;
    uint32_t width;
    uint32_t height;
    uint32_t stride;    /* bytes per row, >= width * 4 */
    uint32_t offset;    /* byte offset of the first row in BAR0 */
};

/* Optional hint; the host also tracks dirty scanout memory on its own. */
struct ppcosx_gpu_cmd_flush {
    struct ppcosx_gpu_cmd_hdr hdr;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

/*
 * CTX_CREATE and CTX_RESIZE. Resizing keeps GL state; contents are lost.
 *
 * `format` gives the buffers the guest application's pixel format has, so
 * the host draws with the same ones: an absent alpha channel reads as 1, and
 * with no depth or stencil buffer those tests always pass, as OpenGL
 * requires. Build it with PPCOSX_GPU_GL_FORMAT().
 */
struct ppcosx_gpu_cmd_gl_ctx_create {
    struct ppcosx_gpu_cmd_hdr hdr;
    uint32_t ctx;
    uint32_t width;
    uint32_t height;
    uint32_t format;
};

#define PPCOSX_GPU_GL_FORMAT(alpha_bits, depth_bits, stencil_bits) \
    (((alpha_bits) ? 1u : 0u) | ((uint32_t)(depth_bits) & 0xff) << 8 | \
     ((uint32_t)(stencil_bits) & 0xff) << 16)
#define PPCOSX_GPU_GL_FORMAT_ALPHA(f)       ((f) & 1)
#define PPCOSX_GPU_GL_FORMAT_DEPTH(f)       (((f) >> 8) & 0xff)
#define PPCOSX_GPU_GL_FORMAT_STENCIL(f)     (((f) >> 16) & 0xff)
#define PPCOSX_GPU_GL_FORMAT_FULL           PPCOSX_GPU_GL_FORMAT(8, 24, 8)

/* CTX_DESTROY and CTX_BIND. */
struct ppcosx_gpu_cmd_gl_ctx {
    struct ppcosx_gpu_cmd_hdr hdr;
    uint32_t ctx;
};

/*
 * Copy the bound context's colour buffer onto the scanout with its top-left
 * corner at (x, y) in scanout pixels; both are signed and the copy is
 * clipped. This is how rendered frames reach the screen for now.
 */
struct ppcosx_gpu_cmd_gl_present {
    struct ppcosx_gpu_cmd_hdr hdr;
    int32_t x;
    int32_t y;
};

/*
 * Copy the bound context's colour buffer into BAR0 at `offset`, as
 * width * height 32-bit big-endian ARGB pixels, bottom row first (the
 * layout glDrawPixels takes). The guest keeps such areas clear of the
 * scanout. This is how a frame gets back into an application's own window.
 */
struct ppcosx_gpu_cmd_gl_readback {
    struct ppcosx_gpu_cmd_hdr hdr;
    uint32_t offset;
};

/*
 * glReadPixels of the bound context: the width x height rectangle at (x, y)
 * in `format` / `type`, tightly packed with multi-byte components
 * big-endian, written to BAR0 at `offset`. As with READBACK the guest
 * kernel driver sets `offset` to the context's own area, and must refuse
 * reads that would not fit in it.
 */
struct ppcosx_gpu_cmd_gl_readpixels {
    struct ppcosx_gpu_cmd_hdr hdr;
    uint32_t offset;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t type;
};

/*
 * Overlays: how GL frames reach application windows without passing back
 * through the guest.
 *
 * The guest fills the window surface of context `ctx` with that context's
 * key colour, PPCOSX_GPU_OVERLAY_KEY(ctx), and places the context's overlay
 * at the surface's screen position. Whenever the host shows the scanout,
 * every scanout pixel inside the overlay that has exactly the key colour
 * shows the overlay's latest frame instead, and a pixel showing the key
 * through a grey translucent layer (a shadow, the Dock) shows the frame
 * through that layer. The guest's window server thereby decides what is
 * visible: anything drawn over the surface covers the key. The guest's own
 * memory is never changed.
 *
 * OVERLAY_SET places (flags bit 0 set) or hides the overlay of any context;
 * it is privileged. OVERLAY_FRAME makes the bound context's current colour
 * buffer that context's overlay frame. A context's overlay goes away with
 * it.
 */
#define PPCOSX_GPU_OVERLAY_KEY(ctx) \
    (0x00ff00ffu | ((uint32_t)(ctx) & 0xff) << 8)
#define PPCOSX_GPU_OVERLAY_VISIBLE  1u

struct ppcosx_gpu_cmd_gl_overlay_set {
    struct ppcosx_gpu_cmd_hdr hdr;
    uint32_t ctx;
    int32_t x;          /* scanout pixels; may be partly off screen */
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
};

#endif /* HW_DISPLAY_PPCOSX_GPU_H */
