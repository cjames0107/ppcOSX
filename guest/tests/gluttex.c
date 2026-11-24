/*
 * gluttex: two textured quads, as an ordinary Mac OS X OpenGL program
 * (Apple's GLUT and OpenGL frameworks only).
 *
 * Left quad:  64x64 RGBA bytes, red/white checks.
 * Right quad: 32x32 RGB in 16-bit 5-6-5 pixels, a green-to-blue ramp taken
 *             out of the middle of a wider buffer (row length and skips),
 *             with a yellow corner patched in by glTexSubImage2D.
 * Between them these cover byte and multi-byte pixel types and the pixel
 * unpack settings.
 *
 *   GEARS_FRAMES=n   draw n frames, print the frame rate, exit
 *   GEARS_ANGLE=a    hold the quads at angle a (reproducible picture)
 *
 * Licensed under the GNU GPL, version 2 or later.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <GLUT/glut.h>

static GLuint gTex[2];
static float gAngle;
static int gFixedAngle, gFrameLimit, gFrames;
static struct timeval gStart;

static void make_textures(void)
{
    static GLubyte checks[64][64][4];
    static GLushort wide[36][40];       /* 32x32 image at column 4, row 2 */
    static GLushort patch[8][8];
    int x, y;

    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            int on = ((x / 8) ^ (y / 8)) & 1;

            checks[y][x][0] = 255;
            checks[y][x][1] = on ? 255 : 40;
            checks[y][x][2] = on ? 255 : 40;
            checks[y][x][3] = 255;
        }
    }
    for (y = 0; y < 36; y++)
        for (x = 0; x < 40; x++)
            wide[y][x] = 0xf800;        /* red: must never be visible */
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++)        /* green ramps across, blue up */
            wide[y + 2][x + 4] = (GLushort) (((x * 2) << 5) | y);
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++)
            patch[y][x] = 0xffe0;       /* yellow */

    glGenTextures(2, gTex);

    glBindTexture(GL_TEXTURE_2D, gTex[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, checks);

    glBindTexture(GL_TEXTURE_2D, gTex[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 40);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 4);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 32, 32, 0, GL_RGB,
                 GL_UNSIGNED_SHORT_5_6_5, wide);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_RGB,
                    GL_UNSIGNED_SHORT_5_6_5, patch);
}

static void quad(GLuint tex, float cx)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glPushMatrix();
    glTranslatef(cx, 0, 0);
    glRotatef(gAngle, 0, 0, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(-0.8f, -0.8f);
    glTexCoord2f(1, 0); glVertex2f( 0.8f, -0.8f);
    glTexCoord2f(1, 1); glVertex2f( 0.8f,  0.8f);
    glTexCoord2f(0, 1); glVertex2f(-0.8f,  0.8f);
    glEnd();
    glPopMatrix();
}

static void display(void)
{
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3ub(255, 255, 255);
    quad(gTex[0], -1.0f);
    quad(gTex[1], 1.0f);
    glutSwapBuffers();

    if (++gFrames == gFrameLimit) {
        struct timeval now;
        double secs;

        gettimeofday(&now, 0);
        secs = (now.tv_sec - gStart.tv_sec) +
               (now.tv_usec - gStart.tv_usec) / 1e6;
        printf("%d frames in %.2f s = %.1f fps\n", gFrames, secs,
               gFrames / secs);
        exit(0);
    }
}

static void idle(void)
{
    if (!gFixedAngle)
        gAngle += 1.0f;
    glutPostRedisplay();
}

static void reshape(int width, int height)
{
    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-2, 2, -1.5, 1.5, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

int main(int argc, char **argv)
{
    glutInit(&argc, argv);
    if (getenv("GEARS_FRAMES"))
        gFrameLimit = atoi(getenv("GEARS_FRAMES"));
    if (getenv("GEARS_ANGLE")) {
        gAngle = (float) atof(getenv("GEARS_ANGLE"));
        gFixedAngle = 1;
    }
    glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE);
    glutInitWindowPosition(120, 80);
    glutInitWindowSize(480, 360);
    glutCreateWindow("Textures");
    glClearColor(0.15f, 0.15f, 0.2f, 1);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    make_textures();
    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutIdleFunc(idle);
    gettimeofday(&gStart, 0);
    glutMainLoop();
    return 0;
}
