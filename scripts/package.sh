#!/bin/bash
# 打包成可直接解压到 /mnt/us 的 tar.gz
#
# 目录布局 (自包含, 不污染 /mnt/us 根目录):
#   extensions/eink-nes/
#     ├── eink-nes          可执行文件
#     ├── eink-nes.sh       KUAL 启动脚本
#     ├── showlog.sh        日志查看
#     ├── menu.json         KUAL 菜单
#     ├── README.txt
#     └── roms/test.nes     自带测试 ROM
set -e
cd "$(dirname "$0")/.."

if [ ! -f build/eink-nes ]; then
    echo "先跑 build_app.sh" >&2
    exit 1
fi

rm -rf dist
EXT=dist/extensions/eink-nes
mkdir -p "$EXT/roms"

cp build/eink-nes "$EXT/eink-nes"
# KUAL 靠 config.xml 发现扩展, menu.json 是 config.xml 指向的菜单数据。
# 两个都必须有; 少了 config.xml, 扩展根本不会出现在 KUAL 列表里。
cp kual/extensions/eink-nes/config.xml "$EXT/config.xml"
cp kual/extensions/eink-nes/menu.json "$EXT/menu.json"
cp kual/extensions/eink-nes/*.sh "$EXT/"
chmod +x "$EXT/eink-nes" "$EXT"/*.sh

# 生成 README 到扩展目录内部
cat > "$EXT/README.txt" << 'EOF'
eink-nes - Kindle Paperwhite 3 NES 模拟器
=========================================

安装
----
把 extensions/eink-nes/ 整个目录拷到 Kindle 的 /mnt/us/extensions/ 下。
(直接解压 tar.gz 到 /mnt/us 即可, 目录结构已经对好)

ROM 放哪里
----------
/mnt/us/extensions/eink-nes/roms/*.nes

所有文件都在 extensions/eink-nes/ 内部, 不在 /mnt/us 根目录留任何东西。
卸载 = 删掉这一个目录。

运行
----
KUAL -> eink-nes -> 启动 NES 模拟器

程序会停掉 framework 接管屏幕。退出后自动恢复 UI。

日志
----
/mnt/us/extensions/eink-nes/eink-nes.log
KUAL 里也有 "查看日志" 菜单项。

出问题怎么办
------------
先 SSH 手动跑, 比 KUAL 更容易看到错误:

  cd /mnt/us/extensions/eink-nes
  sh eink-nes.sh

如果屏幕闪一下就静止, 跑探测模式拿到真实 ioctl 号:

  /tmp/eink-nes.bin --probe

然后把 eink-nes.log 整个发出来。
EOF

# 自带测试 ROM (脚本生成, 无版权内容)
python3 scripts/make_test_rom.py "$EXT/roms/test.nes"

# 打包前先校验扩展完整性。KUAL 少了 config.xml 就不会显示扩展,
# 这种问题本地不卡住就只能到真机上抓瞎。
python3 scripts/check_kual.py "$EXT" || exit 1

tar czf eink-nes-pw3.tar.gz -C dist .
echo "OK: eink-nes-pw3.tar.gz ($(du -h eink-nes-pw3.tar.gz | cut -f1))"
echo "--- 包内文件 ---"
tar tzf eink-nes-pw3.tar.gz
