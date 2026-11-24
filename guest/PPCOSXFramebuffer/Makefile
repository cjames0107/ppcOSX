# Builds PPCOSXFramebuffer.kext inside a PowerPC Mac OS X 10.4 guest with
# Xcode Tools installed. Modern host compilers cannot emit PowerPC Mach-O.
#
#   make            build build/PPCOSXFramebuffer.kext
#   sudo make load  load it now (until reboot)
#   sudo make install   copy to /System/Library/Extensions

NAME    = PPCOSXFramebuffer
KEXT    = build/$(NAME).kext
KFW     = /System/Library/Frameworks/Kernel.framework

# ppcosx-gpu.h is copied next to the sources by tools/make-guest-iso.sh.
CFLAGS  = -arch ppc -Os -Wall -nostdinc -fno-builtin -fno-common -static \
          -mlong-branch -force_cpusubtype_ALL -finline \
          -fno-keep-inline-functions -DKERNEL -DKERNEL_PRIVATE \
          -DDRIVER_PRIVATE -DAPPLE -DNeXT \
          -I$(KFW)/PrivateHeaders -I$(KFW)/Headers -I.
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti -fno-check-new -fapple-kext

all: $(KEXT)

build/$(NAME).o: $(NAME).cpp $(NAME).h ppcosx-gpu.h ppcosx-gl-image.h
	mkdir -p build
	g++ $(CXXFLAGS) -c $(NAME).cpp -o $@

build/kmod_info.o: kmod_info.c
	mkdir -p build
	gcc $(CFLAGS) -c kmod_info.c -o $@

$(KEXT): build/$(NAME).o build/kmod_info.o Info.plist
	rm -rf $(KEXT)
	mkdir -p $(KEXT)/Contents/MacOS
	g++ -arch ppc -static -nostdlib -r build/kmod_info.o build/$(NAME).o \
	    -lkmodc++ -lkmod -lcc_kext -o $(KEXT)/Contents/MacOS/$(NAME)
	cp Info.plist $(KEXT)/Contents/Info.plist

load: $(KEXT)
	rm -rf /tmp/$(NAME).kext
	cp -R $(KEXT) /tmp/$(NAME).kext
	chown -R root:wheel /tmp/$(NAME).kext
	chmod -R go-w /tmp/$(NAME).kext
	kextload -t /tmp/$(NAME).kext

install: $(KEXT)
	rm -rf /System/Library/Extensions/$(NAME).kext
	cp -R $(KEXT) /System/Library/Extensions/$(NAME).kext
	chown -R root:wheel /System/Library/Extensions/$(NAME).kext
	chmod -R go-w /System/Library/Extensions/$(NAME).kext
	touch /System/Library/Extensions

clean:
	rm -rf build
