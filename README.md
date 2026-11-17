# EinkPet — 墨水屏电子宠物

> 一只住在 200×200 像素墨水屏里的电子猫。ESP32-S3 驱动，会吃饭、睡觉、出门远行、串门、
> 收集明信片和收藏品。按键版和触屏版共用一套源码。

**English:** A desktop e-paper pet cat firmware for ESP32-S3 (200×200 1-bit SSD1681).
Single source tree, two variants (physical buttons / capacitive touch). MIT licensed.

---

## 目录

- [功能](#功能)
- [硬件](#硬件)
- [两个版本](#两个版本)
- [快速开始](#快速开始)
- [交互说明](#交互说明)
- [项目结构](#项目结构)
- [架构](#架构)
- [素材与生成脚本](#素材与生成脚本)
- [测试暗门](#测试暗门)
- [文档](#文档)
- [第三方与许可](#第三方与许可)

## 功能

- **日常照料**：喂食（大棒骨 / T骨肉排 / 猫粮 / 小鱼干）、抚摸、心情、体力、经验成长。
- **出门远行**：选目的地、掷骰推进、带回明信片和城市纪念；旅迹页记录走过的城市。
- **串门社交**：拜访好友、留言、明信片转发；支持 TF 卡信导入/导出。
- **收集系统**：收藏品掉落、背包、商店、成就；消耗品 99 封顶。
- **网页配网**：SoftAP + 内置网页，选 WiFi、城市、取名；省市区下拉离线烘焙。
- **天气与时间**：Open-Meteo 天气、离线城市经纬度表、SNTP 校时、清晨/正午/傍晚/深夜/破晓五时段。
- **OTA 升级**：网页推送固件（保留 NVS），app 分区 OTA，双分区回滚。
- **多语言**：简体中文 / 日本語 / 繁體中文，三语完整性由构建闸门强制检查。
- **显示**：1-bit 点阵字体、局部刷新、深夜网点风景（富士山 / 东方明珠 / 星球）、
  科技/普通房间、A/B 两张轮流关机页、网络状态角标、气泡日志。
- **省电**：深睡待机、按键唤醒、RTC 保留姿态、定时唤醒补同步。

## 硬件

| 项 | 规格 |
|---|---|
| 主控 | ESP32-S3（微雪 ESP32-S3-ePaper-1.54 开发板） |
| 屏幕 | SSD1681，200×200，1-bit 黑白电子纸 |
| Flash | 8 MB（分区：`ota_0` / `ota_1` / assets @0x400000 / NVS / mypost） |
| 输入（按键版） | BOOT + POWER 两个实体按键 |
| 输入（触屏版） | FT6336 电容触摸 + 实体 BOOT/POWER |
| 其它 | 充电检测、电量采集、SD 卡（明信片转存） |

## 两个版本

单源 `common/main.c`，`#if TOUCH_DEMO` 编译期分流：

- **按键版**：工程 `fw_epaper_demo/`，交互靠 BOOT/POWER 短按与长按。
- **触屏版**：工程 `fw_touch/`，多 `touch_ft6336.c`，列表页进页无预选框、点击才亮框。

两版共用 `common/`，`sdkconfig` / `partitions.csv` / 资产 bin 必须一致；`check_parity.py`
会强制检查两版接线没有分叉。

## 快速开始

### 1. 环境

- Windows + **ESP-IDF v5.5.3**（工具链路径见 `idf_env.sh` / `find_idf_py.sh`）
- Python 3：**系统 python3** 跑出图脚本，**IDF python** 跑构建闸门

```bash
python3      -m pip install -r requirements.txt
<IDF python> -m pip install -r requirements.txt
# IDF python 路径：bash -c '. ./find_idf_py.sh; find_idf_py'
```

两套解释器都要装：系统 python3 需要 numpy / Pillow / pyserial；
IDF python 必须装 `opencc-python-reimplemented`（缺了三语检查会硬拦构建）。

### 2. 目录约定

脚本不写死 `C:/epaper`，按仓库相对位置推导，换电脑只需保持相对关系：

```
<epaper 根>/<工作区>/<本仓库>/    ← 本仓库
<epaper 根>/inbox/                ← 唯一素材投放区（新图放这里）
<epaper 根>/_build/               ← 编译产物（可删，会自动重建）
```

可用环境变量覆盖：`PET_INBOX`、`PET_BUILD_BASE`、`PET_BUILD_DIR`、`PET_PORT`、
`PET_IDF_PY`、`PET_TMP`。统一解析见 `scripts/_tools/pet_paths.py`。

### 3. 编译与烧录

```bash
bash build2.sh only                       # 按键版：只编译
bash build2_touch.sh only                 # 触屏版：只编译

PET_PORT=COM5 bash build2.sh app          # 按键版：编译 + 只烧 app（保 NVS/WiFi）
PET_PORT=COM7 bash build2_touch.sh app    # 触屏版：同上
```

| 模式 | 行为 |
|---|---|
| `only` | 只编译，不烧录 |
| `app`（默认，**不带参数也是这个**） | 只写 app 分区，NVS / WiFi / 明信片全保留 |
| `migrate` | 分区表 + app + assets，不擦 NVS；改资产分区时用 |
| `all` | **整片擦除**，NVS/WiFi/存档全丢，仅改分区表时用 |

⚠️ 日常升级**永远用 `app`**。改了 assets（`common/*.bin`）才需要 `migrate`。
烧录模式有醒目的防呆提示，别关掉它。

### 4. 版本标识（发版前必查）

`common/main.c` 的 `APP_VER_MINOR` / `APP_VERSION` / `FW_BUILD` +
`fw_epaper_demo/CMakeLists.txt` 与 `fw_touch/CMakeLists.txt` 的 `PROJECT_VER`
——**五处必须是同一个 R 号**，漏改会造成屏上关于页显示假版本号。

## 交互说明

### 按键版

| 操作 | 行为 |
|---|---|
| BOOT 短按 | 选择 / 下一个 |
| BOOT 按住 ≥0.5s | 返回上一级（到点即触发） |
| BOOT 按住 ≥2s | 直接回首页（房间页） |
| POWER 短按 | 确定 / 喂食 |
| POWER 按住 ≥2s | 关机（到点即触发） |
| POWER 按住 ≥8s | 强制重启 |

### 触屏版

- 列表页进页**无预选框**，点击才亮框；详情/确认页是单段式。
- 实体 BOOT/POWER 仍在，语义与按键版对齐；滑动可翻页。
- 配网跳过阈值：按键版 0.5s / 触屏版 3s（防误触口径不同，属设计）。

## 项目结构

| 路径 | 内容 |
|---|---|
| `common/main.c` | 主状态机、页面渲染、输入处理（约 2.4 万行，单源两版） |
| `common/*.c` / `*.h` | 网络同步、配网、OTA、好友、明信片、掉落、商店、经验、i18n、帧缓冲等模块 |
| `common/assets.h` + `common/*.bin` | assets 分区：字库、精灵、地标、天气、明信片、i18n 资产表 |
| `fw_epaper_demo/` | 按键版工程（CMakeLists / main / partitions.csv / sdkconfig） |
| `fw_touch/` | 触屏版工程（多 `touch_ft6336.c`） |
| `scripts/_tools/` | 出图与生成脚本、构建闸门、i18n / 字库检查、路径解析 |
| `build2.sh` / `build2_touch.sh` | 构建 + 烧录（含三语、字库、页面分派、assets 一致性闸门） |
| `prep_assets.py` / `verify_assets.py` | assets 分区打包 / 往返校验 |
| `check_parity.py` / `scripts/refactor_guard.py` | 两版接线一致性 / 重构基线守卫 |
| `watch_serial.py` / `boot_log_probe.py` / `nvs_report.py` | 串口监视 / 启动日志 / NVS 巡检 |
| `docs/` | 变更归档、OTA 设计说明 |

## 架构

- **单源双版**：`#if TOUCH_DEMO` 编译期分流，公共代码零拷贝。`verify_variant_merge.py`
  会比对两版预处理结果，防止一边改了另一边没跟。
- **assets 分区**：所有位图 / 字体 / i18n 表打成 `assets_btn.bin` / `assets_touch.bin`，
  烧在 0x400000；`pack_assets.py --verify` 保证仓库里的 bin 与源码重拼逐字节一致。
- **i18n**：`i18n_table_data.h` 三语表 → `gen_i18n_asset.py` → 资产 bin；构建时
  `check_i18n_full.py` 检查缺条目 / 空值 / 未翻 / 繁体未转，`check_zfont_glyphs.py`
  检查源码用字全部有字形。
- **页面分派**：`page_render` / `touch_tap` 的 `case` 必须对页面枚举穷尽，
  `check_page_dispatch.py` 强制检查；新增页面漏 case 会拦下构建。
- **低内存**：帧缓冲在 PSRAM，静态缓冲外置；栈与 DRAM 余量在编译输出里可见。

## 素材与生成脚本

素材**只放 `<epaper 根>/inbox`**。所有生成脚本从这里读，不再有带下划线的第二份目录。

| 脚本 | 产物 |
|---|---|
| `gen_land_face.py` | 风景地标（富士山 / 东方明珠 / 星球 × 四时段 + 天气场） |
| `gen_room_tech.py` | 科技房间四时段底图 + 窗洞掩膜 |
| `gen_cat_sprites.py` | 猫姿态精灵表 |
| `gen_off_page.py` | A/B 两张关机页 |
| `gen_i18n_asset.py` | i18n 资产 bin（三语） |
| `pack_assets.py` | 两版 assets 分区 bin |

脚本可重复运行。`gen_room_tech.py` 支持只放某一时段的图（其它帧沿用现有产物并告警）；
`gen_land_face.py` 会校验硬编码窗洞与房间掩膜一致，改窗洞必须同步改 `HOLE`。

## 测试暗门

测试模式对玩家默认隐藏。**按键版**：BOOT + POWER 同时按住约 1 秒。
**触屏版**：设置 → 关于，连点固件版本号 6 下。
都会弹出「测试模式已开启 / 已关闭」顶部胶囊提示，两版共用一个标志（不写 NVS，重启恢复隐藏）。

## 文档

- `README.md` — 本文件
- `docs/RELEASE_NOTES_ARCHIVE.md` — 历史版本变更归档（R213 之前）
- `docs/OTA_固件升级_资产存档_20261022.md` — OTA 升级设计存档
- `HANDOVER_20261027.md` — 历史交接快照（停在 R157，仅供考古）

⚠️ 中间过程的体检报告、重构流水账一律**不留在仓库**（结论都已落进代码；留在仓里
只会让人误以为是当前状态）。需要考古请查 git 历史。**当前状态以 git log 和本 README 为准。**

## 第三方与许可

本项目代码使用 [MIT License](LICENSE)。第三方资产：

| 资产 | 来源 / 许可 |
|---|---|
| BoutiqueBitmap9x9 点阵字体 | OFL-1.1 / CC0，可商用（以原作者声明为准） |
| ZLabsPixel12px 点阵字体 | CC0（以原作者声明为准） |
| `managed_components/espressif__esp_jpeg` | Espressif Systems，Apache-2.0（见组件目录） |
| 城市经纬度 / 省市下拉数据 | GeoNames 与 Open-Meteo 数据离线烘焙（各自数据条款适用） |
| 猫姿态 / 房间 / 地标 / 明信片位图 | 项目作者绘制，随本项目发布 |

天气数据来自 Open-Meteo 公共 API，使用时请遵守其服务条款。

## 致谢

- Espressif ESP-IDF 与 SSD1681 显示驱动参考实现
- 微雪电子 ESP32-S3-ePaper-1.54 硬件
- 所有给出反馈、报 bug、画素材的人
