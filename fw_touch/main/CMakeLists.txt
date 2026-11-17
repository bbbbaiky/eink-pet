idf_component_register(
    # ★2026-10-17：ota_up.c = OTA 线上升级（SRCS 列表里不能写 /* 注释 */ —— CMake 会把它当源文件名）
    SRCS "../../common/main.c" "../../common/net_sync.c" "../../common/net_prov.c"
         "../../common/ota_up.c"
         "../../common/ota_net.c"
         "../../common/friend.c"
         "../../common/friend_net.c" "../../common/api_image.c"
    "../../common/friend_ui.c"
         "../../common/loot_system.c"
         "../../common/exp_system.c"
         "../../common/shop_system.c"
         "../../common/post_system.c"
         "../../common/assets_rw.c"
         "../../common/fb_core.c"
         "../../common/text_core.c"
         "../../common/ui_asset.c"
         "touch_ft6336.c"
    INCLUDE_DIRS "." "../../common"
    # v6 分层渲染 + 真实世界同步：
    #   房间×4 + 窗贴片×48 + 月相×8 + 猫精灵×24 + 关机页。
    #   固件运行时把各层按 idx 组合进 40KB 帧缓冲，再打包成 5KB 推屏。
    #   加地标 = 追加 win.bin 的贴片；加姿态 = 追加 cat.bin 精灵；
    #   加月相 = 追加 moon.bin 的贴片，都不用改这里的文件列表。
    # ⛔ strip.bin 已下架（2026-09-16：顶部规格条整条去掉）。
    # ★★ 2026-09-19 新增：UI 页层 + 食盆贴片
    #   ui.bin    12 页 × 5000B：功能页×5（选中态）+ 食物详情页×4 + 配网引导 + 配网成功 + 还在开发中
    #             由 _preview/build_ui_pages.py 从用户设计稿打出（设备端不跑字体）
    #   bowl.bin  食盆贴片 8 槽：4 种食物 ×（满 / 残渣），每片 = u16 几何 + A 覆盖平面 + K 墨平面
    #   off.bin   关机页已改成 **Z Labs 像素 12px**（同 build_ui_pages.py 一起出）
    # ★★ 2026-09-21 新增：首页提示贴片（格式与 bowl.bin 同构）
    # ★★ 2026-09-25 深夜：升级为 patch.bin **状态贴片统一仓** 5 片 ——
    #      [0] 外出立牌 · [1] 明信片角标 · [2..4] WiFi 有/弱/无（28×28 @ (7,8)）
    #      （WiFi 三态从 cat.bin 尾部迁出 —— 用户口径：状态贴片和猫姿态是两类素材）
    #      hint.bin 退役（数据已迁入 patch.bin，备份在 hw/backup/）
    # ★★★ 2026-10-16 拆分重构⑥：**EMBED_FILES 已清空** —— 所有内容资产（明信片/UI/字库/
    #   藏品图标/动画/猫姿/窗/房/月相/食盆/贴片/功能图标/关机页）从「内嵌进 app」改为
    #   打进**独立 assets 数据分区**（单份），app 只留纯代码。
    #   为什么：内嵌 ⇒ A/B 两个 OTA 槽各存一份（≈1.9MB 纯重复），且明信片一长就把两个槽
    #   一起顶大；搬出去后明信片还能单独 OTA、不加固件。详见 docs/OTA联网升级_交互方案_20261016.md §8.6/§11。
    #   ⚠ 资产不再由本文件管理 → 打包/烧录全在 scripts/_tools/pack_assets.py 与
    #     fw/build2.sh 的 migrate 段；**改资产别再往这里加回来**。
    #   （zfont.h / loot_icons.h 仍以 #include 方式参与编译，但已瘦身成只剩常量/断言，
    #     表体在 assets 分区的 zfont.bin / loot_icons.bin 里。）
    # ★★ 依赖清单 —— 这是踩过的坑，别删：
    #   一旦 `REQUIRES` 里列了东西，**原来靠 PRIV_REQUIRES 隐式传递的公共依赖
    #   不会被清空**，但 `driver` 这类**聚合组件**必须显式列出，否则
    #   `driver/gpio.h` / `driver/spi_master.h` 会报 "No such file or directory"。
    #   实测：加 REQUIRES 后 main.c 立刻编不过，就是因为漏了 driver / esp_timer。
    REQUIRES
        driver
        esp_timer
        esp_adc
        esp_partition   # ★2026-10-16 拆分：assets.h 用 esp_partition_find_first/read 读资产分区
        # 联网：WiFi + SNTP 时间 + HTTP 客户端 + 配网网页服务器
        esp_wifi
        esp_netif
        nvs_flash
        esp_http_client
        esp_http_server
        json
        lwip
        mbedtls   # ★G（2026-10-16）：net_sync ws_api_diary 走 HTTPS → esp_crt_bundle.h（证书束）
        sdmmc      # ★2026-10-19：本地升级 ① TF 卡（SDIO 1-bit，GPIO39/41/40）
        fatfs      # ★2026-10-19：TF 卡 FAT 文件系统挂载
        app_update     # esp_ota_get_next_update_partition / 回滚标记
        mqtt           # ★2026-10-24 好友跨设备链路（friend_net.c → mqtt_client.h）
        espressif__esp_jpeg   # ★2026-10-24 深夜 图像通道（api_image.c → jpeg_decoder.h）
    )
# ★★ 2026-10-10（代码遍历批次）加：**严格告警**。
#   背景：原工程只走 IDF 默认的 -Wall，漏掉 -Wextra / -Wshadow，
#   遍历时才发现按键版一直挂着一条 unused-variable 告警没人看见。
#   加这一行之后**两版都是零告警**（按键 0x21cd90 / 触屏 0x2221b0），
#   所以它不是"吵"，是**防回归**：以后谁写出未用变量 / 变量遮蔽 / 隐式符号
#   转换都会立刻冒出来。
# ⚠ 别因为"编译输出多了几行"就删掉 —— 要删请先确认当前确实是零告警。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wshadow -Wno-unused-parameter)
