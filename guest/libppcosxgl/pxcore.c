/*
 * pxcore: see pxcore.h. PowerPC is big-endian, which is the wire byte
 * order, so command words are stored as they are.
 *
 * Licensed under the GNU GPL, version 2 or later.
 */

#include <stdlib.h>

#include "pxcore.h"
#include "ppcosx-gpu.h"
#include "ppcosx-gl-image.h"

#define kPPCOSXGLConnectType    0x7078676c      /* 'pxgl' */
#define kMethodCreateContext    0
#define kMethodSubmit           1
#define kMethodResizeContext    2
#define kMethodSetOverlay       3
#define kMemoryCommands         0
#define kMemoryReadback         1

PXConn *px_open(int width, int height, uint32_t format)
{
    io_service_t service;
    vm_address_t addr = 0;
    vm_size_t size = 0;
    kern_return_t kr;
    int ctx = 0;
    PXConn *px;

    service = IOServiceGetMatchingService(kIOMasterPortDefault,
                  IOServiceMatching("PPCOSXFramebuffer"));
    if (!service)
        return 0;
    px = calloc(1, sizeof(*px));
    kr = px ? IOServiceOpen(service, mach_task_self(), kPPCOSXGLConnectType,
                            &px->connect) : KERN_NO_SPACE;
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) {
        free(px);
        return 0;
    }

    kr = IOConnectMethodScalarIScalarO(px->connect, kMethodCreateContext,
                                       3, 1, width, height, format, &ctx);
    if (kr == KERN_SUCCESS)
        kr = IOConnectMapMemory(px->connect, kMemoryCommands,
                                mach_task_self(), &addr, &size,
                                kIOMapAnywhere);
    if (kr != KERN_SUCCESS) {
        IOServiceClose(px->connect);
        free(px);
        return 0;
    }

    px->buf = (uint32_t *) addr;
    px->cap_words = size / 4;
    px->width = width;
    px->height = height;
    px->format = format;
    px->ctx_id = (uint32_t) ctx;
    return px;
}

static void px_unmap_readback(PXConn *px)
{
    if (px->readback) {
        IOConnectUnmapMemory(px->connect, kMemoryReadback, mach_task_self(),
                             px->readback);
        px->readback = 0;
    }
}

void px_close(PXConn *px)
{
    if (!px)
        return;
    px_flush(px);
    px_unmap_readback(px);
    IOServiceClose(px->connect);    /* the driver destroys the context */
    free(px);
}

int px_flush(PXConn *px)
{
    kern_return_t kr;

    if (px->failed)
        return -1;
    if (px->pos_words == 0)
        return 0;
    kr = IOConnectMethodScalarIScalarO(px->connect, kMethodSubmit, 1, 0,
                                       px->pos_words * 4);
    px->pos_words = 0;
    if (kr != KERN_SUCCESS) {
        px->failed = 1;
        return -1;
    }
    return 0;
}

uint32_t *px_emit(PXConn *px, uint32_t opcode, uint32_t words)
{
    uint32_t *w;

    if (px->failed)
        return 0;
    if (px->pos_words + 2 + words > px->cap_words && px_flush(px) != 0)
        return 0;
    w = px->buf + px->pos_words;
    w[0] = opcode;
    w[1] = (2 + words) * 4;
    px->pos_words += 2 + words;
    return w + 2;
}

int px_resize(PXConn *px, int width, int height, uint32_t format)
{
    kern_return_t kr;

    if (px_flush(px) != 0)
        return -1;
    px_unmap_readback(px);      /* the driver may move the area */
    kr = IOConnectMethodScalarIScalarO(px->connect, kMethodResizeContext,
                                       3, 0, width, height, format);
    if (kr != KERN_SUCCESS)
        return -1;
    px->width = width;
    px->height = height;
    px->format = format;
    return 0;
}

int px_present(PXConn *px, int x, int y)
{
    uint32_t *w = px_emit(px, PPCOSX_GPU_CMD_GL_PRESENT, 2);

    if (!w)
        return -1;
    w = wr_u(w, (uint32_t) x);
    w = wr_u(w, (uint32_t) y);
    return px_flush(px);
}

