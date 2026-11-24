# ppcOSX

A PowerPC Mac emulator for running Mac OS X 10.0 - 10.5, built on a fork of
QEMU 10.1.3 with a paravirtual GPU device for host-accelerated graphics.

## Layout

- `qemu/` - the QEMU fork (its own git checkout, branch `ppcosx`, based on
  tag `v10.1.3`). Our changes:
  - `hw/display/ppcosx-gpu.c` - the paravirtual GPU device
  - `hw/display/ppcosx-gpu-gl.c` - host GL renderer (macOS)
  - `include/hw/display/ppcosx-gpu.h`, `ppcosx-gl-image.h` - guest/host
    protocol, shared with the guest code
  - `hw/misc/macio/pmu.c` - warns when a guest tries to sleep (not emulated)
  - `target/ppc/mmu-hash32.c` - fix for a crash when the monitor or gdb reads
    a guest address at or above 0x80000000 on a 32-bit CPU
- `ppcosx` - launcher with G3 / G4 / G5 profiles, 1-4 CPUs, and the
  emulated Radeon 9600 as the default display
- `tests/gpu_smoke.py` - exercises the GPU device from Open Firmware
- `guest/PPCOSXFramebuffer/` - Mac OS X display driver (kext) for `ppcosx-gpu`,
  including the user client that carries GL batches
- `guest/libppcosxgl/` - guest GL library and `gldemo`
- `guest/PPCOSXGL/` - OpenGL redirector inserted into unmodified applications
- `guest/tests/` - test programs that use only Apple's frameworks: gears,
  textures, vertex arrays, GLU quadrics, and a specular-lighting check
- `guest/install.sh` - scripted install/uninstall of all guest components
- `guest/prebuilt/10.4/` - the driver and redirector built for Mac OS X 10.4,
  so guests need no compiler (made by `tools/build-guest-binaries.sh`)
- `tools/gen_gl.py` - generates the GL marshalling for both sides from one table
- `tools/guest-sync.sh` - copy `guest/` into the running dev guest over SSH
- `tools/build-guest-binaries.sh` - rebuild `guest/prebuilt/` in the dev guest
- `tools/guestctl.py` - screenshot / click / type into a running guest via QMP
- `tools/guest-stack.py` - sample where a running or hung guest's CPU is and
  walk its kernel stack
- `tools/make-guest-iso.sh` - package `guest/` as a CD image for the guest
- `app/` - ppcOSX, the SwiftUI virtual machine manager (see "The ppcOSX
  app")
- `vms/` - guest disks and scratch images (not tracked)
- `build/` - build output (not tracked)

## Build

Needs Homebrew `glib`, `pixman`, `libslirp`, `ninja`, `pkgconf`.

```bash
mkdir -p build && cd build && ../qemu/configure --target-list=ppc-softmmu,ppc64-softmmu --enable-cocoa --enable-slirp --disable-docs --disable-werror --disable-guest-agent --disable-tools
```

```bash
ninja -C build qemu-system-ppc qemu-system-ppc64
```

## The ppcOSX app

`app/ppcOSX.xcodeproj` is a SwiftUI VM manager in the style of UTM or
VirtualBox. It is self-contained: the emulator is built into it. Build the
QEMU fork first (`build/`, with tools enabled for `qemu-img`:
`build/pyvenv/bin/meson configure -Dtools=enabled build`), then the app with
Xcode, or from a shell (any Xcode 16 or later):

```bash
DEVELOPER_DIR=/Applications/Xcode-beta.app/Contents/Developer xcodebuild -project app/ppcOSX.xcodeproj -target ppcOSX -configuration Debug SYMROOT=$PWD/app/build build
```

```bash
open app/build/Debug/ppcOSX.app
```

- **Library.** Each VM is a folder in `~/Documents/PPC VMs` (changeable in
  Settings) named after it: `config.json` (its settings), its disk images
  (`Macintosh HD.qcow2`), `qemu.log`, and `qemu.pid` while it runs. The
  sidebar lists them; the detail page shows the hardware, state, notes and
  Start / Pause / Shut Down / Restart / Force Power Off / Show Window.
- **New Virtual Machine** (File menu, ⌘N): the first page names the VM
  and picks its system from a menu (Mac OS 9.2 or Mac OS X 10.0 - 10.5),
  which sets recommended hardware - processor (G3, G4,
  G4 7447A, G5; only those Macs that ran it), cores (1 on a G3, up to 4),
  memory (within what the system boots with), graphics (Rage 128 for 10.0
  and OS 9, Radeon 7000 for 10.1/10.2, Radeon 9600 from 10.3), disk size -
  then install media (an ISO/CDR image; the VM starts from it the first
  time), storage (a new growable qcow2 disk or an existing image), network
  (shared NAT or none, with port forwards such as 2222 → 22 for SSH),
  sound and input.
- **Settings** for an existing VM edit all of that afterwards: name (which
  renames its folder), system, processor, cores, memory, graphics, startup
  resolution, drives (add new or existing hard disks, CD/DVD drives, USB
  drive images; choose or eject discs; read-only; start-up disk), network,
  input. Hardware is locked while the VM runs.
- Cores, memory and disk sizes take a slider, a stepper or a typed value.
  Cores go up to 4 (1 on a G3); memory up to 2 GB or this Mac's memory if
  less (QEMU's Power Mac takes no more than 2 GB, in either binary), with a
  warning past what the system boots with (512 MB for 10.0); disks up to
  the space free where the VMs live.
- **Sound**: on by default; the Power Mac's sound chip (below), played
  through this Mac's output.
- **Inside the app**: the build's "Bundle Emulator" phase runs
  `tools/bundle-emulator.sh`, which copies `qemu-system-ppc`,
  `qemu-system-ppc64` and `qemu-img` (Contents/Resources/qemu/bin), the
  firmware the Power Macs use (OpenBIOS, the VGA ndrv and BIOSes, keymaps:
  qemu/share/qemu, where the relocatable build looks), the Open Firmware
  Forth generators and `ppcosx machine-data` (CPU profiles and card
  settings, Resources/scripts), and every non-system library QEMU links
  into Contents/Frameworks, relinked to load from there and signed again.
  Nothing outside the app is needed: no Homebrew, Python or project
  folder. Debug builds take `build/` (linked against Homebrew, so they run
  only on this Mac); Release builds take `dist/qemu-build` (below).
- The app builds QEMU's command line itself (`Launcher.swift`), as
  `ppcosx run --vm FOLDER` does, from the same tables and Forth scripts;
  `tools/check-app-command.sh` checks that both give the same command for a
  range of configurations. It runs QEMU directly and controls it over a QMP
  socket; a VM left running when the app quits is found again (by
  `qemu.pid`) when it reopens.

Each running VM has a window of ppcOSX's own: QEMU runs without a user
interface of its own (`-display ppcosx`, `qemu/ui/ppcosx.c`), so no second
application, menu bar or Dock icon appears. QEMU serves the screen on a
Unix socket (`$TMPDIR/ppcosx-<id>.display`): changed rectangles as 32-bit
pixels, one frame at a time (the next only once the app has shown the
last, so a busy app gets fewer, larger frames), the guest's pointer image
and position, and whether there is an absolute pointing device; the app
sends keys (macOS key codes, which QEMU maps), pointer motion and buttons.
The app reconnects when it reopens, so VMs left running are shown again.
- The screen keeps its aspect ratio, pixel for pixel at whole-number
  scales; the window fits the guest's resolution to the display, resizes
  with it, and goes full screen. View > Actual Size (⌥⌘0).
- With the USB tablet (10.3 and later) the guest's pointer follows the
  Mac's, drawn by the guest (its hardware cursor) in place of the Mac's.
  Guests with only a relative mouse (10.0 - 10.2, Mac OS 9) capture the
  pointer on a click. Capture Input (toolbar, ⌥⌘C) also sends every key,
  Command shortcuts included, to the guest; Control-Option gives them back
  (the subtitle says so).
- Otherwise keys go to the guest, except Command shortcuts the menus use:
  Quit, Close, Hide, Minimize, Full Screen and the Virtual Machine menu.
  The library's own (⌘N, ⌘O, ⌘R) step aside while a VM window is in front,
  so the guest's New, Open and Reload work.
- Toolbar: **Power** (Shut Down, Restart, Force Power Off), **Pause** /
  **Resume**, **Devices** (each CD/DVD drive with its disc: Choose Disc
  Image…, Eject; this Mac's USB devices, checked when connected to the
  guest; the Ethernet cable; sound), **Screenshot** (a PNG on the
  Desktop), **Capture Input** and **Full Screen**. Shut Down presses the
  Mac's power key: `system_powerdown` is the power key on the mac99 machine
  (`hw/ppc/mac_newworld.c`), so Mac OS X asks to shut down (or shuts down
  from the login window); Restart and Force Power Off ask first.
- The menu bar's **Virtual Machine** menu does the same for the VM in
  front (or the one selected in the library): Pause (⌥⌘P), Shut Down,
  Restart, Force Power Off, Capture Keyboard and Pointer, Take Screenshot
  (⌥⌘3), Devices.
- Closing a running VM's window offers Shut Down or Keep Running (without
  a window; the library shows it again). Quitting ppcOSX with VMs running
  offers Shut Down (each guest may ask to confirm; ppcOSX quits once they
  have stopped), Leave Running, or Cancel.

`ppcosx run` without the app still opens QEMU's Cocoa window
(`qemu/ui/cocoa.m`, with the same toolbar); `--display-socket PATH` serves
the screen for the app instead.

### Sound: the Screamer

`hw/audio/screamer.c` emulates the Screamer (Apple 343S0184), the
AWACS-compatible codec on the mac-io's "davbus" of G3 and early G4 Macs,
which Apple's `AppleScreamerAudio` (in `Apple02Audio.kext`, from 10.0 up to
Leopard) drives:
- Registers at mac-io 0x14000 (little-endian, 16 bytes apart): sound
  control (sample rate in bits 8-10), codec control (a command: data in
  bits 0-11, register in 12-14; 7 reads one back; bit 24 busy), codec
  status (valid bit 22; a read-back value in bits 4-15, else revision 3),
  clip count, byte swap, and the frame count the driver polls for the
  playback position.
