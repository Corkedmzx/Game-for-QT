"""
MicroPython 点灯 — 与原理图 YD-ESP32-S3-SCH-V1.4 一致

板载 RGB：XL-5050RGBC-WS2812B，DIN 网络名 RGB_CTRL → ESP32-S3 GPIO48（见同一 PDF）。
模组为 ESP32-S3-WROOM-1（U4）；指示灯另有 PWR/串口占用 GPIO，与本 WS2812 无关。
"""
import time

from machine import Pin

USE_NEOPIXEL = True

PIN_NEOPIXEL = 48
# 排针上的 GPIO12 等仅供外接灯试验；板载 WS2812 固定为上面 PIN_NEOPIXEL
PIN_GPIO_LED = 12


def light_neopixel():
    from neopixel import NeoPixel

    pin = Pin(PIN_NEOPIXEL, Pin.OUT)
    np = NeoPixel(pin, 1)
    # RGB，亮度请按需改小避免刺眼（例程里曾用 (5,5,5) 或 (100,0,0)）
    np[0] = (40, 40, 40)
    np.write()
    print("NeoPixel GPIO%d 已点亮" % PIN_NEOPIXEL)


def light_gpio_led(active_high=True):
    led = Pin(PIN_GPIO_LED, Pin.OUT)
    led.value(1 if active_high else 0)
    print("GPIO%d 已输出为 %s（若未亮可改 active_high 或换 PIN）"
          % (PIN_GPIO_LED, "高" if active_high else "低"))


def main():
    if USE_NEOPIXEL:
        light_neopixel()
    else:
        light_gpio_led(active_high=True)

    # 保持运行，避免 REPL/某些情况下立刻结束；按 Ctrl+C 可退出
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        if USE_NEOPIXEL:
            from neopixel import NeoPixel

            pin = Pin(PIN_NEOPIXEL, Pin.OUT)
            np = NeoPixel(pin, 1)
            np[0] = (0, 0, 0)
            np.write()
            print("已关闭 NeoPixel")
        else:
            Pin(PIN_GPIO_LED, Pin.OUT).value(0)


if __name__ == "__main__":
    main()
