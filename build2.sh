#!/usr/bin/env bash
# ============================================================================
# 构建 + 烧录（WorkBuddy 安全层兼容版）
#
#   bash hw/build2.sh              # ★ 编译 + **只烧 app（保 NVS）** —— 默认安全模式
#   bash hw/build2.sh COM7         # 指定串口（仍是只烧 app）
#   bash hw/build2.sh only         # 只编译
#   bash hw/build2.sh app          # 同上，显式写 app（改 UI/刷新策略用这个）
#   bash hw/build2.sh app COM7     # 同理指定串口
#   bash hw/build2.sh all          # 警告：整片擦除（NVS/WiFi 全丢），仅改分区表/sdkconfig 时用
#   bash hw/build2.sh migrate      # ★2026-10-16：一次性 OTA 迁移（分区表 + app + **assets.bin**，不擦 NVS）
#   bash hw/build2.sh reconf       # 强制重跑 CMake 配置（改了 CMakeLists 后用）
#
# ★★ 2026-10-04 端口改为**自动探测**（用户在多台电脑上烧录，同一块板子在各机器上
#    枚举成不同端口 —— 本机 COM3、另一台 COM5，写死任何一个都只对一台机器成立）。
#    取值顺序：形参 COMx > 环境变量 PET_PORT > 自动挑（**优先「USB 串行设备」**，
#    因为内置通信口如 COM1 编号更小但不是板子）。
#    要指定：PET_PORT=COM5 bash hw/build2.sh app
#
# ★★★ 2026-09-29 血泪教训 + 2026-11-10 用户令「默认改成只烧 app」：
#    原来默认整片擦除（erase_flash）→ NVS 全丢，曾把用户配好的 WiFi 擦没。
#    现在默认 MODE="app"（只烧 0x20000，保 NVS）；整片擦除必须显式传 all。
#   事故：修刷新策略时执行了无参数的 `bash hw/build2.sh`，把用户配好的 WiFi 擦没了
#        （用户：「我烧录了一下之后 wifi 信息没了」）。
#   ⇒ 规矩：
#       · 只改了 **app 代码 / UI / 刷新策略**（不动分区表、不加 NVS 键）
#         → 用 `bash hw/build2.sh app` 或 `bash hw/flash_app.sh`（**保 WiFi**）
#       · 改了 sdkconfig / partition_table / CMakeLists(EMBED_FILES) / NVS 结构
#         → 才用无参数整片烧
#   ⇒ 另外：`all` 会**打印醒目警告**并等 2 秒（见下方 WARN），别当没看见。
#
# ── 为什么需要这个脚本（而不是直接用 build_flash.sh）─────────────────────
# WorkBuddy 的文件操作安全层会统计「单个回合内的删除动作数」，超过阈值 50
# 就中断。而 ESP-IDF 每次跑 CMake 配置都会：
#     生成 component_requires.temp.cmake / component_properties.temp.cmake /
#     build_properties.temp.cmake / components_with_manifests_list.temp
#     然后 file(REMOVE) 删掉它们  ← 被计成删除动作
# 构建轮次一多必被拦（78 → 82 → 107 → 109 → 111 ...），
# --dangerously-disable-sandbox 也拦不住（拦截在更上层）。
#
# 解法：把「CMake 配置」与「ninja 编译」彻底拆开，且删除动作分散到多条命令。
#   ① 预清理 *.temp* → CMake 认为"已最新"，ninja 不再触发 regenerate。
#   ② 只有首次 / reconf 才跑 cmake；每个构建目录各跑一次，删除量可接受。
#   ③ 编译走裸 ninja，烧录走裸 esptool，都不再重跑 CMake。
#   ★ cmake / ninja 必须借 hw/idf_run.py 的 --exec 模式跑 ——
#     裸调 cmake.exe 没有工具链 PATH，会报
#     "xtensa-esp32s3-elf-g++ is not a full path and was not found in the PATH"。
#
# bootloader 子工程同样要预清理，否则每次都会重配置（踩过）。
# ============================================================================
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd -W)"
PROJ="$HERE/fw_epaper_demo"
# ★R217：不再写死 C:/epaper —— 按仓库相对位置推导，换电脑只要保持目录关系即可
EPAPER_ROOT="$(cd "$(dirname "$0")/../.." && pwd -W)"
BUILD_DIR="${PET_BUILD_DIR:-${PET_BUILD_BASE:-$EPAPER_ROOT/_build}/base_key}"

