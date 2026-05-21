# ESP32-S3 摇杆板 — 蓝牙与串口双链路说明

## 硬件（ESP32-S3-WROOM-1-N16R8）

- **USB 串口**：通过板载 USB 接 PC（CH343 / USB-Serial-JTAG），协议不变：`JOY …` 行 + `MODE CAL|GAME|SILENT` 命令。
- **蓝牙**：芯片 **内置 BLE 5.0**，**无需额外天线以外接线**（模组已集成天线）。与 PDF 中 S3 规格一致，使用 2.4 GHz BLE，不是经典蓝牙 SPP。
- **摇杆/按键/灯**：接线与原先 `main.c` 头注释相同（X/Y ADC、A–K GPIO、WS2812 GPIO48）。

## 固件行为（互斥单链路）

| 状态 | 串口 (USB) | BLE 广播 |
|------|------------|----------|
| 上电完成 | 等待首字节 | 广播 `QTgame-Joy` |
| PC 打开串口并发送数据 | **占用串口**，停广播 | 停止 |
| PC BLE 连接 | 忽略串口数据 | **占用 BLE**，已连接 |
| 断开当前链路 | 释放后可再广播 | 释放后可再收串口 |

- 串口释放：PC 关闭串口后，固件检测 `usb_serial_jtag_is_connected()` 为假时回到空闲。
- BLE 释放：主机断开 BLE 后重新广播。

## PC 端（QTgame）

- 「摇杆」页选择 **串口** 或 **蓝牙** 后连接；已连接一种方式时，另一种方式的「连接」不可用，须先 **断开**。
- BLE 使用 Nordic UART Service（与固件 UUID 一致），设备名 **`QTgame-Joy`**。

## 编译烧录

```bash
cd d:\esp32\project
idf.py set-target esp32s3
idf.py build flash monitor
```

首次启用蓝牙后若 `sdkconfig` 未更新，可执行：

```bash
idf.py fullclean
idf.py build
```
