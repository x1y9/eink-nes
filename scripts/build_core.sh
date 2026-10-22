#!/bin/bash
# 交叉编译 QuickNES 核心为 ARMv7 静态库
#
# 注意: vendor/qnes 是 kode54/QuickNES_Core 的原始快照 (LGPL-2.1)。
# 编译前必须关掉 zlib —— 否则链接会缺 gzopen/gzread/gzclose。
# 原因: QuickNES 的 blargg_config.h 有【两份】(nes_emu/ 和 fex/),
#       各自独立生效, 必须两处都改。交叉编译环境没有 armhf 的 libz.a。
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
CROSS=arm-linux-gnueabihf-
CXX=${CROSS}g++
AR=${CROSS}ar
SRC=$ROOT/vendor/qnes
OBJ=$ROOT/build/obj
mkdir -p "$OBJ"

if [ ! -d "$SRC/nes_emu" ]; then
    echo "缺 vendor/qnes, 请检查仓库是否完整" >&2
    exit 1
fi

# ---- 打补丁: 关闭 zlib (幂等) ----
for cfg in "$SRC/nes_emu/blargg_config.h" "$SRC/fex/blargg_config.h"; do
    if grep -q '^#define HAVE_ZLIB_H' "$cfg"; then
        sed -i 's|^#define HAVE_ZLIB_H|//#define HAVE_ZLIB_H|' "$cfg"
        echo "patch: 关闭 zlib in ${cfg#$ROOT/}"
    fi
done

CFLAGS="-O3 -fno-exceptions -fno-rtti -fomit-frame-pointer -ffast-math -fno-strict-aliasing -w -fpermissive"
CFLAGS="$CFLAGS -I$SRC -I$SRC/nes_emu -I$SRC/fex -I$SRC/libretro"
ARCH="-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb"

SRCS="abstract_file apu_state Blip_Buffer Effects_Buffer Mapper_Fme7 Mapper_Mmc5
Mapper_Namco106 Mapper_Vrc6 misc_mappers Multi_Buffer Nes_Apu Nes_Buffer Nes_Cart
Nes_Core Nes_Cpu nes_data Nes_Effects_Buffer Nes_Emu Nes_File Nes_Film Nes_Film_Data
Nes_Film_Packer Nes_Fme7_Apu Nes_Mapper nes_mappers Nes_Mmc1 Nes_Mmc3 Nes_Namco_Apu
Nes_Oscs Nes_Ppu Nes_Ppu_Impl Nes_Ppu_Rendering Nes_Recorder Nes_State nes_util
Nes_Vrc6_Apu"

total=39
i=0
echo "编译 $total 个核心文件..."
for s in $SRCS; do
  i=$((i+1))
  printf "\r[%d/%d] %s          " $i $total "$s"
  $CXX $ARCH $CFLAGS -c "$SRC/nes_emu/$s.cpp" -o "$OBJ/$s.o" 2>/dev/null
done
for f in Data_Reader blargg_errors blargg_common; do
  i=$((i+1))
  printf "\r[%d/%d] fex/%s          " $i $total "$f"
  $CXX $ARCH $CFLAGS -c "$SRC/fex/$f.cpp" -o "$OBJ/$f.o" 2>/dev/null
done
echo ""
$AR rcs "$ROOT/build/libqnes.a" "$OBJ"/*.o
echo "OK: build/libqnes.a"
ls -lh "$ROOT/build/libqnes.a"
