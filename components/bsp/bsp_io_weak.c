/* Defaults for the expansion-header part of the BSP contract: a board without a
 * header (or not yet ported) exposes no pins and no bus. Boards override them. */
#include "bsp.h"
#include <stddef.h>

__attribute__((weak)) int bsp_io_pins(const bsp_io_pin_t **out)
{
    if (out) *out = NULL;
    return 0;
}

__attribute__((weak)) void *bsp_io_i2c_bus(void) { return NULL; }
__attribute__((weak)) bool bsp_io_i2c_reserved(uint8_t addr) { (void)addr; return true; }
__attribute__((weak)) void bsp_buttons_hold(bool hold) { (void)hold; }
__attribute__((weak)) int bsp_io_adc_read(int gpio, int *mv) { (void)gpio; if (mv) *mv = -1; return -1; }
