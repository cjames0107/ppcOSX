/*
 * libPPCOSXGL: GLU entry points. Included by ppcosxgl_interpose.c.
 *
 * Apple's GLU does not call the public gl*() functions; it goes straight to
 * the current context's dispatch table, which the interposer never sees.
 * So for a redirected context the GLU calls that change GL state or draw
 * are redone here in terms of the pxi_gl*() wrappers:
 *
 *   matrices   the same matrix is computed and sent to the host, and
 *              Apple's GLU is still called so the software renderer's
 *              state stays right;
 *   mipmaps    level 0 goes to the host, which generates the rest;
 *   quadrics   filled spheres, cylinders and disks are generated here,
 *              vertex for vertex as GLU would;
 *   the rest   NURBS, tessellation that draws for itself, outlined or
 *              point quadrics and flat-shaded spheres hand the context
 *              back to Apple's renderer.
 *
 * Licensed under the GNU GPL, version 2 or later.
 */

#include <math.h>
#include <OpenGL/glu.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- matrices ----------------------------------------------------------- */

static void pxi_host_mult_matrixd(PXConn *px, const double m[16])
{
    uint32_t *w = px_emit(px, PPCOSX_GL_MultMatrixd, 32);
    int i;

    if (w)
        for (i = 0; i < 16; i++)
            w = wr_d(w, m[i]);
}

static void pxi_gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear,
                               GLdouble zFar)
{
    PXConn *px = pxi_current();
    double radians = fovy / 2 * M_PI / 180;
    double deltaZ = zFar - zNear;
    double sine = sin(radians);

    /* GLU itself does nothing for these degenerate arguments. */
    if (px && deltaZ != 0 && sine != 0 && aspect != 0) {
        double m[16] = { 0 };
        double cotangent = cos(radians) / sine;

        m[0] = cotangent / aspect;
        m[5] = cotangent;
        m[10] = -(zFar + zNear) / deltaZ;
        m[11] = -1;
        m[14] = -2 * zNear * zFar / deltaZ;
        pxi_host_mult_matrixd(px, m);
    }
    gluPerspective(fovy, aspect, zNear, zFar);
}

static void pxi_normalize(double v[3])
{
    double r = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

    if (r != 0) {
        v[0] /= r;
        v[1] /= r;
        v[2] /= r;
    }
}

