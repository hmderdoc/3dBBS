#!/bin/bash
# Cross-compile libssh2 (mbedtls crypto backend) for 3DS into
# vendor/libssh2/{include,lib}. The app Makefile enables SSH automatically
# when vendor/libssh2/lib/libssh2.a exists.
#
# Prereq (one-time, root):  dkp-pacman -S 3ds-mbedtls
set -e

VER=1.11.1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="$ROOT/vendor/libssh2"
WORK="${TMPDIR:-/tmp}/libssh2-3ds-build"

export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITARM=${DEVKITARM:-$DEVKITPRO/devkitARM}
export PATH="$DEVKITARM/bin:$PATH"
PORTLIBS="$DEVKITPRO/portlibs/3ds"

if [ ! -f "$PORTLIBS/lib/libmbedcrypto.a" ]; then
    echo "3ds-mbedtls is not installed. Run:"
    echo "  sudo $DEVKITPRO/pacman/bin/pacman -S --noconfirm 3ds-mbedtls"
    exit 1
fi

mkdir -p "$WORK"
cd "$WORK"
if [ ! -d "libssh2-$VER" ]; then
    curl -sL "https://github.com/libssh2/libssh2/releases/download/libssh2-$VER/libssh2-$VER.tar.gz" | tar xz
fi
cd "libssh2-$VER"

ARCH="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft"
export CC=arm-none-eabi-gcc
export AR=arm-none-eabi-ar
export RANLIB=arm-none-eabi-ranlib
export CFLAGS="$ARCH -O2 -ffunction-sections -fdata-sections -D__3DS__ \
  -I$PORTLIBS/include -I$DEVKITPRO/libctru/include"
export CPPFLAGS="$CFLAGS"
export LDFLAGS="$ARCH -L$PORTLIBS/lib -L$DEVKITPRO/libctru/lib"
export LIBS="-lmbedtls -lmbedx509 -lmbedcrypto -lctru"

./configure --host=arm-none-eabi --prefix="$PREFIX" \
    --disable-shared --enable-static \
    --with-crypto=mbedtls --without-libz \
    --disable-examples-build --disable-docs

make -C src -j8
make -C src install
make install-data-am 2>/dev/null || {
    # headers: install manually if the top-level target isn't available
    mkdir -p "$PREFIX/include"
    cp include/libssh2*.h "$PREFIX/include/"
}
echo "libssh2 $VER installed into $PREFIX"
