/*
 * ppcosx-gpu host GL renderer: stub for hosts without one. The device then
 * does not advertise PPCOSX_GPU_F_GL and never calls the other entry points.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "ppcosx-gpu-gl.h"

PPCOSXGL *ppcosx_gl_init(void)
{
    return NULL;
}

void ppcosx_gl_reset(PPCOSXGL *gl)
{
}

uint32_t ppcosx_gl_ctx_create(PPCOSXGL *gl, uint32_t id, uint32_t width,
                              uint32_t height, uint32_t format)
{
    g_assert_not_reached();
}

uint32_t ppcosx_gl_ctx_resize(PPCOSXGL *gl, uint32_t id, uint32_t width,
                              uint32_t height, uint32_t format)
{
    g_assert_not_reached();
}

uint32_t ppcosx_gl_readback(PPCOSXGL *gl, uint8_t *dst, uint64_t dst_size)
{
    g_assert_not_reached();
}

uint32_t ppcosx_gl_ctx_destroy(PPCOSXGL *gl, uint32_t id)
{
    g_assert_not_reached();
}

uint32_t ppcosx_gl_ctx_bind(PPCOSXGL *gl, uint32_t id)
{
    g_assert_not_reached();
}

uint32_t ppcosx_gl_read_pixels(PPCOSXGL *gl, int32_t x, int32_t y,
                               uint32_t width, uint32_t height,
                               uint32_t format, uint32_t type,
                               uint8_t *dst, uint64_t dst_size)
{
    g_assert_not_reached();
}

uint32_t ppcosx_gl_bound_id(PPCOSXGL *gl)
{
    return 0;
}

bool ppcosx_gl_frame_size(PPCOSXGL *gl, uint32_t *width, uint32_t *height)
{
    return false;
}

void ppcosx_gl_read_frame(PPCOSXGL *gl, uint8_t *dst)
{
    g_assert_not_reached();
}

void ppcosx_gl_unbind(PPCOSXGL *gl)
{
}

uint32_t ppcosx_gl_call(PPCOSXGL *gl, uint32_t op, const uint8_t *args,
                        uint32_t words)
{
    g_assert_not_reached();
}

uint32_t ppcosx_gl_present(PPCOSXGL *gl, int32_t x, int32_t y, uint8_t *dst,
                           uint32_t dst_width, uint32_t dst_height,
                           uint32_t dst_stride, PPCOSXGLRect *damage)
{
    g_assert_not_reached();
}
