# Builds libPPCOSXGL.dylib inside a PowerPC Mac OS X 10.4 guest with Xcode
# Tools installed.
#
#   make                 build build/libPPCOSXGL.dylib
#   sudo make install    copy it to /usr/local/lib
#
# Run one application through it with:
#   DYLD_INSERT_LIBRARIES=/usr/local/lib/libPPCOSXGL.dylib /path/to/app
#
# pxcore.[ch] come from ../libppcosxgl; ppcosx-gpu.h, ppcosx-gl-ops.h and
# gl_interpose.inc come from the QEMU tree and tools/gen_gl.py.

CORE   = ../libppcosxgl
CFLAGS = -arch ppc -O2 -Wall -I. -I$(CORE)
DYLIB  = build/libPPCOSXGL.dylib
DEST   = /usr/local/lib

all: $(DYLIB)

$(DYLIB): ppcosxgl_interpose.c ppcosxgl_glu.inc gl_interpose.inc $(CORE)/pxcore.c $(CORE)/pxcore.h
	mkdir -p build
	gcc $(CFLAGS) -dynamiclib -install_name $(DEST)/libPPCOSXGL.dylib \
	    ppcosxgl_interpose.c $(CORE)/pxcore.c \
	    -framework OpenGL -framework ApplicationServices -framework IOKit \
	    -framework CoreFoundation -lobjc -o $@

install: $(DYLIB)
	mkdir -p $(DEST)
	cp $(DYLIB) $(DEST)/libPPCOSXGL.dylib
	chown root:wheel $(DEST)/libPPCOSXGL.dylib
	chmod 755 $(DEST)/libPPCOSXGL.dylib

clean:
	rm -rf build
