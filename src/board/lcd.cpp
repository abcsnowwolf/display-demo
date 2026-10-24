/**
 * @file    lcd.cpp
 * @brief   板载 2.4寸 ST7789V SPI LCD 驱动 (基于 LovyanGFX)
 *
 * 各项参数(引脚/SPI模式/时钟/反显)取自原正点原子 spilcd 驱动实测配置
 */

#include "lcd.h"
#include "xl9555.h"
#include <Arduino.h>

LCD lcd;

LCD::LCD()
{
    {
        auto cfg = _bus.config();
        cfg.spi_host    = SPI2_HOST;      /* GPIO11/12 是 SPI2 的 IO_MUX 引脚, 支持 80MHz */
        cfg.spi_mode    = 3;              /* 与原驱动 SPI_MODE3 一致 */
        cfg.freq_write  = 80000000;       /* 显示异常时可降至 40MHz */
        cfg.spi_3wire   = false;
        cfg.use_lock    = true;
        cfg.dma_channel = SPI_DMA_CH_AUTO;
        cfg.pin_sclk    = 12;
        cfg.pin_mosi    = 11;
        cfg.pin_miso    = -1;
        cfg.pin_dc      = 40;
        _bus.config(cfg);
        _panel.setBus(&_bus);
    }

    {
        auto cfg = _panel.config();
        cfg.pin_cs       = 21;
        cfg.pin_rst      = -1;            /* 复位脚在 XL9555 上, 见 lcd_hw_init() */
        cfg.pin_busy     = -1;
        cfg.panel_width  = 240;
        cfg.panel_height = 320;
        cfg.offset_x     = 0;
        cfg.offset_y     = 0;
        cfg.readable     = false;         /* MISO 未接, 不可回读 */
        cfg.invert       = true;          /* 原驱动初始化时发送 0x21 (INVON) */
        cfg.rgb_order    = false;
        cfg.bus_shared   = false;
        _panel.config(cfg);
    }

    setPanel(&_panel);
}

void lcd_hw_init(void)
{
    /* 背光和复位脚挂在 XL9555 扩展芯片上, 库无法直接控制, 先手动完成硬复位 */
    xl9555_io_config(SLCD_PWR, IO_SET_OUTPUT);
    xl9555_io_config(SLCD_RST, IO_SET_OUTPUT);

    xl9555_pin_set(SLCD_RST, IO_SET_LOW);
    delay(20);
    xl9555_pin_set(SLCD_RST, IO_SET_HIGH);
    delay(120);

    lcd.init();
    lcd.setRotation(1);   /* 横屏, 与原驱动 spilcd_dir=1 一致; 若画面倒置改为 3 */

    lcd_backlight(true);
}

void lcd_backlight(bool on)
{
    xl9555_pin_set(SLCD_PWR, on ? IO_SET_HIGH : IO_SET_LOW);
}
