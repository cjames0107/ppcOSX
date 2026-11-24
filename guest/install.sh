#!/bin/sh
# Install the ppcosx guest components on Mac OS X 10.4 (PowerPC), and
# adjust the guest for the emulator. Non-interactive, so the emulator can
# run it for the user later.
#
#   sudo sh install.sh            install, and turn on for every user
#   sudo sh install.sh source     the same, building from source instead
#   sudo sh install.sh disable    leave installed but switch the OpenGL
#                                 redirector off system-wide (new programs)
#   sudo sh install.sh enable     switch it back on
#   sudo sh install.sh uninstall  remove the driver and turn the redirector
#                                 off for every user
#   sudo sh install.sh purge      after a restart: delete the redirector file
#
# Installs:
#   /System/Library/Extensions/PPCOSXFramebuffer.kext   display + GL channel
#   /usr/local/lib/libPPCOSXGL.dylib                    OpenGL redirector
#
# These come from prebuilt/<Mac OS X version>/ when that exists (made by
# tools/build-guest-binaries.sh on the host), so no compiler is needed.
# Otherwise, or with "source", they are built here, which needs Xcode Tools.
#
# System-wide launch: each user's ~/.MacOSX/environment.plist (and the
# template for future users) gets DYLD_INSERT_LIBRARIES pointing at the
# redirector, which loginwindow applies to everything started in that
# user's session from their next login. A user who already inserts some
# other library is left alone.
#
# Idle system sleep is turned off: the emulator does not emulate sleep, and
# a guest that goes to sleep stops responding. The previous setting is
# saved and put back by uninstall. Display sleep is left alone.
#
# A restart is needed after installing: for the display driver to load and
# for sessions to pick up the redirector.
#
# The redirector file must stay in place while any session still has the
# setting, or programs in that session will fail to start. That is why
# uninstall and purge are separate steps.
set -e
here=$(cd "$(dirname "$0")" && pwd)

KEXT=/System/Library/Extensions/PPCOSXFramebuffer.kext
LIB=/usr/local/lib/libPPCOSXGL.dylib
KEY=DYLD_INSERT_LIBRARIES
MARKER=/Library/Preferences/org.ppcosx.gl.disabled
SLEEP_SAVED=/Library/Preferences/org.ppcosx.sleep-saved
TEMPLATE="/System/Library/User Template/English.lproj"

if [ "$(id -u)" != 0 ]; then
    echo "install.sh: must run as root (use sudo)" >&2
    exit 1
fi

# Every home directory the setting applies to, one per line.
homes() {
    for home in /Users/*; do
        [ -d "$home/Library" ] && echo "$home"
    done
    echo "$TEMPLATE"
}

current() {
    defaults read "$1/.MacOSX/environment" "$KEY" 2>/dev/null || true
}

turn_on() {
    homes | while read -r home; do
        value=$(current "$home")
        if [ -n "$value" ] && [ "$value" != "$LIB" ]; then
            echo "left alone: $home already sets $KEY"
            continue
        fi
        mkdir -p "$home/.MacOSX"
        defaults write "$home/.MacOSX/environment" "$KEY" -string "$LIB"
        # Keep the files owned like the home directory they live in.
        chown -R "$(ls -ld "$home" | awk '{print $3 ":" $4}')" "$home/.MacOSX"
        chmod 755 "$home/.MacOSX"
        chmod 644 "$home/.MacOSX/environment.plist"
    done
}

turn_off() {
    homes | while read -r home; do
        if [ "$(current "$home")" = "$LIB" ]; then
            defaults delete "$home/.MacOSX/environment" "$KEY"
        fi
    done
}

# Turn idle system sleep off, remembering the old setting the first time.
no_idle_sleep() {
    if [ ! -f "$SLEEP_SAVED" ]; then
        pmset -g | awk '$1 == "sleep" { print $2; exit }' > "$SLEEP_SAVED"
    fi
    pmset -a sleep 0 >/dev/null 2>&1
}

restore_idle_sleep() {
    if [ -f "$SLEEP_SAVED" ]; then
        value=$(cat "$SLEEP_SAVED")
        if [ -n "$value" ]; then
            pmset -a sleep "$value" >/dev/null 2>&1
        fi
        rm -f "$SLEEP_SAVED"
    fi
}

# Put the kext and the redirector in place from directory $1.
install_files() {
    rm -rf "$KEXT"
    cp -R "$1/PPCOSXFramebuffer.kext" "$KEXT"
    chown -R root:wheel "$KEXT"
    chmod -R go-w "$KEXT"
    touch /System/Library/Extensions
    mkdir -p "$(dirname "$LIB")"
    cp "$1/libPPCOSXGL.dylib" "$LIB"
    chown root:wheel "$LIB"
    chmod 755 "$LIB"
}

build_from_source() {
    if ! command -v gcc >/dev/null 2>&1; then
        echo "install.sh: no prebuilt binaries for Mac OS X $version and no" \
             "gcc to build them; install Xcode Tools first" >&2
        exit 1
    fi
    make -C "$here/PPCOSXFramebuffer"
    make -C "$here/libppcosxgl"
    make -C "$here/PPCOSXGL"
    staged=$(mktemp -d /tmp/ppcosx.XXXXXX)
    cp -R "$here/PPCOSXFramebuffer/build/PPCOSXFramebuffer.kext" "$staged/"
    cp "$here/PPCOSXGL/build/libPPCOSXGL.dylib" "$staged/"
    install_files "$staged"
    rm -rf "$staged"
    # Building as root must not leave the source tree unwritable for the
    # user who ran sudo.
    if [ -n "$SUDO_UID" ]; then
        chown -R "$SUDO_UID:$SUDO_GID" "$here/PPCOSXFramebuffer/build" \
            "$here/libppcosxgl/build" "$here/PPCOSXGL/build"
    fi
}

version=$(sw_vers -productVersion | cut -d. -f1-2)
prebuilt="$here/prebuilt/$version"

case "$1" in
disable)
    touch "$MARKER"
    echo "OpenGL redirector off for programs started from now on."
    ;;
enable)
    rm -f "$MARKER"
    echo "OpenGL redirector on for programs started from now on."
    ;;
uninstall)
    turn_off
    restore_idle_sleep
    rm -rf "$KEXT"
    touch /System/Library/Extensions
    touch "$MARKER"
    echo "Removed the display driver and switched the redirector off."
    echo "Restart, then run: sudo sh install.sh purge"
    ;;
purge)
    homes | while read -r home; do
        if [ "$(current "$home")" = "$LIB" ]; then
            echo "install.sh: $home still uses the redirector;" \
                 "run uninstall first" >&2
            exit 1
        fi
    done
    rm -f "$LIB" "$MARKER"
    echo "ppcosx guest components fully removed."
    ;;
"" | source)
    if [ "$1" != source ] && [ -d "$prebuilt" ]; then
        install_files "$prebuilt"
        echo "installed prebuilt binaries: $(head -1 "$prebuilt/BUILDINFO")"
    else
        build_from_source
    fi
    rm -f "$MARKER"
    turn_on
    no_idle_sleep
    echo "ppcosx guest components installed; restart to finish."
    ;;
*)
    echo "usage: sudo sh install.sh [source|disable|enable|uninstall|purge]" >&2
    exit 1
    ;;
esac
