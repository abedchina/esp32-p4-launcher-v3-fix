/*
 * Developer Mode - MicroPython on the ESP32-P4 Launcher, for Thonny. By Abed Mihilia.
 *
 * Thonny (Tools > Options > Interpreter: "MicroPython (ESP32)", the board's COM port) talks
 * to this app's REPL over the USB-C cable: run code, see print() output, stop with Ctrl+C,
 * save files. Python's "/" is /sd/python on the card; boot.py and main.py there run at
 * every (soft) start, so a saved program runs without a PC too.
 * The launcher starts this app by itself when Thonny resets the board on connect.
 *
 * Screen: what Python prints, and STOP (Ctrl+C), RUN MAIN.PY (soft reset), X (exit).
 */
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "py/compile.h"
#include "py/runtime.h"
#include "py/gc.h"
#include "py/stackctrl.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "genhdr/mpversion.h"
#include "shared/runtime/pyexec.h"
#include "shared/runtime/gchelper.h"
#include "shared/readline/readline.h"
#include "papp/papp.h"
#include "port/devmode.h"

PAPP_MANIFEST(.id = "abed.devmode", .name = "Developer Mode", .version = PAPP_APP_VERSION, .main_stack = 65536);

#define HEAP_SIZE   (4 * 1024 * 1024)
#define STACK_SIZE  65536

#define W 800
#define H 480

/* == console mirror == */
#define COLS 64
#define ROWS 22
static char con[ROWS][COLS + 1];
static int con_row, con_col;
static volatile int con_dirty;
static int esc;                                    /* inside an ANSI escape sequence */
static papp_mutex_t *con_lock;
static volatile int64_t t_rx, t_stop, t_run;

static void con_newline(void)
{
    if (con_row == ROWS - 1) { memmove(con[0], con[1], sizeof con[0] * (ROWS - 1)); memset(con[ROWS - 1], 0, COLS + 1); }
    else con_row++;
    con_col = 0;
}

void dm_console_write(const char *s, size_t len)
{
    if (con_lock) papp_mutex_lock(con_lock);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (esc) { if (esc == 1 && c != '[') esc = 0; else if (isalpha(c)) esc = 0; else esc = 2; continue; }
        if (c == 0x1b) { esc = 1; continue; }
        if (c == '\r') { con_col = 0; continue; }
        if (c == '\n') { con_newline(); continue; }
        if (c == '\b') { if (con_col) con_col--; continue; }
        if (c < 32 || c > 126) c = c >= 0x80 ? '?' : ' ';
        if (con_col == COLS) con_newline();
        con[con_row][con_col++] = (char)c;
        if (con_col < COLS) con[con_row][con_col] = 0;
    }
    con_dirty = 1;
    if (con_lock) papp_mutex_unlock(con_lock);
}

void dm_activity(int what)
{
    int64_t now = papp_time_us();
    if (what == DM_ACT_RX) t_rx = now;
    else if (what == DM_ACT_STOP) t_stop = now;
    else if (what == DM_ACT_RUN) t_run = now;
}

/* == drawing (5x7 font) == */
static uint16_t *px;
static int stride;

static void rect(int x, int y, int w, int h, uint16_t c)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
    for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) px[j * stride + i] = c;
}

static const char FC[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:-!'.x/*<>%_,()=+#[]\"?;";
static const uint8_t FG[][7] = {
    {0, 0, 0, 0, 0, 0, 0},
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    {0x0E, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11}, {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C},
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11},
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
    {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}, {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},
    {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}, {0x0C, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}, {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11},
    {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}, {0x04, 0x15, 0x0E, 0x1B, 0x0E, 0x15, 0x04},
    {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}, {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08},
    {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F},
    {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}, {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02},
    {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08},
    {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}, {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00},
    {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}, {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E},
    {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}, {0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}, {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08},
};

static void ch(int x, int y, char c, uint16_t col, int sc)
{
    c = (char)toupper((unsigned char)c);
    const char *p = strchr(FC, c);
    if (!p || !c) return;
    const uint8_t *g = FG[p - FC];
    for (int r = 0; r < 7; r++)
        for (int k = 0; k < 5; k++)
            if (g[r] & (0x10 >> k)) rect(x + k * sc, y + r * sc, sc, sc, col);
}
static int tw(const char *s, int sc) { return (int)strlen(s) * 6 * sc - sc; }
static void text(int x, int y, const char *s, uint16_t c, int sc)
{
    for (int i = 0; s[i]; i++) ch(x + i * 6 * sc, y, s[i], c, sc);
}

#define C_BG   PAPP_RGB565(10, 14, 24)
#define C_BAR  PAPP_RGB565(24, 34, 56)
#define C_CON  PAPP_RGB565(150, 255, 170)

