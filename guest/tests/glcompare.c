/*
 * glcompare: draw a set of small scenes, each exercising one part of the
 * OpenGL pipeline, into a pbuffer, and write each picture out, so that the
 * same scenes drawn by two renderers can be compared pixel by pixel.
 *
 *   glcompare DIR [hw|sw]
 *
 * hw (the default) asks for an accelerated renderer drawing to a pbuffer,
 * sw for Apple's software renderer drawing off screen. Pictures go to DIR/NN-name.ppm (64x64, rows top
 * first); one line per scene is printed with its name and mean colour.
 *
 * Licensed under the GNU GPL, version 2 or later.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glu.h>
#include <OpenGL/glext.h>
#ifdef GLCOMPARE_FULLSCREEN
#include <ApplicationServices/ApplicationServices.h>
#endif
#ifdef GLCOMPARE_AGL
#include <Carbon/Carbon.h>
#include <AGL/agl.h>
static AGLContext agl_ctx;
#include <unistd.h>
/* a debug option: set in the environment, or a file /tmp/NAME (for open) */
static int option(const char *name)
{
    char path[64];

    snprintf(path, sizeof(path), "/tmp/%s", name);
    return getenv(name) != NULL || access(path, F_OK) == 0;
}
#endif

#define W 64
#define H 64

static GLuint gTex[3];
static GLubyte offscreen[H][W][4];     /* ARGB, top row first */

static void tri(float r, float g, float b, float x0, float y0, float x1,
                float y1, float x2, float y2, float z)
{
    glColor3f(r, g, b);
    glBegin(GL_TRIANGLES);
    glVertex3f(x0, y0, z);
    glVertex3f(x1, y1, z);
    glVertex3f(x2, y2, z);
    glEnd();
}

static void quad(float x0, float y0, float x1, float y1, float s)
{
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(x0, y0);
    glTexCoord2f(s, 0); glVertex2f(x1, y0);
    glTexCoord2f(s, s); glVertex2f(x1, y1);
    glTexCoord2f(0, s); glVertex2f(x0, y1);
    glEnd();
}

static void make_textures(void)
{
    static GLubyte checks[32][32][4], ramp[16][16][4], big[128][128][3];
    int x, y;

    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++) {
            int on = ((x / 4) ^ (y / 4)) & 1;
            checks[y][x][0] = on ? 255 : 30;
            checks[y][x][1] = on ? 200 : 60;
            checks[y][x][2] = on ? 40 : 220;
            checks[y][x][3] = (GLubyte) (x * 8);
        }
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            ramp[y][x][0] = (GLubyte) (x * 16);
            ramp[y][x][1] = (GLubyte) (y * 16);
            ramp[y][x][2] = 128;
            ramp[y][x][3] = 255;
        }
    for (y = 0; y < 128; y++)
        for (x = 0; x < 128; x++) {
            int on = ((x / 2) ^ (y / 2)) & 1;
            big[y][x][0] = on ? 255 : 0;
            big[y][x][1] = (GLubyte) (on ? 255 : x * 2);
            big[y][x][2] = on ? 255 : 0;
        }
    glGenTextures(3, gTex);
    glBindTexture(GL_TEXTURE_2D, gTex[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, checks);
    glBindTexture(GL_TEXTURE_2D, gTex[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, ramp);
    glBindTexture(GL_TEXTURE_2D, gTex[2]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGB, 128, 128, GL_RGB,
                      GL_UNSIGNED_BYTE, big);
}

static void reset(void)
{
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_FOG);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_CULL_FACE);
    glActiveTextureARB(GL_TEXTURE1_ARB);
    glDisable(GL_TEXTURE_2D);
    glActiveTextureARB(GL_TEXTURE0_ARB);
    glDisable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColorMask(1, 1, 1, 1);
    glShadeModel(GL_SMOOTH);
    glLineWidth(1);
    glPointSize(1);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(0.1f, 0.1f, 0.2f, 0);
    glClearDepth(1);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

