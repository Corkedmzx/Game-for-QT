#include "host_transport.h"

#include "ble_nus_host.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

static const char *TAG = "host_link";
static bool s_stdin_nonblock;

static host_mode_byte_fn s_mode_byte_cb;
static host_link_t s_link = HOST_LINK_NONE;
static bool s_uart_claimed;

static void ble_rx_bridge(uint8_t b)
{
    if (s_mode_byte_cb && s_link == HOST_LINK_BLE) {
        s_mode_byte_cb(b);
    }
}

static void claim_uart_link(void)
{
    if (s_link != HOST_LINK_NONE) {
        return;
    }
    s_link = HOST_LINK_UART;
    s_uart_claimed = true;
    ble_nus_host_stop_advertise();
    ESP_LOGI(TAG, "Host link: UART (BLE advertising stopped)");
}

void host_transport_release_uart(void)
{
    if (s_link != HOST_LINK_UART) {
        return;
    }
    s_link = HOST_LINK_NONE;
    s_uart_claimed = false;
    ble_nus_host_advertise();
    ESP_LOGI(TAG, "Host link: none (UART released, BLE advertising)");
}

static void stdin_nonblock_init(void)
{
    if (s_stdin_nonblock) {
        return;
    }
    int fd = fileno(stdin);
    if (fd < 0) {
        return;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0) {
        s_stdin_nonblock = true;
    }
}

void host_transport_init(host_mode_byte_fn mode_byte_cb)
{
    s_mode_byte_cb = mode_byte_cb;
    s_link = HOST_LINK_NONE;
    s_uart_claimed = false;
    ble_nus_host_set_rx_callback(ble_rx_bridge);
    stdin_nonblock_init();
}

void host_transport_ble_start(void)
{
    ble_nus_host_init();
}

void host_transport_send_joy_line(const char *line, size_t len)
{
    if (!line || len == 0) {
        return;
    }
    if (s_link == HOST_LINK_BLE) {
        if (ble_nus_host_send((const uint8_t *)line, len) < 0) {
            return;
        }
        return;
    }
    if (s_link == HOST_LINK_UART) {
        fwrite(line, 1, len, stdout);
        fflush(stdout);
    }
}

void host_transport_poll_uart(void)
{
    if (s_link == HOST_LINK_BLE) {
        return;
    }

    int fd = fileno(stdin);
    if (fd < 0) {
        return;
    }

    for (;;) {
        unsigned char b;
        ssize_t n = read(fd, &b, 1);
        if (n != 1) {
            break;
        }
        /* 仅识别协议首字节，避免噪声误占用串口 */
        if (s_link == HOST_LINK_NONE && (b == 'M' || b == 'J')) {
            claim_uart_link();
        }
        if (s_mode_byte_cb && s_link == HOST_LINK_UART) {
            s_mode_byte_cb(b);
        }
    }
    /* 不按 usb_serial_jtag_is_connected() 自动释放：Windows 打开 COM 后该 API 仍常为 false */
}

host_link_t host_transport_active_link(void)
{
    return s_link;
}

/* 由 ble gap 在连接/断开时调用（在 ble_nus_host 的 gap_event 里需通知 — 通过外部符号） */
void host_transport_on_ble_connected(void)
{
    if (s_link == HOST_LINK_UART) {
        /* 串口已占用时不应再连上 BLE；若发生则断开 BLE 连接 */
        return;
    }
    s_link = HOST_LINK_BLE;
    ble_nus_host_stop_advertise();
    ESP_LOGI(TAG, "Host link: BLE");
}

void host_transport_on_ble_disconnected(void)
{
    if (s_link == HOST_LINK_BLE) {
        s_link = HOST_LINK_NONE;
        ble_nus_host_advertise();
        ESP_LOGI(TAG, "Host link: none (BLE disconnected)");
    }
}
