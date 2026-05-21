#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** NimBLE Nordic UART Service，供 PC Qt 蓝牙连接 */

void ble_nus_host_init(void);

/** 在未占用串口链路时广播；已连接 BLE 时无操作 */
void ble_nus_host_advertise(void);

void ble_nus_host_stop_advertise(void);

bool ble_nus_host_connected(void);

/** 经 TX 特征 notify 发送数据（须已连接且主机已订阅） */
int ble_nus_host_send(const uint8_t *data, size_t len);

/** 注册 RX 写入回调（按字节喂给 MODE 解析） */
typedef void (*ble_nus_rx_byte_fn)(uint8_t b);
void ble_nus_host_set_rx_callback(ble_nus_rx_byte_fn cb);
