#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""OTA 发布包生成器（make_ota_release.py）—— 2026-10-19 建

把两版编译产物整理成「设备能拉、服务端能放」的发布目录：

    _ota_release/
      pet_key.bin          按键版 app 镜像（pet_epaper_demo.bin 原样拷贝）
      pet_touch.bin        触屏版 app 镜像（pet_epaper_touch.bin 原样拷贝）
      version_key.json     按键版清单（设备侧 OTA_VERSION_URL 拉它）
      version_touch.json   触屏版清单

清单格式 = common/ota_up.c::do_check() 的解析口径：
    {"ver":"6.2", "size":1423424, "url":"https://.../pet_key.bin", "notes":["...","..."]}
    · ver   三段数字（与固件 APP_VERSION 比大小，缺省段当 0）
    · size  字节数（设备用来判「分区装不下」+ 算下载进度）
    · url   固件直链（绝对地址；放哪都行，清单和 bin 可以不同主机）
    · notes 最多 2 行，上「发现新版本」页（半角标点，项目字库无中文标点）

用法：
    python make_ota_release.py --base https://gitee.com/bbbbaiky/eink-pet-ota/raw/master/pet \
        --ver 6.2 --note "修复 明信片角标" --note "背包页改翻页"

    · --ver 省略 = 从 common/main.c 读 APP_VERSION（烧录前忘了 bump 会被这里抓住）
    · --base 省略 = 干跑（只生成本地文件 + 提醒填地址），不打印上传命令

