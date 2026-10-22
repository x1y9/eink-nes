#!/usr/bin/env python3
"""校验打包出来的 KUAL 扩展是否完整。

为什么单独写成脚本:
  KUAL 靠 config.xml 发现扩展。少了它, 扩展根本不会出现在 KUAL 菜单里 ——
  而这种错误在构建阶段完全看不出来, 只有真机打开 KUAL 才会发现。
  所以每次打包后都跑一遍这个检查。

用法: python3 scripts/check_kual.py dist/extensions/eink-nes
"""
import json
import os
import sys
import xml.etree.ElementTree as ET

REQUIRED = ["config.xml", "menu.json", "eink-nes", "eink-nes.sh",
            "showlog.sh", "selftest.sh"]


def main() -> int:
    if len(sys.argv) != 2:
        print("用法: check_kual.py <extension_dir>", file=sys.stderr)
        return 2
    ext = sys.argv[1]
    if not os.path.isdir(ext):
        print(f"扩展目录不存在: {ext}", file=sys.stderr)
        return 1

    # 1. 必需文件
    missing = [f for f in REQUIRED if not os.path.isfile(os.path.join(ext, f))]
    if missing:
        print(f"缺少文件: {', '.join(missing)}", file=sys.stderr)
        return 1
    print("必需文件齐全: " + ", ".join(REQUIRED))

    # 2. config.xml 必须合法, 且 <menu type="json"> 要指向 menu.json。
    #    空标签 <menu type="json"></menu> 在 KUAL 里不会加载任何菜单项,
    #    这是最容易犯又最难发现的错误。
    cfg_path = os.path.join(ext, "config.xml")
    try:
        root = ET.parse(cfg_path).getroot()
    except ET.ParseError as e:
        print(f"config.xml 解析失败: {e}", file=sys.stderr)
        return 1

    if root.tag != "extension":
        print(f"config.xml 根节点应为 <extension>, 实际 <{root.tag}>", file=sys.stderr)
        return 1

    for tag in ("name", "version", "author", "id"):
        el = root.find(f"./information/{tag}")
        if el is None or not (el.text or "").strip():
            print(f"config.xml 的 <information><{tag}> 缺失或为空", file=sys.stderr)
            return 1

    menu_el = root.find("./menus/menu")
    if menu_el is None:
        print("config.xml 缺少 <menus><menu>", file=sys.stderr)
        return 1
    target = (menu_el.text or "").strip()
    if not target:
        print(
            "config.xml 的 <menu type=\"json\"> 是空的, 必须写入 menu.json",
            file=sys.stderr,
        )
        return 1
    if not os.path.isfile(os.path.join(ext, target)):
        print(f"config.xml 指向了不存在的菜单文件: {target}", file=sys.stderr)
        return 1
    print(f"config.xml OK, 菜单数据指向 {target}")

    # 3. menu.json 必须合法
    try:
        menu = json.load(open(os.path.join(ext, target), encoding="utf-8"))
    except json.JSONDecodeError as e:
        print(f"menu.json 解析失败: {e}", file=sys.stderr)
        return 1

    # 4. menu.json 里通过 action 调用的脚本必须真实存在
    problems = []

    def walk(items, path="root"):
        for i, it in enumerate(items):
            name = it.get("name", f"#{i}")
            action = it.get("action", "")
            for tok in action.split():
                if tok.endswith(".sh"):
                    base = os.path.basename(tok)
                    if not os.path.isfile(os.path.join(ext, base)):
                        problems.append(f"{path} / {name}: 引用了不存在的脚本 {base}")
            walk(it.get("items", []), f"{path}/{name}")

    walk(menu.get("items", []))
    if problems:
        for p in problems:
            print(p, file=sys.stderr)
        return 1
    print("menu.json 引用的脚本都存在")

    # 5. 可执行位 (在真实 Kindle 上是 FAT32, 没有执行位, 但包里应该有)
    for f in ("eink-nes", "eink-nes.sh", "showlog.sh"):
        p = os.path.join(ext, f)
        if not os.access(p, os.X_OK):
            print(f"警告: {f} 没有执行位 (Kindle 上 FAT32 无所谓, 但包里应有)")

    print("KUAL 扩展校验通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