static void scene(int n)
{
    switch (n) {
    case 0:                                     /* flat colours */
        glShadeModel(GL_FLAT);
        tri(1, 0, 0, -0.9f, -0.9f, 0.9f, -0.9f, 0, 0.9f, 0);
        tri(0, 1, 0, -0.5f, 0.5f, 0.5f, 0.5f, 0, -0.5f, 0);
        break;
    case 1:                                     /* Gouraud */
        glBegin(GL_TRIANGLES);
        glColor3f(1, 0, 0); glVertex2f(-0.9f, -0.9f);
        glColor3f(0, 1, 0); glVertex2f(0.9f, -0.9f);
        glColor3f(0, 0, 1); glVertex2f(0, 0.9f);
        glEnd();
        break;
    case 2:                                     /* depth test */
        glEnable(GL_DEPTH_TEST);
        tri(1, 0, 0, -0.9f, -0.9f, 0.9f, -0.9f, 0, 0.9f, 0.2f);
        tri(0, 1, 0, -0.9f, 0.5f, 0.9f, 0.5f, 0, -0.9f, -0.2f);
        tri(0, 0, 1, -0.9f, 0, 0.9f, 0.3f, 0.9f, -0.3f, 0.5f);
        break;
    case 3:                                     /* alpha blending */
        tri(1, 0, 0, -0.9f, -0.9f, 0.9f, -0.9f, 0, 0.9f, 0);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBegin(GL_QUADS);
        glColor4f(0, 0, 1, 0); glVertex2f(-0.8f, -0.5f);
        glColor4f(0, 0, 1, 1); glVertex2f(0.8f, -0.5f);
        glColor4f(0, 1, 1, 1); glVertex2f(0.8f, 0.5f);
        glColor4f(0, 1, 1, 0); glVertex2f(-0.8f, 0.5f);
        glEnd();
        break;
    case 4:                                     /* additive blending */
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        tri(0.6f, 0.2f, 0, -0.9f, -0.9f, 0.9f, -0.9f, 0, 0.9f, 0);
        tri(0.6f, 0.2f, 0.5f, -0.9f, 0.5f, 0.9f, 0.5f, 0, -0.9f, 0);
        break;
    case 5:                                     /* alpha test */
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.5f);
        glBegin(GL_QUADS);
        glColor4f(1, 1, 0, 0); glVertex2f(-0.9f, -0.9f);
        glColor4f(1, 1, 0, 1); glVertex2f(0.9f, -0.9f);
        glColor4f(1, 0, 1, 1); glVertex2f(0.9f, 0.9f);
        glColor4f(1, 0, 1, 0); glVertex2f(-0.9f, 0.9f);
        glEnd();
        break;
    case 6:                                     /* texture, REPLACE */
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, gTex[0]);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        quad(-0.9f, -0.9f, 0.9f, 0.9f, 1);
        break;
    case 7:                                     /* bilinear, MODULATE */
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, gTex[1]);
        glColor3f(1, 0.5f, 1);
        glRotatef(20, 0, 0, 1);
        quad(-0.8f, -0.8f, 0.8f, 0.8f, 1);
        break;
    case 8:                                     /* mipmaps, minified */
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, gTex[2]);
        glColor3f(1, 1, 1);
        glMatrixMode(GL_PROJECTION);
        glFrustum(-0.5, 0.5, -0.5, 0.5, 1, 20);
        glMatrixMode(GL_MODELVIEW);
        glTranslatef(0, -1, -3);
        glRotatef(-70, 1, 0, 0);
        quad(-2, -8, 2, 2, 4);
        break;
    case 9:                                     /* linear fog */
        glEnable(GL_FOG);
        {
            GLfloat c[4] = { 0.7f, 0.7f, 0.7f, 1 };
            glFogfv(GL_FOG_COLOR, c);
        }
        glFogi(GL_FOG_MODE, GL_LINEAR);
        glFogf(GL_FOG_START, 1);
        glFogf(GL_FOG_END, 6);
        glMatrixMode(GL_PROJECTION);
        glFrustum(-0.5, 0.5, -0.5, 0.5, 1, 20);
        glMatrixMode(GL_MODELVIEW);
        glTranslatef(0, -1, -2);
        glRotatef(-70, 1, 0, 0);
        glColor3f(0, 0.3f, 1);
        quad(-2, -6, 2, 2, 1);
        break;
    case 10:                                    /* stencil */
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_ALWAYS, 1, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        glColorMask(0, 0, 0, 0);
        tri(1, 1, 1, -0.9f, -0.9f, 0.9f, -0.9f, 0, 0.9f, 0);
        glColorMask(1, 1, 1, 1);
        glStencilFunc(GL_EQUAL, 1, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glColor3f(1, 0.5f, 0);
        glRectf(-1, -0.3f, 1, 0.3f);
        glStencilFunc(GL_NOTEQUAL, 1, 0xff);
        glColor3f(0, 0.5f, 1);
        glRectf(-0.3f, -1, 0.3f, 1);
        break;
    case 11:                                    /* lines and points */
        {
            int i;

            glBegin(GL_LINES);
            for (i = 0; i < 8; i++) {
                float a = i * 0.785f;
                glColor3f(1, i / 8.0f, 0);
                glVertex2f(0, 0);
                glVertex2f(0.9f * cosf(a), 0.9f * sinf(a));
            }
            glEnd();
            glBegin(GL_LINE_STRIP);
            glColor3f(0, 1, 1);
            for (i = 0; i <= 16; i++)
                glVertex2f(-0.9f + i * 0.1125f, -0.8f + 0.1f * (i & 1));
            glEnd();
            glPointSize(1);
            glBegin(GL_POINTS);
            glColor3f(1, 1, 1);
            for (i = 0; i < 16; i++)
                glVertex2f(-0.9f + i * 0.12f, 0.85f);
            glEnd();
        }
        break;
    case 12:                                    /* two texture units */
        glActiveTextureARB(GL_TEXTURE0_ARB);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, gTex[0]);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glActiveTextureARB(GL_TEXTURE1_ARB);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, gTex[1]);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glBegin(GL_QUADS);
        glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 0, 0);
        glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 0, 0); glVertex2f(-0.9f, -0.9f);
        glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 1, 0);
        glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 1, 0); glVertex2f(0.9f, -0.9f);
        glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 1, 1);
        glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 1, 1); glVertex2f(0.9f, 0.9f);
        glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 0, 1);
        glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 0, 1); glVertex2f(-0.9f, 0.9f);
        glEnd();
        break;
    case 13:                                    /* DECAL and ADD, texture alpha */
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, gTex[0]);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
        glColor3f(0, 0.8f, 0.2f);
        quad(-0.9f, 0, 0.9f, 0.9f, 1);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
        glColor3f(0.3f, 0, 0.3f);
        quad(-0.9f, -0.9f, 0.9f, 0, 1);
        break;
    case 14:                                    /* scissor and colour mask */
        glEnable(GL_SCISSOR_TEST);
        glScissor(10, 10, 40, 30);
        tri(1, 1, 0, -0.9f, -0.9f, 0.9f, -0.9f, 0, 0.9f, 0);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(0, 1, 0, 1);
        glColor3f(1, 1, 1);
        glRectf(-0.9f, 0.4f, 0.9f, 0.9f);
        break;
    case 15:                                    /* logic op XOR */
        tri(1, 0.5f, 0.25f, -0.9f, -0.9f, 0.9f, -0.9f, 0, 0.9f, 0);
        glEnable(GL_COLOR_LOGIC_OP);
        glLogicOp(GL_XOR);
        glColor3f(1, 1, 1);
        glRectf(-0.5f, -0.5f, 0.5f, 0.5f);
        break;
    case 16:                                    /* glDrawPixels */
        {
            static GLubyte img[20][30][4];
            int x, y;

            for (y = 0; y < 20; y++)
                for (x = 0; x < 30; x++) {
                    img[y][x][0] = (GLubyte) (x * 8);
                    img[y][x][1] = (GLubyte) (y * 12);
                    img[y][x][2] = 200;
                    img[y][x][3] = 255;
                }
            glRasterPos2f(-0.5f, -0.5f);
            glDrawPixels(30, 20, GL_RGBA, GL_UNSIGNED_BYTE, img);
            glRasterPos2f(0, 0.2f);
            glPixelZoom(1, 1);
            glDrawPixels(30, 20, GL_RGBA, GL_UNSIGNED_BYTE, img);
        }
        break;
    case 17:                                    /* culling and winding */
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        tri(1, 0, 0, -0.9f, -0.9f, 0, -0.9f, -0.45f, 0, 0);    /* CCW: kept */
        tri(0, 1, 0, 0, -0.9f, -0.45f, 0, 0.9f, -0.9f, 0);     /* CW: culled */
        glCullFace(GL_FRONT);
        tri(0, 0, 1, -0.9f, 0.9f, 0, 0.9f, -0.45f, 0.1f, 0);   /* CW: kept */
        break;
    }
}