static void pxi_cross(const double a[3], const double b[3], double out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static void pxi_gluLookAt(GLdouble eyeX, GLdouble eyeY, GLdouble eyeZ,
                          GLdouble centerX, GLdouble centerY,
                          GLdouble centerZ, GLdouble upX, GLdouble upY,
                          GLdouble upZ)
{
    PXConn *px = pxi_current();

    if (px) {
        double forward[3] = { centerX - eyeX, centerY - eyeY, centerZ - eyeZ };
        double up[3] = { upX, upY, upZ };
        double side[3], m[16] = { 0 };
        uint32_t *w;

        pxi_normalize(forward);
        pxi_cross(forward, up, side);
        pxi_normalize(side);
        pxi_cross(side, forward, up);

        m[0] = side[0];  m[4] = side[1];  m[8] = side[2];
        m[1] = up[0];    m[5] = up[1];    m[9] = up[2];
        m[2] = -forward[0];
        m[6] = -forward[1];
        m[10] = -forward[2];
        m[15] = 1;
        pxi_host_mult_matrixd(px, m);

        w = px_emit(px, PPCOSX_GL_Translated, 6);
        if (w) {
            w = wr_d(w, -eyeX);
            w = wr_d(w, -eyeY);
            w = wr_d(w, -eyeZ);
        }
    }
    gluLookAt(eyeX, eyeY, eyeZ, centerX, centerY, centerZ, upX, upY, upZ);
}

static void pxi_gluOrtho2D(GLdouble left, GLdouble right, GLdouble bottom,
                           GLdouble top)
{
    PXConn *px = pxi_current();

    if (px) {
        uint32_t *w = px_emit(px, PPCOSX_GL_Ortho, 12);

        if (w) {
            w = wr_d(w, left);
            w = wr_d(w, right);
            w = wr_d(w, bottom);
            w = wr_d(w, top);
            w = wr_d(w, -1);
            w = wr_d(w, 1);
        }
    }
    gluOrtho2D(left, right, bottom, top);
}

/* ---- mipmaps ------------------------------------------------------------ */

static GLint pxi_gluBuild2DMipmaps(GLenum target, GLint internalFormat,
                                   GLsizei width, GLsizei height,
                                   GLenum format, GLenum type,
                                   const void *data)
{
    PXConn *px = pxi_current();

    if (px && data) {
        PXUnpack unpack;
        uint32_t *w;

        /* The host builds the smaller levels from level 0 itself, and
         * takes any size, so GLU's rescaling is not needed there. */
        pxi_get_unpack(&unpack);
        w = px_emit(px, PPCOSX_GL_TexParameteri, 3);
        if (w) {
            w = wr_u(w, target);
            w = wr_u(w, GL_GENERATE_MIPMAP);
            w = wr_u(w, GL_TRUE);
        }
        if (px_tex_image_2d(px, &unpack, target, 0, internalFormat, width,
                            height, 0, format, type, data) != 0) {
            pxi_unsupported("a gluBuild2DMipmaps format the host path lacks");
        } else {
            GLint generate = GL_FALSE;

            /* Leave the setting as the application has it. */
            glGetTexParameteriv(target, GL_GENERATE_MIPMAP, &generate);
            w = px_emit(px, PPCOSX_GL_TexParameteri, 3);
            if (w) {
                w = wr_u(w, target);
                w = wr_u(w, GL_GENERATE_MIPMAP);
                w = wr_u(w, generate);
            }
        }
    }
    return gluBuild2DMipmaps(target, internalFormat, width, height, format,
                             type, data);
}

static GLint pxi_gluBuild1DMipmaps(GLenum target, GLint internalFormat,
                                   GLsizei width, GLenum format, GLenum type,
                                   const void *data)
{
    pxi_unsupported("gluBuild1DMipmaps");
    return gluBuild1DMipmaps(target, internalFormat, width, format, type,
                             data);
}

/* ---- quadrics ----------------------------------------------------------- */

#define kMaxQuadrics    32

typedef struct PXIQuadric {
    GLUquadric *quad;           /* NULL: free */
    GLenum normals;
    GLenum orientation;
    GLenum style;
    GLboolean texture;
} PXIQuadric;

static PXIQuadric gQuadrics[kMaxQuadrics];

/* Our record of a quadric's settings, created with GLU's defaults. */
static PXIQuadric *pxi_quadric(GLUquadric *quad)
{
    PXIQuadric *q = 0, *spare = 0;
    int i;

    if (!quad)
        return 0;
    pthread_mutex_lock(&gLock);
    for (i = 0; i < kMaxQuadrics; i++) {
        if (gQuadrics[i].quad == quad)
            q = &gQuadrics[i];
        else if (!gQuadrics[i].quad && !spare)
            spare = &gQuadrics[i];
    }
    if (!q && spare) {
        q = spare;
        q->normals = GLU_SMOOTH;
        q->orientation = GLU_OUTSIDE;
        q->style = GLU_FILL;
        q->texture = GL_FALSE;
        q->quad = quad;
    }
    pthread_mutex_unlock(&gLock);
    return q;
}

static void pxi_gluQuadricNormals(GLUquadric *quad, GLenum normal)
{
    PXIQuadric *q = pxi_quadric(quad);

    if (q)
        q->normals = normal;
    gluQuadricNormals(quad, normal);
}

static void pxi_gluQuadricOrientation(GLUquadric *quad, GLenum orientation)
{
    PXIQuadric *q = pxi_quadric(quad);

    if (q)
        q->orientation = orientation;
    gluQuadricOrientation(quad, orientation);
}

static void pxi_gluQuadricDrawStyle(GLUquadric *quad, GLenum draw)
{
    PXIQuadric *q = pxi_quadric(quad);

    if (q)
        q->style = draw;
    gluQuadricDrawStyle(quad, draw);
}

static void pxi_gluQuadricTexture(GLUquadric *quad, GLboolean texture)
{
    PXIQuadric *q = pxi_quadric(quad);

    if (q)
        q->texture = texture;
    gluQuadricTexture(quad, texture);
}

static void pxi_gluDeleteQuadric(GLUquadric *quad)
{
    int i;

    pthread_mutex_lock(&gLock);
    for (i = 0; i < kMaxQuadrics; i++)
        if (gQuadrics[i].quad == quad)
            gQuadrics[i].quad = 0;
    pthread_mutex_unlock(&gLock);
    gluDeleteQuadric(quad);
}

/*
 * The settings to draw `quad` with on the host, or NULL if Apple's GLU
 * should draw it (context not redirected, or a style not done here).
 */
static PXIQuadric *pxi_quadric_for_draw(GLUquadric *quad)
{
    PXIQuadric *q;

    if (!pxi_current())
        return 0;
    q = pxi_quadric(quad);
    if (!q || q->style != GLU_FILL) {
        pxi_unsupported("an outlined or point GLU quadric");
        return 0;
    }
    return q;
}

/*
 * The drawing below follows the reference GLU implementation (SGI's, which
 * Apple's derives from) step for step: same vertex order, texture
 * coordinates and normals, so the host draws the same triangles Apple's
 * GLU would have sent.
 */

#define kMaxSlices      1024
#define kMaxStacks      1024

static void pxi_gluCylinder(GLUquadric *quad, GLdouble base, GLdouble top,
                            GLdouble height, GLint slices, GLint stacks)
{
    static float sin1[kMaxSlices + 1], cos1[kMaxSlices + 1];
    static float sin2[kMaxSlices + 1], cos2[kMaxSlices + 1];
    static float sin3[kMaxSlices + 1], cos3[kMaxSlices + 1];
    PXIQuadric *q = pxi_quadric_for_draw(quad);
    float deltaRadius, length, zNormal, xyNormalRatio;
    int inside, i, j;

    if (!q || slices < 2 || stacks < 1 || base < 0 || top < 0 ||
        height < 0 || slices > kMaxSlices || stacks > kMaxStacks) {
        gluCylinder(quad, base, top, height, slices, stacks);
        return;
    }
    deltaRadius = base - top;
    length = sqrt(deltaRadius * deltaRadius + height * height);
    if (length == 0) {
        gluCylinder(quad, base, top, height, slices, stacks);
        return;
    }
    inside = q->orientation == GLU_INSIDE;
    zNormal = deltaRadius / length;
    xyNormalRatio = height / length;

    for (i = 0; i < slices; i++) {
        float angle = 2 * M_PI * i / slices;
        float face = 2 * M_PI * (i - 0.5f) / slices;
        float sign = inside ? -1 : 1;

        sin1[i] = sin(angle);
        cos1[i] = cos(angle);
        sin2[i] = sign * xyNormalRatio * sin(angle);
        cos2[i] = sign * xyNormalRatio * cos(angle);
        sin3[i] = sign * xyNormalRatio * sin(face);
        cos3[i] = sign * xyNormalRatio * cos(face);
    }
    sin1[slices] = sin1[0];
    cos1[slices] = cos1[0];
    sin2[slices] = sin2[0];
    cos2[slices] = cos2[0];
    sin3[slices] = sin3[0];
    cos3[slices] = cos3[0];

    for (j = 0; j < stacks; j++) {
        float zLow = j * height / stacks;
        float zHigh = (j + 1) * height / stacks;
        float radiusLow = base - deltaRadius * ((float) j / stacks);
        float radiusHigh = base - deltaRadius * ((float) (j + 1) / stacks);

        pxi_glBegin(GL_QUAD_STRIP);
        for (i = 0; i <= slices; i++) {
            float s = 1 - (float) i / slices;

            if (q->normals == GLU_FLAT)
                pxi_glNormal3f(sin3[i], cos3[i], zNormal);
            else if (q->normals == GLU_SMOOTH)
                pxi_glNormal3f(sin2[i], cos2[i], zNormal);
            if (!inside) {
                if (q->texture)
                    pxi_glTexCoord2f(s, (float) j / stacks);
                pxi_glVertex3f(radiusLow * sin1[i], radiusLow * cos1[i],
                               zLow);
                if (q->texture)
                    pxi_glTexCoord2f(s, (float) (j + 1) / stacks);
                pxi_glVertex3f(radiusHigh * sin1[i], radiusHigh * cos1[i],
                               zHigh);
            } else {
                if (q->texture)
                    pxi_glTexCoord2f(s, (float) (j + 1) / stacks);
                pxi_glVertex3f(radiusHigh * sin1[i], radiusHigh * cos1[i],
                               zHigh);
                if (q->texture)
                    pxi_glTexCoord2f(s, (float) j / stacks);
                pxi_glVertex3f(radiusLow * sin1[i], radiusLow * cos1[i],
                               zLow);
            }
        }
        pxi_glEnd();
    }
}

/* gluDisk is gluPartialDisk from 0 through 360 degrees. */
static void pxi_partial_disk(const PXIQuadric *q, double inner, double outer,
                             int slices, int loops, double startAngle,
                             double sweepAngle)
{
    static float sinc[kMaxSlices + 1], cosc[kMaxSlices + 1];
    float deltaRadius = outer - inner;
    float angleOffset = startAngle / 180.0 * M_PI;
    int inside = q->orientation == GLU_INSIDE;
    int i, j, finish;

    for (i = 0; i <= slices; i++) {
        float angle = angleOffset + ((M_PI * sweepAngle) / 180.0) * i / slices;

        sinc[i] = sin(angle);
        cosc[i] = cos(angle);
    }
    if (sweepAngle == 360.0) {
        sinc[slices] = sinc[0];
        cosc[slices] = cosc[0];
    }

    if (q->normals != GLU_NONE)
        pxi_glNormal3f(0, 0, inside ? -1.0f : 1.0f);

    if (inner == 0) {
        float radiusLow = outer - deltaRadius * ((float) (loops - 1) / loops);
        float texLow = radiusLow / outer / 2;

        finish = loops - 1;
        pxi_glBegin(GL_TRIANGLE_FAN);
        if (q->texture)
            pxi_glTexCoord2f(0.5f, 0.5f);
        pxi_glVertex3f(0, 0, 0);
        for (i = 0; i <= slices; i++) {
            int k = inside ? i : slices - i;

            if (q->texture)
                pxi_glTexCoord2f(texLow * sinc[k] + 0.5f,
                                 texLow * cosc[k] + 0.5f);
            pxi_glVertex3f(radiusLow * sinc[k], radiusLow * cosc[k], 0);
        }
        pxi_glEnd();
    } else {
        finish = loops;
    }

    for (j = 0; j < finish; j++) {
        float radiusLow = outer - deltaRadius * ((float) j / loops);
        float radiusHigh = outer - deltaRadius * ((float) (j + 1) / loops);
        float texLow = radiusLow / outer / 2;
        float texHigh = radiusHigh / outer / 2;

        pxi_glBegin(GL_QUAD_STRIP);
        for (i = 0; i <= slices; i++) {
            float r1 = inside ? radiusHigh : radiusLow;
            float r2 = inside ? radiusLow : radiusHigh;
            float t1 = inside ? texHigh : texLow;
            float t2 = inside ? texLow : texHigh;

            if (q->texture)
                pxi_glTexCoord2f(t1 * sinc[i] + 0.5f, t1 * cosc[i] + 0.5f);
            pxi_glVertex3f(r1 * sinc[i], r1 * cosc[i], 0);
            if (q->texture)
                pxi_glTexCoord2f(t2 * sinc[i] + 0.5f, t2 * cosc[i] + 0.5f);
            pxi_glVertex3f(r2 * sinc[i], r2 * cosc[i], 0);
        }
        pxi_glEnd();
    }
}

static void pxi_gluPartialDisk(GLUquadric *quad, GLdouble inner,
                               GLdouble outer, GLint slices, GLint loops,
                               GLdouble start, GLdouble sweep)
{
    PXIQuadric *q = pxi_quadric_for_draw(quad);

    if (!q || slices < 2 || loops < 1 || outer <= 0 || inner < 0 ||
        inner > outer || slices > kMaxSlices) {
        gluPartialDisk(quad, inner, outer, slices, loops, start, sweep);
        return;
    }
    if (sweep < -360)
        sweep = 360;
    if (sweep > 360)
        sweep = 360;
    if (sweep < 0) {
        start += sweep;
        sweep = -sweep;
    }
    pxi_partial_disk(q, inner, outer, slices, loops, start, sweep);
}

static void pxi_gluDisk(GLUquadric *quad, GLdouble inner, GLdouble outer,
                        GLint slices, GLint loops)
{
    PXIQuadric *q = pxi_quadric_for_draw(quad);

    if (!q || slices < 2 || loops < 1 || outer <= 0 || inner < 0 ||
        inner > outer || slices > kMaxSlices) {
        gluDisk(quad, inner, outer, slices, loops);
        return;
    }
    pxi_partial_disk(q, inner, outer, slices, loops, 0, 360);
}

static void pxi_gluSphere(GLUquadric *quad, GLdouble radius, GLint slices,
                          GLint stacks)
{
    static float sin1a[kMaxSlices + 1], cos1a[kMaxSlices + 1];
    static float sin3a[kMaxSlices + 1], cos3a[kMaxSlices + 1];
    static float sin1b[kMaxStacks + 1], cos1b[kMaxStacks + 1];
    static float sin2b[kMaxStacks + 1], cos2b[kMaxStacks + 1];
    static float sin3b[kMaxStacks + 1], cos3b[kMaxStacks + 1];
    PXIQuadric *q = pxi_quadric_for_draw(quad);
    int inside, i, j, start, finish;
    float sign;

    /* Flat-shaded spheres do not yet match GLU's normals exactly. */
    if (q && q->normals == GLU_FLAT) {
        pxi_unsupported("a flat-shaded gluSphere");
        q = 0;
    }
    if (!q || slices < 2 || stacks < 1 || radius < 0 ||
        slices > kMaxSlices || stacks > kMaxStacks) {
        gluSphere(quad, radius, slices, stacks);
        return;
    }
    inside = q->orientation == GLU_INSIDE;
    sign = inside ? -1 : 1;

    for (i = 0; i < slices; i++) {
        float angle = 2 * M_PI * i / slices;
        float face = 2 * M_PI * (i - 0.5f) / slices;

        sin1a[i] = sin(angle);
        cos1a[i] = cos(angle);
        sin3a[i] = sin(face);
        cos3a[i] = cos(face);
    }
    for (j = 0; j <= stacks; j++) {
        float angle = M_PI * j / stacks;
        float face = M_PI * (j - 0.5f) / stacks;

        sin2b[j] = sign * sin(angle);
        cos2b[j] = sign * cos(angle);
        sin3b[j] = sign * sin(face);
        cos3b[j] = sign * cos(face);
        sin1b[j] = radius * sin(angle);
        cos1b[j] = radius * cos(angle);
    }
    /* Make sure it comes to a point. */
    sin1b[0] = 0;
    sin1b[stacks] = 0;
    sin1a[slices] = sin1a[0];
    cos1a[slices] = cos1a[0];
    sin3a[slices] = sin3a[0];
    cos3a[slices] = cos3a[0];

    /* The sin1a / cos1a ring doubles as the smooth normal ring. */
    if (!q->texture) {
        /* Ends as triangle fans, the rest as quad strips. */
        float sintemp2, zHigh, sintemp3 = 0, costemp3 = 0;

        start = 1;
        finish = stacks - 1;

        /* The end at +z (j == 0). */
        sintemp2 = sin1b[1];
        zHigh = cos1b[1];
        if (q->normals == GLU_FLAT) {
            sintemp3 = sin3b[1];
            costemp3 = cos3b[1];
        } else if (q->normals == GLU_SMOOTH) {
            sintemp3 = sin2b[1];
            costemp3 = cos2b[1];
            pxi_glNormal3f(sin1a[0] * sin2b[0], cos1a[0] * sin2b[0],
                           cos2b[0]);
        }
        pxi_glBegin(GL_TRIANGLE_FAN);
        pxi_glVertex3f(0, 0, radius);
        for (i = 0; i <= slices; i++) {
            int k = inside ? i : slices - i;

            if (q->normals == GLU_SMOOTH) {
                pxi_glNormal3f(sin1a[k] * sintemp3, cos1a[k] * sintemp3,
                               costemp3);
            } else if (q->normals == GLU_FLAT) {
                int f = inside ? k : k + 1;

                if (inside ? k != 0 : k != slices)
                    pxi_glNormal3f(sin3a[f] * sintemp3, cos3a[f] * sintemp3,
                                   costemp3);
            }
            pxi_glVertex3f(sintemp2 * sin1a[k], sintemp2 * cos1a[k], zHigh);
        }
        pxi_glEnd();

        /* The end at -z (j == stacks - 1). */
        sintemp2 = sin1b[stacks - 1];
        zHigh = cos1b[stacks - 1];
        if (q->normals == GLU_FLAT) {
            sintemp3 = sin3b[stacks];
            costemp3 = cos3b[stacks];
        } else if (q->normals == GLU_SMOOTH) {
            sintemp3 = sin2b[stacks - 1];
            costemp3 = cos2b[stacks - 1];
            pxi_glNormal3f(sin1a[stacks] * sin2b[stacks],
                           cos1a[stacks] * sin2b[stacks], cos2b[stacks]);
        }
        pxi_glBegin(GL_TRIANGLE_FAN);
        pxi_glVertex3f(0, 0, -radius);
        for (i = 0; i <= slices; i++) {
            int k = inside ? slices - i : i;

            if (q->normals == GLU_SMOOTH) {
                pxi_glNormal3f(sin1a[k] * sintemp3, cos1a[k] * sintemp3,
                               costemp3);
            } else if (q->normals == GLU_FLAT) {
                int f = inside ? k + 1 : k;

                if (inside ? k != slices : k != 0)
                    pxi_glNormal3f(sin3a[f] * sintemp3, cos3a[f] * sintemp3,
                                   costemp3);
            }
            pxi_glVertex3f(sintemp2 * sin1a[k], sintemp2 * cos1a[k], zHigh);
        }
        pxi_glEnd();
    } else {
        start = 0;
        finish = stacks;
    }

    for (j = start; j < finish; j++) {
        float zLow = cos1b[j];
        float zHigh = cos1b[j + 1];
        float sintemp1 = sin1b[j];
        float sintemp2 = sin1b[j + 1];
        float sintemp3 = 0, costemp3 = 0, sintemp4 = 0, costemp4 = 0;

        if (q->normals == GLU_FLAT) {
            sintemp4 = sin3b[j + 1];
            costemp4 = cos3b[j + 1];
        } else if (q->normals == GLU_SMOOTH) {
            if (!inside) {
                sintemp3 = sin2b[j + 1];
                costemp3 = cos2b[j + 1];
                sintemp4 = sin2b[j];
                costemp4 = cos2b[j];
            } else {
                sintemp3 = sin2b[j];
                costemp3 = cos2b[j];
                sintemp4 = sin2b[j + 1];
                costemp4 = cos2b[j + 1];
            }
        }

        pxi_glBegin(GL_QUAD_STRIP);
        for (i = 0; i <= slices; i++) {
            float s = 1 - (float) i / slices;

            if (q->normals == GLU_SMOOTH)
                pxi_glNormal3f(sin1a[i] * sintemp3, cos1a[i] * sintemp3,
                               costemp3);
            if (!inside) {
                if (q->texture)
                    pxi_glTexCoord2f(s, 1 - (float) (j + 1) / stacks);
                pxi_glVertex3f(sintemp2 * sin1a[i], sintemp2 * cos1a[i],
                               zHigh);
            } else {
                if (q->texture)
                    pxi_glTexCoord2f(s, 1 - (float) j / stacks);
                pxi_glVertex3f(sintemp1 * sin1a[i], sintemp1 * cos1a[i],
                               zLow);
            }
            if (q->normals == GLU_SMOOTH)
                pxi_glNormal3f(sin1a[i] * sintemp4, cos1a[i] * sintemp4,
                               costemp4);
            else if (q->normals == GLU_FLAT)
                pxi_glNormal3f(sin3a[i] * sintemp4, cos3a[i] * sintemp4,
                               costemp4);
            if (!inside) {
                if (q->texture)
                    pxi_glTexCoord2f(s, 1 - (float) j / stacks);
                pxi_glVertex3f(sintemp1 * sin1a[i], sintemp1 * cos1a[i],
                               zLow);
            } else {
                if (q->texture)
                    pxi_glTexCoord2f(s, 1 - (float) (j + 1) / stacks);
                pxi_glVertex3f(sintemp2 * sin1a[i], sintemp2 * cos1a[i],
                               zHigh);
            }
        }
        pxi_glEnd();
    }
}

/* ---- parts of GLU not redone here --------------------------------------- */

static void pxi_gluBeginSurface(GLUnurbs *nurb)
{
    pxi_unsupported("GLU NURBS");
    gluBeginSurface(nurb);
}

static void pxi_gluBeginCurve(GLUnurbs *nurb)
{
    pxi_unsupported("GLU NURBS");
    gluBeginCurve(nurb);
}

/*
 * A tessellator hands its triangles to the application's callbacks, which
 * draw with gl*() calls we do see. Without a vertex callback GLU draws the
 * triangles itself, which we do not.
 */
#define kMaxTessellators    32

static GLUtesselator *gCallbackTess[kMaxTessellators];
static int gCallbackTessNext;

static void pxi_gluTessCallback(GLUtesselator *tess, GLenum which,
                                GLvoid (*fn)())
{
    if (fn && (which == GLU_TESS_VERTEX || which == GLU_TESS_VERTEX_DATA)) {
        pthread_mutex_lock(&gLock);
        gCallbackTess[gCallbackTessNext] = tess;
        gCallbackTessNext = (gCallbackTessNext + 1) % kMaxTessellators;
        pthread_mutex_unlock(&gLock);
    }
    gluTessCallback(tess, which, fn);
}

static void pxi_check_tess(GLUtesselator *tess)
{
    int i;

    for (i = 0; i < kMaxTessellators; i++)
        if (gCallbackTess[i] == tess)
            return;
    pxi_unsupported("GLU tessellation without a vertex callback");
}

static void pxi_gluTessBeginPolygon(GLUtesselator *tess, GLvoid *data)
{
    pxi_check_tess(tess);
    gluTessBeginPolygon(tess, data);
}

static void pxi_gluBeginPolygon(GLUtesselator *tess)
{
    pxi_check_tess(tess);
    gluBeginPolygon(tess);
}

#define PXI_GLU_REPLACEMENTS \
    PXI_REPLACE(gluPerspective) \
    PXI_REPLACE(gluLookAt) \
    PXI_REPLACE(gluOrtho2D) \
    PXI_REPLACE(gluBuild2DMipmaps) \
    PXI_REPLACE(gluBuild1DMipmaps) \
    PXI_REPLACE(gluQuadricNormals) \
    PXI_REPLACE(gluQuadricOrientation) \
    PXI_REPLACE(gluQuadricDrawStyle) \
    PXI_REPLACE(gluQuadricTexture) \
    PXI_REPLACE(gluDeleteQuadric) \
    PXI_REPLACE(gluCylinder) \
    PXI_REPLACE(gluDisk) \
    PXI_REPLACE(gluPartialDisk) \
    PXI_REPLACE(gluSphere) \
    PXI_REPLACE(gluBeginSurface) \
    PXI_REPLACE(gluBeginCurve) \
    PXI_REPLACE(gluTessCallback) \
    PXI_REPLACE(gluTessBeginPolygon) \
    PXI_REPLACE(gluBeginPolygon)