发布动作 = 把 _ota_release/ 整个传到 --base 指向的位置（脚本末尾按 base 域名
给出对应上传命令；Gitee 公开仓库 raw / 对象存储 PUT 都行，设备免鉴权 GET）。
改固件源地址只改 common/ota_up.c 的 OTA_URL_BASE 一行（或编译 -DOTA_URL_BASE=...）。
"""
import argparse
import json
import os
import re
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
# ★ 构建产物不在源码树内：build2.sh / build2_touch.sh 的 BUILD_DIR 默认在
#   C:/epaper/_build/{fw_epaper_demo,fw_touch}（源码树 _src_1018 的上级）。
#   与 build2.sh 同口径：可用环境变量 OTA_BUILD_BASE / PET_BUILD_BASE 覆盖。
# ★R217：默认值按仓库相对位置推导，不再写死 C:/epaper/_build。
_REPO = Path(__file__).resolve().parent
BUILD_BASE = Path(os.environ.get("OTA_BUILD_BASE", os.environ.get("PET_BUILD_BASE", str(_REPO.parent.parent / "_build"))))
KEY_BIN = BUILD_BASE / "base_key" / "pet_epaper_demo.bin"      # ★R144：10-25 的 fw_epaper_demo 已过期
TOUCH_BIN = BUILD_BASE / "base_touch" / "pet_epaper_touch.bin"  # ★R144
MAIN_C = HERE / "common" / "main.c"
OUT_DEFAULT = HERE / "_ota_release"

NOTES_MAX = 2  # ota_status_t.notes[2][56] —— 设备只显示 2 行、每行 56 字节


def read_app_version() -> str:
    """从 common/main.c 读 APP_VERSION（形如 "v6.1" → 返回 "6.1"）。"""
    txt = MAIN_C.read_text(encoding="utf-8", errors="replace")
    m = re.search(r'#define\s+APP_VERSION\s+"v?([0-9]+(?:\.[0-9]+){0,2})"', txt)
    if not m:
        sys.exit("!! 在 common/main.c 里找不到 APP_VERSION（形如 #define APP_VERSION \"v6.1\"）")
    return m.group(1)


def ver_tuple(v: str):
    parts = [int(x) for x in v.split(".")]
    while len(parts) < 3:
        parts.append(0)
    return tuple(parts[:3])


def main() -> int:
    ap = argparse.ArgumentParser(description="生成 OTA 发布目录（双版本清单 + 固件 bin）")
    ap.add_argument("--base", default="",
                    help="OTA_URL_BASE：清单与固件的公开目录（设备免鉴权 GET）。"
                         "例：https://gitee.com/bbbbaiky/eink-pet-ota/raw/master/pet")
    ap.add_argument("--ver", default="",
                    help="发布版本号（三段，如 6.2 或 6.2.1）。省略 = 读 common/main.c 的 APP_VERSION")
    ap.add_argument("--note", action="append", default=[],
                    help=f"更新日志（最多 {NOTES_MAX} 条，半角标点）；可重复")
    ap.add_argument("--out", default=str(OUT_DEFAULT), help="输出目录（默认 _ota_release/）")
    args = ap.parse_args()

    fw_ver = read_app_version()
    ver = args.ver or fw_ver
    if not re.fullmatch(r"[0-9]+(\.[0-9]+){0,2}", ver):
        sys.exit(f"!! 版本号格式不对：{ver}（要形如 6.2 / 6.2.1）")

    notes = [n.strip() for n in args.note if n.strip()][:NOTES_MAX]
    for n in notes:
        if len(n.encode("utf-8")) > 56:
            sys.exit(f"!! 日志行超长（设备每行 56 字节）：{n!r}")

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    plan = [("pet_key.bin", KEY_BIN, "version_key.json"),
            ("pet_touch.bin", TOUCH_BIN, "version_touch.json")]
    rows = []
    for bin_name, src, manifest in plan:
        if not src.is_file():
            sys.exit(f"!! 缺编译产物 {src} —— 先跑：bash build2.sh only / bash build2_touch.sh only")
        dst = out / bin_name
        shutil.copyfile(src, dst)
        size = dst.stat().st_size
        if size > 0x1F0000:  # ★R144：partitions.csv 实测单槽 0x1F0000，旧值 0x1A0000 会误拦 R143 之后的固件
            sys.exit(f"!! {bin_name} {size} B 超单槽 1.62MB，设备永远装不下")
        url = (args.base.rstrip("/") + "/" + bin_name) if args.base else f"<填 OTA_URL_BASE>/{bin_name}"
        man = {"ver": ver, "size": size, "url": url, "notes": notes}
        (out / manifest).write_text(json.dumps(man, ensure_ascii=False, indent=1) + "\n",
                                    encoding="utf-8")
        rows.append((manifest, bin_name, size))

    print(f"== OTA 发布目录：{out}")
    print(f"   固件 APP_VERSION = v{fw_ver} · 发布版本 = v{ver}"
          + ("" if args.ver and args.ver != fw_ver else "（= 当前固件，没 bump 就是「已是最新」）"))
    for manifest, bin_name, size in rows:
        print(f"   {manifest:22s} → {bin_name:14s} {size:>8d} B ({size/1024:.0f} KB)")
    if notes:
        for n in notes:
            print(f"   日志：{n}")

    # ── TF 卡包（本地升级 ①）：★2026-10-19 用户令 = **单个文件**，不再「文件夹 + version.json」 ──
    #    设备侧（ota_up.c do_check_sd）扫**卡根目录的 *.bin**、读它**内嵌的 esp_app_desc 版本**
    #    比版本 ⇒ 文件名可带前缀、卡上放**一个**就行；提交物不再有 version.json。
    tf_plan = [(f"PetCat_v{ver}_key.bin",   KEY_BIN),
               (f"PetCat_v{ver}_touch.bin", TOUCH_BIN)]
    print(f"\n== TF 卡包（单文件 · 拷到卡根目录）：{out}")
    for tf_name, src in tf_plan:
        if not src.is_file():
            sys.exit(f"!! 缺编译产物 {src}")
        dst = out / tf_name
        shutil.copyfile(src, dst)
        size = dst.stat().st_size
        if size > 0x1F0000:
            sys.exit(f"!! {tf_name} {size} B 超单槽 1.62MB")
        print(f"   {tf_name:24s} {size:>8d} B")
    print("   拷**对应机型**的那一个文件到 FAT32 卡根目录即可（**不要拷文件夹**）；设备插卡后点「检查版本」。")

    if not args.base:
        print("\n!! 没给 --base：url 字段是占位符。补上再跑一次才会生成能用的清单，"
              "例如：\n   python make_ota_release.py --base https://gitee.com/bbbbaiky/eink-pet-ota/raw/master/pet"
              " --ver 6.2 --note \"...\"")
        return 0

    print("\n== 上传（任选其一，目标是让 base 目录可匿名 GET）：")
    if "gitee.com" in args.base:
        print("   Gitee 公开仓库（推荐：国内直连、git 版本化、免费）：")
        print(f"     cd {out}")
        print("     git init -b main && git add -A && git commit -m \"OTA release v%s\"" % ver)
        print("     git remote add origin https://gitee.com/<你的账号>/<公开仓库>.git")
        print("     git push -u origin main        # 首次之后：git add -A && git commit && git push")
        print("   ⚠ 仓库必须公开（私有 raw 要令牌，设备匿名拉不到）")
        print("   ⚠ 若 base 用 gitee.io（Pages）：push 后等约 1 分钟构建生效")
    else:
        print("   对象存储 / 云开发：把整个目录传到 base 指向的位置，")
        print("   保持 pet_key.bin / pet_touch.bin / version_*.json 四个文件名不变即可。")
    print("\n== 设备侧：烧任意一版带 OTA 的固件后，设置2页「固件升级」或关于页「检查版本」触发。")
    print("   换固件源只改 common/ota_up.c 的 OTA_URL_BASE 一行（或编译时 -DOTA_URL_BASE=...）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
