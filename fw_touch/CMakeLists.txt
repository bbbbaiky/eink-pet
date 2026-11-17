cmake_minimum_required(VERSION 3.16)

# ★★ 2026-10-19 本地升级：PROJECT_VER 写进 bin 内嵌 esp_app_desc（偏移 0x3A），
# 网页/TF 卡升级时设备从 bin 头部解析它当「新版本号」上屏（ota_up.c）。
# ⚠ bump APP_VERSION（common/main.c）时**必须同步改这里**，两边不一致 = 上屏假版本。
# ★R122 修：原值 "v6.2" 落后于 APP_VERSION "v6.4" ⇒ 卡内 bin 内嵌版本只报 6.2，
#   ota_ver_newer() 与设备 6.4 比较**永远判「不新」** ⇒ TF 卡本地升级永远显示「已是最新」。
#   ⇒ PROJECT_VER 必须与 APP_VER_MAJOR/MINOR 同步，且写**纯数字**（与 APP_VERSION 同源；
#     具体值只看下一行 set()，注释里不写死数字，免得又落后于版本）。
set(PROJECT_VER "0.228")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(pet_epaper_touch)   # ★触屏版产物 pet_epaper_touch.bin（与按键版 pet_epaper_demo.bin 区分）