int px_set_overlay(PXConn *px, int x, int y, int width, int height,
                   int visible)
{
    /* A method call of its own, not part of the command stream, so it is
     * safe from another thread. */
    return IOConnectMethodScalarIScalarO(px->connect, kMethodSetOverlay, 5, 0,
                                         x, y, width, height, visible)
           == KERN_SUCCESS ? 0 : -1;
}

int px_overlay_frame(PXConn *px)
{
    if (!px_emit(px, PPCOSX_GPU_CMD_GL_OVERLAY_FRAME, 0))
        return -1;
    return px_flush(px);
}

/* Map the context's readback area into this task, once. */
static int px_map_area(PXConn *px)
{
    if (!px->readback &&
        IOConnectMapMemory(px->connect, kMemoryReadback, mach_task_self(),
                           &px->readback, &px->readback_size,
                           kIOMapAnywhere | kIOMapReadOnly) != KERN_SUCCESS) {
        px->readback = 0;
        return -1;
    }
    return 0;
}

const uint32_t *px_readback(PXConn *px)
{
    uint32_t *w = px_emit(px, PPCOSX_GPU_CMD_GL_READBACK, 1);

    if (!w)
        return 0;
    wr_u(w, 0);                 /* the driver fills in the real offset */
    if (px_flush(px) != 0 || px_map_area(px) != 0)
        return 0;
    return (const uint32_t *) px->readback;
}

/* ---- pixel data --------------------------------------------------------- */

#define kBlobChunk  (64 * 1024)

static int px_send_blob(PXConn *px, const uint8_t *data, uint32_t bytes)
{
    while (bytes) {
        uint32_t n = bytes < kBlobChunk ? bytes : kBlobChunk;
        uint32_t words = (n + 3) / 4;
        uint32_t *w = px_emit(px, PPCOSX_GL_BLOB, 1 + words);

        if (!w)
            return -1;
        w[0] = n;
        w[words] = 0;                   /* zero the padding */
        memcpy(w + 1, data, n);
        data += n;
        bytes -= n;
    }
    return 0;
}

/*
 * Send a width x height image, converting from the application's unpack
 * layout to tightly packed rows in native byte order.
 */
static int px_send_pixels(PXConn *px, const PXUnpack *unpack, int width,
                          int height, uint32_t format, uint32_t type,
                          const void *pixels)
{
    uint32_t bpp = ppcosx_gl_pixel_bytes(format, type);
    uint32_t unit = ppcosx_gl_type_unit(type);
    uint32_t row = (uint32_t) width * bpp;
    uint32_t stride, total = row * (uint32_t) height;
    const uint8_t *src = (const uint8_t *) pixels;
    uint8_t *tight, *dst;
    int y, ret;

    stride = (uint32_t) (unpack->row_length > 0 ? unpack->row_length : width)
             * bpp;
    if ((int) unit < unpack->alignment)
        stride = (stride + unpack->alignment - 1) / unpack->alignment
                 * unpack->alignment;
    src += (uint32_t) unpack->skip_rows * stride
           + (uint32_t) unpack->skip_pixels * bpp;

    if (stride == row && !(unpack->swap_bytes && unit > 1))
        return px_send_blob(px, src, total);

    tight = malloc(total ? total : 1);
    if (!tight)
        return -1;
    dst = tight;
    for (y = 0; y < height; y++, src += stride, dst += row) {
        if (unpack->swap_bytes && unit > 1) {
            uint32_t i, k;

            for (i = 0; i + unit <= row; i += unit)
                for (k = 0; k < unit; k++)
                    dst[i + k] = src[i + unit - 1 - k];
        } else {
            memcpy(dst, src, row);
        }
    }
    ret = px_send_blob(px, tight, total);
    free(tight);
    return ret;
}

