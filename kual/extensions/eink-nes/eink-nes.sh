#!/bin/sh
# eink-nes KUAL launcher
#
# 目录布局 (全部收在 extensions/eink-nes/ 下, 不污染 /mnt/us):
#   /mnt/us/extensions/eink-nes/eink-nes        可执行文件
#   /mnt/us/extensions/eink-nes/eink-nes.log    日志
#   /mnt/us/extensions/eink-nes/roms/*.nes      ROM
#
# 重要: /mnt/us 是 FAT32, 不支持 unix 执行位。chmod +x 会静默失败,
# 直接 exec 会返回 126。所以二进制先拷到 /tmp (tmpfs) 再加执行位运行。

export LC_ALL=C
export LANG=C
export PATH="/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"

BASE="/mnt/us/extensions/eink-nes"
ROMDIR="$BASE/roms"

# ============================================================
# 第一步: 建立日志。日志本身可能建不出来 (FAT 只读 / 权限),
# 所以三个位置依次尝试, 并且把"最终用了哪个"记到 /tmp 里的
# 面包屑文件 —— 便于 SSH 排查。
# ============================================================
BREADCRUMB="/tmp/eink-nes.launcher-ran"

# 面包屑: 用最简单的写法, 不依赖任何变量展开和重定向到 FAT。
# 只要脚本被执行, 这个文件就会出现 (tmpfs 一定可写)。
{
    echo "launcher_start=$(date)"
    echo "pid=$$"
    echo "base_exists=$([ -d "$BASE" ] && echo yes || echo no)"
    echo "base_writable=$([ -w "$BASE" ] && echo yes || echo no)"
    echo "romdir_exists=$([ -d "$ROMDIR" ] && echo yes || echo no)"
    echo "bin_exists=$([ -f "$BASE/eink-nes" ] && echo yes || echo no)"
    echo "args=$*"
    echo "uname=$(uname -a)"
} > "$BREADCRUMB" 2>&1

# 日志: 优先写扩展目录, 失败退到 /tmp
LOGFILE="$BASE/eink-nes.log"
if ! : >> "$LOGFILE" 2>/dev/null; then
    LOGFILE="/tmp/eink-nes.log"
    echo "log_fallback=/tmp/eink-nes.log (cannot write $BASE/eink-nes.log)" >> "$BREADCRUMB"
fi
exec >> "$LOGFILE" 2>&1

log() { echo "[$(date '+%H:%M:%S')] $*"; }

log "=============================================="
log "eink-nes launcher start"
log "BASE=$BASE"
log "LOGFILE=$LOGFILE"
log "ARGS=$*"
log "uname: $(uname -a 2>/dev/null)"
log "pwd: $(pwd 2>/dev/null)"
log "PATH=$PATH"

# 记录扩展目录的真实内容, 确认解压是否完整
log "--- ls $BASE ---"
ls -la "$BASE" 2>&1
log "--- ls $ROMDIR ---"
ls -la "$ROMDIR" 2>&1

# ============================================================
# 第二步: 找可执行文件
# ============================================================
BIN=""
for c in "$BASE/eink-nes" "$BASE/bin/eink-nes"; do
    if [ -f "$c" ]; then BIN="$c"; break; fi
done
if [ -z "$BIN" ]; then
    log "FATAL: 找不到 eink-nes 可执行文件"
    /usr/bin/eips -f -c "eink-nes: binary not found\nin $BASE" 2>/dev/null
    sleep 6
    exit 1
fi
log "BIN=$BIN"

# 记录文件的真实类型 (用 od 读前 4 字节, Kindle 上可能没有 file 命令)
log "magic: $(od -An -tx1 -N4 "$BIN" 2>/dev/null | tr -d ' \n')"
log "size: $(wc -c < "$BIN" 2>/dev/null) bytes"

