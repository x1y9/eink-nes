#!/bin/bash
# 打包成可直接解压到 /mnt/us 的 tar.gz
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)

if [ ! -f build/eink-nes ]; then
    echo "先跑 build_app.sh" >&2
    exit 1
fi

rm -rf dist
mkdir -p dist/extensions/eink-nes dist/roms

cp build/eink-nes dist/eink-nes
cp kual/extensions/eink-nes/*.sh kual/extensions/eink-nes/menu.json dist/extensions/eink-nes/
chmod +x dist/eink-nes dist/extensions/eink-nes/*.sh
cp README.txt dist/README.txt 2>/dev/null || true

# 自带测试 ROM (脚本生成, 无版权内容)
python3 scripts/make_test_rom.py dist/roms/test.nes

tar czf eink-nes-pw3.tar.gz -C dist .
echo "OK: eink-nes-pw3.tar.gz ($(du -h eink-nes-pw3.tar.gz | cut -f1))"
