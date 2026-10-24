/**
 * @file    lcd.h
 * @brief   板载 2.4寸 ST7789V SPI LCD 驱动 (基于 LovyanGFX)
 *
 * 硬件: 正点原子 ESP32-S3 开发板
 *   CS=21, MOSI=11, SCK=12, DC=40
 *   复位(XL9555 P12)和背光(XL9555 P13)挂在 IO 扩展芯片上,
 *   由 lcd_hw_init() 手动控制, 面板配置中 pin_rst = -1
 */

#ifndef __LCD_H
#define __LCD_H

#include <LovyanGFX.hpp>

class LCD : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI _bus;

public:
    LCD();
};

extern LCD lcd;

void lcd_hw_init(void);        /* XL9555 上电复位 + 面板初始化, 默认横屏(320x240) */
void lcd_backlight(bool on);   /* 背光开关 (XL9555 P13) */

#endif
