/*
 * 模拟摇杆扩展板（Keyes 类）+ WS2812(GPIO48)
 *
 * 接线（扩展板右侧双排丝印，竖列为一对）：
 *   X → GPIO6，Y → GPIO7（ADC）
 *   A→9 B→10 C→11 D→12 E→13 F→14 K→15（内部上拉，按下多为低电平）
 *   GND 共地；V / 3V3 按模块供电接 ESP32 3V3（勿超过模块允许电压）。
 *
 * 灯效：任意侧键/K 按下 → 固定颜色；仅摇杆 → 偏移映射 R/G + 固定 B。
 *
 * 上位机：USB 串口或 BLE（Nordic UART）二选一连接；输出相对中心的偏移（死区、滤波、按键防抖）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_adc/adc_oneshot.h"

#include "host_transport.h"
#include "led_strip_encoder.h"

static const char *TAG = "joy_led";

typedef enum {
    BTN_A = 0,
    BTN_B,
    BTN_C,
    BTN_D,
    BTN_E,
    BTN_F,
    BTN_K,
    BTN_COUNT
} btn_index_t;

#define RMT_RESOLUTION_HZ    10000000
#define RGB_GPIO             48
#define LED_NUM              1

#define JOY_X_GPIO           6
#define JOY_Y_GPIO           7

#define BTN_GPIO_A           9
#define BTN_GPIO_B           10
#define BTN_GPIO_C           11
#define BTN_GPIO_D           12
#define BTN_GPIO_E           13
#define BTN_GPIO_F           14
#define BTN_GPIO_K           15

#define ADC_ATTEN            ADC_ATTEN_DB_12

/** 摇杆偏离中心时的缩放分母，偏大则灯变化更钝 */
#define STICK_SCALE_DIV      500

/** 串口摇杆控制：死区（ADC 原始差值），区内视为回中，抑制漂移 */
#define JOY_DEADZONE_ADC     220

/** 摇杆 ADC 一阶低通：new = (LP_ALPHA * old + vx) / LP_DIV，越大越跟手、越小越稳 */
#define JOY_LP_ALPHA         7
#define JOY_LP_DIV           8

/** 按键防抖：同一 mask 连续出现多少次才认定有效 */
#define BTN_DEBOUNCE_READS   4

static uint8_t s_pixels[LED_NUM * 3];

static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_ch_x;
static adc_channel_t s_ch_y;
static adc_unit_t s_adc_unit;

static int s_center_x;
static int s_center_y;

/** 摇杆低通状态（仅 ADC 成功时更新） */
static int s_lp_x;
static int s_lp_y;
static bool s_lp_inited;

/** 按键防抖状态 */
static uint8_t s_btn_candidate;
static uint8_t s_btn_stable;
static int s_btn_same_count;

static const int s_btn_gpios[BTN_COUNT] = {
    BTN_GPIO_A,
    BTN_GPIO_B,
    BTN_GPIO_C,
    BTN_GPIO_D,
    BTN_GPIO_E,
    BTN_GPIO_F,
    BTN_GPIO_K,
};

static void led_set_grb(rmt_channel_handle_t ch, rmt_encoder_handle_t enc, uint8_t r, uint8_t g, uint8_t b)
{
    s_pixels[0] = g;
    s_pixels[1] = b;
    s_pixels[2] = r;
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    /* 仅投递发送，不阻塞等待 rmt_tx_wait_all_done（该等待在串口繁忙时会反复 flush timeout） */
    (void)rmt_transmit(ch, enc, s_pixels, sizeof(s_pixels), &tx_cfg);
}

/** 上次已写入 LED 的颜色，避免每圈刷新 RMT 造成队列堆积 */
static uint8_t s_led_last_r;
static uint8_t s_led_last_g;
static uint8_t s_led_last_b;
static bool s_led_inited;

static void buttons_gpio_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < BTN_COUNT; i++) {
        mask |= (1ULL << s_btn_gpios[i]);
    }
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

