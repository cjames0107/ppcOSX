/*
 * gldemo: a lit, depth-tested spinning cube over a gradient, rendered by the
 * host GPU through libppcosxgl.
 *
 *   gldemo [frames [x y [width height [angle]]]]
 *
 * Draws `frames` frames (default 300) into a width x height drawable
 * (default 400x300) shown at screen position x,y (default 60,60), then
 * prints the frame rate. With `angle`, every frame uses that fixed rotation,
 * which makes the output reproducible for tests.
 *
 * Licensed under the GNU GPL, version 2 or later.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <OpenGL/gl.h>

#include "pxgl.h"

static void face(float nx, float ny, float nz, float r, float g, float b,
                 const float v[4][3])
{
    int i;

    glColor3f(r, g, b);
    glNormal3f(nx, ny, nz);
    for (i = 0; i < 4; i++)
        glVertex3fv(v[i]);
}

static void cube(void)
{
    static const float px[4][3] = {{1,-1,-1},{1,1,-1},{1,1,1},{1,-1,1}};
    static const float nx[4][3] = {{-1,-1,1},{-1,1,1},{-1,1,-1},{-1,-1,-1}};
    static const float py[4][3] = {{-1,1,-1},{-1,1,1},{1,1,1},{1,1,-1}};
    static const float ny[4][3] = {{-1,-1,1},{-1,-1,-1},{1,-1,-1},{1,-1,1}};
    static const float pz[4][3] = {{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
    static const float nz[4][3] = {{1,-1,-1},{-1,-1,-1},{-1,1,-1},{1,1,-1}};

    glBegin(GL_QUADS);
    face( 1, 0, 0, 0.9f, 0.2f, 0.2f, px);
    face(-1, 0, 0, 0.2f, 0.9f, 0.9f, nx);
    face( 0, 1, 0, 0.2f, 0.9f, 0.2f, py);
    face( 0,-1, 0, 0.9f, 0.2f, 0.9f, ny);
    face( 0, 0, 1, 0.2f, 0.3f, 0.9f, pz);
    face( 0, 0,-1, 0.9f, 0.9f, 0.2f, nz);
    glEnd();
}

static void frame(float angle, float aspect)
{
    static const float light_pos[4] = { 2.0f, 3.0f, 4.0f, 0.0f };
    static const float ambient[4] = { 0.25f, 0.25f, 0.25f, 1.0f };

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Background: unlit gradient drawn in normalised device coordinates. */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glBegin(GL_QUADS);
    glColor3f(0.05f, 0.05f, 0.20f); glVertex2f(-1, -1); glVertex2f(1, -1);
    glColor3f(0.35f, 0.55f, 0.85f); glVertex2f(1, 1);   glVertex2f(-1, 1);
    glEnd();

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-0.5 * aspect, 0.5 * aspect, -0.5, 0.5, 1.0, 20.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
    glTranslatef(0.0f, 0.0f, -5.0f);
    glRotatef(25.0f, 1.0f, 0.0f, 0.0f);
    glRotatef(angle, 0.0f, 1.0f, 0.0f);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
    glEnable(GL_CULL_FACE);
    glShadeModel(GL_SMOOTH);
    cube();
}

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 300;
    int x = argc > 3 ? atoi(argv[2]) : 60;
    int y = argc > 3 ? atoi(argv[3]) : 60;
    int width = argc > 5 ? atoi(argv[4]) : 400;
    int height = argc > 5 ? atoi(argv[5]) : 300;
    int fixed = argc > 6;
    float angle = fixed ? (float) atof(argv[6]) : 0.0f;
    struct timeval t0, t1;
    double secs;
    int i;

    if (pxglOpen(width, height) != 0) {
        fprintf(stderr, "gldemo: cannot open a GL context "
                        "(is PPCOSXFramebuffer loaded and host GL on?)\n");
        return 1;
    }

    gettimeofday(&t0, 0);
    for (i = 0; i < frames; i++) {
        frame(angle, (float) width / (float) height);
        if (pxglPresent(x, y) != 0) {
            fprintf(stderr, "gldemo: present failed at frame %d\n", i);
            return 1;
        }
        if (!fixed)
            angle += 2.0f;
    }
    gettimeofday(&t1, 0);
    pxglClose();

    secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
    printf("%d frames of %dx%d in %.2f s = %.1f fps\n", frames, width, height,
           secs, secs > 0 ? frames / secs : 0.0);
    return 0;
}
