# QT 摇杆项目（ESP32 + QTgame 测试应用）

基于 **ESP32-S3** 的模拟摇杆扩展板固件，配套 **Qt 6** 桌面端 **QTgame**，用于验证摇杆输入（USB 串口 / BLE）、校准与多款小游戏操控。仓库同时提供 **Windows x64 预编译包**，无需自行编译即可连接硬件试玩。

## 功能概览

| 模块 | 说明 |
|------|------|
| **摇杆固件** | 读取 X/Y ADC、按键 A～F/K，WS2812 灯效；经 USB 串口或 BLE（Nordic UART）向上位机发送 `JOY` 行 |
| **QTgame** | 摇杆连接/校准页 + 游戏大厅；支持串口与蓝牙二选一连接 |
| **内置游戏** | 雷霆战机、俄罗斯方块、2048、中国象棋、国际象棋（均需 `MODE GAME` 后接收摇杆流） |

上位机与固件为**单链路互斥**：同一时刻仅 USB 串口或 BLE 之一占用；断开当前链路后，固件可恢复另一种连接方式（详见 [`摇杆-ESP32/HARDWARE_BLE.md`](摇杆-ESP32/HARDWARE_BLE.md)）。

## 目录结构

```
.
├── README.md                 # 本文件
├── 摇杆-ESP32/               # ESP-IDF 固件（目标芯片 ESP32-S3）
│   ├── main/                 # 摇杆采样、灯效、串口/BLE 传输
│   ├── HARDWARE_BLE.md       # 双链路（串口 / BLE）行为说明
│   └── 摇杆与ESP32引脚接线说明.md
├── QT源码-仅供参考/          # Qt 6 工程源码（CMake）
└── QTgame-win64/             # Windows 64 位预编译运行包（含 Qt 运行时 DLL）
```

> **上传 Git 建议**：勿提交 `摇杆-ESP32/build/`、Qt 的 `build/`、`.qtcreator/` 等构建产物；可添加 `.gitignore` 忽略 `build/`、`*.user`、`sdkconfig.old` 等。

## 硬件要求

- **开发板**：ESP32-S3（工程默认 N16R8 类模组；板载 USB 串口 + 内置 BLE）
- **摇杆**：Keyes 类模拟摇杆扩展板（X/Y + 侧键 A～F + 中央键 K）
- **接线**：以固件 [`摇杆-ESP32/main/main.c`](摇杆-ESP32/main/main.c) 与 [`摇杆-ESP32/摇杆与ESP32引脚接线说明.md`](摇杆-ESP32/摇杆与ESP32引脚接线说明.md) 为准  

  | 功能 | GPIO |
  |------|------|
  | X / Y | 6 / 7 |
  | A～F / K | 9～14 / 15 |
  | WS2812 | 48 |

- **PC 端**：Windows 10/11（64 位）；使用蓝牙时需系统蓝牙可用，且 Qt 使用 **Qt Bluetooth** 模块

## 通信协议（简要）

固件与 QTgame 通过**文本行**交互（串口波特率由 USB 虚拟串口决定；BLE 走 Nordic UART Service，广播名 **`QTgame-Joy`**）。

**下位机 → 上位机（摇杆数据）**

```
JOY <dx> <dy> <mask_hex>
```

- `dx`、`dy`：相对中心的偏移（整数，经死区与滤波）
- `mask_hex`：按键位掩码低 8 位（`A=bit0` …，与 [`QT源码-仅供参考/shared_joy_state.h`](QT源码-仅供参考/shared_joy_state.h) 中 `JoyMask` 一致）

**上位机 → 下位机（模式）**

| 命令 | 含义 |
|------|------|
| `MODE SILENT` | 静默，不上报 `JOY`（默认上电态） |
| `MODE CAL` | 校准页：持续上报，用于中点与方向标定 |
| `MODE GAME` | 游戏页：按变化上报，降低流量 |

