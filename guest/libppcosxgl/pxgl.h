/*
 * libppcosxgl: OpenGL entry points that marshal calls to the host renderer
 * through the ppcosx-gpu device.
 *
 * This is the stage before a system OpenGL renderer plug-in: a program links
 * this library instead of OpenGL.framework, uses <OpenGL/gl.h> for types and
 * constants, and manages its drawable with the four calls below. One
 * context per process.
 *
 * Licensed under the GNU GPL, version 2 or later.
 */

#ifndef PXGL_H
#define PXGL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Create the context and its width x height drawable. 0 on success. */
int pxglOpen(int width, int height);

/* Send any buffered GL commands to the host. 0 on success. */
int pxglFlush(void);

/* Show the drawable with its top-left corner at screen pixel (x, y). */
int pxglPresent(int x, int y);

void pxglClose(void);

#ifdef __cplusplus
}
#endif

#endif /* PXGL_H */
