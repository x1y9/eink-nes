#!/bin/sh
# 在 Kindle 屏幕上显示运行日志 (用 eips 文字模式)
LOG="/mnt/us/eink-nes.log"
[ -f "$LOG" ] || LOG="/tmp/eink-nes.log"
if [ -f "$LOG" ]; then
    /usr/bin/eips -f -c "$(tail -20 "$LOG" | tr '\n' ' ')" 2>/dev/null
else
    /usr/bin/eips -f -c "no log found" 2>/dev/null
fi
sleep 5