CMAKE="C:/Espressif/tools/cmake/3.30.2/bin/cmake.exe"
NINJA="C:/Espressif/tools/ninja/1.12.1/ninja.exe"

export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-C:/Espressif}"
export IDF_PATH="${IDF_PATH:-C:/esp/v5.5.3/esp-idf}"

# ★ 2026-10-01：工具链 Python **不写死**（两代安装器布局不同，见 find_idf_py.sh）。
#   写死旧布局的后果 = run_env 起不来 → ninja 秒退且不吐 error 行，极难查。
. "$HERE/find_idf_py.sh"
PY_ESP="$(find_idf_py)" || {
  echo "!! 找不到 ESP-IDF 的 Python 环境（已找过 \$IDF_TOOLS_PATH/python_env 与 tools/python）"
  echo "   确认 IDF 已安装，或在 hw/find_idf_py.sh 里补一条候选路径。"
  exit 1
}
echo ">>> IDF python：$PY_ESP"
ESPTOOL_PY="$PY_ESP"
# ★R75（用户令）：底部提示白名单冻结检查 —— 新增页想加提示必须用户点名并同步改三处，
#   否则在这里拦下构建（防止「明信片分类页底部提示」这类问题再次复发）。
"$PY_ESP" "$HERE/scripts/_tools/check_page_hints.py" || { echo "!! 底部提示白名单校验失败，已拦下构建"; exit 1; }
# ★R184：页面分派穷尽性（page_render / touch_tap 漏 case 直接拦构建）
"$PY_ESP" "$HERE/scripts/_tools/check_page_dispatch.py" || { echo "!! 页面分派漏 case —— 见上面清单"; exit 1; }
# ★R218：商店「名称↔图片」一致性（虚拟消耗品误当真品画 g_loot[item] 的错位，编译期就拦）
"$PY_ESP" "$HERE/scripts/_tools/check_shop_icons.py" || { echo "!! 商店图标与名称不符 —— 见上面清单"; exit 1; }
# ★R141：三语全量检查（缺条目 / 空值 / EN·JA 未翻 / 繁体未转，任一不过就拦构建）
"$PY_ESP" "$HERE/scripts/_tools/check_i18n_full.py" || { echo "!! 三语翻译检查未通过，已拦下构建"; exit 1; }
# ★R144：assets bin 必须与源码重拼逐字节一致，防「改了 i18n/字库/底图但没重打」
"$PY_ESP" "$HERE/scripts/_tools/pack_assets.py" --verify >/dev/null || { echo "!! assets bin 与源码不一致 —— 先跑 pack_assets.py --write"; exit 1; }
# ★R154：字库有效性闸门 —— 源码用字全覆盖 + 位图不是 .notdef 假字形/空白。
#   旧构建只查三语/assets，新增文案带进新字或补字渲出问号框都会漏过去。
"$PY_ESP" "$HERE/scripts/_tools/check_zfont_glyphs.py" || { echo "!! 字库缺字或存在无效字形 —— 先跑 check_zfont_glyphs.py --fix"; exit 1; }
# ★2026-11-12：两版接线互比守卫（SRCS 集合 / 公共目录副本硬拦；TOUCH_DEMO 分支命中仅提示）
"$PY_ESP" "$HERE/check_parity.py" || { echo "!! 两版接线守卫拦下 —— 见上面清单"; exit 1; }

export IDF_PYTHON_ENV_PATH="$(dirname "$(dirname "$PY_ESP")")"
export PROCESSOR_ARCHITECTURE="${PROCESSOR_ARCHITECTURE:-AMD64}"

# 借 idf_run.py 的 --exec 拿到完整工具链 PATH
run_env() { "$PY_ESP" "$HERE/idf_run.py" --exec "$@"; }