typedef struct { int x, y, w, h; const char *t; uint16_t c; } btn_t;
static const btn_t B_STOP = { 492, 6, 120, 44, "STOP", PAPP_RGB565(200, 30, 40) };
static const btn_t B_RUN  = { 620, 6, 120, 44, "MAIN.PY", PAPP_RGB565(40, 110, 200) };

static void button(const btn_t *b)
{
    rect(b->x, b->y, b->w, b->h, b->c);
    text(b->x + (b->w - tw(b->t, 2)) / 2, b->y + (b->h - 14) / 2, b->t, 0xFFFF, 2);
}

static bool hit(const btn_t *b, int x, int y) { return x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h; }

static volatile bool ui_run;
/* Started by the launcher because Thonny connected (argv[1] = "thonny"): main.py does NOT
 * run by itself during this session, so Thonny finds an idle board and lists the files at
 * once. Opened from the home screen: boot.py + main.py run at every (soft) start, like a
 * board on its own. The MAIN.PY button runs main.py once in both cases. */
static bool thonny_session;
static volatile bool run_main_next;

/* camera view (a model is loaded from Python): picture left, buttons + console right */
static const btn_t B_STOP2 = { 648, 60, 146, 44, "STOP", PAPP_RGB565(200, 30, 40) };
static const btn_t B_RUN2  = { 648, 112, 146, 44, "MAIN.PY", PAPP_RGB565(40, 110, 200) };

static void draw_x(void)
{
    rect(W - 52, 6, 44, 44, PAPP_RGB565(20, 30, 50));
    for (int k = 0; k < 24; k++) { rect(W - 42 + k, 16 + k, 3, 3, 0xFFFF); rect(W - 42 + k, 39 - k, 3, 3, 0xFFFF); }
}

static void draw_camera_view(papp_surface_t *s)
{
    px = papp_disp_lock(s, &stride);
    int r = dm_ai_draw(px, stride, rect, text);
    if (r == 0) return;                                             /* no new frame: keep the last one */
    if (r < 0) {
        rect(0, 0, 640, H, C_BG);
        text(40, 200, "CONNECT A USB CAMERA", PAPP_RGB565(255, 190, 90), 3);
        text(40, 240, "TO THE HIGH-SPEED USB-C PORT", PAPP_RGB565(170, 190, 220), 2);
    }
    rect(640, 0, W - 640, H, C_BAR);
    text(648, 22, "PYTHON", 0xFFFF, 2);
    draw_x();
    button(&B_STOP2);
    button(&B_RUN2);
    if (con_lock) papp_mutex_lock(con_lock);
    for (int k = 0; k < 24; k++) {                                  /* the console's last lines, small */
        int row = ROWS - 24 + k;
        if (row < 0) continue;
        char line[27];
        snprintf(line, sizeof line, "%.26s", con[row]);
        text(646, 168 + k * 13, line, C_CON, 1);
    }
    if (con_lock) papp_mutex_unlock(con_lock);
    papp_disp_present(s);
    if (r < 0) papp_delay_ms(100);
}

