#!/usr/bin/env python3
# ============================================================================
# gen_i18n_asset.py —— 生成 assets 分区的 I18N 区域二进制（2026-10-20）
# ----------------------------------------------------------------------------
# 背景：
#   固件运行时多语言查表（t_()）原本把 EN/JA/ZHT 三张翻译表以 `static const`
#   数组形式编译进 **rodata**（约 113 KB），纯占固件体积。这些表与固件代码
#   无关、属于「内容」，应和明信片/字库一样搬进 **assets 分区**（单份、可单独 OTA）。
#
# 真源：
#   common/i18n_table_data.h —— 仅被本脚本解析，**不编译进固件**（main.c 不再
#   包含它），保留它只是为了「改翻译后能重新生成」这个能力。
#   （原先 EN_TABLE/JA_TABLE/ZHT_TABLE 在 i18n_table.h，现已拆出数据到本文件，
#    i18n_table.h 只留 typedef；详见 i18n_table.h 顶部说明。）
#
# 产出：
#   common/i18n_asset.bin —— I18N 区域，由 pack_assets.py 作为 **第 22 项（最后）**
#   拼进 assets.bin。改翻译后流程：python scripts/_tools/gen_i18n_asset.py
#                                 → python scripts/_tools/pack_assets.py --write
#
# 区域格式（全 LE）：
#   [0..3]   u32 magic 'I18N' (0x4E383149u，LE 字节序 49 31 38 4E)
#   [4..7]   u32 version (=1)
#   [8..11]  u32 n（条目数）
#   [12..15] u32 reserved (0)
#   [16 .. 16+16*n)  索引：n × {u32 zh_off; u32 en_off; u32 ja_off; u32 zht_off}
#                          （偏移均相对区域起点；位置 0/1/2/3 对应 LANG_ZH/EN/JA/ZHT）
#   [16+16*n ..] 字符串池：每条目按 zh,en,ja,zht 顺序拼接，各以 '\0' 结尾（UTF-8）
# 条目按 zh 的 UTF-8 字节序升序（与固件 t_() 的 strcmp 二分查找一致）。
# ============================================================================
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_SRC = os.path.join(ROOT, "common", "i18n_table_data.h")
DEFAULT_OUT = os.path.join(ROOT, "common", "i18n_asset.bin")

I18N_MAGIC = 0x4E383149  # "I18N"（LE：字节序 49 31 38 4E）
I18N_VER = 1


def _unesc(s: str) -> str:
    return (s.replace('\\"', '"').replace("\\'", "'")
             .replace("\\n", "\n").replace("\\t", "\t").replace("\\\\", "\\"))


def parse_tables(path: str):
    src = open(path, encoding="utf-8").read()
    pat = re.compile(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}')
    def block(name):
        m = re.search(r'static const i18n_ent_t\s+' + re.escape(name)
                      + r'\[\]\s*=\s*\{(.*?)\n\};', src, re.S)
        if not m:
            return None
        return [(a, b) for a, b in pat.findall("{" + m.group(1) + "}")]
    en, ja, zt = block("EN_TABLE"), block("JA_TABLE"), block("ZHT_TABLE")
    if not (en and ja and zt):
        raise SystemExit("!! 无法解析三张表（EN/JA/ZHT）于 " + path)
    if not (len(en) == len(ja) == len(zt)):
        raise SystemExit("!! 三张表条数不一致: %d/%d/%d" % (len(en), len(ja), len(zt)))
    return en, ja, zt


def build_i18n_region(path: str = None) -> bytes:
    en, ja, zt = parse_tables(path or DEFAULT_SRC)
    # 对齐校验：三表同一下标必须同一 zh 键
    for i in range(len(en)):
        if not (en[i][0] == ja[i][0] == zt[i][0]):
            raise SystemExit("!! 第 %d 条 zh 键不对齐: %r / %r / %r"
                             % (i, en[i][0], ja[i][0], zt[i][0]))
    # 组元组并按 zh 的 UTF-8 字节序升序（UTF-8 字节序 == 码点序 == strcmp 序）
    entries = sorted(zip((e[0] for e in en), (e[1] for e in en),
                         (j[1] for j in ja), (z[1] for z in zt)),
                    key=lambda x: x[0].encode("utf-8"))
    n = len(entries)
    pool_base = 16 + 16 * n          # 字符串池在区域中的起点（偏移均相对区域起点）
    pool = bytearray()
    offs = []
    for zh, en_s, ja_s, zt_s in entries:
        o = [0, 0, 0, 0]
        for k, s in enumerate((zh, en_s, ja_s, zt_s)):
            o[k] = pool_base + len(pool)   # 区域相对偏移（固件按区域起点解）
            pool += s.encode("utf-8") + b"\x00"
        offs.append(o)
    index = bytearray()
    for o in offs:
        for v in o:
            index += struct.pack("<I", v)
    hdr = struct.pack("<IIII", I18N_MAGIC, I18N_VER, n, 0)
    return bytes(hdr) + bytes(index) + bytes(pool)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_OUT
    region = build_i18n_region()
    open(out, "wb").write(region)
    en, ja, zt = parse_tables(DEFAULT_SRC)
    print(">>> I18N 区域已生成: %s (%d B, %d 条目)" % (out, len(region), len(en)))


if __name__ == "__main__":
    main()