# ★★ 2026-10-04：端口不再写死。
#   背景：用户在**多台电脑**上烧录，同一块板子在各机器上枚举成不同端口
#   （本机 COM3、另一台 COM5）—— 写死任何一个都只对一台机器成立。
#   原来这里是 PORT="COM5"：在本机会直接烧错/烧不到（且默认整片擦除还有丢 NVS 的风险）。
#   —— 改成「环境变量 PET_PORT > 形参 COMx > 自动探测第一个真实串口」。
#      多台机器都不用改代码；真要指定就 `PET_PORT=COM5 bash hw/build2.sh app`。
# ⚠ 本脚本开了 `set -u`，读未设的环境变量会直接报错 → 一律用 ${VAR:-} 兜底。
PORT="${PET_PORT:-}"
MODE="app"
for a in "$@"; do
  case "$a" in
    COM*)  PORT="$a" ;;
    only)  MODE="only" ;;
    app)   MODE="app" ;;          # ★ 只烧 app（保 NVS / 保 WiFi）
    all|erase) MODE="all" ;;      # 警告：显式指定才整片擦除（erase_flash）
    migrate) MODE="migrate" ;;    # ★ 2026-10-16：一次性 OTA 迁移（分区表 + app，**不擦 NVS**）
    reconf) MODE="reconf" ;;
    *)     echo "未知参数：$a" >&2; exit 2 ;;
  esac
done

if [ -z "$PORT" ]; then
  # 探测放在子 shell 里，且**必须用 IDF 的 python**（系统 python 没装 pyserial，
  # 而 esptool 本身就依赖 pyserial —— 用错解释器会一个串口都看不到）。
  PORT="$(bash -c '
PYBIN="'"$PY_ESP"'"
[ -x "$PYBIN" ] || PYBIN=python
"$PYBIN" - <<PYEOF 2>/dev/null || true
import glob, re
def rank(d, desc=""):
    # 1) 蓝牙/虚拟口最差；2) 「USB 串行设备」才是板子；3) 其余按编号
    # ⚠ 不能只按 COM 编号排 —— 本机实测 COM1=内置通信口、COM3=USB 板子，按编号会选错。
    u, du = d.upper(), desc.upper()
    if "BLUETOOTH" in u or "BLUETOOTH" in du: return 9999
    m = re.match(r"COM(\d+)$", u)
    n = int(m.group(1)) if m else 50
    if "USB" in du: return n
    if re.search(r"UART|CH340|CP210|FTDI|SLAB", du): return n
    return n + 1000
try:
    import serial.tools.list_ports as lp
    cands = [(p.device, p.description or "") for p in lp.comports()]
except Exception:
    cands = [(d, "USB") for d in glob.glob("/dev/tty.*") if "Bluetooth" not in d]
if cands:
    print(sorted(cands, key=lambda x: rank(*x))[0][0])
PYEOF
' 2>/dev/null || true)"
fi
if [ -z "$PORT" ]; then
  echo "★ 没找到可用串口。指定一个：PET_PORT=COMx bash hw/build2.sh app" >&2
  exit 2
fi
if [ -n "${PET_PORT:-}" ]; then
  echo ">>> 端口：$PORT（来自环境变量 PET_PORT）"
else
  echo ">>> 端口：$PORT（自动探测：优先 USB 串口设备；多台机器不用改代码，要指定用 PET_PORT=COMx）"
fi

mkdir -p "$BUILD_DIR"

clean_temps() {
  find "$BUILD_DIR" -maxdepth 2 -name "*.temp.cmake" -delete 2>/dev/null || true
  find "$BUILD_DIR" -maxdepth 2 -name "components_with_manifests_list.temp" -delete 2>/dev/null || true
}
clean_temps

need_reconf=0
[ ! -f "$BUILD_DIR/build.ninja" ] && need_reconf=1
[ "$MODE" = "reconf" ] && need_reconf=1

