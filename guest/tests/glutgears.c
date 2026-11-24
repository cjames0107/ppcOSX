/*
 * glutgears: three meshing gears, as an ordinary Mac OS X OpenGL program.
 *
 * It links only Apple's GLUT and OpenGL frameworks and knows nothing about
 * ppcosx, which makes it the test subject for the interposer: run it plain
 * for Apple's software renderer, or with DYLD_INSERT_LIBRARIES set to
 * libPPCOSXGL.dylib for the host GPU.
 *
 *   GEARS_FRAMES=n   draw n frames, print the frame rate, exit
 *   GEARS_ANGLE=a    hold the gears at angle a (reproducible picture)
 *
 * Licensed under the GNU GPL, version 2 or later.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <GLUT/glut.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static GLuint gGear[3];
static float gAngle;
static int gFixedAngle;
static int gFrameLimit;
static int gFrames;
static struct timeval gStart;

/* One gear, centred on the z axis, built from flat-shaded quads. */
static void gear(float inner, float outer, float width, int teeth,
                 float depth)
{
    float r0 = inner, r1 = outer - depth / 2, r2 = outer + depth / 2;
    float da = 2 * M_PI / teeth / 4;
    float z = width / 2;
    int i;

    glShadeModel(GL_FLAT);

    /* Front and back faces. */
    for (i = 0; i < teeth; i++) {
        float a = i * 2 * M_PI / teeth;
        float sign;

        for (sign = 1; sign >= -1; sign -= 2) {
            glNormal3f(0, 0, sign);
            glBegin(GL_QUADS);
            if (sign > 0) {
                glVertex3f(r0 * cos(a), r0 * sin(a), z);
                glVertex3f(r1 * cos(a), r1 * sin(a), z);
                glVertex3f(r1 * cos(a + 4 * da), r1 * sin(a + 4 * da), z);
                glVertex3f(r0 * cos(a + 4 * da), r0 * sin(a + 4 * da), z);
                glVertex3f(r1 * cos(a), r1 * sin(a), z);
                glVertex3f(r2 * cos(a + da), r2 * sin(a + da), z);
                glVertex3f(r2 * cos(a + 2 * da), r2 * sin(a + 2 * da), z);
                glVertex3f(r1 * cos(a + 3 * da), r1 * sin(a + 3 * da), z);
            } else {
                glVertex3f(r0 * cos(a + 4 * da), r0 * sin(a + 4 * da), -z);
                glVertex3f(r1 * cos(a + 4 * da), r1 * sin(a + 4 * da), -z);
                glVertex3f(r1 * cos(a), r1 * sin(a), -z);
                glVertex3f(r0 * cos(a), r0 * sin(a), -z);
                glVertex3f(r1 * cos(a + 3 * da), r1 * sin(a + 3 * da), -z);
                glVertex3f(r2 * cos(a + 2 * da), r2 * sin(a + 2 * da), -z);
                glVertex3f(r2 * cos(a + da), r2 * sin(a + da), -z);
                glVertex3f(r1 * cos(a), r1 * sin(a), -z);
            }
            glEnd();
        }
    }

    /* Outward faces of the teeth, and the rim between them. */
    glBegin(GL_QUADS);
    for (i = 0; i < teeth; i++) {
        float a = i * 2 * M_PI / teeth;
        float pr[5], pa[5];
        int k;

        pr[0] = r1; pa[0] = a;
        pr[1] = r2; pa[1] = a + da;
        pr[2] = r2; pa[2] = a + 2 * da;
        pr[3] = r1; pa[3] = a + 3 * da;
        pr[4] = r1; pa[4] = a + 4 * da;
        for (k = 0; k < 4; k++) {
            float x0 = pr[k] * cos(pa[k]), y0 = pr[k] * sin(pa[k]);
            float x1 = pr[k + 1] * cos(pa[k + 1]);
            float y1 = pr[k + 1] * sin(pa[k + 1]);

            glNormal3f(y1 - y0, -(x1 - x0), 0);
            glVertex3f(x0, y0, z);
            glVertex3f(x0, y0, -z);
            glVertex3f(x1, y1, -z);
            glVertex3f(x1, y1, z);
        }
    }
    glEnd();

    /* The bore. */
    glShadeModel(GL_SMOOTH);
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i <= teeth; i++) {
        float a = i * 2 * M_PI / teeth;

        glNormal3f(-cos(a), -sin(a), 0);
        glVertex3f(r0 * cos(a), r0 * sin(a), -z);
        glVertex3f(r0 * cos(a), r0 * sin(a), z);
    }
    glEnd();
}

static void display(void)
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glPushMatrix();
    glRotatef(20, 1, 0, 0);
    glRotatef(30, 0, 1, 0);

    glPushMatrix();
    glTranslatef(-3.0f, -2.0f, 0);
    glRotatef(gAngle, 0, 0, 1);
    glCallList(gGear[0]);
    glPopMatrix();

    glPushMatrix();
    glTranslatef(3.1f, -2.0f, 0);
    glRotatef(-2 * gAngle - 9, 0, 0, 1);
    glCallList(gGear[1]);
    glPopMatrix();

    glPushMatrix();
    glTranslatef(-3.1f, 4.2f, 0);
    glRotatef(-2 * gAngle - 25, 0, 0, 1);
    glCallList(gGear[2]);
    glPopMatrix();

    glPopMatrix();
    glutSwapBuffers();

    if (++gFrames == gFrameLimit) {
        struct timeval now;
        double secs;

        gettimeofday(&now, 0);
        secs = (now.tv_sec - gStart.tv_sec) +
               (now.tv_usec - gStart.tv_usec) / 1e6;
        printf("%d frames in %.2f s = %.1f fps (%s)\n", gFrames, secs,
               gFrames / secs, (const char *) glGetString(GL_RENDERER));
        exit(0);
    }
}

static void idle(void)
{
    if (!gFixedAngle)
        gAngle += 2.0f;
    glutPostRedisplay();
}

static void reshape(int width, int height)
{
    float h = (float) height / (float) width;

    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1, 1, -h, h, 5, 60);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0, 0, -40);
}

static void init(void)
{
    static const GLfloat pos[4] = { 5, 5, 10, 0 };
    static const GLfloat red[4] = { 0.8f, 0.1f, 0.0f, 1 };
    static const GLfloat green[4] = { 0.0f, 0.8f, 0.2f, 1 };
    static const GLfloat blue[4] = { 0.2f, 0.2f, 1.0f, 1 };
    static const GLfloat *colour[3] = { red, green, blue };
    static const float shape[3][4] = {
        { 1.0f, 4.0f, 1.0f, 20 }, { 0.5f, 2.0f, 2.0f, 10 },
        { 1.3f, 2.0f, 0.5f, 10 },
    };
    int i;

    glClearColor(0.1f, 0.1f, 0.15f, 1);
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    glEnable(GL_CULL_FACE);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_NORMALIZE);

    gGear[0] = glGenLists(3);
    for (i = 0; i < 3; i++) {
        gGear[i] = gGear[0] + i;
        glNewList(gGear[i], GL_COMPILE);
        glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, colour[i]);
        gear(shape[i][0], shape[i][1], shape[i][2], (int) shape[i][3], 0.7f);
        glEndList();
    }
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

    glutInitDisplayMode(GLUT_RGB | GLUT_DEPTH | GLUT_DOUBLE);
    glutInitWindowPosition(120, 80);
    glutInitWindowSize(500, 400);
    glutCreateWindow("Gears");
    init();
    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutIdleFunc(idle);
    gettimeofday(&gStart, 0);
    glutMainLoop();
    return 0;
}
