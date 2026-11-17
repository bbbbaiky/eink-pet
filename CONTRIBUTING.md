# 贡献指南

这个项目是「一句一句迭代」出来的：**每次只改一处，能编译、能上机、能回退**。
不要把整个工程推倒重写。

## 动手前

1. 读 `README.md`，确认你改的是按键版、触屏版还是两版共用代码。
2. 别改交互手感（BOOT/POWER 的档位、返回逻辑、暗门手势）——那是反复实机调出来的。
   真要改，先在 issue 里说清楚为什么。
3. 工作台不要堆大目录：`_build/`、`__pycache__/`、`*.zip` 都已被 `.gitignore` 排除，
   新产物也请放仓库外。

## 改代码的规矩

- **单源双版**：`common/main.c` 用 `#if TOUCH_DEMO` 分流。改公共逻辑时要确认两版都编得过。
  `check_parity.py` 会检查两版接线没分叉。
- **改资产必须重打包**：改了 `common/*.bin` 的源文件（素材图 / i18n / 字库），
  要重跑对应的 `scripts/_tools/gen_*.py`，再跑 `pack_assets.py --write`
  （按键版要加 `--variant btn`），最后 `pack_assets.py --verify` 必须逐字节一致。
- **文案三语齐全**：新增上屏中文串必须在 `i18n_table_data.h` 的 EN/JA/ZHT 三张表都补齐。
  构建闸门会拦下缺项、空值、未翻和繁体未转。
- **新页面要登记**：`page_render` 和 `touch_tap` 的 `case` 必须对页面枚举穷尽，
  漏一个 `check_page_dispatch.py` 就拦构建。
- **素材只放 `<epaper 根>/inbox`**。别再建 `_inbox` 之类的第二目录。

## 发版流程

1. 改完跑两版编译：
   ```bash
   bash build2.sh only
   bash build2_touch.sh only
   ```
   所有闸门必须全绿（三语、字库、页面分派、assets 一致性、两版接线）。
2. **版本号五处一致**：`common/main.c` 的 `APP_VER_MINOR` / `APP_VERSION` / `FW_BUILD`
   + 两份 `CMakeLists.txt` 的 `PROJECT_VER`，同一个 R 号。
3. 实机烧录验证（`app` 模式保 NVS），确认无 panic、启动日志版本正确。
4. commit 信息以 `R<号>:` 开头，一句话说清改了什么。

## 提交 PR

- 一个 PR 只做一件事。描述里写清楚：改了哪个版本、怎么验证的、有没有实机截图/日志。
- 不要提交编译产物、ZIP 包、NVS dump、个人 WiFi 配置或任何密钥。
- 如果你改了字体或其它第三方资产，在 PR 里说明来源和许可。

## 报 bug

给出：版本号（屏上关于页 / 串口 `R<号>`）、哪个版本（按键/触屏）、
复现步骤、串口日志（有的话）。别只说"不对"。