int px_tex_image_2d(PXConn *px, const PXUnpack *unpack, uint32_t target,
                    int level, uint32_t internalformat, int width, int height,
                    int border, uint32_t format, uint32_t type,
                    const void *pixels)
{
    uint32_t *w;

    if (width < 0 || height < 0 ||
        width > PPCOSX_GL_MAX_TEX_SIZE + 2 ||
        height > PPCOSX_GL_MAX_TEX_SIZE + 2)
        return -1;
    if (pixels) {
        if (!ppcosx_gl_pixel_bytes(format, type))
            return -1;
        if (width == 0 || height == 0)
            pixels = 0;
        else if (px_send_pixels(px, unpack, width, height, format, type,
                                pixels) != 0)
            return 0;                   /* link down, not a format problem */
    }
    w = px_emit(px, PPCOSX_GL_TexImage2D, 9);
    if (!w)
        return 0;
    w = wr_u(w, target);
    w = wr_u(w, level);
    w = wr_u(w, internalformat);
    w = wr_u(w, width);
    w = wr_u(w, height);
    w = wr_u(w, border);
    w = wr_u(w, format);
    w = wr_u(w, type);
    w = wr_u(w, pixels != 0);
    return 0;
}

int px_tex_sub_image_2d(PXConn *px, const PXUnpack *unpack, uint32_t target,
                        int level, int xoffset, int yoffset, int width,
                        int height, uint32_t format, uint32_t type,
                        const void *pixels)
{
    uint32_t *w;

    if (!pixels || width <= 0 || height <= 0)
        return 0;                       /* nothing to upload */
    if (width > PPCOSX_GL_MAX_TEX_SIZE + 2 ||
        height > PPCOSX_GL_MAX_TEX_SIZE + 2 ||
        !ppcosx_gl_pixel_bytes(format, type))
        return -1;
    if (px_send_pixels(px, unpack, width, height, format, type, pixels) != 0)
        return 0;
    w = px_emit(px, PPCOSX_GL_TexSubImage2D, 8);
    if (!w)
        return 0;
    w = wr_u(w, target);
    w = wr_u(w, level);
    w = wr_u(w, xoffset);
    w = wr_u(w, yoffset);
    w = wr_u(w, width);
    w = wr_u(w, height);
    w = wr_u(w, format);
    w = wr_u(w, type);
    return 0;
}

void px_delete_textures(PXConn *px, int n, const uint32_t *ids)
{
    uint32_t *w;
    int i;

    if (n <= 0 || n > 4096 || !ids)
        return;
    w = px_emit(px, PPCOSX_GL_DeleteTextures, 1 + n);
    if (!w)
        return;
    w = wr_u(w, n);
    for (i = 0; i < n; i++)
        w = wr_u(w, ids[i]);
}

/* ---- vertex arrays ------------------------------------------------------ */