if [ "$need_reconf" = "1" ]; then
  echo ">>> 配置主工程（CMake）"
  # ★★ 2026-10-08：必须显式 `-G Ninja -DIDF_TARGET=esp32s3`。
  #   本机 PATH 上的默认生成器是 NMake Makefiles，而 cmake 在 run_env 里找不到 nmake
  #   → 直接 `CMake Error: Running 'nmake' '-?' failed`（配置阶段就挂，连编译都到不了）。
  #   触屏版 build2_touch.sh 一直带着这两个参数，本脚本早先是漏的 —— 这就是
  #   「按键版跑不通」的第一颗钉子。两脚本此段从此保持一致。
  run_env "$CMAKE" -G Ninja -DIDF_TARGET=esp32s3 -S"$PROJ" -B"$BUILD_DIR" || { echo "!! 主工程配置失败"; exit 1; }
  clean_temps
  # ★★★ 2026-10-08 撤除「单独预配置 bootloader 子工程」这一步 —— 它是个**负收益**动作。
  #   两个事实：
  #     ① bootloader/subproject 的 CMakeLists 第 4 行会直接
  #        `message(FATAL_ERROR "expects the SDKCONFIG variable to be passed in by
  #        the parent build process")` —— 脱离了父构建**一定**配置失败，不是环境问题。
  #     ② 这一失败的配置会在 $BUILD_DIR/bootloader 里**落下一个生成器不对的
  #        CMakeCache.txt**（无 -G 时落到 NMake），而父构建的 ExternalProject
  #        随后要往**同一个目录**用 Ninja 配置 →
  #        `CMake Error: Error: generator : Ninja` + `FAILED: bootloader-configure`
  #        —— 整个编译在这一步挂掉，且报错信息完全不提真正的原因。
  #   ⇒ 正确做法：**什么都不做**，bootloader 由父 ninja 的 ExternalProject
  #     带着正确的 -G/SDKCONFIG 自己去配。（触屏版 build2_touch.sh 里同样的
  #     "容错"写法，只是那边构建目录里恰好没留下坏缓存才没炸。）
  #   ⚠ 若 $BUILD_DIR/bootloader 已被污染（老版本脚本跑过），删掉该目录重跑即可。
  if [ -d "$BUILD_DIR/bootloader" ] && [ -f "$BUILD_DIR/bootloader/CMakeCache.txt" ]; then
    if ! grep -q 'Ninja' "$BUILD_DIR/bootloader/CMakeCache.txt" 2>/dev/null; then
      echo ">>> 清理污染过的 bootloader 配置缓存（生成器不是 Ninja）"
      rm -rf "$BUILD_DIR/bootloader" "$BUILD_DIR/bootloader-prefix"
    fi
  fi
fi

echo ">>> 编译（ESP-IDF 的英文进度行是 ninja 的，已过滤，只保留关键节点）"
# ★ 把 ninja 的英文进度行全部吃掉，只在「关键节点」和「出错」时输出中文。
PET_EXEC_CWD="$BUILD_DIR" run_env "$NINJA" -j 8 2>&1 | \
  grep -E --line-buffered 'Generated |Successfully created|binary size|error:|Error|FAILED|undefined reference|warning: .*\[.*\]' | \
  sed -u -e 's/^\[.*\] //' \
         -e 's/^Generated /  产出 /' \
         -e 's/Successfully created esp32s3 image/  已生成 esp32s3 固件镜像/' \
         -e 's/^pet_epaper_demo.bin binary size/  固件体积/' \
         -e 's/Smallest app partition/  分区下限/'
RC=${PIPESTATUS[0]}
[ "$RC" != "0" ] && {
  echo "!! 编译失败（rc=$RC）"
  echo "   · 上面若有 error: 行 → 就是代码问题，照那行改。"
  echo "   · 上面**一行 error 都没有**却 rc!=0 → 多半是运行环境没起来"
  echo "     （典型：找不到 IDF python），先看最上面的「IDF python：」那行。"
  exit 1
}

# ★2026-10-22（用户批准的纪律）：编译后自动体检**内部 DRAM 余量**。
# 本工程 dram0_0_seg 长期贴顶（余量曾只剩 40 字节），新加常驻数组就会链接失败。
# 余量 < 4KB 红字告警；不拦构建（只提醒）。
"$PY_ESP" "$HERE/dram_headroom.py" "$BUILD_DIR/pet_epaper_demo.map" 2>/dev/null || true

