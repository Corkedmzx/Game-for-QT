#pragma once

#include <QtGlobal>

#include <atomic>

/** 串口摇杆归一化状态，供校准页与游戏共享（原子字段可与模拟线程并发读写） */
struct SharedJoyState {
    std::atomic<float> nx{0.f}; /**< -1..1，右为正 */
    std::atomic<float> ny{0.f}; /**< -1..1，上为正（推杆向上屏幕往上飞） */
    std::atomic<quint32> buttonMask{0};
    std::atomic<bool> serialConnected{false};
};

/** 与固件 JOY 行低 8 位一致：A=bit0 … */
namespace JoyMask {
inline constexpr quint32 A = 1u << 0;
inline constexpr quint32 B = 1u << 1;
inline constexpr quint32 C = 1u << 2;
inline constexpr quint32 D = 1u << 3;
}