/** 按下为低电平 → 返回 1<<btn */
static uint8_t buttons_read_mask(void)
{
    uint8_t m = 0;
    for (int i = 0; i < BTN_COUNT; i++) {
        if (gpio_get_level(s_btn_gpios[i]) == 0) {
            m |= (uint8_t)(1u << i);
        }
    }
    return m;
}

static esp_err_t adc_hw_init(void)
{
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(JOY_X_GPIO, &s_adc_unit, &s_ch_x));
    adc_unit_t uy;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(JOY_Y_GPIO, &uy, &s_ch_y));
    if (uy != s_adc_unit) {
        ESP_LOGE(TAG, "X/Y 不在同一 ADC 单元，请换 GPIO");
        return ESP_ERR_NOT_SUPPORTED;
    }

    adc_oneshot_unit_init_cfg_t init = {
        .unit_id = s_adc_unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init, &s_adc));

    adc_oneshot_chan_cfg_t ch_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_ch_x, &ch_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_ch_y, &ch_cfg));

    ESP_LOGI(TAG, "ADC 单元%u：X=GPIO%d → ch%u，Y=GPIO%d → ch%u",
             (unsigned)s_adc_unit + 1u, JOY_X_GPIO, (unsigned)s_ch_x, JOY_Y_GPIO, (unsigned)s_ch_y);
    return ESP_OK;
}

