#!/usr/bin/env python3
"""生成 NES 测试 ROM (NROM/mapper 0)，验证 eink-nes 渲染。
画面: 16 条横向灰度带 (每行一种 tile) + 水平滚动。"""
import sys, struct

prg = bytearray(32768)
chr_rom = bytearray(8192)

def tile_from_fn(idx, fn):
    base = idx * 16
    for y in range(8):
        lo = hi = 0
        for x in range(8):
            v = fn(x, y) & 3
            lo |= (v & 1) << (7 - x)
            hi |= ((v >> 1) & 1) << (7 - x)
        chr_rom[base + y] = lo
        chr_rom[base + 8 + y] = hi

# tile 0..7: 不同明暗 / 图案
tile_from_fn(0, lambda x, y: 0)                         # 全 0 (背景)
tile_from_fn(1, lambda x, y: 1)                         # 暗
tile_from_fn(2, lambda x, y: 2)                         # 中
tile_from_fn(3, lambda x, y: 3)                         # 亮
tile_from_fn(4, lambda x, y: 3 if (x ^ y) & 1 else 0)   # 棋盘
tile_from_fn(5, lambda x, y: x & 3)                     # 横向渐变
tile_from_fn(6, lambda x, y: y & 3)                     # 纵向渐变
tile_from_fn(7, lambda x, y: 3 if x < 2 else 0)         # 竖条

code = bytearray()
def e(*bs): code.extend(bs)
def bne(target):
    """相对下一条指令"""
    e(0xD0, (target - (len(code) + 2)) & 0xFF)
def bpl(target):
    e(0x10, (target - (len(code) + 2)) & 0xFF)

RESET = 0x8000   # 代码放在 PRG offset 0 => CPU $8000 (32K 映射)

e(0x78)                 # SEI
e(0xA2, 0xFF)           # LDX #$FF
e(0x9A)                 # TXS
e(0xD8)                 # CLD
e(0xA9, 0x00)
e(0x8D, 0x00, 0x20)     # STA $2000
e(0x8D, 0x01, 0x20)     # STA $2001

# 等两帧：PPU 完成上电初始化
for _ in range(2):
    w = len(code)
    e(0x2C, 0x02, 0x20)  # BIT $2002
    bpl(w)

# 清 nametable ($2000-$23FF, 1KB)
e(0xA9, 0x20); e(0x8D, 0x06, 0x20)
e(0xA9, 0x00); e(0x8D, 0x06, 0x20)
e(0xA2, 0x04)           # 4 页
e(0xA0, 0x00)
e(0xA9, 0x00)
clr = len(code)
e(0x8D, 0x07, 0x20)
e(0x88)
bne(clr)
e(0xCA)
bne(clr)

# 写 nametable：第 N 行填 tile (N & 7)
e(0xA9, 0x20); e(0x8D, 0x06, 0x20)
e(0xA9, 0x00); e(0x8D, 0x06, 0x20)
e(0xA2, 0x1E)           # 30 行
row_l = len(code)
e(0xA0, 0x20)           # 32 列
col_l = len(code)
e(0x8A)                 # TXA (行号)
e(0x29, 0x07)           # AND #$07
e(0x8D, 0x07, 0x20)     # STA $2007
e(0x88)                 # DEY
bne(col_l)
e(0xCA)                 # DEX
bne(row_l)

# 调色板 $3F00-$3F1F: 灰阶阶梯 $30..$3F
e(0xA9, 0x3F); e(0x8D, 0x06, 0x20)
e(0xA9, 0x00); e(0x8D, 0x06, 0x20)
e(0xA0, 0x00)
pal_l = len(code)
e(0x98)                 # TYA
e(0x29, 0x0F)
e(0x09, 0x30)           # ORA #$30
e(0x8D, 0x07, 0x20)
e(0xC8)
e(0xC0, 0x20)
bne(pal_l)

# 开渲染：显示背景
e(0xA9, 0x0A); e(0x8D, 0x01, 0x20)

# 主循环：每帧水平滚动
spin = len(code)
e(0xAD, 0x02, 0x20)     # LDA $2002 (复位地址锁存)
e(0xEE, 0x00, 0x03)     # INC $0300
e(0xAD, 0x00, 0x03)     # LDA $0300
e(0x8D, 0x05, 0x20)     # STA $2005
e(0xA9, 0x00)
e(0x8D, 0x05, 0x20)     # STA $2005
e(0x4C, (RESET + spin) & 0xFF, ((RESET + spin) >> 8) & 0xFF)

prg[0:len(code)] = code

# CPU $FFFA/$FFFC/$FFFE 在 32K PRG 映射下 = PRG offset 0x7FFA/0x7FFC/0x7FFE
# ($8000 对应 offset 0)。写 0x3FFA 是错的 —— 那是 16K bank 内的位置。
vec = 0x7FFA
for i in range(3):
    prg[vec + i*2: vec + i*2 + 2] = struct.pack('<H', RESET)

header = b'NES\x1a' + bytes([2, 1, 0x00, 0x00]) + b'\x00' * 8
out = header + bytes(prg) + bytes(chr_rom)

path = sys.argv[1] if len(sys.argv) > 1 else 'test.nes'
open(path, 'wb').write(out)
print(f"wrote {path}: {len(out)} bytes, code={len(code)}B @ $%04X" % RESET)
