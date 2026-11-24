# Builds libppcosxgl.a and the gldemo test program inside a PowerPC
# Mac OS X 10.4 guest with Xcode Tools installed.
#
# ppcosx-gpu.h, ppcosx-gl-ops.h and gl_encode.inc come from the QEMU tree and
# tools/gen_gl.py; tools/make-guest-iso.sh ships them next to these sources.

CFLAGS = -arch ppc -O2 -Wall -I.

all: build/gldemo

build/pxgl.o: pxgl.c pxgl.h pxcore.h gl_encode.inc ppcosx-gpu.h ppcosx-gl-ops.h
	mkdir -p build
	gcc $(CFLAGS) -c pxgl.c -o $@

build/pxcore.o: pxcore.c pxcore.h ppcosx-gpu.h ppcosx-gl-image.h
	mkdir -p build
	gcc $(CFLAGS) -c pxcore.c -o $@

build/libppcosxgl.a: build/pxgl.o build/pxcore.o
	rm -f $@
	ar rcs $@ build/pxgl.o build/pxcore.o

build/gldemo: gldemo.c pxgl.h build/libppcosxgl.a
	gcc $(CFLAGS) gldemo.c build/libppcosxgl.a -framework IOKit \
	    -framework CoreFoundation -o $@

clean:
	rm -rf build