## 快速开始（预编译 QTgame）

1. 将 `摇杆-ESP32` 固件烧录到 ESP32-S3（见下文「固件编译与烧录」）。
2. 用 USB 连接开发板，或等待 BLE 广播 **`QTgame-Joy`**。
3. 进入 `QTgame-win64/`，运行 **`QTgame.exe`**。
4. 打开应用内 **「摇杆」** 标签：
   - **串口**：选择对应 COM 口 → 连接 → 校准后可切到 **「游戏」** 标签试玩；
   - **蓝牙**：扫描并连接 `QTgame-Joy`（须先断开串口，反之亦然）。
5. 进入具体游戏前，应用会向固件发送 `MODE GAME`；返回摇杆页或校准时会切换为 `MODE CAL` 等（逻辑见 `joystick_setup_widget`）。

## 固件编译与烧录

**环境**：[ESP-IDF](https://docs.espressif.com/projects/esp-idf/)（建议 v5.x），已安装工具链并设置 `IDF_PATH`。

```bash
cd 摇杆-ESP32
idf.py set-target esp32s3
idf.py build
idf.py -p <COM端口> flash monitor
```

首次启用蓝牙后若配置异常，可执行：

```bash
idf.py fullclean
idf.py build
```

更多链路说明见 [`摇杆-ESP32/HARDWARE_BLE.md`](摇杆-ESP32/HARDWARE_BLE.md)。

## QTgame 源码编译

**环境**

- Qt **6.x**（Widgets、**SerialPort**、**Bluetooth**）
- CMake ≥ 3.16，C++17 编译器（Windows 推荐 MSVC 2022 64-bit）

**步骤**

```bash
cd QT源码-仅供参考
cmake -B build -S . -DCMAKE_PREFIX_PATH=<你的Qt安装路径>/msvc2022_64
cmake --build build --config Release
```

或在 **Qt Creator** 中打开 `QT源码-仅供参考/CMakeLists.txt`，选择对应 Kit 构建。

构建产物为 `QTgame` 可执行文件；Windows 发布时需一并部署 Qt 相关 DLL（可参考 `QTgame-win64/` 中的文件布局）。

## 按键与游戏（参考）

- **摇杆页**：连接设备、串口/BLE 切换、摇杆中点校准、原始数据预览。
- **游戏页大厅**（串口已连接时）：**B** 切换游戏焦点，**A** 确认进入，**C** 切换标签页（与游戏内摇杆逻辑互斥，见 `MainWindow`）。
- 各游戏内映射见对应 `*_window.cpp`（如雷霆战机、方块、象棋等）。

## 常见问题

| 现象 | 建议 |
|------|------|
| 搜不到蓝牙设备 | 确认固件已烧录、未被 USB 串口占用；PC 蓝牙已开启；设备名应为 `QTgame-Joy` |
| 串口连上无数据 | 在摇杆页连接后发送 `MODE CAL` / `MODE GAME`；检查是否另一终端占用 COM 口 |
| 摇杆漂移 | 在摇杆页执行重新校准；固件侧有 ADC 死区与低通（见 `main.c` 宏） |
| Qt 蓝牙不可用 | 确认安装时勾选 Bluetooth 模块；Windows 需系统蓝牙栈正常 |

## 许可证与声明

- `QT源码-仅供参考/` 目录名表示源码供集成与二次开发参考，请按你方项目需要调整工程名、资源与发布方式。
- 预编译包 `QTgame-win64/` 内含 Qt 运行时，再分发时请遵守 [Qt 许可协议](https://www.qt.io/licensing)。
- ESP-IDF 与乐鑫 SDK 遵循其各自开源许可。

## 相关文档

- [摇杆与 ESP32 引脚接线说明](摇杆-ESP32/摇杆与ESP32引脚接线说明.md)
- [蓝牙与串口双链路说明](摇杆-ESP32/HARDWARE_BLE.md)
