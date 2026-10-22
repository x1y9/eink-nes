#!/bin/sh
# 极简自检: 只做一件事 —— 证明 KUAL 能执行本目录下的脚本。
# 不做任何可能失败的操作 (不停 framework, 不碰 framebuffer, 不拷二进制)。
#
# 结果写在两个地方:
#   /tmp/eink-nes.selftest       (tmpfs, 一定能写)
#   /mnt/us/extensions/eink-nes/selftest.txt  (可写就写)
# 屏幕上也会用 eips 显示结果。

BASE="/mnt/us/extensions/eink-nes"
OUT="/tmp/eink-nes.selftest"

{
    echo "=== eink-nes selftest ==="
    echo "time: $(date)"
    echo "pid: $$"
    echo "args: $*"
    echo "pwd: $(pwd)"
    echo "uname: $(uname -a)"
    echo ""
    echo "--- 环境 ---"
    echo "SHELL=$SHELL"
    echo "PATH=$PATH"
    echo "USER=$USER"
    echo "id: $(id 2>&1)"
    echo ""
    echo "--- 扩展目录 ---"
    ls -la "$BASE" 2>&1
    echo ""
    echo "--- roms 目录 ---"
    ls -la "$BASE/roms" 2>&1
    echo ""
    echo "--- 二进制 ---"
    if [ -f "$BASE/eink-nes" ]; then
        echo "存在, $(wc -c < "$BASE/eink-nes") bytes"
        echo "magic: $(od -An -tx1 -N4 "$BASE/eink-nes" 2>/dev/null | tr -d ' \n')"
    else
        echo "缺失!"
    fi
    echo ""
    echo "--- config.xml ---"
    cat "$BASE/config.xml" 2>&1
} > "$OUT" 2>&1

# 尽力也写一份到扩展目录 (只读挂载时会失败, 无所谓)
cp "$OUT" "$BASE/selftest.txt" 2>/dev/null || true

# 屏幕上显示 (eips 单行, 用 | 分隔)
SUMMARY=$(head -12 "$OUT" 2>/dev/null | tr '\n' '|')
/usr/bin/eips -f -c "$SUMMARY" 2>/dev/null

sleep 10
