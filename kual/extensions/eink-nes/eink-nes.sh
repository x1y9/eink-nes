#!/bin/sh
# eink-nes KUAL launcher
#
# 目录布局 (全部收在 extensions/eink-nes/ 下, 不污染 /mnt/us):
#   /mnt/us/extensions/eink-nes/eink-nes        可执行文件
#   /mnt/us/extensions/eink-nes/eink-nes.log    日志
#   /mnt/us/extensions/eink-nes/roms/*.nes      ROM
#
# 重要: /mnt/us 是 FAT32, 不支持 unix 执行位。chmod +x 会静默失败,
# 直接 exec 会返回 126。所以所有启动都走 `sh <bin>` 或者把 bin 拷到
# /tmp (tmpfs, 支持执行位) 再跑。

export LC_ALL=C
export LANG=C

BASE="/mnt/us/extensions/eink-nes"
LOGFILE="$BASE/eink-nes.log"
ROMDIR="$BASE/roms"

# ---- 日志: 用 exec 把整个脚本的 stdout/stderr 重定向进日志 ----
# 先确保目录存在 (FAT 上 mkdir -p 可用)
mkdir -p "$BASE" "$ROMDIR" 2>/dev/null
: > "$LOGFILE" 2>/dev/null || LOGFILE="/tmp/eink-nes.log"
exec >> "$LOGFILE" 2>&1

log() { echo "[$(date '+%H:%M:%S')] $*"; }

log "=============================================="
log "eink-nes launcher start"
log "BASE=$BASE"
log "uname: $(uname -a 2>/dev/null)"

# ---- 找可执行文件 ----
BIN=""
for c in "$BASE/eink-nes" "$BASE/bin/eink-nes"; do
    [ -f "$c" ] && BIN="$c" && break
done
if [ -z "$BIN" ]; then
    log "FATAL: 找不到 eink-nes 可执行文件"
    log "目录内容:"
    ls -la "$BASE" >> "$LOGFILE" 2>&1
    /usr/bin/eips -f -c "eink-nes: binary not found in $BASE" 2>/dev/null
    sleep 5
    exit 1
fi
log "BIN=$BIN"
log "file info: $(ls -l "$BIN")"

# ---- 关键: FAT 无执行位, 拷到 tmpfs 再跑 ----
RUNBIN="/tmp/eink-nes.bin"
rm -f "$RUNBIN"
if cp "$BIN" "$RUNBIN" 2>>"$LOGFILE"; then
    chmod 755 "$RUNBIN"
    log "已拷贝到 $RUNBIN ($(wc -c < "$RUNBIN") bytes)"
else
    log "WARN: 拷贝失败, 尝试直接 sh 执行"
    RUNBIN="$BIN"
fi

# ---- 参数 ----
# KUAL 不传位置参数, 但手动跑时可以带: sh eink-nes.sh --probe
# 以及第一个非选项参数当作 ROM 路径
ROM="$EINK_NES_ROM"
PASSTHRU=""
for arg in "$@"; do
    case "$arg" in
        -*) PASSTHRU="$PASSTHRU $arg" ;;
        *)  [ -f "$arg" ] && ROM="$arg" || PASSTHRU="$PASSTHRU $arg" ;;
    esac
done

if [ -z "$ROM" ] || [ ! -f "$ROM" ]; then
    ROM=$(ls "$ROMDIR"/*.nes 2>/dev/null | head -1)
fi
log "ROM=$ROM"
log "PASSTHRU=$PASSTHRU"

# ---- 显示启动横幅, 让用户知道程序接管了屏幕 ----
/usr/bin/eips -f -c "eink-nes starting...\n\nlog: $LOGFILE" 2>/dev/null

# ---- 停 UI 释放 framebuffer ----
# PW3 的 UI 进程是 cvm (Kindle 阅读器), framework 是它的 init 脚本。
# 只停 framework 不够, cvm 可能残留; 两者都处理。
stop_ui() {
    log "stopping framework"
    if [ -x /etc/init.d/framework ]; then
        /etc/init.d/framework stop >> "$LOGFILE" 2>&1
    fi
    # 兜底: 直接杀 cvm / reader
    killall cvm 2>/dev/null && log "killed cvm"
    killall -q pillow 2>/dev/null
    sleep 1
}

restore_ui() {
    log "restoring framework"
    /usr/sbin/lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null
    if [ -x /etc/init.d/framework ]; then
        /etc/init.d/framework start >> "$LOGFILE" 2>&1
    fi
    rm -f /tmp/eink-nes.bin
    log "launcher exit"
    log "=============================================="
}

trap restore_ui EXIT INT TERM

# 阻止休眠
/usr/sbin/lipc-set-prop com.lab126.powerd preventScreenSaver 1 2>/dev/null

stop_ui

# ---- 检查 framebuffer ----
if [ ! -e /dev/fb0 ]; then
    log "FATAL: /dev/fb0 不存在"
    /usr/bin/eips -f -c "no /dev/fb0" 2>/dev/null
    sleep 5
    exit 1
fi
log "fb0: $(ls -l /dev/fb0)"

# ---- 检查程序能否跑起来 ----
# 注意: 不要用 `prog | while read`, 那样 $? 是管道尾部的返回值, 拿不到程序退出码。
# 先写临时文件再读, 保留真实 rc。
if [ -z "$PASSTHRU" ]; then
    log "--- self test (--dry-run) ---"
    "$RUNBIN" --dry-run -r "$ROMDIR" -n 60 > /tmp/eink-nes.selftest 2>&1
    RC=$?
    log "self test rc=$RC"
    while IFS= read -r line; do log "  $line"; done < /tmp/eink-nes.selftest
    rm -f /tmp/eink-nes.selftest
    if [ "$RC" != "0" ]; then
        log "WARN: 自检失败 (rc=$RC), 仍尝试正式运行"
    fi
fi

# ---- 正式运行 ----
if [ -n "$ROM" ] && [ -f "$ROM" ]; then
    log "--- run: $RUNBIN -r $ROMDIR $PASSTHRU $ROM ---"
    # shellcheck disable=SC2086
    "$RUNBIN" -r "$ROMDIR" $PASSTHRU "$ROM"
    log "eink-nes exit code: $?"
elif [ -n "$PASSTHRU" ]; then
    # 诊断模式 (如 --probe) 不需要 ROM
    log "--- run: $RUNBIN $PASSTHRU ---"
    # shellcheck disable=SC2086
    "$RUNBIN" $PASSTHRU
    log "eink-nes exit code: $?"
else
    log "FATAL: $ROMDIR 下没有 .nes 文件"
    log "把 ROM 放到 $ROMDIR/ 后重试"
    /usr/bin/eips -f -c "No .nes in roms/" 2>/dev/null
    sleep 5
fi

# trap 恢复 UI