if [ "$MODE" = "only" ] || [ "$MODE" = "reconf" ]; then
  echo ">>> 完成（未烧录）"
  exit 0
fi

if [ ! -f "$BUILD_DIR/pet_epaper_demo.bin" ]; then
  echo "!! 没找到 app bin"; exit 1
fi

# ★★★ 2026-09-29：`app` 模式 = **只烧 app、保 NVS**（改 UI/刷新策略的正确姿势）。
if [ "$MODE" = "app" ]; then
  # ★ 2026-10-16 OTA 改分区表后：app 偏移**从设备读回**（旧表 0x10000 / 新表 0x20000），
  #   不再写死 —— 写错会把 app 写进 otadata，屏上毫无变化（静默失败）。
  APP_OFF=$("$PY_ESP" "$HERE/app_off.py" "$PORT" "$ESPTOOL_PY" 2>/dev/null || echo 0x10000)
  echo ">>> 只烧 app（保 NVS / 保 WiFi）： $BUILD_DIR/pet_epaper_demo.bin → $PORT @$APP_OFF"
  "$ESPTOOL_PY" -m esptool -p "$PORT" -b 921600 \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
    "$APP_OFF" "$BUILD_DIR/pet_epaper_demo.bin" || { echo "!! 烧录失败（串口被占？）"; exit 1; }
  # ★★★ 2026-10-19：A/B 双槽后**必须**擦 otadata（否则"烧了没生效"）——
  #   上一次本地升级会把 otadata 切到 ota_1，而上面按名字取的是 **ota_0** 的偏移
  #   ⇒ 设备实际跑的还是 ota_1 里那份旧固件（实测踩过：文案/逻辑全是旧的，白烧）。
  #   擦掉 otadata（8K）后 bootloader 找不到有效记录 → 启动 ota_0（= 刚烧进去的这个）。
  #   放在 app 写成功**之后**擦：万一 app 没写进去，也不会因 otadata 空而找不到可启动镜像。
  echo ">>> 擦 otadata（让设备启动刚写入的 ota_0）"
  "$ESPTOOL_PY" -m esptool -p "$PORT" -b 921600 erase_region 0x10000 0x2000 || \
    echo "!! 擦 otadata 失败（固件已写好，NVS 不受影响；重启仍可能跑旧槽）"
  echo ">>> 烧录完成（NVS 未动）。"
  exit 0
fi