static const char *names[] = {
    "flat", "gouraud", "depth", "blend", "additive", "alphatest",
    "tex-replace", "tex-bilinear", "mipmap", "fog", "stencil", "lines",
    "multitexture", "decal-add", "scissor-mask", "logicop", "drawpixels",
    "culling"
};

int main(int argc, char **argv)
{
    const char *dir;
    int sw;

    /* launched as an application (10.2), the first argument is -psn_... */
    if (argc > 1 && !strncmp(argv[1], "-psn", 4)) {
        argc = 1;
    }
    dir = argc > 1 ? argv[1] : "/tmp";
    sw = argc > 2 && !strcmp(argv[2], "sw");
    CGLPixelFormatAttribute attrs[16];
    CGLPixelFormatObj pf;
    CGLContextObj ctx;
#if !defined(GLCOMPARE_FULLSCREEN) && !defined(GLCOMPARE_AGL)
    CGLPBufferObj pb;
#endif
    GLint n = 0, i = 0;
    static GLubyte px[H][W][4];
    int s;

    attrs[i++] = kCGLPFAColorSize; attrs[i++] = 24;
    attrs[i++] = kCGLPFAAlphaSize; attrs[i++] = 8;
    attrs[i++] = kCGLPFADepthSize; attrs[i++] = 16;
    attrs[i++] = kCGLPFAStencilSize; attrs[i++] = 8;
    if (sw) {
        attrs[i++] = kCGLPFAOffScreen;
        attrs[i++] = kCGLPFARendererID;
        attrs[i++] = 0x00020200;   /* Apple software renderer */
    } else {
        attrs[i++] = kCGLPFAAccelerated;
        attrs[i++] = kCGLPFANoRecovery;
#ifdef GLCOMPARE_FULLSCREEN
        attrs[i++] = kCGLPFAFullScreen;
        attrs[i++] = kCGLPFADoubleBuffer;
        attrs[i++] = kCGLPFADisplayMask;
        attrs[i++] = CGDisplayIDToOpenGLDisplayMask(kCGDirectMainDisplay);
#endif
    }
    attrs[i] = 0;
#ifdef GLCOMPARE_AGL
    /*
     * 10.2: no pbuffers, and full-screen CGL refuses the drawable; draw
     * into an on-screen Carbon window through AGL and read the back buffer.
     */
    if (!sw) {
        static GLint aa[] = {
            AGL_RGBA, AGL_DOUBLEBUFFER, AGL_ACCELERATED, AGL_NO_RECOVERY,
            AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8, AGL_BLUE_SIZE, 8,
            AGL_ALPHA_SIZE, 8, AGL_DEPTH_SIZE, 16, AGL_STENCIL_SIZE, 8,
            AGL_NONE
        };
        Rect r;
        WindowRef win;
        AGLPixelFormat apf;
        AGLContext actx;

        SetRect(&r, 40, 60, 40 + W, 60 + H);
        if (CreateNewWindow(kDocumentWindowClass, kWindowNoAttributes, &r,
                            &win)) {
            fprintf(stderr, "no window\n");
            return 1;
        }
        ShowWindow(win);
        SelectWindow(win);
        /* let the window server put the window on screen */
        RunCurrentEventLoop(1.0);
        apf = aglChoosePixelFormat(NULL, 0, aa);
        if (!apf || !(actx = aglCreateContext(apf, NULL)) ||
            !aglSetDrawable(actx, GetWindowPort(win)) ||
            !aglSetCurrentContext(actx)) {
            fprintf(stderr, "no AGL context: %s\n",
                    (const char *) aglErrorString(aglGetError()));
            return 1;
        }
        aglUpdateContext(actx);
        agl_ctx = actx;
        goto have_context;
    }
#endif
    if (CGLChoosePixelFormat(attrs, &pf, &n) || !pf) {
        fprintf(stderr, "no pixel format\n");
        return 1;
    }
    if (CGLCreateContext(pf, NULL, &ctx)) {
        fprintf(stderr, "no context\n");
        return 1;
    }
    if (sw) {
        /* the software renderer draws off screen rather than to pbuffers */
        if (CGLSetOffScreen(ctx, W, H, W * 4, offscreen)) {
            fprintf(stderr, "no off-screen context\n");
            return 1;
        }
    } else {
#ifdef GLCOMPARE_FULLSCREEN
        /*
         * 10.2 has no pbuffers: draw full screen (the display captured)
         * into the bottom-left W x H and read that back.
         */
        CGLError e;

        if (CGDisplayCapture(kCGDirectMainDisplay)) {
            fprintf(stderr, "cannot capture the display\n");
            return 1;
        }
        if ((e = CGLSetFullScreen(ctx))) {
            fprintf(stderr, "no full-screen context: %s\n", CGLErrorString(e));
            return 1;
        }
#elif !defined(GLCOMPARE_AGL)
        if (CGLCreatePBuffer(W, H, GL_TEXTURE_2D, GL_RGBA, 0, &pb) ||
            CGLSetPBuffer(ctx, pb, 0, 0, 0)) {
            fprintf(stderr, "no pbuffer\n");
            return 1;
        }
#endif
    }
    CGLSetCurrentContext(ctx);
#ifdef GLCOMPARE_AGL
have_context:
#endif
    printf("renderer: %s\n", glGetString(GL_RENDERER));
    glViewport(0, 0, W, H);
    make_textures();
    for (s = 0; s < (int) (sizeof(names) / sizeof(names[0])); s++) {
        char path[512];
        double sum[3] = { 0, 0, 0 };
        FILE *f;
        int x, y;

        reset();
        scene(s);
        glFinish();
        if (sw) {
            /* read the buffer itself: ARGB, top row first */
            for (y = 0; y < H; y++)
                for (x = 0; x < W; x++) {
                    px[H - 1 - y][x][0] = offscreen[y][x][1];
                    px[H - 1 - y][x][1] = offscreen[y][x][2];
                    px[H - 1 - y][x][2] = offscreen[y][x][3];
                    px[H - 1 - y][x][3] = offscreen[y][x][0];
                }
        } else {
#if defined(GLCOMPARE_FULLSCREEN) || defined(GLCOMPARE_AGL)
            glReadBuffer(GL_BACK);
#endif
#ifdef GLCOMPARE_AGL
            /* GLCOMPARE_FRONT: present the frame and read the front buffer */
            if (option("GLCOMPARE_FRONT")) {
                aglSwapBuffers(agl_ctx);
                glFinish();
                glReadBuffer(GL_FRONT);
            }
#endif
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
#ifdef GLCOMPARE_AGL
            if (option("GLCOMPARE_PAUSE")) {
                printf("read %02x%02x%02x%02x\n", px[0][0][0], px[0][0][1],
                       px[0][0][2], px[0][0][3]);
                fflush(stdout);
                sleep(6);
            }
            if (option("GLCOMPARE_SHOW")) {
                if (!option("GLCOMPARE_FRONT"))
                    aglSwapBuffers(agl_ctx);
                RunCurrentEventLoop(2.0);
            }
#endif
        }
        snprintf(path, sizeof(path), "%s/%02d-%s.ppm", dir, s, names[s]);
        f = fopen(path, "wb");
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (y = H - 1; y >= 0; y--)
            for (x = 0; x < W; x++) {
                fwrite(px[y][x], 1, 3, f);
                sum[0] += px[y][x][0];
                sum[1] += px[y][x][1];
                sum[2] += px[y][x][2];
            }
        fclose(f);
        printf("%02d %-13s mean %5.1f %5.1f %5.1f  err %x\n", s, names[s],
               sum[0] / (W * H), sum[1] / (W * H), sum[2] / (W * H),
               glGetError());
    }
#ifdef GLCOMPARE_FULLSCREEN
    if (!sw) {
        CGLSetCurrentContext(NULL);
        CGLClearDrawable(ctx);
        CGDisplayRelease(kCGDirectMainDisplay);
    }
#endif
    return 0;
}
