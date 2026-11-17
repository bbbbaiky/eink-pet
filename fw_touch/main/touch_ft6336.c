/* ============================================================================
 * touch_ft6336.c —— FT6336 触摸驱动（轮询版，里程碑 1）
 * 详见头文件说明。日志 tag = "touch"。
 * ==========================================================================*/
#include "touch_ft6336.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "touch";

/* 引脚（对照头文件说明；错了好改 —— 就这一处） */
#define PIN_TP_SDA      47
#define PIN_TP_SCL      48
#define PIN_TP_RST      7
#define PIN_TP_INT      21      /* 本驱动不用中断，配输入悬空即可 */

#define FT_ADDR         0x38
#define REG_TD_STATUS   0x02
#define REG_TOUCH1_XH   0x03
#define REG_FIRMID      0xA8

static i2c_master_bus_handle_t s_bus;    /* 总线句柄（i2c_new_master_bus 出参） */
static i2c_master_dev_handle_t s_dev;    /* 设备句柄（i2c_master_bus_add_device 出参） */

static int rd_reg(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 50 /*ms*/);
}

int tp_init(void)
{
    /* INT 配输入即可（轮询模式不依赖它） */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_TP_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    /* 复位：低 10ms → 高，等 200ms 让 FT6336 起来 */
    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << PIN_TP_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst);
    gpio_set_level((gpio_num_t)PIN_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)PIN_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* I2C 总线（触屏版固件独占：没有 RTC/传感器驱动共用） */
    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t)PIN_TP_SDA,
        .scl_io_num = (gpio_num_t)PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* 板上大概率有外部上拉，内部再叠一层无害 */
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &s_bus));

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = FT_ADDR,
        .scl_speed_hz = 400 * 1000,
    };
    esp_err_t e = i2c_master_bus_add_device(s_bus, &dev, &s_dev);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2C add dev 失败 %s", esp_err_to_name(e));
        return -1;
    }

    /* 探活：读固件版本寄存器 0xA8（读得动就算通） */
    uint8_t id = 0;
    e = rd_reg(REG_FIRMID, &id, 1);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "FT6336 无应答（%s）—— 第一嫌疑：TP 引脚定义（SDA=%d SCL=%d RST=%d）",
                 esp_err_to_name(e), PIN_TP_SDA, PIN_TP_SCL, PIN_TP_RST);
        return -1;
    }
    ESP_LOGI(TAG, "FT6336 在线 FIRMID=0x%02X", id);
    return 0;
}

int tp_poll(int *x, int *y)
{
    static int err_quiet;                       /* I2C 连错限频打日志 */
    uint8_t d[1 + 6 * 2];                       /* 状态 + 两点（单人触摸够用） */
    if (rd_reg(REG_TD_STATUS, d, sizeof d) != ESP_OK) {
        if (++err_quiet % 50 == 1)
            ESP_LOGW(TAG, "I2C 读失败（限频日志 x%d）", err_quiet);
        return -1;
    }
    int n = d[0] & 0x0F;
    if (n == 0 || n > 2)
        return 0;

    const uint8_t *p = d + 1;                   /* 触摸点 1 */
    int px = ((p[0] & 0x0F) << 8) | p[1];
    int py = ((p[2] & 0x0F) << 8) | p[3];
    *x = px;
    *y = py;
    return 1;
}