int px_draw(PXConn *px, uint32_t mode, int first, int count,
            uint32_t index_type, const void *indices,
            const PXArray *arrays, int narrays)
{
    uint32_t *rebased = 0;
    uint32_t vertices, nindices = 0;
    uint32_t *w;
    int i, ret = 0;

    if (count <= 0)
        return 0;
    if (narrays < 1 || narrays > PPCOSX_GL_MAX_ARRAYS || first < 0)
        return -1;
    for (i = 0; i < narrays; i++)
        if (!ppcosx_gl_array_unit(arrays[i].type) || arrays[i].size < 1 ||
            arrays[i].size > 4 || arrays[i].stride < 0)
            return -1;

    if (indices) {
        uint32_t lo = 0xffffffff, hi = 0, k;

        if ((uint32_t) count > PPCOSX_GL_MAX_INDICES)
            return -1;
        rebased = malloc((size_t) count * 4);
        if (!rebased)
            return -1;
        for (k = 0; k < (uint32_t) count; k++) {
            uint32_t v;

            switch (index_type) {
            case 0x1401: v = ((const uint8_t *) indices)[k];  break;
            case 0x1403: v = ((const uint16_t *) indices)[k]; break;
            case 0x1405: v = ((const uint32_t *) indices)[k]; break;
            default:
                free(rebased);
                return -1;
            }
            rebased[k] = v;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        if (hi - lo >= PPCOSX_GL_MAX_VERTICES) {
            free(rebased);
            return -1;
        }
        for (k = 0; k < (uint32_t) count; k++)
            rebased[k] -= lo;
        first = (int) lo;
        vertices = hi - lo + 1;
        nindices = (uint32_t) count;
    } else {
        if ((uint32_t) count > PPCOSX_GL_MAX_VERTICES)
            return -1;
        vertices = (uint32_t) count;
    }

    /* Each array, packed: only vertices first .. first + vertices - 1. */
    for (i = 0; i < narrays && ret == 0; i++) {
        uint32_t elem = (uint32_t) arrays[i].size *
                        ppcosx_gl_array_unit(arrays[i].type);
        uint32_t stride = arrays[i].stride ? (uint32_t) arrays[i].stride
                                           : elem;
        const uint8_t *src = (const uint8_t *) arrays[i].pointer
                             + (size_t) first * stride;

        if (stride == elem) {
            ret = px_send_blob(px, src, vertices * elem);
        } else {
            uint8_t *tight = malloc((size_t) vertices * elem);
            uint32_t v;

            if (!tight) {
                ret = -2;
                break;
            }
            for (v = 0; v < vertices; v++)
                memcpy(tight + (size_t) v * elem, src + (size_t) v * stride,
                       elem);
            ret = px_send_blob(px, tight, vertices * elem);
            free(tight);
        }
    }
    if (ret == 0 && nindices)
        ret = px_send_blob(px, (const uint8_t *) rebased, nindices * 4);
    free(rebased);
    if (ret != 0)
        return 0;                       /* link down: nothing more to do */

    w = px_emit(px, PPCOSX_GL_Draw, 4 + narrays * 3);
    if (!w)
        return 0;
    w = wr_u(w, mode);
    w = wr_u(w, vertices);
    w = wr_u(w, nindices);
    w = wr_u(w, narrays);
    for (i = 0; i < narrays; i++) {
        w = wr_u(w, arrays[i].kind);
        w = wr_u(w, arrays[i].size);
        w = wr_u(w, arrays[i].type);
    }
    return 0;
}

/* ---- reading pixels back ------------------------------------------------ */

int px_read_pixels(PXConn *px, const PXUnpack *pack, int x, int y, int width,
                   int height, uint32_t format, uint32_t type, void *pixels)
{
    uint32_t bpp = ppcosx_gl_pixel_bytes(format, type);
    uint32_t unit = ppcosx_gl_type_unit(type);
    uint32_t row, stride, rows_per_read;
    uint8_t *dst = (uint8_t *) pixels;
    int done = 0;

    if (!bpp || width <= 0 || height <= 0 ||
        width > PPCOSX_GL_MAX_TEX_SIZE || height > PPCOSX_GL_MAX_TEX_SIZE)
        return -1;
    if (px_flush(px) != 0 || px_map_area(px) != 0)
        return 0;               /* link down: leave the buffer alone */
    row = (uint32_t) width * bpp;
    if (row > px->readback_size)
        return -1;
    rows_per_read = px->readback_size / row;

    /* Where the application wants each row. */
    stride = (uint32_t) (pack->row_length > 0 ? pack->row_length : width) * bpp;
    if ((int) unit < pack->alignment)
        stride = (stride + pack->alignment - 1) / pack->alignment
                 * pack->alignment;
    dst += (uint32_t) pack->skip_rows * stride
           + (uint32_t) pack->skip_pixels * bpp;

    while (done < height) {
        int n = height - done;
        uint32_t *w;
        const uint8_t *src;
        int r;

        if ((uint32_t) n > rows_per_read)
            n = (int) rows_per_read;
        w = px_emit(px, PPCOSX_GPU_CMD_GL_READPIXELS, 7);
        if (!w)
            return 0;
        w = wr_u(w, 0);         /* the driver fills in the real offset */
        w = wr_u(w, (uint32_t) x);
        w = wr_u(w, (uint32_t) (y + done));
        w = wr_u(w, (uint32_t) width);
        w = wr_u(w, (uint32_t) n);
        w = wr_u(w, format);
        w = wr_u(w, type);
        if (px_flush(px) != 0)
            return 0;

        src = (const uint8_t *) px->readback;
        for (r = 0; r < n; r++, src += row, dst += stride) {
            if (pack->swap_bytes && unit > 1) {
                uint32_t i, k;

                for (i = 0; i + unit <= row; i += unit)
                    for (k = 0; k < unit; k++)
                        dst[i + k] = src[i + unit - 1 - k];
            } else {
                memcpy(dst, src, row);
            }
        }
        done += n;
    }
    return 0;
}
