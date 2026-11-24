/*
 * ppcosx-gpu host GL renderer interface.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef HW_DISPLAY_PPCOSX_GPU_GL_H
#define HW_DISPLAY_PPCOSX_GPU_GL_H

typedef struct PPCOSXGL PPCOSXGL;

typedef struct PPCOSXGLRect {
    uint32_t x, y, width, height;
} PPCOSXGLRect;

/* Returns NULL when the host has no usable renderer. */
PPCOSXGL *ppcosx_gl_init(void);
void ppcosx_gl_reset(PPCOSXGL *gl);

/* All of these return a PPCOSX_GPU_ST_* code. */
uint32_t ppcosx_gl_ctx_create(PPCOSXGL *gl, uint32_t id, uint32_t width,
                              uint32_t height, uint32_t format);
uint32_t ppcosx_gl_ctx_resize(PPCOSXGL *gl, uint32_t id, uint32_t width,
                              uint32_t height, uint32_t format);
uint32_t ppcosx_gl_ctx_destroy(PPCOSXGL *gl, uint32_t id);
uint32_t ppcosx_gl_ctx_bind(PPCOSXGL *gl, uint32_t id);
void ppcosx_gl_unbind(PPCOSXGL *gl);
uint32_t ppcosx_gl_call(PPCOSXGL *gl, uint32_t op, const uint8_t *args,
                        uint32_t words);
/* Copies the bound context's image into dst; *damage is what it touched. */
uint32_t ppcosx_gl_present(PPCOSXGL *gl, int32_t x, int32_t y, uint8_t *dst,
                           uint32_t dst_width, uint32_t dst_height,
                           uint32_t dst_stride, PPCOSXGLRect *damage);

/* Writes the bound context's image, bottom row first, to dst if it fits. */
uint32_t ppcosx_gl_readback(PPCOSXGL *gl, uint8_t *dst, uint64_t dst_size);

/* glReadPixels of the bound context into dst, tightly packed, big-endian. */
uint32_t ppcosx_gl_read_pixels(PPCOSXGL *gl, int32_t x, int32_t y,
                               uint32_t width, uint32_t height,
                               uint32_t format, uint32_t type,
                               uint8_t *dst, uint64_t dst_size);

/* Id of the bound context, 0 if none. */
uint32_t ppcosx_gl_bound_id(PPCOSXGL *gl);
/* Size of the bound context's drawable; false if none is bound. */
bool ppcosx_gl_frame_size(PPCOSXGL *gl, uint32_t *width, uint32_t *height);
/* The bound context's image, top row first, big-endian xRGB, into dst. */
void ppcosx_gl_read_frame(PPCOSXGL *gl, uint8_t *dst);

#endif /* HW_DISPLAY_PPCOSX_GPU_GL_H */
