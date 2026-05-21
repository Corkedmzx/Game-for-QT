/*
 * 上位机链路：USB 串口（控制台 stdin/stdout）与 BLE Nordic UART 二选一。
 * 上电后 BLE 广播等待；串口收到首字节则占用串口并停广播。
 * BLE 连接则占用 BLE 并忽略串口数据。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    HOST_LINK_NONE = 0,
    HOST_LINK_UART,
    HOST_LINK_BLE,
} host_link_t;

/** 每收到 MODE 命令流中的一个字节时回调（与原先 process_mode_byte 一致） */
typedef void (*host_mode_byte_fn)(uint8_t b);

void host_transport_init(host_mode_byte_fn mode_byte_cb);

/** 启动 BLE 栈并开始广播（在 app_main 中 ADC 等初始化完成后调用） */
void host_transport_ble_start(void);

/** 发送一行 JOY 文本（自动选择当前链路；无链路时不发送） */
void host_transport_send_joy_line(const char *line, size_t len);

/** 轮询 USB 串口命令（主循环调用） */
void host_transport_poll_uart(void);

/** PC 发 MODE SILENT 或主动断开串口时调用，恢复 BLE 广播 */
void host_transport_release_uart(void);

host_link_t host_transport_active_link(void);

/** 由 BLE GAP 事件调用（勿在应用层直接调用） */
void host_transport_on_ble_connected(void);
void host_transport_on_ble_disconnected(void);
