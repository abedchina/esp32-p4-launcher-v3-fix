/*
 * Blink 33 - the first example for students: an LED on GPIO 33 blinks.
 *
 * Wiring: GPIO33 -> resistor 220-330 ohm -> LED (long leg) -> LED (short leg) -> GND
 *
 * Screen: green = LED on, dark = LED off. Tap the screen = blink faster / slower.
 * X (top right) = exit (the launcher turns pin 33 off by itself).
 */
#include "papp/papp.h"

PAPP_MANIFEST(.id = "student.blink33", .name = "Blink 33", .version = PAPP_APP_VERSION);

#define LED 33                     /* the pin the LED is connected to */

int main(void)
{
    /* a screen of 800 x 480 points (only for the colour and the X button) */
    papp_surface_cfg_t cfg = { .width = 800, .height = 480, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
    papp_surface_t *screen = papp_disp_surface_create(&cfg);

    papp_pin_mode(LED, PAPP_IO_OUTPUT);       /* pin 33 = output */

    int on = 0;                               /* 0 = LED off, 1 = LED on */
    int wait = 500;                           /* milliseconds between changes */
    int64_t last = papp_time_us();
    int touching = 0;

    while (!papp_exit_requested()) {
        /* 1. time to change the LED? */
        if (papp_time_us() - last >= wait * 1000) {
            last = papp_time_us();
            on = !on;                         /* on -> off, off -> on */
            papp_pin_write(LED, on);          /* <- this line turns the LED on / off */
        }

        /* 2. touch: X = exit, anywhere else = faster / slower */
        papp_touch_point_t t;
        int n = papp_disp_touch(screen, &t, 1);
        if (n > 0 && !touching) {
            if (t.x > 740 && t.y < 60) break;           /* X */
            wait = wait == 500 ? 150 : wait == 150 ? 1000 : 500;
        }
        touching = n > 0;

        /* 3. draw: green when the LED is on */
        int stride;
        uint16_t *px = papp_disp_lock(screen, &stride);
        uint16_t colour = on ? PAPP_RGB565(40, 220, 90) : PAPP_RGB565(20, 30, 45);
        for (int y = 0; y < 480; y++)
            for (int x = 0; x < 800; x++)
                px[y * stride + x] = colour;
        for (int k = 0; k < 30; k++) {                  /* the X */
            for (int d = 0; d < 4; d++) {
                px[(14 + k) * stride + 755 + k + d] = 0xFFFF;
                px[(14 + k) * stride + 785 - k + d] = 0xFFFF;
            }
        }
        papp_disp_present(screen);
        papp_delay_ms(20);
    }

    papp_pin_write(LED, 0);                   /* LED off before leaving */
    papp_disp_surface_destroy(screen);
    return 0;
}
