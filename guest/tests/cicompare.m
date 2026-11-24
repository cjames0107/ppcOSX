/*
 * cicompare: run Core Image filters on the GPU and in Core Image's software
 * renderer, and write each result out, so that the two can be compared.
 *
 *   cicompare DIR hw|sw
 *
 * hw renders through a CIContext on an accelerated OpenGL pbuffer and reads
 * the pixels back; sw uses a CIContext on a bitmap with
 * kCIContextUseSoftwareRenderer. Colour matching is off in both, so only
 * the renderers differ. Each filter's result goes to DIR/NN-name.ppm; a
 * line per filter (name, milliseconds) goes to standard output.
 *
 * Build: gcc -o cicompare cicompare.m -framework Foundation \
 *            -framework QuartzCore -framework OpenGL -framework ApplicationServices
 */
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <sys/time.h>
#include <stdarg.h>

#ifndef IW
#define IW 128
#endif
#ifndef IH
#define IH IW
#endif

static double now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/* A test picture: gradients, a checkerboard and some discs. */
static CGImageRef make_input(void)
{
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    unsigned char *px = calloc(IW * IH, 4);
    CGContextRef c = CGBitmapContextCreate(px, IW, IH, 8, IW * 4, cs,
                                           kCGImageAlphaPremultipliedLast);
    int x, y;

    for (y = 0; y < IH; y++) {
        for (x = 0; x < IW; x++) {
            unsigned char *p = px + (y * IW + x) * 4;
            int check = ((x / 16) + (y / 16)) & 1;

            p[0] = x * 2;
            p[1] = y * 2;
            p[2] = check ? 220 : 40;
            p[3] = 255;
        }
    }
    CGContextSetRGBFillColor(c, 1, 0.9, 0.1, 1);
    CGContextFillEllipseInRect(c, CGRectMake(20, 20, 40, 40));
    CGContextSetRGBFillColor(c, 0.1, 0.3, 1, 0.7);
    CGContextFillEllipseInRect(c, CGRectMake(60, 50, 50, 50));
    CGContextSetRGBStrokeColor(c, 1, 1, 1, 1);
    CGContextSetLineWidth(c, 3);
    CGContextStrokeRect(c, CGRectMake(8, 80, 50, 30));
    CGImageRef img = CGBitmapContextCreateImage(c);
    CGContextRelease(c);
    CGColorSpaceRelease(cs);
    return img;
}

/* A filter with its defaults, then keys and values (nil-terminated). */
static void add(NSMutableArray *list, NSString *name, ...)
{
    CIFilter *f = [CIFilter filterWithName:name];
    va_list ap;
    id key;

    if (!f) {
        fprintf(stderr, "no filter %s\n", [name UTF8String]);
        return;
    }
    [f setDefaults];
    va_start(ap, name);
    while ((key = va_arg(ap, id)) != nil) {
        [f setValue:va_arg(ap, id) forKey:key];
    }
    va_end(ap);
    [list addObject:f];
}

static NSNumber *num(float x)
{
    return [NSNumber numberWithFloat:x];
}