static void calibrate_center(void)
{
    int sx = 0, sy = 0, n = 0;
    for (int i = 0; i < 24; i++) {
        int vx, vy;
        if (adc_oneshot_read(s_adc, s_ch_x, &vx) != ESP_OK) {
            continue;
        }
        if (adc_oneshot_read(s_adc, s_ch_y, &vy) != ESP_OK) {
            continue;
        }
        sx += vx;
        sy += vy;
        n++;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (n > 0) {
        s_center_x = sx / n;
        s_center_y = sy / n;
        ESP_LOGI(TAG, "摇杆中点 raw：X≈%d Y≈%d（请保持摇杆居中后上电；重启可重采）", s_center_x, s_center_y);
    } else {
        s_center_x = 2048;
        s_center_y = 2048;
        ESP_LOGW(TAG, "中点校准失败，使用默认 2048");
    }
}

/** 低通滤波 + 相对中心的偏移（用于串口：死区内为 0） */
static void joy_filter_and_delta(int raw_x, int raw_y,
                                 bool adc_ok,
                                 int *out_dx, int *out_dy)
{
    if (!adc_ok) {
        *out_dx = 0;
        *out_dy = 0;
        return;
    }

    if (!s_lp_inited) {
        s_lp_x = raw_x;
        s_lp_y = raw_y;
        s_lp_inited = true;
    } else {
        s_lp_x = (JOY_LP_ALPHA * s_lp_x + raw_x) / JOY_LP_DIV;
        s_lp_y = (JOY_LP_ALPHA * s_lp_y + raw_y) / JOY_LP_DIV;
    }

    int dx = s_lp_x - s_center_x;
    int dy = s_lp_y - s_center_y;

    if (dx > -JOY_DEADZONE_ADC && dx < JOY_DEADZONE_ADC) {
        dx = 0;
    }
    if (dy > -JOY_DEADZONE_ADC && dy < JOY_DEADZONE_ADC) {
        dy = 0;
    }

    *out_dx = dx;
    *out_dy = dy;
}

/** 防抖后的按键掩码（按下为低 → bit 置 1） */
static uint8_t buttons_read_mask_debounced(uint8_t raw_mask)
{
    if (raw_mask == s_btn_candidate) {
        if (s_btn_same_count < 1000) {
            s_btn_same_count++;
        }
    } else {
        s_btn_candidate = raw_mask;
        s_btn_same_count = 1;
    }

    if (s_btn_same_count >= BTN_DEBOUNCE_READS) {
        s_btn_stable = s_btn_candidate;
    }
    return s_btn_stable;
}

static uint8_t scale_stick(int raw, int center)
{
    int d = raw - center;
    int ad = d >= 0 ? d : -d;
    int v = (ad * 255) / STICK_SCALE_DIV;
    if (v > 255) {
        v = 255;
    }
    return (uint8_t)v;
}

static bool btn_color(uint8_t pressed_mask, uint8_t *r, uint8_t *g, uint8_t *b)
{
    static const uint8_t cols[BTN_COUNT][3] = {
        { 255, 0, 0 },
        { 0, 255, 0 },
        { 0, 0, 255 },
        { 255, 255, 0 },
        { 255, 0, 255 },
        { 0, 255, 255 },
        { 255, 255, 255 },
    };
    for (int i = 0; i < BTN_COUNT; i++) {
        if (pressed_mask & (1u << i)) {
            *r = cols[i][0];
            *g = cols[i][1];
            *b = cols[i][2];
            return true;
        }
    }
    return false;
}

/** GAME：有操作才发、限频（手感足够，减轻 PC 串口与解析压力） */
#define GAME_MIN_INTERVAL_US    (33 * 1000) /**< ~30 行/秒上限 */
#define GAME_STICK_THRESH_ADC    14          /**< 位移阈值略大，抑制噪声与小抖动 */

/** CAL：校准页需连续帧（摇杆居中时 dx=dy=0 也必须上报，否则 PC 无法凑满学习样本） */
#define CAL_STREAM_INTERVAL_US  (25 * 1000) /**< ~40 行/秒，足够采样且流量可控 */

/** PC 下发：MODE SILENT | MODE CAL | MODE GAME（经 UART 或 USB-Serial-JTAG 控制台输入） */
typedef enum {
    JOY_MODE_SILENT = 0,
    JOY_MODE_CAL,
    JOY_MODE_GAME
} joy_uart_mode_t;

/*
 * 默认 SILENT：上电完成 ADC 中点采样后不发 JOY；由 PC 发 MODE CAL（校准页）或 MODE GAME（游戏）。
 * 避免校准完成后仍按 ms 级刷屏；亦减轻 USB 串口与 Qt 解析负载。
 */
static joy_uart_mode_t s_joy_mode = JOY_MODE_SILENT;

#define MODE_CMD_MAX 80
static char s_mode_cmd[MODE_CMD_MAX];
static size_t s_mode_cmd_len;

static int s_out_dx;
static int s_out_dy;
static uint8_t s_out_mask;
static int64_t s_out_t_us;
static bool s_out_valid;

/** 收到 MODE CAL / MODE GAME 后首圈只同步 shadow，不输出 JOY（静止时零刷屏） */
static bool s_shadow_sync_pending;

static void process_mode_byte(uint8_t b)
{
    if (b == '\r') {
        return;
    }
    if (b == '\n') {
        s_mode_cmd[s_mode_cmd_len] = '\0';
        s_mode_cmd_len = 0;
        if (strcmp(s_mode_cmd, "MODE SILENT") == 0) {
            s_joy_mode = JOY_MODE_SILENT;
            host_transport_release_uart();
        } else if (strcmp(s_mode_cmd, "MODE CAL") == 0) {
            s_joy_mode = JOY_MODE_CAL;
            /* CAL 必须立即开始周期上报，不做 GAME 那种 shadow 静默同步 */
            s_shadow_sync_pending = false;
            s_out_t_us = 0; /* 首帧立即输出，避免进入校准后短暂无数据 */
        } else if (strcmp(s_mode_cmd, "MODE GAME") == 0) {
            s_joy_mode = JOY_MODE_GAME;
            s_shadow_sync_pending = true;
        }
        return;
    }
    if (s_mode_cmd_len + 1 < MODE_CMD_MAX) {
        s_mode_cmd[s_mode_cmd_len++] = (char)b;
    }
}

static void joy_print_line(int dx, int dy, uint8_t mask, bool adc_ok)
{
    char buf[48];
    int n;
    if (adc_ok) {
        n = snprintf(buf, sizeof(buf), "JOY %d %d %02x\n", dx, dy, (unsigned)mask);
    } else {
        n = snprintf(buf, sizeof(buf), "JOY -1 -1 %02x\n", (unsigned)mask);
    }
    if (n > 0) {
        host_transport_send_joy_line(buf, (size_t)n);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "模拟摇杆：X=GPIO%d Y=GPIO%d；按键 A..F,K=GPIO%d-%d",
             JOY_X_GPIO, JOY_Y_GPIO, BTN_GPIO_A, BTN_GPIO_K);

    buttons_gpio_init();
    ESP_ERROR_CHECK(adc_hw_init());
    calibrate_center();

    rmt_channel_handle_t led_chan = NULL;
    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = RGB_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .trans_queue_depth = 16,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &led_chan));

    rmt_encoder_handle_t enc = NULL;
    led_strip_encoder_config_t enc_cfg = { .resolution = RMT_RESOLUTION_HZ };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&enc_cfg, &enc));
    ESP_ERROR_CHECK(rmt_enable(led_chan));

    host_transport_init(process_mode_byte);
    host_transport_ble_start();

    /*
     * 链路：USB 串口 115200 8N1 或 BLE NUS，二选一（见 host_transport.c）。
     * 行格式 "JOY <dx> <dy> <mask_hex>\\n"
     * SILENT：不发；CAL：固定节拍连续发；GAME：变化+限频。
     */
    while (1) {
        host_transport_poll_uart();

        uint8_t raw_btn = buttons_read_mask();
        uint8_t mask = buttons_read_mask_debounced(raw_btn);

        int vx = 0;
        int vy = 0;
        esp_err_t rax = adc_oneshot_read(s_adc, s_ch_x, &vx);
        esp_err_t ray = adc_oneshot_read(s_adc, s_ch_y, &vy);
        const bool adc_ok = (rax == ESP_OK && ray == ESP_OK);

        int dx = 0;
        int dy = 0;
        joy_filter_and_delta(vx, vy, adc_ok, &dx, &dy);

        if (s_shadow_sync_pending && s_joy_mode == JOY_MODE_GAME) {
            s_out_dx = dx;
            s_out_dy = dy;
            s_out_mask = mask;
            s_out_t_us = esp_timer_get_time();
            s_out_valid = true;
            s_shadow_sync_pending = false;
        }

        bool should_print = false;
        if (s_joy_mode == JOY_MODE_CAL) {
            int64_t now = esp_timer_get_time();
            if (now - s_out_t_us >= CAL_STREAM_INTERVAL_US) {
                should_print = true;
            }
        } else if (s_joy_mode == JOY_MODE_GAME) {
            int64_t now = esp_timer_get_time();
            const int stick_thr = GAME_STICK_THRESH_ADC;
            const int64_t min_gap_us = GAME_MIN_INTERVAL_US;

            bool mask_chg = (mask != s_out_mask);
            int ddx = dx - s_out_dx;
            int ddy = dy - s_out_dy;
            if (ddx < 0) {
                ddx = -ddx;
            }
            if (ddy < 0) {
                ddy = -ddy;
            }
            bool stick_chg = (ddx >= stick_thr || ddy >= stick_thr);
            if (mask_chg) {
                should_print = true;
            } else if (stick_chg && (now - s_out_t_us >= min_gap_us)) {
                should_print = true;
            }
        }

        if (should_print) {
            joy_print_line(dx, dy, mask, adc_ok);
            s_out_dx = dx;
            s_out_dy = dy;
            s_out_mask = mask;
            s_out_t_us = esp_timer_get_time();
            s_out_valid = true;
        }

        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        bool have_rgb = false;
        if (btn_color(mask, &r, &g, &b)) {
            have_rgb = true;
        } else if (adc_ok) {
            r = scale_stick(s_lp_x, s_center_x);
            g = scale_stick(s_lp_y, s_center_y);
            b = 48;
            have_rgb = true;
        }
        if (have_rgb && (!s_led_inited || r != s_led_last_r || g != s_led_last_g || b != s_led_last_b)) {
            led_set_grb(led_chan, enc, r, g, b);
            s_led_last_r = r;
            s_led_last_g = g;
            s_led_last_b = b;
            s_led_inited = true;
        }

        int delay_ms = 20;
        if (s_joy_mode == JOY_MODE_CAL || s_joy_mode == JOY_MODE_GAME) {
            delay_ms = 15;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
