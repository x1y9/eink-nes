#!/bin/sh
# eink-nes KUAL 启动脚本
# 放在 extensions/eink-nes/ 下, 由 KUAL 调用

export LC_ALL=C
export LANG=C

# Kindle 上可执行文件和 ROM 的位置
BASE="/mnt/us/extensions/eink-nes"
BIN="$BASE/eink-nes"
ROMDIR="/mnt/us/roms"
LOGFILE="/mnt/us/eink-nes.log"

# 确保日志可写
: > "$LOGFILE" 2>/dev/null || LOGFILE="/tmp/eink-nes.log"

echo "=== eink-nes start $(date) ===" >> "$LOGFILE"

# 停掉 Kindle 的 framework, 释放 framebuffer (否则画面会被 UI 抢回)
# 用 lipc 通知比较友好; 直接 kill 更稳
restore_ui() {
    echo "restoring ui..." >> "$LOGFILE"
    /usr/sbin/lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null
    # 重新拉起 framework
    if [ -e /etc/init.d/framework ]; then
        /etc/init.d/framework start >> "$LOGFILE" 2>&1
    fi
}

trap restore_ui EXIT INT TERM

# 阻止休眠
/usr/sbin/lipc-set-prop com.lab126.powerd preventScreenSaver 1 2>/dev/null

# 停 framework
if [ -e /etc/init.d/framework ]; then
    /etc/init.d/framework stop >> "$LOGFILE" 2>&1
    sleep 2
fi

chmod +x "$BIN" 2>/dev/null

# ROM: 优先用命令行传的第一个参数, 否则扫描 $ROMDIR
ROM="$1"
if [ -z "$ROM" ]; then
    # 找第一个 .nes
    ROM=$(find "$ROMDIR" -maxdepth 1 -iname "*.nes" 2>/dev/null | head -1)
fi

echo "rom=$ROM" >> "$LOGFILE"

if [ -n "$ROM" ] && [ -f "$ROM" ]; then
    "$BIN" -r "$ROMDIR" "$ROM" >> "$LOGFILE" 2>&1
else
    # 没 ROM: 让程序自己扫描并报告
    "$BIN" -r "$ROMDIR" >> "$LOGFILE" 2>&1
    # 把提示显示在屏幕上 (用 eips, Kindle 自带的文字工具)
    /usr/bin/eips -f -c "No .nes found in $ROMDIR" 2>/dev/null
    sleep 3
fi

echo "=== eink-nes exit $? ===" >> "$LOGFILE"
# trap 会恢复 UI