static int ui_thread(void *arg)
{
    papp_surface_t *s = arg;
    bool was = false;
    int64_t t_draw = 0;
    while (ui_run) {
        papp_touch_point_t tp;
        int n = papp_disp_touch(s, &tp, 1);
        bool camv = dm_ai_active();
        if (n > 0 && !was) {
            if (tp.x > W - 56 && tp.y < 56) {                       /* X: leave Developer Mode */
                dm_quit = true;
                mp_sched_keyboard_interrupt();
            } else if (hit(camv ? &B_STOP2 : &B_STOP, tp.x, tp.y)) {  /* Ctrl+C */
                mp_sched_keyboard_interrupt();
                dm_activity(DM_ACT_STOP);
            } else if (hit(camv ? &B_RUN2 : &B_RUN, tp.x, tp.y)) {    /* stop + soft reset + run main.py */
                run_main_next = true;                               /* also in a Thonny session */
                mp_sched_keyboard_interrupt();
                papp_delay_ms(50);
                dm_feed("\x02\x04");                                /* friendly REPL, Ctrl+D */
                dm_activity(DM_ACT_RUN);
            }
        }
        was = n > 0;
        if (camv) { draw_camera_view(s); t_draw = 0; continue; }
        int64_t now = papp_time_us();
        if (!con_dirty && now - t_draw < 250000) { papp_delay_ms(20); continue; }
        con_dirty = 0;
        t_draw = now;
        px = papp_disp_lock(s, &stride);
        rect(0, 0, W, H, C_BG);
        rect(0, 0, W, 56, C_BAR);
        text(12, 8, "DEVELOPER MODE", 0xFFFF, 3);
        bool usb = dm_usb_connected();
        const char *st = now - t_stop < 1500000 ? "STOPPED (CTRL+C)" :
                         now - t_rx < 3000000 ? "THONNY IS TALKING TO THE BOARD" :
                         usb ? "USB CABLE CONNECTED - OPEN THONNY" : "CONNECT THE USB-C CABLE TO THE PC";
        text(12, 38, st, usb ? PAPP_RGB565(120, 230, 150) : PAPP_RGB565(255, 190, 90), 1);
        text(272, 16, "MICROPYTHON " MICROPY_VERSION_STRING, PAPP_RGB565(170, 190, 220), 2);
        button(&B_STOP);
        button(&B_RUN);
        rect(W - 52, 6, 44, 44, PAPP_RGB565(20, 30, 50));             /* X */
        for (int k = 0; k < 24; k++) { rect(W - 42 + k, 16 + k, 3, 3, 0xFFFF); rect(W - 42 + k, 39 - k, 3, 3, 0xFFFF); }
        if (con_lock) papp_mutex_lock(con_lock);
        for (int r = 0; r < ROWS; r++) text(14, 66 + r * 18, con[r], C_CON, 2);
        if (con_lock) papp_mutex_unlock(con_lock);
        text(14, H - 12, "THONNY: TOOLS - OPTIONS - INTERPRETER - MICROPYTHON (ESP32) - THIS BOARD'S COM PORT.  FILES: /SD/PYTHON",
             PAPP_RGB565(120, 140, 170), 1);
        papp_disp_present(s);
    }
    return 0;
}

/* == MicroPython glue == */
void gc_collect(void)
{
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}

void nlr_jump_fail(void *val)
{
    (void)val;
    papp_log("MicroPython: fatal error (nlr_jump_fail)\n");
    papp_exit(3);
    for (;;) {}
}

#ifndef NDEBUG
void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    papp_log("MicroPython assert %s:%d %s\n", file, line, expr);
    papp_exit(4);
    for (;;) {}
}
#endif

int main(int argc, char **argv)
{
    thonny_session = argc > 1 && !strcmp(argv[1], "thonny");
    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
    papp_surface_t *s = papp_disp_surface_create(&cfg);
    if (!s) return 2;
    const papp_if_thrd_t *thrd = papp_if_thrd();
    con_lock = thrd ? papp_mutex_create(true) : NULL;
    ui_run = true;
    papp_thread_cfg_t tc = { .name = "ui", .stack_size = 8192, .prio = PAPP_PRIO_NORMAL };
    papp_thread_t *ui = papp_thread_create(ui_thread, s, &tc);

    uint8_t *heap = papp_alloc(HEAP_SIZE, PAPP_MEM_LARGE, 16);
    if (!heap) { dm_console_write("NOT ENOUGH MEMORY\n", 18); papp_delay_ms(3000); return 3; }
    papp_fs()->mkdir(DM_ROOT);
    dm_hal_start();

    volatile int stack_top;
    while (!dm_quit) {
        mp_stack_set_top((void *)&stack_top);
        mp_stack_set_limit(STACK_SIZE - 12 * 1024);
        gc_init(heap, heap + HEAP_SIZE);
        mp_init();
        mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(qstr_from_str("/lib")));
        readline_init0();
        dm_os_reset();

        bool run_main = run_main_next || (!thonny_session && pyexec_mode_kind == PYEXEC_MODE_FRIENDLY_REPL);
        run_main_next = false;
        if (thonny_session && !run_main && pyexec_mode_kind == PYEXEC_MODE_FRIENDLY_REPL)
            mp_hal_stdout_tx_str("Thonny session: main.py does not start by itself (MAIN.PY button / Run)\r\n");
        if (run_main) {                                             /* like a board: boot.py, then main.py */
            pyexec_file_if_exists("boot.py");
            if (!dm_quit) pyexec_file_if_exists("main.py");
        }
        for (;;) {
            if (dm_quit) break;
            if (pyexec_mode_kind == PYEXEC_MODE_RAW_REPL) {
                if (pyexec_raw_repl() != 0) break;
            } else {
                if (pyexec_friendly_repl() != 0) break;
            }
        }
        dm_ai_stop();                                               /* model + camera off */
        dm_machine_reset();                                         /* pins / buses off */
        gc_sweep_all();
        mp_deinit();
        if (!dm_quit) mp_hal_stdout_tx_str("MPY: soft reboot\r\n");
    }

    dm_hal_stop();
    ui_run = false;
    if (ui) papp_thread_join(ui, 2000, NULL);
    papp_free(heap);
    papp_disp_surface_destroy(s);
    return 0;
}
