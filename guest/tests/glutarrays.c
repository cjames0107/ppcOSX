/*
 * glutarrays: vertex arrays, as an ordinary Mac OS X OpenGL program
 * (Apple's GLUT and OpenGL frameworks only).
 *
 * Left:  a triangle from separate packed arrays with glDrawArrays.
 * Right: a square from one interleaved array with glDrawElements, using
 *        indices that start partway into the array.
 *
 *   GEARS_FRAMES=n   draw n frames, then exit
 *   ARRAYS_VBO=1     also bind a buffer object first, which the host path
 *                    does not follow: checks the hand-back to Apple's
 *                    renderer
 *
 * Licensed under the GNU GPL, version 2 or later.
 */

#include <stdio.h>
#include <stdlib.h>
#include <GLUT/glut.h>

static int gFrameLimit, gFrames;

/* Interleaved position + colour, as many programs lay vertices out. */
struct vertex {
    GLfloat x, y;
    GLubyte r, g, b, a;
};

static void display(void)
{
    static const GLfloat verts[] = {
        -0.9f, -0.7f,  -0.1f, -0.7f,  -0.5f, 0.8f
    };
    static const GLubyte cols[] = { 255, 0, 0,  0, 255, 0,  0, 0, 255 };
    /* The square uses only vertices 4-7; 0-3 must never be drawn. */
    static const struct vertex square[8] = {
        { -5, -5, 255, 0, 255, 255 }, { 5, -5, 255, 0, 255, 255 },
        { 5, 5, 255, 0, 255, 255 },   { -5, 5, 255, 0, 255, 255 },
        { 0.1f, -0.7f, 255, 255, 0, 255 }, { 0.9f, -0.7f, 0, 255, 255, 255 },
        { 0.9f, 0.8f, 255, 0, 255, 255 },  { 0.1f, 0.8f, 255, 255, 255, 255 },
    };
    static const GLushort indices[] = { 4, 5, 6,  4, 6, 7 };

    glClearColor(0.9f, 0.9f, 0.9f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);

    /* Separate, tightly packed arrays, drawn in order. */
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glColorPointer(3, GL_UNSIGNED_BYTE, 0, cols);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* One interleaved array with a stride, drawn through indices. */
    glVertexPointer(2, GL_FLOAT, sizeof(struct vertex), &square[0].x);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(struct vertex), &square[0].r);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);

    glutSwapBuffers();
    if (++gFrames == gFrameLimit)
        exit(0);
}

int main(int argc, char **argv)
{
    glutInit(&argc, argv);
    if (getenv("GEARS_FRAMES"))
        gFrameLimit = atoi(getenv("GEARS_FRAMES"));
    glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE);
    glutInitWindowPosition(120, 80);
    glutInitWindowSize(400, 300);
    glutCreateWindow("Arrays");
    if (getenv("ARRAYS_VBO")) {
        GLuint buffer;

        glGenBuffers(1, &buffer);
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    glutDisplayFunc(display);
    glutIdleFunc(glutPostRedisplay);
    glutMainLoop();
    return 0;
}
