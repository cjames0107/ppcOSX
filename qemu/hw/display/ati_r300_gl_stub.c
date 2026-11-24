/*
 * QEMU ATI SVGA emulation
 * Radeon 9600 (R300) 3D engine: no host GPU on this platform, so every
 * draw is done in software (ati_r300.c).
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "ati_r300_gl.h"

bool ati_r300_gl_begin(ATIVGAState *s, const R300GLDesc *d)
{
    return false;
}

void ati_r300_gl_triangle(ATIVGAState *s, const float *v0, const float *v1,
                          const float *v2)
{
}

void ati_r300_gl_end(ATIVGAState *s)
{
}

void ati_r300_gl_flush(ATIVGAState *s)
{
}

void ati_r300_gl_touch(ATIVGAState *s)
{
}

void ati_r300_gl_sync_range(ATIVGAState *s, uint64_t off, uint64_t len)
{
}

void ati_r300_gl_changed(ATIVGAState *s, uint64_t off, uint64_t len)
{
}
