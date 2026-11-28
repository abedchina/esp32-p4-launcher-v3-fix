/* HAL of the Developer Mode MicroPython port (port/mphal.c). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "shared/runtime/interrupt_char.h"

#define mp_hal_pin_obj_t int

mp_uint_t mp_hal_ticks_ms(void);
mp_uint_t mp_hal_ticks_us(void);
mp_uint_t mp_hal_ticks_cpu(void);
uint64_t  mp_hal_time_ns(void);
void      mp_hal_delay_ms(mp_uint_t ms);
void      mp_hal_delay_us(mp_uint_t us);
uintptr_t mp_hal_stdio_poll(uintptr_t poll_flags);