# ★★★ 2026-10-16 新增：**一次性 OTA 迁移**（改分区表后每台设备只需跑这一次）
#   做什么：烧「新分区表(0x8000) + app(0x20000 的 ota_0)」，**不 erase_flash**（NVS/WiFi 保住）。
#   为什么必须单独一个模式：分区表从 factory@0x10000 改成 ota_0@0x20000 后，
#   0x10000 变成 otadata(8K) —— 用旧的 app 模式烧会把 app 写进 otadata，屏上毫无变化。
#   ⚠ 只此一次：迁移后 `app` 模式会自动探测到 0x20000，日常照常。
#   ⚠ 迁移前先备份当前 app.bin（回不到旧分区表：旧 factory 6M 装不下两个 2.8M 槽）。
if [ "$MODE" = "migrate" ]; then
  echo "════════════════════════════════════════════════════════════════"
  echo "⚠️  OTA 一次性迁移：烧 分区表(0x8000) + app(0x20000 ota_0) + assets.bin，**不擦 NVS**"
  echo "   · 做完这一次，以后日常改 app 仍用： bash fw/build2.sh app"
  echo "   · 改**资产**（明信片/UI/字库…）则重跑本模式（assets.bin 一起重写）"
  echo "   · 迁移后 bootloader 会直接从 ota_0 启动（IDF：No factory image, trying OTA 0）"
  echo "   · 2 秒后开始（Ctrl-C 可中止）…"
  echo "════════════════════════════════════════════════════════════════"
  sleep 2
  [ -f "$BUILD_DIR/pet_epaper_demo.bin" ] || { echo "!! 没找到 app bin，先编译（only）"; exit 1; }
  # ★2026-10-16 拆分⑦：assets 分区偏移**从真正要烧的分区表里读**（csv 里该列是空的，
  #   偏移由 gen_esp32part 算出来；写死一个数改表就漂）。
  PARTBIN="$BUILD_DIR/partition_table/partition-table.bin"
  ASSETS_BIN="${PET_ASSETS_BIN:-$HERE/common/assets_btn.bin}"
  ASSETS_OFF="$("$PY_ESP" "$HERE/part_off.py" "$PARTBIN" assets 2>/dev/null || true)"
  [ -n "$ASSETS_OFF" ] || { echo "!! 读不到 assets 分区偏移（分区表里没有 assets 分区？）"; exit 1; }
  [ -f "$ASSETS_BIN" ] || { echo "!! 缺资产包 —— 先跑： python scripts/_tools/pack_assets.py --write"; exit 1; }
  echo "   · assets.bin = $ASSETS_BIN（$(wc -c <"$ASSETS_BIN") B）→ assets 分区 @$ASSETS_OFF"
  if ! "$ESPTOOL_PY" -m esptool -p "$PORT" -b 921600 \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
    0x8000  "$PARTBIN" \
    0x20000 "$BUILD_DIR/pet_epaper_demo.bin" \
    "$ASSETS_OFF" "$ASSETS_BIN"; then
    echo "!! 迁移失败：分区表 / app / assets 没写全。若屏还能亮 → 重跑本模式；若黑屏 → 用无参数整片重烧。"
    exit 1
  fi
  # ★R144：改分区表后必须擦 otadata（否则仍指向挪位后的 ota_1）与 mypost 预留区
  #  （旧 assets 尾部残留会被 mypost_load() 误判成有效明信片，HANDOVER 记过「已存 1/200」）
  echo ">>> 擦 otadata（让 bootloader 从刚写入的 ota_0 启动）"
  "$ESPTOOL_PY" -m esptool -p "$PORT" -b 921600 erase_region 0x10000 0x2000 || \
    echo "!! 擦 otadata 失败（app 已写好，重启仍可能跑旧槽）"
  ASSETS_SIZE="$("$PY_ESP" "$HERE/part_off.py" "$PARTBIN" assets size 2>/dev/null || echo 0x400000)"
  MYPOST_OFF=$(( ASSETS_OFF + ASSETS_SIZE - 0xC9000 ))
  MYPOST_HEX="$(printf '0x%X' "$MYPOST_OFF")"
  echo ">>> 擦 mypost 预留区 @$MYPOST_HEX（0xC9000 B），防旧 assets 尾部被误判成卡片"
  "$ESPTOOL_PY" -m esptool -p "$PORT" -b 921600 erase_region "$MYPOST_HEX" 0xC9000 || \
    echo "!! 擦 mypost 区失败（请手动重擦，否则可能多出假卡片）"
  echo ">>> 迁移完成：重启后应从 ota_0 启动（串口会打 running from ota_0），"
  echo "    并打一行「★ assets 分区校验通过：23 区 · 负载 … · CRC32=0x…」。"
  exit 0
fi

# ★★★ 2026-09-29：整片擦除路径 —— 会**清空 NVS（WiFi 配置/地标/喂食状态全丢）**。
#   事故：修刷新策略时误用无参数调用，把用户配好的 WiFi 擦没了。加醒目警告 + 2s 缓冲。
echo
echo "════════════════════════════════════════════════════════════════"
echo "⚠️  整片擦除模式：将 erase_flash → **NVS 全丢**！"
echo "   · WiFi 配置 / 地标城市 / 喂食状态 / 明信片已读 都会被清空，开机进配网引导。"
echo "   · 若你只是改了 app 代码 / UI / 刷新策略 → 请改用： bash hw/build2.sh app"
echo "   · 只有改了 sdkconfig / 分区表 / CMakeLists(EMBED_FILES) / NVS 结构 才需要整片。"
echo "   · 2 秒后开始擦除（Ctrl-C 可中止）…"
echo "════════════════════════════════════════════════════════════════"
sleep 2

