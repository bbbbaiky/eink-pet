/* ★★ 2026-10-08 治本：工程变体开关。
   本文件**每个工程各一份**（放在各自 main/ 下，靠 INCLUDE_DIRS "." 被 fw/common/main.c 找到），
   唯一职责 = 告诉共用源「我是哪一版」：

     TOUCH_DEMO = 1  → 触屏版 fw_touch    ：触摸交互 + 读 assets_touch/ui_touch.bin + 编触摸驱动
     TOUCH_DEMO = 0  → 按键版 fw_epaper_demo：BOOT/POWER 按键交互 + 读 assets/ui.bin

   ⚠ 别把具体的业务宏往这里塞 —— 这里是**变体身份**，不是杂物间。
   ⚠ 改这个值等于换一台设备，改完两边都要重新编译验证。 */
#ifndef VARIANT_H
#define VARIANT_H

#define TOUCH_DEMO  1       /* 触屏版 */

#endif /* VARIANT_H */
