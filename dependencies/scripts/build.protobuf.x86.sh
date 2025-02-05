#!/bin/sh
#

export ABI=x86
export TOOLCHAIN=$toolchain/$platform.$ABI
export INSTALL_PREFIX=$dependencies/build/$ABI
export SYSROOT=$TOOLCHAIN/sysroot
export PATH=$TOOLCHAIN/bin:$PATH
export CC="i686-linux-android-clang --sysroot $SYSROOT"
export CXX="i686-linux-android-clang++ --sysroot $SYSROOT"
export RANLIB=$TOOLCHAIN/bin/i686-linux-android-ranlib

d=$(dirname $0)
cd $d

[ ! -e configure ] && ./autogen.sh

make clean
make distclean

./configure \
    --host=i686-linux-android \
    --with-protoc=protoc \
    --with-sysroot="$SYSROOT" \
    --disable-shared \
    --prefix="$INSTALL_PREFIX" \
    --enable-cross-compile \
    CFLAGS="-fPIC -march=i686 -D__ANDROID_API__=$API_LEVEL" \
    CXXFLAGS="-fPIC -frtti -fexceptions -march=i686 -D__ANDROID_API__=$API_LEVEL" \
    LIBS="-llog -lz -lc++_static"

make 
make install
