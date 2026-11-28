/*
 * IO Lab - the board's pin header on screen (launcher 0.33+, IO service). By Abed Mihilia.
 *
 * Left: the 2x13 header of the Guition ESP32-P4 4.3" drawn like its pin card. Tap a green
 * GPIO to select it, then on the right: IN / PULL-UP / OUT 0 / OUT 1 / PWM / ADC / FREE.
 * The level of every pin in use is shown live (inputs are read all the time, ADC pins show
 * millivolts). TEST runs a self test with nothing connected (loopbacks inside the chip:
 * digital, pulls, PWM timing, UART, SPI, I2C protection) and prints the results; they also
 * go to the log. STOP = emergency stop (every pin back to a plain input). X exits, and
 * the launcher frees all pins on exit too.
 */
#include "papp/papp.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

PAPP_MANIFEST(.id = "abed.iolab", .name = "IO Lab", .version = PAPP_APP_VERSION, .main_stack = 16384);

#define W 800
#define H 480

static uint16_t *px;
static int stride;

/* ══ drawing ═════════════════════════════════════════════════════════════ */
static void rect(int x, int y, int w, int h, uint16_t c)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
    for (int j = y0; j < y1; j++) {
        uint16_t *r = &px[j * stride];
        for (int i = x0; i < x1; i++) r[i] = c;
    }
}
static void frame_rect(int x, int y, int w, int h, int t, uint16_t c)
{
    rect(x, y, w, t, c); rect(x, y + h - t, w, t, c);
    rect(x, y, t, h, c); rect(x + w - t, y, t, h, c);
}

