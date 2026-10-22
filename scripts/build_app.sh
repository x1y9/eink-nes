#!/bin/bash
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
CROSS=arm-linux-gnueabihf-
CXX=${CROSS}g++
SRC=$ROOT/vendor/qnes
OBJ=$ROOT/build/obj

ARCH="-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb"
CFLAGS="-O2 -fno-exceptions -fno-rtti -fomit-frame-pointer -w -I$SRC -I$SRC/nes_emu -I$SRC/fex"

echo "编译 main.cpp ..."
$CXX $ARCH $CFLAGS -c src/main.cpp -o build/main.o

echo "编译 eink_refresh.c ..."
${CROSS}gcc $ARCH -O2 -w -Isrc -c src/eink_refresh.c -o build/eink_refresh.o

echo "链接 ..."
# 静态链接, 兼容 Kindle 老 glibc (2.12)
$CXX $ARCH -static -o build/eink-nes build/main.o build/eink_refresh.o build/libqnes.a -lm
echo "OK: build/eink-nes"
file build/eink-nes
ls -lh build/eink-nes