# ============================================================
# 第三步: 拷到 tmpfs 再加执行位 (FAT32 无执行位)
# ============================================================
RUNBIN="/tmp/eink-nes.bin"
rm -f "$RUNBIN" 2>/dev/null
if cp "$BIN" "$RUNBIN" 2>>"$LOGFILE"; then
    chmod 755 "$RUNBIN"
    log "已拷贝到 $RUNBIN ($(wc -c < "$RUNBIN" 2>/dev/null) bytes)"
    # 校验大小一致, 确认 cp 没截断
    S1=$(wc -c < "$BIN" 2>/dev/null)
    S2=$(wc -c < "$RUNBIN" 2>/dev/null)
    log "size check: src=$S1 dst=$S2"
    if [ "$S1" != "$S2" ]; then
        log "FATAL: 拷贝大小不一致, 可能磁盘空间不足"
        /usr/bin/eips -f -c "copy truncated\nsrc=$S1 dst=$S2" 2>/dev/null
        sleep 6
        exit 1
    fi
else
    log "WARN: 拷贝失败, 尝试直接执行 $BIN"
    RUNBIN="$BIN"
fi

# ============================================================
# 第四步: 参数 / ROM
# ============================================================
ROM="$EINK_NES_ROM"
PASSTHRU=""
for arg in "$@"; do
    case "$arg" in
        -*) PASSTHRU="$PASSTHRU $arg" ;;
        *)  if [ -f "$arg" ]; then ROM="$arg"; else PASSTHRU="$PASSTHRU $arg"; fi ;;
    esac
done

if [ -z "$ROM" ] || [ ! -f "$ROM" ]; then
    ROM=$(ls "$ROMDIR"/*.nes 2>/dev/null | head -1)
fi
log "ROM=$ROM"
log "PASSTHRU=$PASSTHRU"

# ============================================================
# 第五步: 停 UI 释放 framebuffer
# ============================================================
stop_ui() {
    log "stopping framework"
    if [ -x /etc/init.d/framework ]; then
        /etc/init.d/framework stop >> "$LOGFILE" 2>&1
    else
        log "  /etc/init.d/framework 不存在"
    fi
    killall cvm 2>/dev/null && log "  killed cvm"
    killall -q pillow 2>/dev/null
    sleep 1
}

restore_ui() {
    log "restoring framework"
    /usr/sbin/lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null
    if [ -x /etc/init.d/framework ]; then
        /etc/init.d/framework start >> "$LOGFILE" 2>&1
    fi
    rm -f /tmp/eink-nes.bin 2>/dev/null
    log "launcher exit rc=$?"
    log "=============================================="
}

trap restore_ui EXIT
trap 'log "caught INT/TERM"; exit 1' INT TERM

/usr/sbin/lipc-set-prop com.lab126.powerd preventScreenSaver 1 2>/dev/null

# 先显示横幅, 证明脚本跑到了这里 (屏幕上能看到)
/usr/bin/eips -f -c "eink-nes\nlauncher running\nlog: $LOGFILE" 2>/dev/null
log "eips banner shown"

stop_ui

if [ ! -e /dev/fb0 ]; then
    log "FATAL: /dev/fb0 不存在"
    /usr/bin/eips -f -c "no /dev/fb0" 2>/dev/null
    sleep 6
    exit 1
fi
log "fb0: $(ls -l /dev/fb0 2>&1)"

# ============================================================
# 第六步: 正式运行
# ============================================================
if [ -n "$PASSTHRU" ]; then
    log "--- run: $RUNBIN $PASSTHRU ---"
    "$RUNBIN" $PASSTHRU
    log "eink-nes exit code: $?"
elif [ -n "$ROM" ] && [ -f "$ROM" ]; then
    log "--- run: $RUNBIN -r $ROMDIR $ROM ---"
    "$RUNBIN" -r "$ROMDIR" "$ROM"
    log "eink-nes exit code: $?"
else
    log "FATAL: $ROMDIR 下没有 .nes 文件"
    /usr/bin/eips -f -c "No .nes found in\n$ROMDIR" 2>/dev/null
    sleep 6
fi

# trap restore_ui 会做收尾
