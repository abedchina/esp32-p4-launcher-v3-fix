/*
 * Developer Mode HAL: Python's stdin/stdout on the board's USB serial port (PAPP_IF_TTY,
 * the COM port Thonny opens) and mirrored on the screen; timing from the launcher.
 *
 * An RX thread drains the port into a ring: Ctrl+C (while code runs) raises
 * KeyboardInterrupt at once, everything else waits for mp_hal_stdin_rx_chr().
 */
#include <string.h>
#include "py/runtime.h"
#include "py/mphal.h"
#include "py/stream.h"
#include "papp/papp.h"
#include "devmode.h"

static const papp_if_tty_t *s_tty;
static uint8_t s_ring[8192];
static volatile uint32_t s_head, s_tail;          /* head: writer (RX thread), tail: reader */
static papp_thread_t *s_rx_thread;
static volatile bool s_rx_run;

volatile bool dm_quit;                             /* app asked to exit */
volatile int dm_inject;                            /* a key from the screen buttons, -1 none */

static void ring_put(uint8_t c)
{
    uint32_t n = (s_head + 1) % sizeof s_ring;
    if (n == s_tail) return;                       /* full: drop (Thonny's raw paste waits for us) */
    s_ring[s_head] = c;
    s_head = n;
}

static int rx_thread(void *arg)
{
    (void)arg;
    uint8_t buf[256];
    while (s_rx_run) {
        int n = s_tty ? s_tty->read(buf, sizeof buf, 50) : 0;
        if (n <= 0) { if (!s_tty) papp_delay_ms(50); continue; }
        for (int i = 0; i < n; i++) {
            if (buf[i] == mp_interrupt_char) {     /* Ctrl+C while Python code runs */
                s_head = s_tail;                   /* like a real board: drop what was typed ahead */
                mp_sched_keyboard_interrupt();
                dm_activity(DM_ACT_STOP);
            } else {
                ring_put(buf[i]);
            }
        }
        dm_activity(DM_ACT_RX);
    }
    return 0;
}

void dm_hal_start(void)
{
    s_tty = papp_tty();
    if (s_tty && s_tty->open() != 0) s_tty = NULL;
    dm_inject = -1;
    s_rx_run = true;
    papp_thread_cfg_t tc = { .name = "tty", .stack_size = 4096, .prio = PAPP_PRIO_NORMAL };
    s_rx_thread = papp_thread_create(rx_thread, NULL, &tc);
}

void dm_hal_stop(void)
{
    s_rx_run = false;
    if (s_rx_thread) papp_thread_join(s_rx_thread, 2000, NULL);
    s_rx_thread = NULL;
    if (s_tty) s_tty->close();
}

bool dm_usb_connected(void)
{
    return s_tty && s_tty->connected();
}

void dm_feed(const char *s)                        /* type into the REPL (screen buttons) */
{
    while (*s) ring_put((uint8_t)*s++);
}

/* ── stdio ──────────────────────────────────────────────────────────────── */

int mp_hal_stdin_rx_chr(void)
{
    for (;;) {
        if (s_tail != s_head) {
            uint8_t c = s_ring[s_tail];
            s_tail = (s_tail + 1) % sizeof s_ring;
            return c;
        }
        if (dm_quit) return 4;                     /* Ctrl+D: the REPL leaves, the app exits */
        MICROPY_EVENT_POLL_HOOK
        papp_delay_ms(1);
    }
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len)
{
    if (s_tty && len) s_tty->write(str, len);
    dm_console_write(str, len);
    return len;
}

uintptr_t mp_hal_stdio_poll(uintptr_t poll_flags)
{
    uintptr_t ret = 0;
    if ((poll_flags & MP_STREAM_POLL_RD) && s_tail != s_head) ret |= MP_STREAM_POLL_RD;
    if (poll_flags & MP_STREAM_POLL_WR) ret |= MP_STREAM_POLL_WR;
    return ret;
}

/* ── time ───────────────────────────────────────────────────────────────── */

mp_uint_t mp_hal_ticks_ms(void)  { return (mp_uint_t)(papp_time_us() / 1000); }
mp_uint_t mp_hal_ticks_us(void)  { return (mp_uint_t)papp_time_us(); }
mp_uint_t mp_hal_ticks_cpu(void) { return (mp_uint_t)papp_time_us(); }

uint64_t mp_hal_time_ns(void)
{
    int64_t s;
    if (papp_utc_time(&s)) return (uint64_t)s * 1000000000ull + (uint64_t)(papp_time_us() % 1000000) * 1000ull;
    return (uint64_t)papp_time_us() * 1000ull;
}

void mp_hal_delay_ms(mp_uint_t ms)
{
    uint64_t end = (uint64_t)papp_time_us() + (uint64_t)ms * 1000;
    for (;;) {
        MICROPY_EVENT_POLL_HOOK                    /* Ctrl+C / STOP work inside sleep() */
        int64_t left = (int64_t)(end - (uint64_t)papp_time_us());
        if (left <= 0) break;
        papp_delay_ms(left > 10000 ? 10 : (uint32_t)((left + 999) / 1000 > 10 ? 10 : (left + 999) / 1000));
    }
}

void mp_hal_delay_us(mp_uint_t us)
{
    if (us >= 2000) { mp_hal_delay_ms(us / 1000); us %= 1000; }
    papp_delay_us(us);
}

uint32_t mp_port_random_seed(void)
{
    return (uint32_t)papp_time_us() ^ 0x9E3779B9u;
}

void mp_port_idle(void)
{
    if (papp_exit_requested() && !dm_quit) {       /* HOME / X / DevLink stop */
        dm_quit = true;
        mp_sched_keyboard_interrupt();
    }
}