static const char FC[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:-!'.x/*<>%_,()";
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
static void text_in(int x, int y, int w, int h, const char *s, uint16_t c, int sc)   /* centred in a box */
{
    text(x + (w - tw(s, sc)) / 2, y + (h - 7 * sc) / 2, s, c, sc);
}

#define C_BG     PAPP_RGB565(14, 20, 32)
#define C_PANEL  PAPP_RGB565(28, 38, 58)
#define C_RED    PAPP_RGB565(220, 30, 40)
#define C_GND    PAPP_RGB565(10, 10, 10)
#define C_GPIO   PAPP_RGB565(110, 200, 80)
#define C_GPIO_D PAPP_RGB565(50, 95, 40)
#define C_I2C    PAPP_RGB565(190, 150, 230)
#define C_C6     PAPP_RGB565(70, 170, 230)
#define C_ORANGE PAPP_RGB565(245, 150, 30)
#define C_SEL    PAPP_RGB565(255, 230, 60)
#define C_WHITE  0xFFFF
#define C_DIM    PAPP_RGB565(150, 160, 180)
#define C_OK     PAPP_RGB565(90, 230, 120)
#define C_BAD    PAPP_RGB565(255, 90, 90)

/* ══ the header of the Guition 4.3" ═══════════════════════════════════════ */
enum { K_PWR, K_GND, K_GPIO, K_I2C, K_C6, K_ESP3V3 };
typedef struct { const char *label; int kind, gpio; } cell_t;
static const cell_t LEFT[13] = {
    {"3V3", K_PWR, -1}, {"3V3", K_PWR, -1}, {"GND", K_GND, -1},
    {"GPIO52", K_GPIO, 52}, {"GPIO51", K_GPIO, 51}, {"GPIO50", K_GPIO, 50}, {"GPIO49", K_GPIO, 49},
    {"GPIO35", K_GPIO, 35}, {"GPIO34", K_GPIO, 34}, {"GPIO32", K_GPIO, 32}, {"GPIO28", K_GPIO, 28},
    {"I2C SDA", K_I2C, -1}, {"I2C SCL", K_I2C, -1},
};
static const cell_t RIGHT[13] = {
    {"5V", K_PWR, -1}, {"5V", K_PWR, -1}, {"GND", K_GND, -1},
    {"GPIO33", K_GPIO, 33}, {"GPIO31", K_GPIO, 31}, {"GPIO30", K_GPIO, 30}, {"GPIO29", K_GPIO, 29},
    {"GND", K_GND, -1}, {"ESP 3V3", K_ESP3V3, -1}, {"C6 RXD", K_C6, -1}, {"C6 TXD", K_C6, -1},
    {"C6 IO9", K_C6, -1}, {"C6 PU", K_C6, -1},
};

#define HX   16                   /* header drawing */
#define HY   58
#define CW   150
#define CH   30
#define CG   2
#define CONW 36

static const papp_if_io_t *io;
static papp_io_pin_t pins[24];
static int npins;
static int sel = 28;
static int mv_of[64];
static int pwm_duty[64];

static papp_io_pin_t *pin_of(int g)
{
    for (int i = 0; i < npins; i++) if (pins[i].gpio == g) return &pins[i];
    return NULL;
}

static uint16_t kind_col(int k)
{
    switch (k) {
    case K_PWR: return C_RED;
    case K_GND: return C_GND;
    case K_I2C: return C_I2C;
    case K_C6: return C_C6;
    case K_ESP3V3: return C_ORANGE;
    default: return C_GPIO;
    }
}

static void state_text(int g, char *b, int n)
{
    papp_io_pin_t *p = pin_of(g);
    if (!p) { snprintf(b, n, "-"); return; }
    switch (p->use) {
    case PAPP_IO_USE_INPUT:  snprintf(b, n, "IN %d", p->level); break;
    case PAPP_IO_USE_OUTPUT: snprintf(b, n, "OUT %d", p->level); break;
    case PAPP_IO_USE_PWM:    snprintf(b, n, "PWM %d%%", pwm_duty[g] * 100 / 65535); break;
    case PAPP_IO_USE_ADC:    snprintf(b, n, "%dMV", mv_of[g]); break;
    case PAPP_IO_USE_UART:   snprintf(b, n, "UART"); break;
    case PAPP_IO_USE_SPI:    snprintf(b, n, "SPI"); break;
    default:                 b[0] = 0; break;
    }
}

static void draw_cell(int x, int y, const cell_t *c, bool right)
{
    uint16_t col = kind_col(c->kind);
    papp_io_pin_t *p = c->gpio >= 0 ? pin_of(c->gpio) : NULL;
    if (c->kind == K_GPIO && p && p->use == PAPP_IO_USE_FREE) col = C_GPIO_D;
    if (c->kind == K_GPIO && p && (p->use == PAPP_IO_USE_OUTPUT || p->use == PAPP_IO_USE_INPUT) && p->level)
        col = PAPP_RGB565(170, 255, 120);
    rect(x, y, CW, CH, col);
    if (c->gpio >= 0 && c->gpio == sel) frame_rect(x - 2, y - 2, CW + 4, CH + 4, 3, C_SEL);
    uint16_t tc = c->kind == K_GPIO || c->kind == K_I2C || c->kind == K_ESP3V3 ? C_GND : C_WHITE;
    (void)right;
    if (c->kind == K_GPIO) {
        char st[16];
        state_text(c->gpio, st, sizeof st);
        text(x + 6, y + 8, c->label, tc, 2);
        if (st[0]) text(x + CW - tw(st, 1) - 5, y + 11, st, PAPP_RGB565(10, 40, 10), 1);
    } else {
        text_in(x, y, CW, CH, c->label, tc, 2);
    }
}

static void draw_header(void)
{
    int cx = HX + CW + 4;
    rect(cx, HY - 6, CONW, 13 * (CH + CG) + 10, PAPP_RGB565(30, 30, 34));
    for (int r = 0; r < 13; r++) {
        int y = HY + r * (CH + CG);
        draw_cell(HX, y, &LEFT[r], false);
        draw_cell(cx + CONW + 4, y, &RIGHT[r], true);
        rect(cx + 8, y + 11, 8, 8, PAPP_RGB565(200, 200, 205));
        rect(cx + CONW - 16, y + 11, 8, 8, PAPP_RGB565(200, 200, 205));
    }
}

/* ══ buttons on the right ═════════════════════════════════════════════════ */
typedef struct { int x, y, w, h; const char *t; } btn_t;
#define PX 400
enum { B_IN, B_PU, B_O0, B_O1, B_PWM, B_ADC, B_FREE, B_TEST, B_STOP, B_N };
static const btn_t BT[B_N] = {
    {PX, 150, 120, 46, "IN"}, {PX + 128, 150, 120, 46, "PULL-UP"}, {PX + 256, 150, 120, 46, "FREE"},
    {PX, 204, 120, 46, "OUT 0"}, {PX + 128, 204, 120, 46, "OUT 1"}, {PX + 256, 204, 120, 46, "PWM"},
    {PX, 258, 120, 46, "ADC"},
    {PX + 128, 258, 120, 46, "TEST"}, {PX + 256, 258, 120, 46, "STOP"},
};
/* table order above is by position; map ids to it */
static const int BID[B_N] = { B_IN, B_PU, B_FREE, B_O0, B_O1, B_PWM, B_ADC, B_TEST, B_STOP };

static char lines[12][48];
static uint16_t line_col[12];
static int nlines;

static void say(uint16_t col, const char *s)
{
    if (nlines == 12) { memmove(lines, lines[1], sizeof lines[0] * 11); memmove(line_col, line_col + 1, sizeof line_col[0] * 11); nlines = 11; }
    snprintf(lines[nlines], sizeof lines[0], "%s", s);
    line_col[nlines++] = col;
    papp_log("IOLAB %s\n", s);
}

static void draw_panel(void)
{
    rect(PX - 8, HY - 6, W - PX, H - HY - 2, C_PANEL);
    papp_io_pin_t *p = pin_of(sel);
    char b[48];
    snprintf(b, sizeof b, "GPIO%d", sel);
    text(PX, HY + 4, b, C_SEL, 4);
    if (p) {
        snprintf(b, sizeof b, "%s%s%s", (p->caps & PAPP_IO_CAP_ADC) ? "ADC  " : "",
                 (p->caps & PAPP_IO_CAP_PWM) ? "PWM  " : "", (p->caps & PAPP_IO_CAP_BOOT) ? "BOOT PIN" : "");
        text(PX + 170, HY + 8, b, C_DIM, 2);
        state_text(sel, b, sizeof b);
        text(PX, HY + 50, b[0] ? b : "FREE", C_WHITE, 3);
    }
    if (p && (p->caps & PAPP_IO_CAP_BOOT)) text(PX, HY + 80, "NOTHING MAY HOLD IT AT POWER-UP", C_ORANGE, 1);
    if (sel == 35) text(PX + 200, HY + 80, "PULL-UP ON THE BOARD: READS 1", C_ORANGE, 1);
    text(PX, HY + 80 + 12, "3.3V ONLY - NOT 5V", C_DIM, 1);
    for (int i = 0; i < B_N; i++) {
        const btn_t *t = &BT[i];
        int id = BID[i];
        bool dis = id == B_ADC && !(p && (p->caps & PAPP_IO_CAP_ADC));
        uint16_t c = id == B_STOP ? C_RED : id == B_TEST ? PAPP_RGB565(40, 110, 200) : PAPP_RGB565(55, 70, 100);
        if (dis) c = PAPP_RGB565(40, 45, 55);
        rect(t->x, t->y, t->w, t->h, c);
        text_in(t->x, t->y, t->w, t->h, t->t, dis ? C_DIM : C_WHITE, 2);
    }
    for (int i = 0; i < nlines; i++) text(PX, 318 + i * 13, lines[i], line_col[i], 1);
}

static void btn_x(void)
{
    rect(W - 52, 4, 44, 44, PAPP_RGB565(20, 30, 50));
    for (int k = 0; k < 24; k++) { rect(W - 42 + k, 14 + k, 3, 3, C_WHITE); rect(W - 42 + k, 37 - k, 3, 3, C_WHITE); }
}

/* ══ self test (nothing connected to the header) ══════════════════════════ */
static void result(const char *what, int ok, int total)
{
    char b[48];
    snprintf(b, sizeof b, "%s %d/%d %s", what, ok, total, ok == total ? "PASS" : "FAIL");
    say(ok == total ? C_OK : C_BAD, b);
}

static void self_test(void)
{
    nlines = 0;
    say(C_WHITE, "SELF TEST - NOTHING ON THE HEADER");
    int dig = 0, pull = 0;
    char bad[48] = "";
    for (int i = 0; i < npins; i++) {
        int g = pins[i].gpio, ok = 1;
        ok &= io->mode(g, PAPP_IO_OUTPUT) == 0;
        ok &= io->write(g, 1) == 0 && io->read(g) == 1;
        ok &= io->write(g, 0) == 0 && io->read(g) == 0;
        dig += ok;
        io->mode(g, PAPP_IO_INPUT_PULLUP);
        papp_delay_ms(5);
        int up = io->read(g);
        io->mode(g, PAPP_IO_INPUT_PULLDOWN);
        papp_delay_ms(5);
        int dn = io->read(g);
        if ((up == 1 && dn == 0) || (g == 35 && up == 1)) pull++;   /* 35: pull-up on the board (boot pin) */
        else {
            size_t l = strlen(bad);
            snprintf(bad + l, sizeof bad - l, " %d(UP%d DN%d)", g, up, dn);
        }
        io->release(g);
    }
    result("DIGITAL OUT/IN", dig, npins);
    result("PULL-UP/DOWN", pull, npins);
    if (bad[0]) { char t2[48]; snprintf(t2, sizeof t2, "PIN%s", bad); say(C_ORANGE, t2); }

    /* PWM 1 kHz: 25 % = 250 us high, 50 % = 500 us */
    int pw = 0;
    io->pwm(28, 1000, 16384);
    int32_t a = io->pulse_in(28, 1, 100000);
    io->pwm(28, 1000, 32768);
    int32_t b = io->pulse_in(28, 1, 100000);
    pw += a > 230 && a < 270;
    pw += b > 470 && b < 530;
    io->release(28);
    char t[48];
    snprintf(t, sizeof t, "PWM 1KHZ HIGH %dUS / %dUS", (int)a, (int)b);
    say(C_DIM, t);
    result("PWM TIMING", pw, 2);

    /* ADC reads on every ADC pin (value not checked: the pin floats) */
    int ad = 0, na = 0;
    for (int i = 0; i < npins; i++) {
        if (!(pins[i].caps & PAPP_IO_CAP_ADC)) continue;
        int mv = -1, raw = io->adc(pins[i].gpio, &mv);
        na++;
        ad += raw >= 0 && raw <= 4095 && mv >= 0;
        io->release(pins[i].gpio);
    }
    result("ADC READ", ad, na);

    /* UART 1 loopback: TX and RX on the same pin */
    int ua = 0, uo = io->uart_open(1, 30, 30, 115200), un = -99;
    if (uo == 0) {
        char rx[8] = {0};
        io->uart_read(1, rx, sizeof rx, 20);                 /* drop anything old */
        io->uart_write(1, "HELLO", 5);
        un = io->uart_read(1, rx, 5, 200);
        ua = un == 5 && !memcmp(rx, "HELLO", 5);
        io->uart_close(1);
    }
    if (!ua) { snprintf(t, sizeof t, "UART OPEN %d READ %d", uo, un); say(C_ORANGE, t); }
    result("UART LOOPBACK", ua, 1);

    /* SPI loopback: MOSI and MISO on the same pin */
    int sp = 0, so = io->spi_open(31, 32, 32), sd = -99, sr = -99;
    uint8_t stx[4] = { 0xA5, 0x5A, 0x3C, 0xC3 }, srx[4] = {0};
    if (so == 0) {
        sd = io->spi_device(-1, 1000000, 0);
        if (sd >= 0) sr = io->spi_transfer(sd, stx, srx, 4);
        sp = sr == 4 && !memcmp(stx, srx, 4);
        io->spi_close();
    }
    if (!sp) {
        snprintf(t, sizeof t, "SPI OPEN %d DEV %d XFER %d RX %02X%02X", so, sd, sr, srx[0], srx[1]);
        say(C_ORANGE, t);
    }
    result("SPI LOOPBACK", sp, 1);

    /* protection: board chips and system pins are refused */
    int pr = 0;
    pr += io->i2c_probe(0x5D) < 0 && io->i2c_write(0x18, "x", 1) < 0;   /* touch, codec */
    pr += io->mode(23, PAPP_IO_OUTPUT) < 0;                            /* backlight */
    pr += io->mode(43, PAPP_IO_OUTPUT) < 0;                            /* SD card clock */
    result("PROTECTION", pr, 3);

    int found = 0;
    for (int ad7 = 0x08; ad7 < 0x78; ad7++) found += io->i2c_probe((uint8_t)ad7) == 0;
    snprintf(t, sizeof t, "I2C DEVICES ON THE HEADER: %d", found);
    say(C_DIM, t);

    io->all_off();
    npins = io->pins(pins, 24);
    int freed = 0;
    for (int i = 0; i < npins; i++) freed += pins[i].use == PAPP_IO_USE_FREE;
    result("STOP ALL", freed, npins);
}

/* ══ main ════════════════════════════════════════════════════════════════ */
static void apply(int id)
{
    papp_io_pin_t *p = pin_of(sel);
    if (!p) return;
    int r = 0;
    char b[48];
    switch (id) {
    case B_IN:   r = io->mode(sel, PAPP_IO_INPUT); break;
    case B_PU:   r = io->mode(sel, PAPP_IO_INPUT_PULLUP); break;
    case B_O0:   r = io->mode(sel, PAPP_IO_OUTPUT); if (!r) r = io->write(sel, 0); break;
    case B_O1:   r = io->mode(sel, PAPP_IO_OUTPUT); if (!r) r = io->write(sel, 1); break;
    case B_PWM:
        pwm_duty[sel] = p->use == PAPP_IO_USE_PWM ? (pwm_duty[sel] + 16384) % 81920 : 16384;
        if (pwm_duty[sel] > 65535) pwm_duty[sel] = 65535;
        if (pwm_duty[sel] == 0) pwm_duty[sel] = 16384;
        r = io->pwm(sel, 1000, (uint32_t)pwm_duty[sel]);
        break;
    case B_ADC:  if (p->caps & PAPP_IO_CAP_ADC) r = io->adc(sel, &mv_of[sel]); break;
    case B_FREE: r = io->release(sel); break;
    case B_STOP: io->all_off(); say(C_ORANGE, "STOP - ALL PINS ARE INPUTS AGAIN"); break;
    case B_TEST: self_test(); break;
    }
    if (r < 0) { snprintf(b, sizeof b, "GPIO%d: ERROR %d", sel, r); say(C_BAD, b); }
}

int main(int argc, char **argv)
{
    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
    papp_surface_t *s = papp_disp_surface_create(&cfg);
    if (!s) return 2;
    io = papp_io();
    bool was = false;
    int64_t t_adc = 0;
    if (io) npins = io->pins(pins, 24);
    if (!io || npins == 0)
        say(C_BAD, io ? "THIS BOARD HAS NO PIN HEADER" : "LAUNCHER 0.33 OR NEWER NEEDED");
    else if (argc > 1 && !strcmp(argv[1], "test"))          /* started as "io_lab.papp test" */
        self_test();
    while (!papp_exit_requested()) {
        papp_touch_point_t tp[2];
        int n = papp_disp_touch(s, tp, 2);
        if (n > 0 && !was) {
            int x = tp[0].x, y = tp[0].y;
            if (x > W - 60 && y < 56) break;
            if (io && npins) {
                for (int r = 0; r < 13; r++) {
                    int yy = HY + r * (CH + CG);
                    if (y < yy || y >= yy + CH) continue;
                    if (x >= HX && x < HX + CW && LEFT[r].gpio >= 0) sel = LEFT[r].gpio;
                    int rx = HX + CW + 4 + CONW + 4;
                    if (x >= rx && x < rx + CW && RIGHT[r].gpio >= 0) sel = RIGHT[r].gpio;
                }
                for (int i = 0; i < B_N; i++) {
                    const btn_t *t = &BT[i];
                    if (x >= t->x && x < t->x + t->w && y >= t->y && y < t->y + t->h) apply(BID[i]);
                }
            }
        }
        was = n > 0;
        if (io && npins) {
            npins = io->pins(pins, 24);
            int64_t now = papp_time_us();
            bool adc_now = now - t_adc > 150000;
            if (adc_now) t_adc = now;
            for (int i = 0; i < npins; i++) {
                int g = pins[i].gpio;
                if (pins[i].use == PAPP_IO_USE_INPUT) pins[i].level = (uint8_t)(io->read(g) > 0);
                else if (pins[i].use == PAPP_IO_USE_ADC && adc_now) io->adc(g, &mv_of[g]);
            }
        }
        px = papp_disp_lock(s, &stride);
        rect(0, 0, W, H, C_BG);
        text(HX, 14, "IO LAB", C_WHITE, 4);
        text(HX + 150, 24, "PIN HEADER - GUITION ESP32-P4 4.3", C_DIM, 2);
        draw_header();
        draw_panel();
        btn_x();
        papp_disp_present(s);
        papp_delay_ms(30);
    }
    papp_disp_surface_destroy(s);
    return 0;
}
