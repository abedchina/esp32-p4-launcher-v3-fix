/* Developer Mode: what the port files share. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Python's "/" is this folder on the SD card (like the flash of an ESP32 board);
 * paths that start with /sd reach the whole card. */
#define DM_ROOT "/sd/python"

enum { DM_ACT_RX, DM_ACT_STOP, DM_ACT_RUN, DM_ACT_IDLE };

extern volatile bool dm_quit;
extern volatile int dm_inject;

void dm_hal_start(void);
void dm_hal_stop(void);
bool dm_usb_connected(void);
void dm_feed(const char *s);
void dm_console_write(const char *s, size_t len);
void dm_activity(int what);
void dm_path(const char *py, char *out, size_t n);     /* Python path -> card path */
void dm_os_reset(void);                                 /* cwd back to "/" (soft reset) */
void dm_machine_reset(void);                            /* pins / buses off (soft reset) */

/* p4.ai (port/modp4.c): model + camera; the screen thread draws the live picture */
#include <stdint.h>
bool dm_ai_active(void);
void dm_ai_stop(void);
int  dm_ai_draw(uint16_t *px, int stride, void (*rect)(int, int, int, int, uint16_t),
                void (*text)(int, int, const char *, uint16_t, int));