- Samples, 16-bit stereo, come from DBDMA channel 0x10 (mac-io 0x8800);
  each output command completes once the audio backend has played its
  bytes, which paces the guest. Recording (0x8900) gives silence at the
  same rate. Interrupts: codec 0x18, output DMA 0x09, input DMA 0x0a.
- Volume and mute follow the codec: register 1 mutes the headphone (A) and
  speaker (C) outputs, registers 2 and 4 attenuate them in 1.5 dB steps.
- The machine has it when given an audiodev:
  `-M mac99,audiodev=snd0 -audiodev coreaudio,id=snd0` (`ppcosx run
  --audio coreaudio`, the default; `--audio none` leaves it out; `--audio
  wav,path=out.wav` records instead, for testing).
- OpenBIOS has no node for it; `tools/sound-nvramrc.sh` writes one in Forth:
  `/pci@f2000000/mac-io@c/davbus` (its registers, interrupts and
  interrupt-parent; Mac OS X's kernel stops early without the last) with a
  `sound` child: model 343S0184 and the `sound-objects` string Apple's
  `AudioDeviceTreeParser` reads (NUL-separated objects of "key value"
  pairs: one output, the internal speaker). The Forth is the value of its
  own variable, `ppcosx-sound`, which nvramrc evaluates first.
- NVRAM: OpenBIOS's variable partition now takes all of its bank
  (`hw/nvram/mac_nvram.c`); with 0xc10 bytes, Mac OS X 10.0's BootX gave up
  (back to Open Firmware) when the Rage 128's Forth and the sound node left
  it no room to store a variable.
- Tested, recorded to WAV: Tiger 10.4.6 and Leopard 10.5.4 (speech with
  `say`; volume 25% is 18 dB down, as the codec registers say), Jaguar
  10.2 (alert sounds). Cheetah 10.0's driver attaches but keeps both
  outputs muted at full attenuation; not yet played there. Mac OS 9 is
  untested.

The app also covers the rest of what the emulator is built with:
- **Screen sharing** (Settings → Sharing): a VNC server for the VM, on a
  port from 5900, for this Mac only or the network, password protected
  (kept in the keychain, given to QEMU over QMP), optionally with TLS (a
  certificate authority and server certificate made with macOS's
  `openssl` in the VM's `tls` folder; clients must trust `tls/ca-cert.pem`)
  and JPEG compression. The detail page's Open button opens it in Screen
  Sharing.
- **USB passthrough** (Settings → USB): host devices to connect whenever
  the VM starts, by vendor and product ID (also attachable live from the
  window's Devices menu).
- **Remote images** (Settings → Storage → Add Remote Image): a hard disk
  over SSH (`ssh://user@host/path`, libssh, using the SSH agent and
  `~/.ssh/known_hosts`) or a disc image over HTTPS (curl, read-only).
- **Screenshot** in the detail page's toolbar (QMP `screendump`), and
  **Compact** for a stopped VM's qcow2 disks: `qemu-img convert -c` with
  zstd, so the image takes only what the guest wrote, compressed. New
  disks are made with zstd as their compression type.

### A distributable build

`tools/release.sh` makes a release in `dist/release`: `ppcOSX.app`, a
disk image to drag it to Applications from, and the matching source
archive. It is universal, for Apple silicon and Intel Macs with macOS 14
Sonoma or later:
- `tools/build-dist-deps.sh` builds the libraries QEMU uses (GLib, pixman,
  libslirp, GnuTLS with GMP/Nettle/libtasn1, OpenSSL and libssh, libusb,
  libpng, libjpeg-turbo, zstd, capstone) from source for macOS 14, since
  Homebrew's are built for the Mac they are installed on. Each architecture
  is built on its own (GMP, Nettle and OpenSSL have assembly for each CPU;
  Meson builds for x86_64 get a cross file, so pixman picks x86 SIMD) into
  `dist/prefix-arm64` and `dist/prefix-x86_64`; every dylib's install name
  is set to `dist/prefix/lib`, where `lipo` merges each pair into a
  universal one. libffi, zlib and curl are macOS's own. Nothing from
  Homebrew is seen: pkg-config looks only in the architecture's prefix.
  (nasm, if installed, gives the x86_64 libjpeg-turbo its SIMD code.)
- `tools/build-dist-qemu.sh` builds the fork against them for each
  architecture (`dist/qemu-build-arm64`, and `dist/qemu-build-x86_64`
  cross-compiled with `--cpu=x86_64`) with every optional feature the app
  uses: TLS, SSH and HTTPS images, USB passthrough, VNC with JPEG, PNG,
  zstd, capstone. `lipo` merges the binaries into `dist/qemu-build`.
- The Release configuration builds the app for arm64 and x86_64 and
  bundles `dist/qemu-build` (`tools/bundle-emulator.sh` takes
  `QEMU_BUILD`). Development builds (`build/`, linked against Homebrew)
  stay arm64 only. The app is about 128 MB, the disk image 31 MB.
- `tools/build-dist-qemu.sh` relinks QEMU when a library in the prefixes is
  newer than its binaries (ninja does not track them).
- The Intel half runs on Apple silicon under Rosetta, for testing:
  `arch -x86_64 .../qemu-system-ppc ...`. Tested so (release build, Tiger
  10.4.6 from its disk): it boots (in 70 s, 33 s native), plays sound, the
  app's display and the Radeon 9600's 3D on the host GPU work, and
  `qemu-img` makes zstd images. Speed on real Intel Macs, and the 3D path
  on Intel and AMD GPUs, are untested (`PPCOSX_ATI_GPU=0` falls back to the
  software engine).
- `tools/collect-licenses.sh` puts every licence in
  Contents/Resources/Licenses, with a source-code note;
  `tools/make-source-archive.sh` makes that source (the fork, the app,
  scripts, and the library archives as downloaded).
- `tools/sign-app.sh` signs inside out with the hardened runtime; QEMU
  gets `com.apple.security.cs.allow-jit` (`app/Signing/qemu.entitlements`),
  as TCG writes arm64 code into MAP_JIT memory. `SIGN_IDENTITY` picks the
  certificate: ad hoc (`-`) by default, which other Macs open only after
  Privacy & Security → Open Anyway. With a Developer ID identity,
  `tools/release.sh` also notarizes the disk image (`tools/notarize.sh`,
  through a `notarytool` keychain profile, `ppcosx-notary` by default) and
  staples the ticket:

```bash
SIGN_IDENTITY="Developer ID Application: NAME (TEAMID)" tools/release.sh
```

## Use

```bash
./ppcosx cpus
```

```bash
./ppcosx create-disk tiger.qcow2 40G
```

```bash
./ppcosx run --cpu g4 --hd tiger.qcow2 --cdrom tiger-install.iso --boot d --guest 10.4
```

The default display is an emulated Radeon 9600 on the AGP bus, which Apple's
own drivers run in every Mac OS X from 10.3 on, installers included: a guest
has Quartz Extreme and (10.4 and 10.5) Core Image from its first boot, with
nothing installed in it. The screen stays dark until Mac OS X's display
driver loads (the card has no firmware ROM). `--display radeon7000` gives the
Radeon 7000 (Quartz Extreme only; also Mac OS X 10.1 and 10.2),
`--display rage128` a Rage 128 Pro (for 10.0, with 2D and OpenGL; see
"Mac OS X 10.0 - 10.2") and `--display vga` QEMU's standard VGA.

`--guest` names the Mac OS X version, for version-specific adjustments; with
`--guest 10.5` the pointer lands where the host's is (Leopard trims 7.5% off
each end of a tablet's range, see "Mac OS X versions"). The machine's USB
keyboard identifies as an Apple Pro Keyboard, so Leopard doesn't ask you to
identify it. With `--guest 10.0`, `10.1` or `10.2` there is no tablet
(those versions can't use an absolute pointer), so the window captures the
host pointer as a real mouse would. `--cpu g5` gives a G5 (the original
970). `--smp N` gives N
CPUs, `--ssh-port` / `--qmp-port` forward the guest's SSH and QEMU's machine
protocol to the host for scripting. Add `--legacy-input` for 10.0/10.1,
`--gpu` to attach the paravirtual GPU as a second display or `--gpu-primary`
to make it the only one (needs the guest driver installed; install the OS
with the standard display first), `--dry-run` to print the QEMU command.
Arguments after `--` go to QEMU.

## Building the guest components

Current host compilers cannot emit PowerPC Mach-O, so the guest pieces are
built inside a Mac OS X 10.4 guest with Xcode Tools (from the Tiger DVD).
With the development guest running, `tools/build-guest-binaries.sh` builds
them there and copies the results into `guest/prebuilt/10.4/`; rerun it
after changing anything under `guest/`. Users' guests then install those
binaries and need no developer tools.

## Guest components

Everything installed in the guest goes in by one non-interactive script, so
the emulator can apply it for the user later: `sudo sh install.sh` in the
`guest/` folder (for example from `vms/guest-src.iso`, made by
`tools/make-guest-iso.sh`), then restart. It installs the display driver and the
OpenGL redirector and turns the redirector on for every user by setting
`DYLD_INSERT_LIBRARIES` in each `~/.MacOSX/environment.plist` (and the new-user
template). `install.sh disable` / `enable` switch the redirector off and on
system-wide without a restart; `PPCOSXGL_DISABLE=1` does it for one program;
it also turns idle system sleep off (see below);
`install.sh uninstall`, a restart, then `install.sh purge` remove everything.
It installs the prebuilt binaries for the guest's Mac OS X version when
there are some, so no compiler is needed; `install.sh source` builds from
source instead (needs Xcode Tools).

Zero-install fallback: Apple's stock `ATIRage128.kext` attaches to QEMU's
`-device ati-vga,model=rage128p` on 10.4, and the stock ATI drivers match on
PCI IDs alone, so emulating a real card's 3D engine would need nothing in
the guest. QEMU implements only that card's 2D today.

## How redirected windows reach the screen

Each redirected window's surface is filled once with a key colour unique to
its GL context (magenta, with the context number in green), and from then
on its frames go straight to the host instead of through Apple's renderer.
When the host shows the screen it puts the latest frame wherever that key
colour is visible, so the guest's window server still decides what of the
window shows: windows, menus and the Dock in front cover it, and hiding or
minimising it removes it. Where a grey translucent layer lies over the key
(window shadows, the Dock's backdrop, menus, Dashboard's dimming) the frame
shows through that layer correctly. A background thread in the redirector
follows window moves, even for programs that are not redrawing.

Limits: coloured translucent edges over a GL window, such as the rims of
Dock icons, keep a thin pink fringe; screenshots taken inside the guest show
the key colour instead of the GL picture; a window being dragged shows key
colour at its new position for up to 50 ms. This needs a single display
driven by `ppcosx-gpu`; with any other setup, or with `PPCOSXGL_COPYBACK=1`,
frames are copied back into the window through Apple's renderer, which is
correct everywhere but no faster than Apple's renderer itself.

## Quartz Extreme and Core Image

Both are decided inside Mac OS X, not per application. Quartz Extreme is
the window server's GL compositor; `Configuration.plist` in CoreGraphics'
Resources asks for a display of at least 16-bit depth, 16 MB of VRAM and an
`IOAGPDevice` parent, and the compositor then needs an IOAccelerator service
for the display plus a hardware OpenGL renderer: a GPU driver bundle (about
60 undocumented `gld*` entry points) that Tiger's OpenGL loads for that
accelerator. Core Image runs in each application and falls back to software
unless it gets an accelerated context with ARB fragment and vertex programs
and pixel buffers. The redirector covers neither.

The chosen route is to emulate a real ATI Radeon so that Apple's own drivers,
already on every Tiger install, do all of it, with nothing of ours in the
guest:

1. Radeon 7000 (RV100) for Quartz Extreme: fixed-function 3D, transform done
   by the driver. QEMU's `ati-vga,model=rv100` already emulates its 2D.
2. Radeon 9600 (RV350, R300 family) for Core Image: adds vertex and fragment
   shaders; command processor, memory and display carry over from step 1.

### Radeon 7000 status (Tiger 10.4.6)

Run with `-vga none -device ati-vga,model=rv100,romfile=,vgamem_mb=32` and an
NVRAM boot script that gives the card the device-tree name and type of a
Mac Radeon, so Apple's ATI ndrv (`AppleNDRV/ATIDriver.bundle`) drives it:

    -prom-env 'use-nvramrc?=true' -prom-env 'nvramrc=" /pci@f2000000/pci1002,5159@f"
     find-device " ATY,Rage6ag" device-name " display" device-type device-end'

`tools/ati-probe.sh` boots a throwaway snapshot of the dev guest that way.
Nothing of ours runs in the guest: the display, 2D and OpenGL are Apple's
own `ATIDriver` ndrv, `ATIRadeon.kext` and `ATIRadeonGLDriver.bundle`.

What the emulation adds to QEMU's `ati-vga`:
- PLL, memory-controller and command-queue registers the ndrv and kext poll.
- Both BAR apertures. Video memory is kept in aperture 1's byte order (the
  one big-endian Macs draw through), following `SURFACE_CNTL` as the driver
  changes it, so aperture 1 is plain memory; aperture 0 becomes swizzled MMIO
  only when its order differs.
- The command processor (`ati_cp.c`): PM4 type 0/1/2/3 packets from the
  command queue and indirect buffers, scratch-register write-back (fences)
  and the PCI GART (`AIC_*`), so commands, uploads and readbacks can live in
  system memory. Packets can be logged with `PPCOSX_ATI_DUMP` (see the
  probe script).
- 2D engine: card addresses in video memory or the GART, scissor clipping,
  `PAINT_MULTI`/`BITBLT_MULTI` packets, byte-aligned `SRC_OFFSET`/
  `DST_OFFSET`, and the bus byte swapping (`SRC_ENDIAN`/`DST_ENDIAN`) that
  texture uploads and pixel readbacks use.
- The R100 3D engine (`ati_3d.c`), rasterised on the host into video
  memory: triangles, strips, fans, rectangles, lines and points from
  immediate-mode packets; depth (16 and 24-bit) and stencil; three texture
  units with every R100 texel format except DXTn, wrap modes, bilinear
  filtering, mipmaps and rectangle textures; the texture combiners; alpha
  test, fog, blending, logic ops, colour masks and scissors; colour buffers
  in video memory or the GART.

Results: the desktop, windows and OpenGL programs run on Apple's drivers.
`guest/tests/glcompare.c` draws 18 scenes (shading, depth, blending, alpha
test, textures, mipmaps, fog, stencil, lines, multitexture, texture modes,
scissor/colour mask, logic op, glDrawPixels, culling) on the emulated card
and on Apple's software renderer; `tools/ati-glcompare.sh` compares them,
and 17 match to within a level on average (the lines scene differs on 2% of
pixels, at line ends). Gears runs at about 130 fps.

### Quartz Extreme: the Radeon on the AGP bus

Tiger's compositor only runs on a display whose card is an `IOAGPDevice`.
With `ATI_AGP=1`, `tools/ati-probe.sh` puts the card in slot 16 of the
UniNorth AGP bus (`-device ati-vga,...,agp=on,bus=agp,addr=0x10`), and
Quartz Extreme comes on (System Profiler: "Bus: AGP", "Quartz Extreme:
Supported"); the window server then composites with Apple's Radeon GL
driver on the emulated 3D engine, genie effect included. Still nothing of
ours in the guest. What it took:
- QEMU's AGP host bridge gets the bus windows of a real PowerMac3,x (memory
  at 0x90000000-0x9fffffff, I/O at 0xf0000000), UniNorth-style
  configuration cycles, the address-select register `AppleMacRiscAGP`
  checks (bit 9: 0x90000000), an AGP 2.0 capability, and the UniNorth GART
  registers (base, aperture, control; the ready bit in internal status
  that `setAGPEnable` waits for; invalidations finish at once).
- The card's AGP accesses (`MC_AGP_LOCATION` from `AGP_BASE`) go through
  that GART (`unin_agp_translate`); Apple's driver keeps its command
  buffers there.
- OpenBIOS does not scan the AGP bus, so at reset the machine assigns the
  card's BARs as firmware would (`core99_agp_firmware`: video memory at
  0x90000000, registers at 0x94000000, I/O at 0x400, pin A to OpenPIC
  0x1b), and the NVRAM boot script builds the bus node `/pci@f0000000`
  (`uni-north`, ranges, interrupt map) and the card's node with matching
  `reg`, `assigned-addresses` and a `model`. WindowServer reads every AGP
  card's `model` and crashes without one.
- `CRTC_OFFSET` now moves the scanout, for drivers that flip pages.

### Core Image: the Radeon 9600 (R300)

`tools/ati-probe-r300.sh` (and `tools/run-tiger-accel.sh`, by default) puts
an emulated Radeon 9600 (RV350, PCI 0x4150, 64MB) on the AGP bus, named
`ATY,Pheonix` (Apple's spelling), a name every version's ATI ndrv accepts;
Tiger and Leopard also take `ATY,Whelk`, the name of Apple's G5 9600
boards, but Panther has no ndrv for it. Apple's `.Display_DualHead` ndrv,
`ATIRadeon9700.kext` and `ATIRadeon9700GLDriver.bundle` drive it, and
System Profiler reports "Quartz Extreme: Supported" and "Core Image:
Supported". Again nothing of ours is in the guest. What the R300 needed
(`ati_r300.c`, about 3,000 lines, plus changes to `ati.c`/`ati_cp.c`):
- The CP's bus-mastered ring buffer (`CP_RB_*`): Apple's R300 driver feeds
  the CP through it rather than the PIO queue. Its read pointer and the
  scratch registers are written back to memory little-endian (the swap
  bits of `CP_RB_RPTR_ADDR` / `SCRATCH_ADDR`), and `SW_INT_FIRE` raises the
  software interrupt the driver's fences wait for.
- The PCI GART table's address is in register 0xab0 on the R300, not
  `AIC_PT_BASE`; reading it from the old place scattered the card's writes
  over guest memory.
- A 64KB register BAR and the 3D engine: vertex fetch from packets or from
  vertex arrays (`VAP_PROG_STREAM_CNTL`, `VAP_VTX_AOS_*`), the vertex
  program (PVS) interpreter, viewport transform and near-plane clipping,
  point sprites with `GA_POINT_S0..T1` (the compositor blits windows as
  single points), the rasteriser's interpolators (RS), a fragment program
  (US) interpreter shading 2x2 quads so texture LOD has real derivatives
  (nodes, texture instructions, the RGB/alpha ALU with pre-subtract,
  modifiers and fp24 constants), 16 texture units with integer and float
  formats, FG fog and alpha test, Z/stencil, blending and logic ops into
  8-bit, 16-bit and floating-point colour buffers.
- `US_CODE_OFFSET`'s texture fields are at bits 17:13 (the Linux headers
  are off by one), and the R300's fragment selectors 15-19 are the
  pre-subtract result.
- 2D packets with bit 7 of the opcode clear (`0x1a`, `0x1b`) are
  PAINT/BITBLT_MULTI without their state header; the compositor copies
  windows from the screen with them while they are dragged.
- HyperZ fast depth clears (`3D_CLEAR_ZMASK` with `ZB_BW_CNTL` fast fill):
  depth is kept uncompressed, so the buffer is filled with
  `ZB_DEPTHCLEARVALUE` before the next draw that uses it.
- What Chess needed: multisample resolves (`RB3D_AARESOLVE_*`; the driver
  draws antialiased windows into an offscreen buffer and resolves them to
  the window, which is a copy here as the card has no real multisampling),
  vertex arrays in video memory read without the bus byte swap (the swap
  applies to system memory only), and the vertex program's dual math
  (a second scalar op per instruction, bit 28 of the destination word)
  writing the alternate temporaries, plus `LIT`'s operand order.

Results (Tiger 10.4.6, compared with Apple's software renderers):
- The desktop composites correctly (windows, shadows, Dock, menu bar).
- `tools/ati-glcompare.sh`: 17 of 18 GL scenes match; lines differ on 3.5%
  of pixels.
- `tools/ati-cicompare.sh` runs 28 Core Image filters (colour adjustments,
  blurs, distortions, generators, crystallize, bloom, blend modes,
  compositing) through Core Image's GPU path on the emulated card and
  through its software renderer: all 28 match, the worst on 0.6% of
  pixels (edge interpolation). At 512x512 Twirl differs on 7% of pixels:
  outside the twirl's radius the filter should return its input, and the
  card's picture equals the input there exactly, while Core Image's
  software renderer is off on 10.5% of those pixels (the checkerboard's
  edges shifted by part of a pixel), so the reference is the one that is
  off. The GPU path is the R300's: Core Image's
  kernels arrive as 117 different fragment programs. It is not faster than
  Core Image's own software path, as the emulated GPU is itself software.

### The Radeon 9600's 3D on the host GPU

On macOS hosts the R300 engine draws on the host's GPU (OpenGL 4.1 core
through CGL, `ati_r300_gl.c`); QEMU prints "R300 3D on the host GPU" with
the renderer's name. `PPCOSX_ATI_GPU=0` uses the software engine instead,
which is also what other hosts get.
- Vertex work stays on the CPU (fetch, vertex program, clipping, culling,
  points and lines as triangles); the GPU gets window-space triangles with
  the rasteriser's outputs and does interpolation, the fragment program
  (translated to GLSL and cached), texturing, fog, alpha test,
  depth/stencil, blending and logic ops into host copies of the card's
  colour and depth buffers.
- Video memory stays the reference. QEMU's dirty log is harvested once per
  command batch into a generation number per 4KB page; a host copy is
  reloaded when a page under it changed since it was in step. What the GPU
  drew goes back to video memory at the end of each batch (before the
  guest can see the batch finished), before the 2D engine reads or writes
  that memory and before any draw done in software. Textures drawn earlier
  in the same batch are copied on the GPU.
- Draws the GPU path does not handle (other colour formats, cube or 3D
  textures, signed texture components, buffers in system memory) fall back
  to the software engine.
- Accuracy: the 18 GL scenes match Apple's software renderer as before
  (depth and alpha test now exactly, as the GPU tests alpha before writing
  depth); all 28 Core Image filters match Core Image's software renderer.
- Speed, Core Image at 1024x1024 (`tools/ati-cicompare.sh LABEL 1024`):
  Gaussian blur 0.2 s against 4.5 s in Core Image's own software renderer,
  zoom blur 0.12 s against 17.8 s, colour filters 13-50 ms against
  0.3-0.8 s. Compositing (`tools/ati-dragbench.sh`): the card's command
  processor spends 3.4 s on a fixed set of window drags, against 5.6 s in
  software (an earlier build, before the Chess fixes, measured 2.4 s); most of what remains is waiting for each batch's pixels to come
  back from the GPU, which keeps video memory exact for the guest.

The software engine got faster too: the fragment program is decoded once
per draw, only the temporaries it uses are cleared per pixel, triangles
of 16384 pixels or more are shaded by several host threads in row bands
(`PPCOSX_R300_THREADS`, default one per host core up to 8; the result is
the same as on one thread), and the compositor's window blits (point
sprites mapping texels one to one) are copied row by row. Core Image at
512x512 on the software engine: 3.9 s for the 28 filters, from 9.1 s.

`PPCOSX_ATI_CP_THREAD=1` runs the R300's command processor on a thread of
its own instead of in the register write that kicks the ring. The guest
can run on while a batch is drawn, and `RBBM_STATUS` reports the engine
busy until the ring is empty. Apple's driver waits for nearly every batch
it sends, though, so little overlaps. Measured, gears drew 107.6 fps with
the thread against 113.0 fps without, and Core Image (28 filters at
512x512) took no less time. So it is off by default. The host GPU context is
taken and released on each use, as the thread and the vCPUs both use it.

`PPCOSX_ATI_3D_STATS=1` prints draw counts and where the time goes (draw
submission, page tracking, uploads, read-backs, 2D).

The hardware cursor: Apple's Radeon drivers use a 64x64 ARGB cursor
(`CRTC_GEN_CNTL` cursor mode 3), now decoded for QEMU's host cursor. They
move it with the update lock held, so its position and visibility go to
the display whenever any cursor register is written or the screen is
refreshed, not only on unlocked position writes.

## CPUs: G3, G4, G5 and multiprocessor Macs

Both emulated Radeons work with any of the CPUs Tiger runs on, and with up
to four of them:

```bash
PPCOSX_CPU=g5 PPCOSX_SMP=4 tools/run-tiger-accel.sh
```

```bash
./ppcosx run --cpu g4 --smp 2 --hd tiger.qcow2
```

`PPCOSX_CPU` is `g3` (PowerPC 750), `g4` (7400, the default), `g5` (970)
or any QEMU CPU name; `PPCOSX_SMP` (or `--smp`) is 1 to 4. With more than one
CPU each gets its own host thread (multi-threaded TCG, which QEMU offers only
in `qemu-system-ppc64`, so that binary runs multiprocessor G3s and G4s too).
The probe scripts take `ATI_CPU` and `ATI_SMP`, and `PPCOSX_SSH_PORT` /
`PPCOSX_QMP_PORT` to run a second guest beside the first.

Tested on Tiger 10.4.6 with the Radeon 9600: a G3, a G4, a G5, a dual G4, a
dual G5 and a quad G5 all boot to the desktop with Quartz Extreme and Core
Image, the GL scenes and Core Image filters compare exactly as on the G4,
and on the multiprocessor ones independent processes run in parallel (four
CPU-bound processes on the quad G5: 6.6 s each, against 5.7 s for one
alone). The Radeon 7000 was checked again on the G4. Mac OS X sees the G5
as a PowerMac3,1 (a G4 tower) with a 970, which Apple's G4 platform driver
handles. (Those runs used the 970FX; the profile is now the original 970, retested
with Panther, Tiger and Leopard, single, dual and quad; see "Mac OS X
versions".)

The G3 needed nothing. The G5 (`hw/ppc/mac_newworld.c`, `target/ppc`,
`hw/pci-host/uninorth.c`, the firmware's device tree through nvramrc):
- Registers QEMU's 970 lacked: HSPRG0/1 (the kernel's exception vectors
  save registers there) and SCOMC/SCOMD (it reads and writes back the L2
  configuration through them).
- HID5 starts in 32-byte `dcbz` mode, as Apple's firmware leaves it: the
  kernel's mapping allocator clears 32-byte lines with `dcbz`, and the
  970's native 128-byte `dcbz` wiped neighbouring mappings ("mapping_alloc
  - empty mapping block").
- On a data segment fault the 970 sets DSISR (to the store bit only); the
  kernel reads it, and a stale "no PTE" bit left by an earlier fault made it
  treat the segment fault as a page fault forever.
- `slbmte` ignores what the 970 ignores: the kernel passes the faulting
  address itself, low bits and all, so the bits between the ESID and the
  index, the index's upper bits and the segment size field are dropped.
- The U3 bridge (the G5's one bus) got the UniNorth AGP bridge's
  AGP capability, address-select register and GART (U3 GART entries are
  big-endian page numbers with bit 31 valid), and its bus is named `agp` so
  the card goes on `bus=agp` on both machines.
- Apple's PCI driver finds a bus's configuration registers at its I/O base
  (from "ranges") + 0x800000; QEMU's and OpenBIOS's G5 put the I/O space at
  0xf2000000, so the U3's registers are shown at 0xf2800000 too.
- OpenBIOS probes the G5's bus itself, so the nvramrc only names the card's
  node and gives it its model, and widens the bus's declared memory range
  to the 0x80000000-0xefffffff QEMU decodes (OpenBIOS declares 256MB and
  then places the card's registers above it).

Multiprocessor (`hw/ppc/mac_newworld.c`, `hw/misc/macio/gpio.c`,
`hw/intc/openpic.c`, `tools/smp-nvramrc.sh`):
- Secondary CPUs start powered off. Mac OS X (AppleCore99PE's Core99CPU)
  starts one by strobing its soft-reset line, KeyLargo GPIO 0x5b, 0x5c,
  0x67 or 0x68, low then high; QEMU then gives that CPU a system reset
  interrupt at 0x100, where the kernel waits for it. Each CPU's PIR is its
  number.
- The KeyLargo MPIC takes up to 4 CPUs, and each CPU's interrupt inputs are
  now its own (they were all wired to the last CPU created).
- OpenBIOS lists only the boot CPU; the nvramrc adds the others, copies of
  CPU 0 with their own `reg`, `state` "stopped" (Core99CPU requires a
  state; "running" marks the boot CPU) and `soft-reset`, the KeyLargo GPIO
  that starts the CPU (0x5c, 0x67, 0x68 for CPUs 1-3, the lines Apple's
  multiprocessor Core99 machines use). Apple's MacRISC2CPU strobes the line
  `soft-reset` names; without it, it started CPU 1 but never CPUs 2 and 3,
  while the interrupt controller still sent them device interrupts, so the
  disk never answered and a quad stopped at "Still waiting for root
  device" (dark screen on the Radeon).
- The nvramrc selects `/cpus` once and does all its work there. In
  OpenBIOS `find-device` and `device-end` change the dictionary search
  order, so a word that ran `device-end` couldn't be found again: the
  second `cpu-add` stopped the firmware ("cpu-add:" then "of_startup
  returned!"), and only duals ever booted.
- With one host thread for all CPUs, a CPU spinning on a kernel lock held
  the thread for a whole time slice and the guest crawled; hence a thread
  per CPU. That exposed a deadlock: the R300 host-GPU backend took QEMU's
  dirty log from inside a register write on a vCPU, and taking it waits for
  every other vCPU, one of which was waiting for this one (to flush TLBs).
  On a vCPU the backend now takes the snapshot without that wait.

Not emulated: freezing the timebase while the CPUs synchronise it
(KeyLargo GPIO 0x73 is ignored), so the CPUs' timebases can differ by the
time the kernel's synchronisation takes; nothing in testing showed an
effect.

## Mac OS X versions: Panther, Tiger and Leopard

Tested installs: Panther 10.3.0 (`vms/panther.qcow2`), Tiger 10.4.6
(`vms/tiger.qcow2`), Leopard 10.5.4 (`vms/leopard.qcow2`), each with its
`*-account.txt` beside it. On the emulated Radeon 9600, with Apple's own
drivers and nothing of ours installed:

| | Quartz Extreme | Core Image | OpenGL scenes vs software (worst) | Core Image filters vs software (worst) |
|---|---|---|---|---|
| Panther 10.3.0 | yes | none in 10.3 | 3.5% of pixels | - |
| Tiger 10.4.6 | yes | yes | 3.5% | 0.6% |
| Leopard 10.5.4 | yes | yes ("Hardware Accelerated") | 1.8% | 9.8% (CIPointillize: a few dots take a neighbour's colour; GPU precision picks a different nearest point) |

The 3.5% is the line-drawing scene, as before. Panther's and Leopard's
installers also run on the Radeon 9600, so a VM can use it from its first
boot.

Other CPUs, all on the Radeon 9600 with `ppcosx` defaults:

| | G4 | dual G4 | quad G4 | G5 | dual G5 | quad G5 |
|---|---|---|---|---|---|---|
| Panther 10.3.0 | QE, SSH at 56 s | QE, 54 s | not tested | QE, 60 s | not tested | not tested |
| Tiger 10.4.6 | QE + CI, 35 s | not retested | not tested | QE + CI, 61 s | QE + CI, 60 s | QE + CI, 60 s |
| Leopard 10.5.4 | QE + CI, 60 s | QE + CI, 60 s | QE + CI, 34 s | QE + CI, 60 s | QE + CI, 59 s | QE + CI, 59 s |

(QE: Quartz Extreme; CI: Core Image; times are power-on to SSH answering,
with the desktop up.) A Perl loop that takes 3.4 s on Leopard on a G4 takes
4.4 s on the G5, and an idle G5 guest uses about 10% of a host core. Four
of those loops at once take 6 s on Leopard as a quad G5, 8 s as a dual G5
and 11 s as a dual G4, so the CPUs run in parallel.
Panther's `system_profiler` has no Displays section; Quartz
Extreme was checked with `guest/tests/qecheck.c`
(`CGDisplayUsesOpenGLAcceleration`). The comparison programs are built in
the Tiger guest and kept in `guest/prebuilt/10.4/tests/` (`ATI_BINS=` that
directory makes `tools/ati-glcompare.sh` / `ati-cicompare.sh` copy them in
rather than compile; `glcompare-10.3` avoids `cosf`/`sinf`, which Panther's
libSystem lacks). `tools/ati-probe.sh` takes `ATI_DISK=` for another guest.

What the other versions needed:
- **NVRAM.** Mac OS X could never read the firmware variables (`nvram -p`
  failed with kIOReturnUnsupported, Tiger included), and Panther's installer
  then refused every disk: "You cannot start up your computer using this
  volume" (it wants to set the startup disk). Apple's `AppleCore99NVRAM`
  reads the NVRAM as two flat 8 KiB flash banks, takes the one with a valid
  header and the newest generation, and finds the variables in a partition
  named `common`. QEMU had 8 KiB in all, at every other byte, an Adler-32
  seeded with 0 instead of 1 (so no bank was ever valid) and no `common`
  partition. Now (`hw/nvram/mac_nvram.c`, `mac_newworld.c`): 16 KiB, one
  byte per address (OpenBIOS is told with `FW_CFG_PPC_NVRAM_FLAT`), bank A
  OpenBIOS's, bank B a Core99 bank with `common` (seeded with `-prom-env`),
  and the flash's erase/program/status commands, so Mac OS X's saves work
  too. Its `common` has signature 0x71 rather than 0x70, since OpenBIOS takes
  every 0x70 partition as its own; at each machine reset Mac OS X's newest
  bank moves to B and A goes back to OpenBIOS, which would otherwise load
  Mac OS X's `boot-command=mac-boot` and stop at its prompt. `nvram`
  settings and `bless --setOF` now survive restarts.
- **The 8 GB rule.** Panther's installer also limits some Macs to volumes
  within the disk's first 8 GB; it did not apply here once NVRAM worked.
- **Panther's display driver.** Panther 10.3.0 has no ndrv for `ATY,Whelk`
  ("Not usable", no framebuffer); hence the name `ATY,Pheonix`.
- **Panther's vertex programs.** Its GL driver writes vertex program data to
  register 0x2204, Tiger's to `VAP_PVS_UPLOAD_DATA` (0x2208); both are the
  data port now (everything was drawn degenerate).
- **Byte-swapped video memory on the host GPU.** Panther sets aperture 1 to
  swap 32-bit words (`SURFACE_CNTL`), Tiger does not. In that mode the GL
  backend's write-back copied whole rows of a temporary buffer, so the
  columns beside a dirty rectangle got uninitialised bytes; window title
  bars and anything drawn in small off-screen buffers came out scrambled.
  It now writes only the rectangle's columns of each row.
- **Leopard's pointer.** Leopard's HID driver trims 7.5% off each end of an
  absolute pointer's logical range (`kIOHIDAbsoluteAxisBoundsRemovalPercentage`,
  IOHIDFamily-258), so the pointer ran 1/0.85 times too far from the centre;
  Panther and Tiger do not trim. `usb-tablet` has an `abs-margin` property
  that squeezes positions into the middle of the range; `ppcosx --guest 10.5`
  sets it to 2458 (7.5%), which puts the pointer within a pixel of the
  host's.

- **Leopard's keyboard check.** Leopard runs its Keyboard Setup Assistant
  ("press the key to the right of Shift") for any keyboard that isn't
  Apple's; QEMU's had QEMU's vendor ID (0x0627). `usb-kbd` now has
  `vendorid` / `productid` properties, and the keyboard the mac99 machine
  creates itself presents an Apple Pro Keyboard (ANSI, 0x05ac/0x020b), so
  Leopard knows the layout and goes straight on. `ppcosx` no longer adds a
  second `usb-kbd` (QEMU gave every key press to both). Checked by deleting
  Leopard's record of known keyboards: no assistant with the Apple IDs, the
  assistant again when a keyboard with QEMU's IDs is added.

- **Text in Quartz GL apps.** Leopard apps that set `QuartzGLEnable` in
  their Info.plist (Activity Monitor and System Preferences among them) draw
  their window contents on the GPU, text included: glyphs go into an 8-bit
  glyph cache in video memory through 2D blits whose source is *host data*,
  the glyph bytes written to the `HOST_DATA0`-`HOST_DATA_LAST` registers
  (0x17c0-0x17e0) right after `DST_HEIGHT_WIDTH`. The 2D engine ignored the
  source selection and copied from `DEFAULT_OFFSET` instead, so the cache
  filled with whatever was there and every glyph came out as bars. Host
  data blits now collect those bytes (packed from row to row, the first
  pixel in a word's most significant byte) and write the rectangle when it
  is full (`ati_2d_host_data` in `hw/display/ati_2d.c`). The CP's
  `CNTL_BITBLT` packet (0x92), which these apps also send, is handled too.
  With the host GPU backend a few labels in those windows are still lost
  (Activity Monitor's "% User", part of "Processes" and of the CPU graph's
  title); the software backend (`PPCOSX_ATI_GPU=0`) draws them, so the
  cause is in `ati_r300_gl.c` and not yet found.

The G5 (`--cpu g5`) needed four more fixes, all in QEMU:
- **Panther wouldn't start on a G5.** BootX drew half the Apple logo and
  stopped: the kernel had returned to it at once (BootX's `FailToBoot(6)`,
  after `CallKernel`). Panther 10.3.0's kernel predates the 970FX that the
  G5 profile emulated and returns on a processor it doesn't know. The
  profile is now the original 970 (`970_v2.2`, as in the first Power Mac
  G5s), which every version from 10.2.7 on knows.
- **NVRAM read back empty on a G5.** The G5 kernel copies the flash out with
  8-byte loads; the NVRAM device accepted at most 4, so the loads failed and
  both banks looked invalid. Leopard's IODTNVRAM then walked a partition map
  whose first header had length 0, forever, in IOKit's configuration
  thread. That thread starved everything else: it was why the G5 took four
  times as long to boot, ran user programs five times slower than its own
  CPU time suggested, and took minutes to bring up the display. The device
  now accepts 8-byte accesses.
- **HID0 could not be written.** QEMU gave the 970's HID0 a write-to-clear
  handler, so the kernel could never set a bit in it. Mac OS X marks "idle"
  by setting HID0's doze/nap bits and its interrupt entry reads them to send
  an interrupted idle loop back to the scheduler; without them the CPU went
  back into the idle loop after the first interrupt and never ran a thread
  again (once the NVRAM fix let it go idle at all). HID0 is an ordinary
  register now, and QEMU's power-save check uses the 970's bit positions
  (bits 7 to 9) rather than the G4's, so an idle G5 really halts.
- **Waking from nap with interrupts off.** To time its wake-ups the kernel
  naps with MSR[EE] clear and a 16-tick decrementer and expects the 970 to
  wake and carry on at the next instruction. QEMU only woke a halted CPU for
  an interrupt it could deliver; a 970 in doze or nap now also wakes for a
  pending decrementer or external interrupt (`ppc_970_nap_wakeup`).

Not done yet:
- The Setup Assistant runs as usual on a new guest's first boot.

## Mac OS X 10.0 - 10.2: Cheetah, Puma and Jaguar

The install discs in `install images/`: 10.0.3 (4K78), 10.1 (5G64) and
10.2 (6C115, two discs). What each ships for ATI cards:

| | ATI display driver (NDRV) knows | Kexts, 2D and GL | On our cards |
|---|---|---|---|
| 10.2 | Rage 128, Radeon (`ATY,Rage6ag`), Radeon 8500 | Radeon, Radeon 8500, Rage 128 | Radeon 7000: desktop, Quartz Extreme, OpenGL |
| 10.1 | Rage 128, Radeon (`ATY,Rage6ag`) | Radeon, Rage 128 | Radeon 7000: desktop, OpenGL (10.1 has no Quartz Extreme) |
| 10.0.3 | Rage 128 only (`ATIDriverUpdate`: `ATY,Rage128Pd` etc.; Radeons of 2001 carried their NDRV in ROM) | Radeon, Rage 128 | Rage 128: desktop with 2D acceleration, OpenGL (below); standard VGA: desktop |

None knows the R300, so the Radeon 9600 is for 10.3 on.

| VM | Run with | Account |
|---|---|---|
| Jaguar 10.2 (`vms/jaguar.qcow2`) | `./ppcosx run --hd vms/jaguar.qcow2 --display radeon7000 --guest 10.2` | `vms/jaguar-account.txt` |
| Puma 10.1 (`vms/puma.qcow2`) | `./ppcosx run --hd vms/puma.qcow2 --display radeon7000 --guest 10.1 --legacy-input` | `vms/puma-account.txt` |
| Cheetah 10.0.3 (`vms/cheetah.qcow2`) | `./ppcosx run --hd vms/cheetah.qcow2 --display rage128 --guest 10.0 --legacy-input --ram 512` | `vms/cheetah-account.txt` |

### OpenGL on the Radeon 7000 under 10.1 and 10.2

`guest/tests/glcompare.c` built with `-DGLCOMPARE_AGL`
(`guest/prebuilt/10.4/tests/glcompare-10.2`, built on Tiger) draws the 18
scenes in an AGL window (10.1 and 10.2 have no pbuffers, and a full-screen
CGL context gave "invalid drawable"). It has to run as an application
(wrapped in a `.app` and started with `open`) to get a window on screen.
Results against Apple's software renderer, the same on both:

| Scene | Pixels off |
|---|---|
| 15 of 18 (flat, gouraud, depth, blend, additive, bilinear, mipmap, fog, stencil, decal-add, scissor/mask, drawpixels, culling, ...) | 0.0% |
| alpha test, texture replace, multitexture, logic op | 0.1 - 0.4% |
| lines | 3.3% (line ends; 2.1% on Tiger) |

Gaps in the emulated Radeon 7000 these drivers found, now filled
(`hw/display/ati*.c`); Tiger's results on both cards are unchanged:
- **Readback (10.2)**: Jaguar's GL driver reads pixels back with the
  video **bus-master DMA engine** (`DMA_VID_TABLE_ADDR`, 0x7a0; also
  `DMA_GUI_TABLE_ADDR`): a table of 16-byte descriptors (source, destination,
  command with byte count, byte swaps and end of list) copying video memory
  to GART pages. Without it every readback was black.
- **Readback (10.1)**: Puma's reads the 32-bit back buffer directly through
  the framebuffer aperture while the desktop is at 16 bits, relying on a
  **SURFACE** range (`SURFACE0_LOWER/UPPER_BOUND`/`INFO`, 0xb04 + 16n) that
  gives that range its own byte swapping. Surfaces are now windows over the
  aperture in their own byte order; without them red read back as cyan.
- **glDrawPixels (10.1)**: sent as `CNTL_HOSTDATA_BLT` (CP packet 0x94):
  the 2D header, source colours, position, size, word count and the pixels.
- **Mipmaps (10.2)**: Jaguar sends projective texture coordinates
  (s*q, t*q, q); the level of detail was taken per triangle from s and t
  undivided, so the base level was always used. It is now computed per
  pixel from the screen-space derivatives of the perspective-correct
  coordinates, as the hardware does.

### Mac OS X 10.0 and the Rage 128

10.0 installs on QEMU's standard VGA (`--display vga`, 512 MB), with ADB
input (`--legacy-input`). Its display drivers know only the Rage 128, so
`ppcosx --display rage128` adds QEMU's Rage 128 Pro (1002:5046) on the AGP
bus as `ATY,Rage128Pd`, the AGP Power Mac G4's card. 10.0 then runs on it
with Apple's 2D acceleration (the desktop, windows, dragging, minimising
into the Dock, menus and scrolling all draw through the card's 2D engine)
and Apple's "Rage 128 OpenGL Engine": Chess plays, drawn by the emulated 3D
engine. What it took:
- **A boot display.** 10.0's kernel stops at its first console message when
  BootX hands it no screen (10.1 on cope; there is no FCode ROM for the
  emulated cards). `ppcosx` now does in Open Firmware Forth what the card's
  ROM would: sets 1024x768 at 8 bits per pixel through the card's
  registers, describes the framebuffer (`address`, `width`, `height`,
  `linebytes`, `depth`) with `open`/`close`/`write`/`set-colors`/`color!`/
  `fill-rectangle`/`draw-rectangle`/`read-rectangle` methods, gives the AGP
  bus node `open`/`close` (opening a device opens its parents), makes the
  card the console, and sets `frame-buffer-adr` (BootX's own rectangle
  words draw there; it was 0, so BootX painted over the exception vectors).
  BootX then shows its grey screen and happy Mac, and the kernel boots.
  (`set-colors` must not use `>r` inside its loop: `i` reads the return
  stack.)
- Apple's `ATIDriverUpdate` NDRV and `ATIRage128` kext drive the card and set
  800x600 at 32 bits from the EDID. QEMU's Rage 128 needed the CRTC total
  and pitch registers to accept 16-bit writes, and `PM4_STAT` (0x7b8) to
  report a free command FIFO, or the window server waited forever.
- **2D acceleration.** The window server draws through
  `ATIRage128GA.plugin`, the only display driver it accepts for this card.
  The plugin loads the command engine's microcode and sends each blit as an
  indirect buffer through the PIO FIFO, which QEMU's command processor
  already ran. Three things in QEMU's Rage 128 2D engine were missing or
  wrong:
  - the "context" registers `DST_PITCH_OFFSET_C`, `DP_GUI_MASTER_CNTL_C`,
    `SC_TOP_LEFT_C` and `SC_BOTTOM_RIGHT_C` (0x1c80 - 0x1c8c) were ignored.
    They now set the same state as the plain registers, and
    `DP_GUI_MASTER_CNTL_C` also sets left-to-right, top-to-bottom drawing,
    since the plugin never writes `DP_CNTL` (an inference from its
    behaviour, not from documentation);
  - blits added the display start (`CRTC_OFFSET`) to every address, but
    Apple's driver gives absolute card addresses;
  - blits ignored the scissor, which is inclusive on the Rage 128.
- **OpenGL.** glcompare (the 10.2 build runs on 10.0) matches 10.0's
  software renderer in 17 of 18 scenes: 15 at 0.0 - 0.1%, texture replace
  0.3%, logic op 0.4%, lines 3.3% (as on the Radeon 7000). The 18th,
  multitexture, 10.0's software renderer draws with the first texture only;
  against 10.2's software renderer it is 0.3% off. What was needed:
  - **A Rage 128 3D engine** (`ati_3d.c`, sharing the Radeon 7000's
    rasteriser). Apple's GL driver sends `3D_RNDR_GEN_PRIM` packets (0x25:
    vertex format, primitive control, vertices in window coordinates; the
    format bits are the ones the R100 kept) in indirect buffers, and state in
    the `_C` registers: `TEX_CNTL_C` (Z, stencil, textures, fog, blending,
    alpha test enables), `MISC_3D_STATE_CNTL_REG` (blend factors, alpha
    test), `Z_STEN_CNTL_C` (the R100's layout), `PM4_VC_FPU_SETUP` (culling,
    flat or Gouraud), `PRIM_`/`SEC_TEX_CNTL_C`, `TEX_SIZE_PITCH_C` and the
    per-level `TEX_n_OFFSET_C` (level of size 2^n), the texture combiners
    `PRIM_`/`SEC_TEXTURE_COMBINE_CNTL_C`, and the 2D engine's destination,
    scissor and ROP3 (logic ops). `SETUP_CNTL`'s `TEXTURE_ST_DIRECT` means
    texture coordinates come multiplied by 1/w. Encodings are from
    xf86-video-r128's `r128_reg.h` and Mesa's r128 driver.
  - **Byte order.** `CONFIG_CNTL`'s aperture swap (`APER_1_ENDIAN`, set by
    the NDRV) now works as on the Radeons: video memory is kept in the CPU's
    order and the engines swap. Before, the 2D and 3D engines wrote pixels
    the CPU read back byte-reversed (red as cyan): 10.0 reads the GL back
    buffer with the CPU.
  - **Texture uploads** are `CNTL_HOSTDATA_BLT` packets with the texels
    inline, in big-endian order (`DP_DATATYPE`'s `HOST_BIG_ENDIAN_EN`, which
    `DP_GUI_MASTER_CNTL` writes now leave alone); host data blits had been
    left out for the Rage 128.
  - **Display sleep stops GL.** The `ATIRage128` kext stops the command
    engine when the display sleeps (Energy Saver) and restarts it at wake;
    GL work sent meanwhile is dropped (`clientMemoryForType` goes to
    `command_noop`), so pictures read back empty. Moving the mouse wakes it.
  10.0 has no Quartz Extreme.
- 10.0's kernel also needs at most 512 MB here (`--ram 512`).

### Mac OS 9.2.1 on the Rage 128 (in progress)

`install images/9.2.1/macos_921_ppc.iso` boots and installs with
`./ppcosx run --hd vms/os9.qcow2 --cdrom "install images/9.2.1/macos_921_ppc.iso"
--boot d --display rage128 --legacy-input --ram 512` (Drive Setup
initialises the disk; untick "Update Apple Hard Disk Drivers" in the
installer's Options). The first install attempt stopped with "Problems
were encountered reading the source file 'Big System Morsels'"; the second
went through. The installed system boots from the disk at 800x600 with
ATI's 9.2.1 extensions (ATI Driver Update, Graphics, 3D, Rage 128 3D,
Video Accelerator, Resource Manager, MPP Manager). What does not work yet:
- **ATI Video Accelerator hangs startup.** The whole system stops (the
  CPU idles in the nanokernel, no register is polled) after the driver
  enables an unnamed `GEN_INT_CNTL` interrupt, bit 10, beside vertical
  blank; force-quitting it, or moving it out of Extensions (it is on the
  desktop of `vms/os9.qcow2`), lets the system run. Raising bit 10 with
  every vertical blank stalls the boot instead.
- **No QuickDraw acceleration.** ATI Graphics Accelerator loads the
  engine's microcode and puts it in direct-register (non-PM4) mode, but
  window moves and scrolling are drawn by the CPU: no 2D engine register
  is written.
- RAVE and OpenGL are untested; the guest's Restart leaves the machine in
  Open Firmware (quit QEMU and start it again).

### Notes for installing 10.0 - 10.2

- **The installer CD panicked the kernel** in `IOService::activityTickle`
  when the disc was the hard disk's slave; it now goes on the second ATA bus
  (`index=2`), as in real Power Macs.
- **The picture was garbled on the Radeon 7000** (Jaguar changes the CRTC
  from 16 to 32 bits per pixel with the CRTC running); a change of depth,
  pitch or resolution now switches the mode.
- **Input**: 10.0 - 10.2 cannot use an absolute pointer, so `ppcosx --guest
  10.0/10.1/10.2` leaves QEMU's tablet out; 10.0 and 10.1 need ADB
  (`--legacy-input`, Cuda), 10.2 uses USB (PMU).
- **Disk**: erase it with Mac OS 9 drivers (10.2's Disk Utility: "Install
  Mac OS 9 Drivers"; 10.0's Drive Setup installs them anyway), or the
  installer stops with "Couldn't prepare volume for installation". 10.2
  installs disc 1, restarts, then boot the hard disk with disc 2 inserted.
- **Setup Assistant**: Cmd-Q on the registration page offers Skip, so
  nothing is sent to Apple.
- **Remote login** for tests: 10.1 and 10.2 append `SSHSERVER=-YES-` to
  `/etc/hostconfig` and run `/System/Library/StartupItems/SSH/SSH start`
  (password logins; Jaguar's OpenSSH 3.4p1 refused our key after a
  restart). 10.0 has no sshd: the Cheetah test VM runs telnetd from inetd on
  port 22 (`ssh stream tcp nowait root /usr/libexec/tcpd telnetd` in
  `/etc/inetd.conf`), so `ppcosx --ssh-port` reaches it with telnet. The
  login shell is tcsh (`2>&1` is a syntax error there).

## Sleep

Sleep is not emulated. Mac OS X's idle sleep (10 minutes by default) sends
the PMU a sleep command that QEMU cannot carry out, and the guest then stops
responding for good, with its clock frozen. `install.sh` turns idle sleep
off (`pmset -a sleep 0`) and `uninstall` restores the old setting; QEMU
prints a warning if a guest tries to sleep anyway. Choosing Sleep from the
Apple menu still hangs the guest.

## Graphics status

| Stage | State |
|---|---|
| Unaccelerated framebuffer (QEMU VGA + `qemu_vga.ndrv`) | works, default display |
| `ppcosx-gpu` PCI device, command transport, host scanout | done, tested from firmware |
| Guest framebuffer driver (`PPCOSXFramebuffer.kext`) | works on 10.4.6 as the primary display (`--gpu-primary`), 640x480 to 1920x1200, unaccelerated; blank until the driver loads; other OS versions untested |
| GL command stream + host renderer | 275 generated fixed-function GL 1.x calls, texture upload (2D, rectangle and cube targets), vertex-array draws and `glReadPixels` (colour, depth and other formats); no shaders or queries. The host's buffers match the application's pixel format (alpha, depth, stencil). Host side replays into Apple's OpenGL 2.1-on-Metal. `-trace ppcosx_gl_call` logs every call the host receives |
| Guest GL library (`libppcosxgl`) + `gldemo` | works on 10.4.6: programs linked against it render on the host GPU (about 650 fps at 1024x768 for the demo cube); frames are copied over the screen, not into a window |
| OpenGL redirector for unmodified apps (`guest/PPCOSXGL`) | on system-wide in the 10.4.6 dev guest. Handles the calls above, single- and double-buffered windows, and the GLU matrix, mipmap and quadric calls (Apple's GLU bypasses the public gl* functions, so the redirector redraws quadrics vertex for vertex as GLU does). Chess stays on the host and is playable (it picks squares with `glReadPixels`). Buffer objects, shaders, selection, NURBS, flat-shaded spheres and offscreen/full-screen drawables hand the context back to Apple's renderer, with the window keeping its last frame. Programs that call the context dispatch table directly (CGLMacro) are not detected |
| Rendering accuracy against Apple's software renderer | Chess and the test programs match to within about 1 level on average (differences on polygon edges only), once `PPCOSXGL_MATCH_SOFTWARE=1` copies one bug of Apple's renderer: it drops specular highlights under `GL_SEPARATE_SPECULAR_COLOR`. Without that switch the host draws them, as the OpenGL spec and real graphics cards do, so Chess's pieces look glossier than on Apple's renderer |
| Direct presentation of redirected windows by the host | works: frames go to a host overlay per window (see below). Gears runs at about 460 fps against 40 on Apple's renderer, the texture test at 515 against 40 |
| Emulated Radeon 7000 with Apple's own drivers | desktop, 2D and OpenGL work (3D engine matches Apple's software renderer on 17 of 18 test scenes); see "Radeon 7000 status" |
| Emulated Radeon 9600 (R300) with Apple's own drivers | desktop, 2D, OpenGL with vertex and fragment programs; 17 of 18 GL test scenes match; see "Core Image: the Radeon 9600" |
| Quartz Extreme | works with either emulated Radeon on the AGP bus; see "Quartz Extreme: the Radeon on the AGP bus" |
| Core Image | works on the emulated Radeon 9600, drawn on the host GPU on macOS: 28 of 28 test filters match Core Image's software renderer, up to 140 times faster than it at 1024x1024 |
| CPUs | G3, G4 and G5, 1 to 4 of them (dual G4s, quad G5s), all with Quartz Extreme and Core Image on the emulated Radeon 9600; see "CPUs: G3, G4, G5 and multiprocessor Macs" |
| Mac OS X versions | Panther 10.3, Tiger 10.4 and Leopard 10.5 with Quartz Extreme (and Core Image on 10.4/10.5) on the emulated Radeon 9600, installers included; `ppcosx` uses it by default; see "Mac OS X versions". Jaguar 10.2 (Quartz Extreme, OpenGL) and Puma 10.1 (OpenGL) on the Radeon 7000; Cheetah 10.0 (2D acceleration, OpenGL) on the Rage 128; see "Mac OS X 10.0 - 10.2" |
| NVRAM | readable and writable by Mac OS X (firmware variables, startup disk) on G4s and G5s, kept across restarts within a run |
| G5 | Panther, Tiger and Leopard boot in about a minute, single or dual, with Quartz Extreme and Core Image; see "Mac OS X versions" |
| USB keyboard | presents an Apple Pro Keyboard, so Leopard skips its Keyboard Setup Assistant |

`tools/run-tiger-accel.sh` starts Tiger in a window on the emulated
Radeon 9600 (`PPCOSX_CARD=r100` for the Radeon 7000), with Quartz Extreme
and Core Image, from its own copy of the dev guest's disk
(`vms/tiger-accel.qcow2`).
`tools/run-tiger-dev.sh` starts the Tiger development guest headless;
`tools/guest-ssh.sh <command>` runs a command in it.
