/*
 * libppcosxgl: see pxgl.h. The gl*() functions here append to the one
 * process-wide connection.
 *
 * Licensed under the GNU GPL, version 2 or later.
 */

#include <OpenGL/gl.h>

#include "pxgl.h"
#include "pxcore.h"
#include "ppcosx-gpu.h"
#include "ppcosx-gl-ops.h"

static PXConn *gConn;
static GLuint gNextList = 1;
static GLuint gNextTexture = 1;
static PXUnpack gUnpack = { 4, 0, 0, 0, 0 };

#define PX_EMIT(opcode, words) (gConn ? px_emit(gConn, opcode, words) : 0)
#include "gl_encode.inc"

/* Calls that need an answer are satisfied locally. */

GLuint glGenLists(GLsizei range)
{
    GLuint first = gNextList;

    if (range <= 0)
        return 0;
    gNextList += range;
    return first;
}

GLenum glGetError(void)
{
    return GL_NO_ERROR;
}

void glGenTextures(GLsizei n, GLuint *textures)
{
    GLsizei i;

    for (i = 0; i < n; i++)
        textures[i] = gNextTexture++;
}

void glDeleteTextures(GLsizei n, const GLuint *textures)
{
    if (gConn)
        px_delete_textures(gConn, n, (const uint32_t *) textures);
}

void glPixelStorei(GLenum pname, GLint param)
{
    switch (pname) {
    case GL_UNPACK_ALIGNMENT:   gUnpack.alignment = param;   break;
    case GL_UNPACK_ROW_LENGTH:  gUnpack.row_length = param;  break;
    case GL_UNPACK_SKIP_ROWS:   gUnpack.skip_rows = param;   break;
    case GL_UNPACK_SKIP_PIXELS: gUnpack.skip_pixels = param; break;
    case GL_UNPACK_SWAP_BYTES:  gUnpack.swap_bytes = param;  break;
    }
}

void glTexImage2D(GLenum target, GLint level, GLenum internalformat,
                  GLsizei width, GLsizei height, GLint border, GLenum format,
                  GLenum type, const GLvoid *pixels)
{
    if (gConn)
        px_tex_image_2d(gConn, &gUnpack, target, level, internalformat,
                        width, height, border, format, type, pixels);
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLsizei width, GLsizei height, GLenum format,
                     GLenum type, const GLvoid *pixels)
{
    if (gConn)
        px_tex_sub_image_2d(gConn, &gUnpack, target, level, xoffset, yoffset,
                            width, height, format, type, pixels);
}

int pxglOpen(int width, int height)
{
    if (gConn)
        return -1;
    gConn = px_open(width, height, PPCOSX_GPU_GL_FORMAT_FULL);
    return gConn ? 0 : -1;
}

int pxglFlush(void)
{
    return gConn ? px_flush(gConn) : -1;
}

int pxglPresent(int x, int y)
{
    return gConn ? px_present(gConn, x, y) : -1;
}

void pxglClose(void)
{
    px_close(gConn);
    gConn = 0;
}
