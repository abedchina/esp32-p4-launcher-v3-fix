/*
 * Expansion header of the Guition ESP32-P4 4.3" (the 2x13 pin header) for the IO service.
 *
 * Left column (top to bottom):  3V3 3V3 GND 52 51 50 49 35 34 32 28 SDA(7) SCL(8)
 * Right column:                 5V  5V  GND 33 31 30 29 GND ESP_3V3 C6_RX C6_TX C6_IO9 C6_PU
 *
 * Handed out to apps: GPIO 28-35 and 49-52. 49-52 are ADC2 channels. 34 and 35 are
 * strapping pins of the P4 (boot mode): usable after boot, but nothing may hold them at
 * power-up. GPIO 35 has a pull-up on the board (measured: reads 1 with the internal
 * pull-down on), so as an input it idles high - wire buttons on it to GND. The C6 pins (its UART, IO9 boot and CHIP_PU) belong to the Wi-Fi chip and are
 * never handed out. The I2C pair is the board's own bus (GT911 touch 0x5D/0x14, ES8311
 * codec 0x18): apps reach other addresses on it through the IO service.
 * All pins are 3.3 V only (not 5 V tolerant).
 */
#include "board_internal.h"
#include "board_pins.h"

static const bsp_io_pin_t s_pins[] = {
    { 28, BSP_IO_DIGITAL | BSP_IO_PWM },
    { 29, BSP_IO_DIGITAL | BSP_IO_PWM },
    { 30, BSP_IO_DIGITAL | BSP_IO_PWM },
    { 31, BSP_IO_DIGITAL | BSP_IO_PWM },
    { 32, BSP_IO_DIGITAL | BSP_IO_PWM },
    { 33, BSP_IO_DIGITAL | BSP_IO_PWM },
    { 34, BSP_IO_DIGITAL | BSP_IO_PWM | BSP_IO_BOOT },
    { 35, BSP_IO_DIGITAL | BSP_IO_PWM | BSP_IO_BOOT },
    { 49, BSP_IO_DIGITAL | BSP_IO_PWM | BSP_IO_ADC },
    { 50, BSP_IO_DIGITAL | BSP_IO_PWM | BSP_IO_ADC },
    { 51, BSP_IO_DIGITAL | BSP_IO_PWM | BSP_IO_ADC },
    { 52, BSP_IO_DIGITAL | BSP_IO_PWM | BSP_IO_ADC },
};

int bsp_io_pins(const bsp_io_pin_t **out)
{
    if (out) *out = s_pins;
    return (int)(sizeof(s_pins) / sizeof(s_pins[0]));
}

void *bsp_io_i2c_bus(void)
{
    return board_i2c_bus();
}

bool bsp_io_i2c_reserved(uint8_t addr)
{
    return addr == 0x5D || addr == 0x14 ||      /* GT911 touch (either strap)  */
           addr == 0x18 ||                      /* ES8311 codec                */
           addr < 0x08 || addr > 0x77;          /* I2C reserved address ranges */
}
