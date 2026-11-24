/*
 * glutquadrics: textured, lit GLU quadrics in every orientation, as an
 * ordinary Mac OS X OpenGL program (Apple's GLUT, GLU and OpenGL only).
 *
 * The redirector redraws GLU quadrics itself (Apple's GLU bypasses the
 * public gl*() calls), so this compares that against Apple's GLU. The
 * texture is deliberately lopsided so any mirroring or rotation of texture
 * coordinates shows.
 *
 * Top row, GLU_OUTSIDE: cone (gluCylinder), gluDisk with a hole,
 *   gluPartialDisk, gluSphere.
 * Bottom row: the same with GLU_INSIDE.
 *
 *   GEARS_FRAMES=n   draw n frames, then exit
 *   QUAD_NORMALS=flat use GLU_FLAT normals (default GLU_SMOOTH)
 *   QUAD_TEXTURE=off  no texture coordinates (spheres then use fans at
 *                     the poles)
 *   QUAD_STATIC=1     draw only when GLUT asks for it, not continuously
 *                     (a static picture, as most applications show)
 *
 * Licensed under the GNU GPL, version 2 or later.
 */

#include <stdlib.h>
#include <string.h>
#include <GLUT/glut.h>

static GLUquadric *gQuad;
static int gFrameLimit, gFrames;

static void make_texture(void)
{
    static GLubyte tex[32][32][3];
    int x, y;

    /* A gradient in s, stripes in t, and a red block at the origin. */
    for (y = 0; y < 32; y++) {
        for (x = 0; x < 32; x++) {
            tex[y][x][0] = (GLubyte) (x * 8);
            tex[y][x][1] = (y / 4) % 2 ? 220 : 60;
            tex[y][x][2] = 255 - (GLubyte) (x * 8);
            if (x < 8 && y < 8) {
                tex[y][x][0] = 255;
                tex[y][x][1] = tex[y][x][2] = 0;
            }
        }
    }
    glBindTexture(GL_TEXTURE_2D, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 32, 32, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, tex);
}

static void place(int column, int row)
{
    glLoadIdentity();
    glTranslatef(-3.0f + 2.0f * column, row ? -1.1f : 1.1f, -9);
    glRotatef(-60, 1, 0, 0);
    glRotatef(25, 0, 0, 1);
}

static void display(void)
{
    static const GLfloat light_pos[4] = { 1, 2, 3, 0 };
    int row;

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glLightfv(GL_LIGHT0, GL_POSITION, light_pos);

    for (row = 0; row < 2; row++) {
        gluQuadricOrientation(gQuad, row ? GLU_INSIDE : GLU_OUTSIDE);
        place(0, row);
        gluCylinder(gQuad, 0.8, 0.3, 1.2, 12, 3);
        place(1, row);
        gluDisk(gQuad, 0.3, 0.8, 12, 2);
        place(2, row);
        gluPartialDisk(gQuad, 0.2, 0.8, 10, 2, 30, 250);
        place(3, row);
        gluSphere(gQuad, 0.7, 12, 8);
    }
    glutSwapBuffers();
    if (++gFrames == gFrameLimit)
        exit(0);
}

int main(int argc, char **argv)
{
    glutInit(&argc, argv);
    if (getenv("GEARS_FRAMES"))
        gFrameLimit = atoi(getenv("GEARS_FRAMES"));
    glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE | GLUT_DEPTH);
    glutInitWindowPosition(120, 80);
    glutInitWindowSize(640, 400);
    glutCreateWindow("Quadrics");

    make_texture();
    glEnable(GL_TEXTURE_2D);    /* may be turned off again below */
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);     /* shows which way each face winds */

    gQuad = gluNewQuadric();
    gluQuadricNormals(gQuad, getenv("QUAD_NORMALS") &&
                      !strcmp(getenv("QUAD_NORMALS"), "flat")
                      ? GLU_FLAT : GLU_SMOOTH);
    if (getenv("QUAD_TEXTURE") && !strcmp(getenv("QUAD_TEXTURE"), "off")) {
        glDisable(GL_TEXTURE_2D);
    } else {
        gluQuadricTexture(gQuad, GL_TRUE);
    }

    glMatrixMode(GL_PROJECTION);
    gluPerspective(40, 640.0 / 400.0, 1, 30);
    glMatrixMode(GL_MODELVIEW);
    glClearColor(0.2f, 0.2f, 0.25f, 1);
    glutDisplayFunc(display);
    if (!getenv("QUAD_STATIC"))
        glutIdleFunc(glutPostRedisplay);
    glutMainLoop();
    return 0;
}