static NSArray *make_filters(CIImage *in)
{
    NSMutableArray *list = [NSMutableArray array];
    CIVector *center = [CIVector vectorWithX:IW / 2 Y:IH / 2];
    NSAffineTransform *t = [NSAffineTransform transform];
    CIImage *grad;

    [t translateXBy:IW / 2 yBy:IH / 2];
    [t rotateByDegrees:30];
    [t scaleBy:0.8];
    [t translateXBy:-IW / 2 yBy:-IH / 2];
    {
        CIFilter *g = [CIFilter filterWithName:@"CIRadialGradient"];

        [g setDefaults];
        [g setValue:center forKey:@"inputCenter"];
        [g setValue:num(10) forKey:@"inputRadius0"];
        [g setValue:num(70) forKey:@"inputRadius1"];
        grad = [g valueForKey:@"outputImage"];
    }

    add(list, @"CIColorInvert", @"inputImage", in, nil);
    add(list, @"CIColorControls", @"inputImage", in,
        @"inputSaturation", num(1.8), @"inputBrightness", num(0.1),
        @"inputContrast", num(1.3), nil);
    add(list, @"CISepiaTone", @"inputImage", in, @"inputIntensity", num(0.9),
        nil);
    add(list, @"CIHueAdjust", @"inputImage", in, @"inputAngle", num(1.5), nil);
    add(list, @"CIExposureAdjust", @"inputImage", in, @"inputEV", num(0.7),
        nil);
    add(list, @"CIGammaAdjust", @"inputImage", in, @"inputPower", num(0.6),
        nil);
    add(list, @"CIColorMatrix", @"inputImage", in,
        @"inputRVector", [CIVector vectorWithX:0 Y:1 Z:0 W:0],
        @"inputGVector", [CIVector vectorWithX:0 Y:0 Z:1 W:0],
        @"inputBVector", [CIVector vectorWithX:1 Y:0 Z:0 W:0],
        @"inputBiasVector", [CIVector vectorWithX:0.1 Y:0 Z:0 W:0], nil);
    add(list, @"CIColorMonochrome", @"inputImage", in, nil);
    add(list, @"CIGaussianBlur", @"inputImage", in, @"inputRadius", num(4),
        nil);
    add(list, @"CIMotionBlur", @"inputImage", in, @"inputRadius", num(8), nil);
    add(list, @"CIZoomBlur", @"inputImage", in, @"inputCenter", center, nil);
    add(list, @"CIUnsharpMask", @"inputImage", in, @"inputRadius", num(3),
        nil);
    add(list, @"CIEdges", @"inputImage", in, nil);
    add(list, @"CIPixellate", @"inputImage", in, @"inputCenter", center,
        @"inputScale", num(8), nil);
    add(list, @"CITwirlDistortion", @"inputImage", in, @"inputCenter", center,
        @"inputRadius", num(50), nil);
    add(list, @"CIBumpDistortion", @"inputImage", in, @"inputCenter", center,
        @"inputRadius", num(50), @"inputScale", num(0.6), nil);
    add(list, @"CIPinchDistortion", @"inputImage", in, @"inputCenter", center,
        @"inputRadius", num(60), nil);
    add(list, @"CIAffineTransform", @"inputImage", in, @"inputTransform", t,
        nil);
    add(list, @"CICheckerboardGenerator", @"inputCenter", center,
        @"inputWidth", num(10), nil);
    add(list, @"CIRadialGradient", @"inputCenter", center,
        @"inputRadius0", num(5), @"inputRadius1", num(60), nil);
    add(list, @"CIStripesGenerator", @"inputCenter", center, nil);
    add(list, @"CICrystallize", @"inputImage", in, @"inputCenter", center,
        nil);
    add(list, @"CIBloom", @"inputImage", in, nil);
    add(list, @"CIPointillize", @"inputImage", in, @"inputCenter", center,
        nil);
    add(list, @"CIKaleidoscope", @"inputImage", in, @"inputCenter", center,
        nil);
    add(list, @"CIColorPosterize", @"inputImage", in, nil);
    add(list, @"CIMultiplyBlendMode", @"inputImage", in,
        @"inputBackgroundImage", grad, nil);
    add(list, @"CISourceOverCompositing", @"inputImage",
        [in imageByApplyingTransform:CGAffineTransformMakeScale(0.5, 0.5)],
        @"inputBackgroundImage", in, nil);
    return list;
}

static void write_ppm(NSString *dir, int n, NSString *name,
                      const unsigned char *rgba, int stride, bool flip)
{
    NSString *path = [NSString stringWithFormat:@"%@/%02d-%@.ppm", dir, n,
                      name];
    FILE *fp = fopen([path fileSystemRepresentation], "wb");
    int x, y;

    if (!fp) {
        return;
    }
    fprintf(fp, "P6\n%d %d\n255\n", IW, IH);
    for (y = 0; y < IH; y++) {
        const unsigned char *row = rgba + (flip ? IH - 1 - y : y) * stride;

        for (x = 0; x < IW; x++) {
            fwrite(row + x * 4, 1, 3, fp);
        }
    }
    fclose(fp);
}

