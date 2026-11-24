/*
 * QEMU ATI SVGA emulation
 * Radeon 9600 (R300) 3D engine: drawing on the host GPU.
 *
 * ati_r300.c keeps vertex processing (fetch, vertex program, clipping,
 * culling, points and lines as triangles) and hands each draw's triangles
 * here with the state below. The host GPU rasterises them and runs the
 * fragment program, translated to GLSL, with texturing, fog, alpha test,
 * depth/stencil, blending and logic ops, into host copies of the card's
 * colour and depth buffers.
 *
 * Video memory stays the reference: host copies are checked against it
 * (by content hash, once per batch) before use, and what the GPU drew is
 * written back before anything else can look: at the end of each command
 * batch, before 2D engine operations and before draws done in software.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef ATI_R300_GL_H
#define ATI_R300_GL_H

#include "ati_int.h"

#define R300GL_MAX_VARY 16

typedef struct R300GLDesc {
    /* colour buffer (in video memory) */
    uint64_t color_off;
    uint32_t color_pitch;           /* pixels */
    int color_fmt;                  /* RB3D_COLORPITCH COLORFORMAT */
    int color_bpp;
    /* depth and stencil */
    bool z_on, s_on;
    uint64_t depth_off;
    uint32_t depth_pitch;           /* pixels */
    int depth_bpp;
    uint32_t zb_cntl, zs_cntl;
    uint32_t stencil_ref, stencil_mask, stencil_wmask;
    /* colour operations */
    uint32_t cblend, ablend, blend_color, cmask, rop;
    uint32_t alpha_func;            /* FG_ALPHA_FUNC */
    bool fog_on;
    float fog_scale, fog_offset, fog_color[3];
    int clip_x0, clip_y0, clip_x1, clip_y1;     /* inclusive */
    uint32_t cull;                  /* SU_CULL_MODE: front face */
    /* fragment program */
    int nnodes;
    struct {
        int alu_start, alu_end, tex_start, tex_end;
    } nodes[4];
    uint32_t tex_inst[32];
    uint32_t alu[64][4];            /* rgb_addr, rgb_inst, alpha_addr,
                                       alpha_inst */
    float consts[32][4];
    /* rasteriser: varying i sets temporary vary_dst[i] */
    int nvary;
    int vary_dst[R300GL_MAX_VARY];
} R300GLDesc;

/*
 * Vertex layout handed to ati_r300_gl_triangle: window x, y, z, then
 * 1/w (1 when there is no perspective), the varyings, the fog coordinate.
 */
#define R300GL_VERTEX_FLOATS(nvary) (4 + 4 * (nvary) + 1)

/* Start a draw; false if the host GPU cannot do it (draw in software). */
bool ati_r300_gl_begin(ATIVGAState *s, const R300GLDesc *d);
void ati_r300_gl_triangle(ATIVGAState *s, const float *v0, const float *v1,
                          const float *v2);
void ati_r300_gl_end(ATIVGAState *s);

/* Write what the GPU drew back to video memory. */
void ati_r300_gl_flush(ATIVGAState *s);
/* Video memory may have changed behind the GPU's back: recheck copies. */
void ati_r300_gl_touch(ATIVGAState *s);

/*
 * The same for one range of video memory: before others read or write it
 * (write back what the GPU drew there), and after they wrote it.
 */
void ati_r300_gl_sync_range(ATIVGAState *s, uint64_t off, uint64_t len);
void ati_r300_gl_changed(ATIVGAState *s, uint64_t off, uint64_t len);

#endif