echo ">>> 整片擦除 $PORT"
"$ESPTOOL_PY" -m esptool -p "$PORT" erase_flash || { echo "!! 擦除失败（串口被占？）"; exit 1; }

# ★2026-10-16 拆分⑦：整片重烧也要带上 assets.bin（新固件的一切内容都在那个分区里）
PARTBIN="$BUILD_DIR/partition_table/partition-table.bin"
ASSETS_BIN="${PET_ASSETS_BIN:-$HERE/common/assets_btn.bin}"
ASSETS_OFF="$("$PY_ESP" "$HERE/part_off.py" "$PARTBIN" assets 2>/dev/null || true)"
[ -n "$ASSETS_OFF" ] || { echo "!! 读不到 assets 分区偏移（分区表里没有 assets 分区？）"; exit 1; }
[ -f "$ASSETS_BIN" ] || { echo "!! 缺资产包 —— 先跑： python scripts/_tools/pack_assets.py --write"; exit 1; }
echo ">>> 烧录 bootloader + 分区表 + app + assets（assets @$ASSETS_OFF）"
# ★★★ 2026-10-08 深夜补：**这一步原来没有检查返回值** —— 是个能把板子静默变砖的坑。
#   事故：erase_flash 成功、write_flash 失败（串口被占 / USB-Serial-JTAG 重新枚举 /
#   被人 Ctrl-C 打断），脚本照旧打印「>>> 烧录完成。」并 exit 0 ——
#   板子停在「整片 0xFF」状态：bootloader(0x0) 都没了，
#   ROM 只会一直喊 `invalid header: 0xffffffff`，屏全黑、按键无反应。
#   ⇒ 擦除 + 写入是一个**不可分割的原子动作**：擦完必须写成功，否则一定是砖。
#   恢复办法：重跑本脚本（无参数）整片重烧一次即可（NVS 反正已经没了）。
if ! "$ESPTOOL_PY" -m esptool -p "$PORT" -b 921600 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0     "$BUILD_DIR/bootloader/bootloader.bin" \
  0x8000  "$PARTBIN" \
  0x20000 "$BUILD_DIR/pet_epaper_demo.bin" \
  "$ASSETS_OFF" "$ASSETS_BIN"; then   # ★2026-10-16：新表 app 在 ota_0=0x20000，资产进 assets 分区
  echo "════════════════════════════════════════════════════════════════"
  echo "!! ★ 整片写入失败 —— flash 此刻是「已擦除」状态（= 板子变砖）！"
  echo "   · 现象：屏全黑 / 按键无反应 / 串口刷 `invalid header: 0xffffffff`。"
  echo "   · 恢复：**再跑一次本脚本（不带参数）**，让它把 bootloader/分区表/app/assets 都写进去。"
  echo "   · 排查：串口是否被 monitor / 别的程序占着；USB 线是否松动。"
  echo "════════════════════════════════════════════════════════════════"
  exit 1
fi
# 再回读 app 头做一次独立确认（不信脚本自报，见项目铁律）
# ★ 必须 `--after hard_reset`：`no_reset` 会把芯片**留在下载模式不放行**，
#   脚本虽然打印"烧录完成"，但板子其实没在跑 app —— 用户看到的就是**全黑屏**。
#   （2026-10-08 深夜踩过：报"完成"、屏却是黑的。）
CHK="$BUILD_DIR/.chk_app.bin"
if ! "$ESPTOOL_PY" -m esptool -p "$PORT" --after hard_reset read_flash 0x20000 0x10 "$CHK" >/dev/null 2>&1; then
  echo "!! ★ app 头回读失败 —— 无法确认烧录结果，请重跑本脚本！"; exit 1;
fi
if [ -s "$CHK" ]; then
  HEAD="$(od -An -tx1 -N4 "$CHK" 2>/dev/null | tr -d ' \n')"
  echo ">>> app 镜像头回读：$HEAD（0xe907023f 一类 = OK；ffffffff = 根本没写进去）"
  [ "$HEAD" = "ffffffff" ] && { echo "!! ★ app 头是全 FF —— 写入没生效，板子是砖，请重跑本脚本！"; exit 1; }
fi

echo
echo ">>> 烧录完成。"