int main(int argc, char **argv)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSString *dir;
    bool hw;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    NSDictionary *opts;
    CIContext *ci;
    unsigned char *px = calloc(IW * IH, 4);
    CGContextRef bm = NULL;
    NSArray *filters;
    unsigned i;

    if (argc < 3) {
        fprintf(stderr, "usage: cicompare DIR hw|sw\n");
        return 1;
    }
    dir = [NSString stringWithUTF8String:argv[1]];
    hw = strcmp(argv[2], "sw") != 0;

    if (hw) {
        CGLPixelFormatAttribute attrs[] = {
            kCGLPFAAccelerated, kCGLPFANoRecovery, kCGLPFAColorSize, 24,
            kCGLPFAAlphaSize, 8, 0
        };
        CGLPixelFormatObj pf;
        CGLContextObj ctx;
        CGLPBufferObj pb;
        GLint npf;

        if (CGLChoosePixelFormat(attrs, &pf, &npf) || !pf ||
            CGLCreateContext(pf, NULL, &ctx) ||
            CGLCreatePBuffer(IW, IH, GL_TEXTURE_2D, GL_RGBA, 0, &pb) ||
            CGLSetPBuffer(ctx, pb, 0, 0, 0)) {
            fprintf(stderr, "no accelerated pbuffer\n");
            return 1;
        }
        CGLSetCurrentContext(ctx);
        fprintf(stderr, "renderer: %s\n", glGetString(GL_RENDERER));
        glViewport(0, 0, IW, IH);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, IW, 0, IH, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        opts = [NSDictionary dictionaryWithObjectsAndKeys:
                [NSNull null], kCIContextWorkingColorSpace,
                (id)cs, kCIContextOutputColorSpace, nil];
        ci = [CIContext contextWithCGLContext:ctx pixelFormat:pf
                                      options:opts];
    } else {
        bm = CGBitmapContextCreate(px, IW, IH, 8, IW * 4, cs,
                                   kCGImageAlphaPremultipliedLast);
        opts = [NSDictionary dictionaryWithObjectsAndKeys:
                [NSNull null], kCIContextWorkingColorSpace,
                (id)cs, kCIContextOutputColorSpace,
                [NSNumber numberWithBool:YES],
                kCIContextUseSoftwareRenderer, nil];
        ci = [CIContext contextWithCGContext:bm options:opts];
    }
    if (!ci) {
        fprintf(stderr, "no CIContext\n");
        return 1;
    }

    CGImageRef inimg = make_input();
    CIImage *in = [CIImage imageWithCGImage:inimg];
    CGRect rect = CGRectMake(0, 0, IW, IH);

    filters = make_filters(in);
    for (i = 0; i < [filters count]; i++) {
        NSAutoreleasePool *p2 = [[NSAutoreleasePool alloc] init];
        CIFilter *f = [filters objectAtIndex:i];
        NSString *name = [[f attributes] objectForKey:kCIAttributeFilterName];
        CIImage *out = [f valueForKey:@"outputImage"];
        double t0 = now_ms(), t1;

        if (hw) {
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
            [ci drawImage:out atPoint:CGPointZero fromRect:rect];
            glFinish();
            t1 = now_ms();
            glReadPixels(0, 0, IW, IH, GL_RGBA, GL_UNSIGNED_BYTE, px);
            write_ppm(dir, i, name, px, IW * 4, true);
        } else {
            CGContextClearRect(bm, rect);
            [ci drawImage:out atPoint:CGPointZero fromRect:rect];
            CGContextFlush(bm);
            t1 = now_ms();
            write_ppm(dir, i, name, px, IW * 4, false);
        }
        printf("%-28s %8.2f ms\n", [name UTF8String], t1 - t0);
        fflush(stdout);
        [p2 release];
    }
    [pool release];
    return 0;
}
