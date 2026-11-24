/*
 * pxcore: one connection to the ppcosx-gpu GL channel, i.e. one host GL
 * context plus the command buffer shared with the PPCOSXFramebuffer driver.
 * Used by both libppcosxgl (one context per process) and the interposer
 * (one per application GL context).
 *
 * Licensed under the GNU GPL, version 2 or later.
 */

#ifndef PXCORE_H
#define PXCORE_H

#include <stdint.h>
#include <string.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>

typedef struct PXConn {
    io_connect_t connect;
    uint32_t *buf;              /* command buffer shared with the driver */
    uint32_t cap_words;
    uint32_t pos_words;
    int failed;                 /* a submit failed; stop sending */
    int width, height;
    uint32_t format;            /* PPCOSX_GPU_GL_FORMAT() */
    uint32_t ctx_id;            /* host context id; sets the overlay key */
    vm_address_t readback;      /* mapped readback area, 0 if not mapped */
    vm_size_t readback_size;
    int compiling;              /* between glNewList and glEndList */
    void *owner;                /* for whoever opened the connection */
} PXConn;

/* One client vertex array as the application specified it. */
typedef struct PXArray {
    uint32_t kind;              /* PPCOSX_GL_ARRAY_* */
    int size;                   /* components per vertex */
    uint32_t type;              /* GL type of each component */
    int stride;                 /* bytes between vertices; 0 = packed */
    const void *pointer;        /* vertex 0 */
} PXArray;

/* How the application has laid out pixel data it passes to GL. */
typedef struct PXUnpack {
    int alignment;
    int row_length;
    int skip_rows;
    int skip_pixels;
    int swap_bytes;
} PXUnpack;

/*
 * Create a host context with a width x height drawable whose buffers are
 * given by `format` (PPCOSX_GPU_GL_FORMAT()). NULL on failure.
 */
PXConn *px_open(int width, int height, uint32_t format);
void px_close(PXConn *px);

/* Send buffered commands. 0 on success. */
int px_flush(PXConn *px);

/* Reserve a command with `words` argument words; NULL if the link is down. */
uint32_t *px_emit(PXConn *px, uint32_t opcode, uint32_t words);

/* Change the drawable, keeping GL state. Contents become undefined. */
int px_resize(PXConn *px, int width, int height, uint32_t format);

/* Copy the drawable onto the screen with its top-left corner at (x, y). */
int px_present(PXConn *px, int x, int y);

/*
 * Overlays (see ppcosx-gpu.h): place or hide this context's overlay at a
 * screen rectangle, and send the drawable's current image to it. The
 * window surface must show PPCOSX_GPU_OVERLAY_KEY(px->ctx_id) for the
 * frame to appear.
 */
int px_set_overlay(PXConn *px, int x, int y, int width, int height,
                   int visible);
int px_overlay_frame(PXConn *px);

/*
 * Flush, then return the drawable's pixels: width * height 32-bit
 * big-endian ARGB, bottom row first. Valid until the next resize or close.
 */
const uint32_t *px_readback(PXConn *px);

/*
 * Texture uploads. Pixel data is repacked tightly and sent ahead of the
 * command. Each returns 0 on success and -1 if the format/type pair is one
 * the host path does not handle (nothing is sent in that case).
 */
int px_tex_image_2d(PXConn *px, const PXUnpack *unpack, uint32_t target,
                    int level, uint32_t internalformat, int width, int height,
                    int border, uint32_t format, uint32_t type,
                    const void *pixels);
int px_tex_sub_image_2d(PXConn *px, const PXUnpack *unpack, uint32_t target,
                        int level, int xoffset, int yoffset, int width,
                        int height, uint32_t format, uint32_t type,
                        const void *pixels);
void px_delete_textures(PXConn *px, int n, const uint32_t *ids);

/*
 * glReadPixels from the drawable into `pixels`, laid out as `pack` (the
 * application's GL_PACK_* settings) says. Returns -1, reading nothing, for
 * a format/type pair the host path does not handle.
 */
int px_read_pixels(PXConn *px, const PXUnpack *pack, int x, int y, int width,
                   int height, uint32_t format, uint32_t type, void *pixels);

/*
 * Draw `count` vertices starting at `first` from the given arrays, or, if
 * `indices` is not NULL, the `count` vertices it names (`index_type` is
 * GL_UNSIGNED_BYTE, _SHORT or _INT). Only the vertices actually used are
 * sent. Returns -1, sending nothing, for types or sizes the host path does
 * not take.
 */
int px_draw(PXConn *px, uint32_t mode, int first, int count,
            uint32_t index_type, const void *indices,
            const PXArray *arrays, int narrays);

/* Number of values a glLight/glMaterial/glFog/glTex* parameter array has. */
static inline int px_pname_count(uint32_t pname)
{
    switch (pname) {
    case 0x1200: /* AMBIENT */
    case 0x1201: /* DIFFUSE */
    case 0x1202: /* SPECULAR */
    case 0x1203: /* POSITION */
    case 0x1600: /* EMISSION */
    case 0x1602: /* AMBIENT_AND_DIFFUSE */
    case 0x0B53: /* LIGHT_MODEL_AMBIENT */
    case 0x0B66: /* FOG_COLOR */
    case 0x2201: /* TEXTURE_ENV_COLOR */
    case 0x1004: /* TEXTURE_BORDER_COLOR */
    case 0x2501: /* OBJECT_PLANE */
    case 0x2502: /* EYE_PLANE */
        return 4;
    case 0x1204: /* SPOT_DIRECTION */
    case 0x1603: /* COLOR_INDEXES */
        return 3;
    default:
        return 1;
    }
}

static inline uint32_t *wr_u(uint32_t *w, uint32_t v)
{
    *w = v;
    return w + 1;
}

static inline uint32_t *wr_f(uint32_t *w, float v)
{
    memcpy(w, &v, 4);
    return w + 1;
}

static inline uint32_t *wr_d(uint32_t *w, double v)
{
    memcpy(w, &v, 8);
    return w + 2;
}

#endif /* PXCORE_H */
