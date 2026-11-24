/*
 * ppcosx-gpu: hand-written GL commands that carry pixel or vertex data,
 * and the size rules guest and host must agree on.
 *
 * Shared with the guest, so it uses only fixed-width types and spells GL
 * constants out numerically.
 *
 * Pixel data always travels tightly packed (rows are width * bytes-per-pixel
 * with no padding) in the guest's memory layout. Component types wider than
 * a byte are therefore big-endian on the wire.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef HW_DISPLAY_PPCOSX_GL_IMAGE_H
#define HW_DISPLAY_PPCOSX_GL_IMAGE_H

/*
 * Append to the bound context's staging buffer. Arguments: byte count, then
 * the bytes, zero-padded to a multiple of four. The next command that takes
 * pixel data consumes and empties the buffer; any other use is an error.
 */
#define PPCOSX_GL_BLOB              0x1800
/* target level internalformat width height border format type has_data */
#define PPCOSX_GL_TexImage2D        0x1801
/* target level xoffset yoffset width height format type */
#define PPCOSX_GL_TexSubImage2D     0x1802
/* n ids */
#define PPCOSX_GL_DeleteTextures    0x1803

/*
 * Draw from vertex arrays. Arguments: mode, vertex count, index count,
 * array count, then (kind, size, type) per array.
 *
 * The staging buffer holds each array in the order listed, tightly packed
 * (vertex count * size elements of `type`, big-endian), followed by
 * `index count` 32-bit big-endian indices, every one below the vertex
 * count. With an index count of zero the vertices are drawn in order.
 * The arrays apply to this one draw; no array state persists on the host.
 */
#define PPCOSX_GL_Draw              0x1804

#define PPCOSX_GL_ARRAY_VERTEX      0
#define PPCOSX_GL_ARRAY_NORMAL      1
#define PPCOSX_GL_ARRAY_COLOR       2
#define PPCOSX_GL_ARRAY_TEXCOORD0   8       /* + texture unit, 0..7 */
#define PPCOSX_GL_MAX_ARRAYS        11
#define PPCOSX_GL_MAX_VERTICES      (1u << 20)
#define PPCOSX_GL_MAX_INDICES       (1u << 22)

#define PPCOSX_GL_MAX_BLOB          (64u << 20)
#define PPCOSX_GL_MAX_TEX_SIZE      4096

/* Bytes per pixel for a format/type pair; 0 if this protocol lacks it. */
static inline uint32_t ppcosx_gl_pixel_bytes(uint32_t format, uint32_t type)
{
    uint32_t components, size;

    switch (type) {
    case 0x8032: /* UNSIGNED_BYTE_3_3_2 */
    case 0x8362: /* UNSIGNED_BYTE_2_3_3_REV */
        return 1;
    case 0x8363: /* UNSIGNED_SHORT_5_6_5 */
    case 0x8364: /* UNSIGNED_SHORT_5_6_5_REV */
    case 0x8033: /* UNSIGNED_SHORT_4_4_4_4 */
    case 0x8365: /* UNSIGNED_SHORT_4_4_4_4_REV */
    case 0x8034: /* UNSIGNED_SHORT_5_5_5_1 */
    case 0x8366: /* UNSIGNED_SHORT_1_5_5_5_REV */
    case 0x85BA: /* UNSIGNED_SHORT_8_8_APPLE */
    case 0x85BB: /* UNSIGNED_SHORT_8_8_REV_APPLE */
        return 2;
    case 0x8035: /* UNSIGNED_INT_8_8_8_8 */
    case 0x8367: /* UNSIGNED_INT_8_8_8_8_REV */
    case 0x8036: /* UNSIGNED_INT_10_10_10_2 */
    case 0x8368: /* UNSIGNED_INT_2_10_10_10_REV */
        return 4;
    case 0x1400: /* BYTE */
    case 0x1401: /* UNSIGNED_BYTE */
        size = 1;
        break;
    case 0x1402: /* SHORT */
    case 0x1403: /* UNSIGNED_SHORT */
        size = 2;
        break;
    case 0x1404: /* INT */
    case 0x1405: /* UNSIGNED_INT */
    case 0x1406: /* FLOAT */
        size = 4;
        break;
    default:
        return 0;
    }

    switch (format) {
    case 0x1900: /* COLOR_INDEX */
    case 0x1902: /* DEPTH_COMPONENT */
    case 0x1903: /* RED */
    case 0x1904: /* GREEN */
    case 0x1905: /* BLUE */
    case 0x1906: /* ALPHA */
    case 0x1909: /* LUMINANCE */
        components = 1;
        break;
    case 0x190A: /* LUMINANCE_ALPHA */
        components = 2;
        break;
    case 0x1907: /* RGB */
    case 0x80E0: /* BGR */
        components = 3;
        break;
    case 0x1908: /* RGBA */
    case 0x80E1: /* BGRA */
    case 0x8000: /* ABGR_EXT */
        components = 4;
        break;
    default:
        return 0;
    }
    return components * size;
}

/* Size in bytes of each component as stored: decides byte swapping. */
static inline uint32_t ppcosx_gl_type_unit(uint32_t type)
{
    switch (type) {
    case 0x1400: case 0x1401: case 0x8032: case 0x8362:
        return 1;
    case 0x1402: case 0x1403: case 0x8363: case 0x8364: case 0x8033:
    case 0x8365: case 0x8034: case 0x8366: case 0x85BA: case 0x85BB:
        return 2;
    default:
        return 4;
    }
}

/* Bytes per element of a vertex-array type; 0 if not a valid one. */
static inline uint32_t ppcosx_gl_array_unit(uint32_t type)
{
    switch (type) {
    case 0x1400: /* BYTE */
    case 0x1401: /* UNSIGNED_BYTE */
        return 1;
    case 0x1402: /* SHORT */
    case 0x1403: /* UNSIGNED_SHORT */
        return 2;
    case 0x1404: /* INT */
    case 0x1405: /* UNSIGNED_INT */
    case 0x1406: /* FLOAT */
        return 4;
    case 0x140A: /* DOUBLE */
        return 8;
    default:
        return 0;
    }
}

#endif /* HW_DISPLAY_PPCOSX_GL_IMAGE_H */
