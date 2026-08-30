# -*- coding: utf-8 -*-
"""check_glyphs.py -- 校验 UI 源码用到的字符是否全部在字库中

用法: python tools/check_glyphs.py
新增文案后若报 MISSING, 重跑 gen_msyh_font.py 重新生成字库。
"""
import io
import re
import os
import glob
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(ROOT, "components", "ui", "fonts", "lv_font_msyh_16.c")

s = io.open(FONT, encoding="utf-8", errors="replace").read()
ul = re.search(r"unicode_list\[\]\s*=\s*\{(.*?)\};", s, re.S)
rs = re.search(r"\.range_start\s*=\s*(0x[0-9A-Fa-f]+)", s)
range_start = int(rs.group(1), 16) if rs else 0x20
covered = {range_start + int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{4})", ul.group(1))}

used = {}
files_all = (glob.glob(os.path.join(ROOT, "components/ui/pages/*.c"))
             + [os.path.join(ROOT, "components/ui/smartknob_ui.c")])
for p in files_all:
    src = io.open(p, encoding="utf-8").read()

    def decode(sstr):
        out = bytearray()
        i = 0
        while i < len(sstr):
            if sstr[i] == "\\" and i + 1 < len(sstr) and sstr[i + 1] == "x":
                out.append(int(sstr[i + 2:i + 4], 16))
                i += 4
            else:
                out.extend(sstr[i].encode("utf-8"))
                i += 1
        return out.decode("utf-8", errors="replace")

    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', src):
        try:
            txt = decode(m.group(1))
        except Exception:
            continue
        for ch in txt:
            if ord(ch) >= 0x2E80:
                used.setdefault(ch, set()).add(os.path.basename(p))

missing = {ch: f for ch, f in sorted(used.items()) if ord(ch) not in covered}
print("used CJK chars: %d, font covered: %d" % (len(used), len(covered)))
if missing:
    print("--- MISSING GLYPHS ---")
    for ch, f in missing.items():
        print("U+%04X %s <- %s" % (ord(ch), ch, ",".join(sorted(f))))
    sys.exit(1)
print("OK: all covered")