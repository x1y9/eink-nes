#!/bin/sh
# 在 Kindle 屏幕上显示运行日志 (用 eips 文字模式)
BASE="/mnt/us/extensions/eink-nes"
LOG="$BASE/eink-nes.log"
[ -f "$LOG" ] || LOG="/tmp/eink-nes.log"

if [ -f "$LOG" ]; then
    # 只取最后 15 行, 去掉换行 (eips -c 单次调用不适合长文本)
    TXT=$(tail -15 "$LOG" | tr '\n' '|')
    /usr/bin/eips -f -c "$TXT" 2>/dev/null
else
    /usr/bin/eips -f -c "no log at $LOG" 2>/dev/null
fi

# 同时把完整日志拷到根目录方便 USB 取出? 不, 保持目录干净。
# 取日志请用 USB 访问 /mnt/us/extensions/eink-nes/eink-nes.log 或 scp。

sleep 8
