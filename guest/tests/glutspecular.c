/*
 * glutspecular: one lit, textured square with a strong specular highlight,
 * as an ordinary Mac OS X OpenGL program (Apple's GLUT and OpenGL only).
 *
 * The texture is a flat 50% grey, modulated by the lighting. With
 * GL_SEPARATE_SPECULAR_COLOR the highlight is added after texturing, so the
 * centre comes out brighter than with GL_SINGLE_COLOR, where the highlight
 * is halved by the texture too. The local-viewer setting moves the
 * highlight, which shows off-centre. Comparing runs on each renderer shows
 * whether that renderer honours each setting.
 *
 *   SPEC_MODE=separate   GL_SEPARATE_SPECULAR_COLOR (default: single)
 *   SPEC_VIEWER=infinite GL_LIGHT_MODEL_LOCAL_VIEWER off (default: on)
 *   GEARS_FRAMES=n       draw n frames, print the centre pixel, exit
 *
 * Licensed under the GNU GPL, version 2 or later.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLUT/glut.h>

static int gFrameLimit, gFrames;

static void display(void)
{
    /* Kept below saturation so every lighting term shows in the result. */
    static const GLfloat diffuse[4] = { 0.3f, 0.3f, 0.3f, 1 };
    static const GLfloat specular[4] = { 0.6f, 0.6f, 0.6f, 1 };
    static const GLfloat light_pos[4] = { 0, 0, 3, 1 };

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
    glTranslatef(0, 0, -2);
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, diffuse);
    glMaterialfv(GL_FRONT, GL_SPECULAR, specular);
    glMaterialf(GL_FRONT, GL_SHININESS, 8);

    /* Finely tessellated so per-vertex lighting can show a highlight. */
    {
        int i, j, n = 32;

        glNormal3f(0, 0, 1);
        for (j = 0; j < n; j++) {
            glBegin(GL_QUAD_STRIP);
            for (i = 0; i <= n; i++) {
                float x = -1 + 2.0f * i / n;

                glTexCoord2f((float) i / n, (float) j / n);
                glVertex3f(x, -1 + 2.0f * j / n, 0);
                glTexCoord2f((float) i / n, (float) (j + 1) / n);
                glVertex3f(x, -1 + 2.0f * (j + 1) / n, 0);
            }
            glEnd();
        }
    }
    glutSwapBuffers();

    if (++gFrames == gFrameLimit) {
        GLubyte rgba[4];
        GLint vp[4];

        /* Read back what this renderer drew (Apple's, or the host's frame
         * copied into the window by the redirector). */
        glGetIntegerv(GL_VIEWPORT, vp);
        glReadBuffer(GL_FRONT);
        glReadPixels(vp[2] / 2, vp[3] / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                     rgba);
        printf("centre %d %d %d", rgba[0], rgba[1], rgba[2]);
        glReadPixels(vp[2] * 3 / 4, vp[3] / 2, 1, 1, GL_RGBA,
                     GL_UNSIGNED_BYTE, rgba);
        printf("  right %d %d %d\n", rgba[0], rgba[1], rgba[2]);
        exit(0);
    }
}

int main(int argc, char **argv)
{
    static GLubyte grey[8][8][3];
    const char *mode = getenv("SPEC_MODE");

    glutInit(&argc, argv);
    if (getenv("GEARS_FRAMES"))
        gFrameLimit = atoi(getenv("GEARS_FRAMES"));
    glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE | GLUT_DEPTH);
    glutInitWindowPosition(120, 80);
    glutInitWindowSize(300, 300);
    glutCreateWindow("Specular");

    memset(grey, 128, sizeof(grey));
    glBindTexture(GL_TEXTURE_2D, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 8, 8, 0, GL_RGB, GL_UNSIGNED_BYTE,
                 grey);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_TEXTURE_2D);

    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_DEPTH_TEST);
    glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER,
                  getenv("SPEC_VIEWER") && !strcmp(getenv("SPEC_VIEWER"),
                                                   "infinite")
                  ? GL_FALSE : GL_TRUE);
    glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL,
                  mode && !strcmp(mode, "separate")
                  ? GL_SEPARATE_SPECULAR_COLOR : GL_SINGLE_COLOR);

    glMatrixMode(GL_PROJECTION);
    glFrustum(-0.5, 0.5, -0.5, 0.5, 1, 10);
    glMatrixMode(GL_MODELVIEW);

    glClearColor(0, 0, 0, 1);
    glutDisplayFunc(display);
    glutIdleFunc(glutPostRedisplay);
    glutMainLoop();
    return 0;
}
