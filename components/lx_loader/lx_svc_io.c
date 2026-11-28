/*
 * IO service: the board's expansion header for apps (PAPP_IF_IO).
 *
 * Only pins the board lists in bsp_io_pins() are accepted. Everything an app takes
 * (pins, PWM channels, UARTs, the SPI bus, I2C device handles) is recorded here and
 * released in lx_svc_io_end_run(), so a crashed or killed app never leaves a motor or
 * relay switched on. While an app holds header pins the gamepad add-on (which reads the
 * same pins) is paused through bsp_buttons_hold().
 *
 * PWM uses LEDC timers 1..3 and channels 1..7 (timer 0 / channel 0 drive the backlight
 * on some boards). UART 1 and 2 are free (the console is UART 0). SPI is SPI2_HOST.
 */
#include <string.h>
#include "lx_services.h"
#include "bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"

static const char *TAG = "lx_io";

#define MAXP        24
#define PWM_T0      1                       /* first LEDC timer / channel apps may use */
#define PWM_NT      3
#define PWM_NCH     7
#define I2C_MAXDEV  8
#define I2C_HZ      100000
#define I2C_TMO_MS  50
#define SPI_MAXDEV  4
#define SPI_MAXLEN  4096

typedef struct {
    uint8_t gpio, caps, use, level;
    int8_t  pwm_ch;                         /* LEDC channel index 0..6, -1 none */
} pin_t;

static SemaphoreHandle_t s_lock;
static pin_t s_pin[MAXP];
static int   s_npin;
static bool  s_held;                        /* gamepad paused for this run */

static struct { uint32_t freq; int bits; int users; } s_tmr[PWM_NT];
static bool s_ch_used[PWM_NCH];
static uint8_t s_ch_tmr[PWM_NCH];

static struct { uint8_t addr; i2c_master_dev_handle_t dev; } s_i2c[I2C_MAXDEV];

static bool s_uart[3];
static int8_t s_uart_pins[3][2];

static bool s_spi_bus;
static int8_t s_spi_pins[3];
static spi_device_handle_t s_spi_dev[SPI_MAXDEV];
static int8_t s_spi_cs[SPI_MAXDEV];

/* ── helpers ────────────────────────────────────────────────────────────── */

static void lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutex();
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

static void unlock(void) { xSemaphoreGiveRecursive(s_lock); }

static void load_pins(void)
{
    if (s_npin) return;
    const bsp_io_pin_t *p = NULL;
    int n = bsp_io_pins(&p);
    for (int i = 0; i < n && i < MAXP; i++) {
        s_pin[i] = (pin_t){ .gpio = p[i].gpio, .caps = p[i].caps, .pwm_ch = -1 };
    }
    s_npin = n < MAXP ? n : MAXP;
}

static pin_t *find(int gpio)
{
    load_pins();
    for (int i = 0; i < s_npin; i++)
        if (s_pin[i].gpio == gpio) return &s_pin[i];
    return NULL;
}

static void hold_pad(void)
{
    if (!s_held) { bsp_buttons_hold(true); s_held = true; }
}

static void pin_float(int gpio)               /* plain input, no pulls: the safe state */
{
    gpio_reset_pin(gpio);
    gpio_config_t c = { .pin_bit_mask = 1ULL << gpio, .mode = GPIO_MODE_INPUT,
                        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE };
    gpio_config(&c);
}

static void pwm_detach(pin_t *p)
{
    if (p->pwm_ch < 0) return;
    int ch = p->pwm_ch;
    ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)(PWM_T0 + ch), 0);
    int t = s_ch_tmr[ch];
    if (s_tmr[t].users > 0 && --s_tmr[t].users == 0) {
        ledc_timer_pause(LEDC_LOW_SPEED_MODE, (ledc_timer_t)(PWM_T0 + t));
        s_tmr[t].freq = 0;
    }
    s_ch_used[ch] = false;
    p->pwm_ch = -1;
}

/* Give the pin up (whatever it did) and leave it floating. Not for UART / SPI pins. */
static void pin_free(pin_t *p)
{
    pwm_detach(p);
    pin_float(p->gpio);
    p->use = PAPP_IO_USE_FREE;
    p->level = 0;
}

/* A pin that UART / SPI hold can't be taken for anything else until they close. */
static bool busy(pin_t *p) { return p->use == PAPP_IO_USE_UART || p->use == PAPP_IO_USE_SPI; }

static int claim_bus_pin(int gpio, uint8_t use)
{
    if (gpio < 0) return 0;
    pin_t *p = find(gpio);
    if (!p || busy(p)) return PAPP_E_ARG;
    pwm_detach(p);
    p->use = use;
    hold_pad();
    return 0;
}

static void free_bus_pin(int gpio)
{
    pin_t *p = gpio >= 0 ? find(gpio) : NULL;
    if (!p) return;
    p->use = PAPP_IO_USE_FREE;              /* busy() is false now */
    pin_free(p);
}

/* ── digital / ADC / PWM ────────────────────────────────────────────────── */

static int io_pins(papp_io_pin_t *out, int max)
{
    SVC_ENTER();
    lock();
    load_pins();
    for (int i = 0; out && i < s_npin && i < max; i++)
        out[i] = (papp_io_pin_t){ .gpio = s_pin[i].gpio, .caps = s_pin[i].caps,
                                  .use = s_pin[i].use, .level = s_pin[i].level };
    int n = s_npin;
    unlock();
    SVC_LEAVE();
    return n;
}

static int io_mode(int gpio, int mode)
{
    SVC_ENTER();
    lock();
    int r = 0;
    pin_t *p = find(gpio);
    if (!p || busy(p) || mode < PAPP_IO_INPUT || mode > PAPP_IO_OUTPUT_OPEN_DRAIN) { r = PAPP_E_ARG; goto out; }
    hold_pad();
    bool to_out = mode == PAPP_IO_OUTPUT || mode == PAPP_IO_OUTPUT_OPEN_DRAIN;
    bool was_out = p->use == PAPP_IO_USE_OUTPUT;
    pwm_detach(p);
    if (!(to_out && was_out)) gpio_reset_pin(gpio);       /* PIN_FUNC_GPIO again (after ADC / PWM) */
    if (to_out && !was_out) { gpio_set_level(gpio, 0); p->level = 0; }   /* start low */
    gpio_config_t c = { .pin_bit_mask = 1ULL << gpio };
    switch (mode) {
    case PAPP_IO_OUTPUT:            c.mode = GPIO_MODE_INPUT_OUTPUT; break;     /* read() sees the level */
    case PAPP_IO_OUTPUT_OPEN_DRAIN: c.mode = GPIO_MODE_INPUT_OUTPUT_OD; break;
    case PAPP_IO_INPUT_PULLUP:      c.mode = GPIO_MODE_INPUT; c.pull_up_en = GPIO_PULLUP_ENABLE; break;
    case PAPP_IO_INPUT_PULLDOWN:    c.mode = GPIO_MODE_INPUT; c.pull_down_en = GPIO_PULLDOWN_ENABLE; break;
    default:                        c.mode = GPIO_MODE_INPUT; break;
    }
    if (gpio_config(&c) != ESP_OK) { r = PAPP_E_IO; goto out; }
    if (to_out) gpio_set_level(gpio, p->level);
    p->use = to_out ? PAPP_IO_USE_OUTPUT : PAPP_IO_USE_INPUT;
out:
    unlock();
    SVC_LEAVE();
    return r;
}

static int io_write(int gpio, int level)
{
    SVC_ENTER();
    lock();
    int r = 0;
    pin_t *p = find(gpio);
    if (!p) r = PAPP_E_ARG;
    else {
        if (p->use != PAPP_IO_USE_OUTPUT) {                  /* Arduino habit: write without pinMode */
            unlock();
            r = io_mode(gpio, PAPP_IO_OUTPUT);
            lock();
        }
        if (r == 0) { p->level = level ? 1 : 0; gpio_set_level(gpio, p->level); }
    }
    unlock();
    SVC_LEAVE();
    return r;
}

static int io_read(int gpio)
{
    SVC_ENTER();
    lock();
    int r;
    pin_t *p = find(gpio);
    if (!p || busy(p)) r = PAPP_E_ARG;
    else {
        if (p->use != PAPP_IO_USE_INPUT && p->use != PAPP_IO_USE_OUTPUT) {
            unlock();
            r = io_mode(gpio, PAPP_IO_INPUT);
            lock();
            if (r < 0) goto out;
        }
        r = gpio_get_level(gpio);
        p->level = (uint8_t)r;
    }
out:
    unlock();
    SVC_LEAVE();
    return r;
}

static int io_adc(int gpio, int *mv)
{
    SVC_ENTER();
    lock();
    int r;
    pin_t *p = find(gpio);
    if (!p || busy(p) || !(p->caps & BSP_IO_ADC)) { if (mv) *mv = -1; r = PAPP_E_ARG; goto out; }
    hold_pad();
    if (p->use != PAPP_IO_USE_ADC) { pwm_detach(p); p->use = PAPP_IO_USE_ADC; }
    r = bsp_io_adc_read(gpio, mv);
    if (r < 0) r = PAPP_E_IO;
out:
    unlock();
    SVC_LEAVE();
    return r;
}

static int timer_for(uint32_t freq, int *bits_out)
{
    for (int t = 0; t < PWM_NT; t++)
        if (s_tmr[t].users > 0 && s_tmr[t].freq == freq) { *bits_out = s_tmr[t].bits; return t; }
    for (int t = 0; t < PWM_NT; t++) {
        if (s_tmr[t].users > 0) continue;
        /* the finest resolution LEDC can reach at this frequency */
        for (int bits = 14; bits >= 1; bits--) {
            ledc_timer_config_t tc = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = (ledc_timer_bit_t)bits,
                                       .timer_num = (ledc_timer_t)(PWM_T0 + t), .freq_hz = freq,
                                       .clk_cfg = LEDC_AUTO_CLK };
            if (ledc_timer_config(&tc) == ESP_OK) {
                s_tmr[t].freq = freq; s_tmr[t].bits = bits;
                *bits_out = bits;
                return t;
            }
        }
        return PAPP_E_ARG;                                   /* frequency out of range */
    }
    return PAPP_E_LIMIT;                                     /* 3 different frequencies already */
}

static int io_pwm(int gpio, uint32_t freq, uint32_t duty)
{
    SVC_ENTER();
    lock();
    int r = 0;
    pin_t *p = find(gpio);
    if (!p || busy(p) || !(p->caps & BSP_IO_PWM)) { r = PAPP_E_ARG; goto out; }
    if (freq == 0) {                                         /* off: a low output */
        pwm_detach(p);
        unlock();
        r = io_write(gpio, 0);
        lock();
        goto out;
    }
    if (duty > 65535) duty = 65535;
    hold_pad();
    int ch = p->pwm_ch, bits = 0;
    if (ch >= 0 && s_tmr[s_ch_tmr[ch]].freq != freq) { pwm_detach(p); ch = -1; }
    if (ch < 0) {
        for (int c = 0; c < PWM_NCH; c++) if (!s_ch_used[c]) { ch = c; break; }
        if (ch < 0) { r = PAPP_E_LIMIT; goto out; }
        int t = timer_for(freq, &bits);
        if (t < 0) { r = t; goto out; }
        if (p->use != PAPP_IO_USE_OUTPUT) gpio_reset_pin(gpio);
        ledc_channel_config_t cc = { .gpio_num = gpio, .speed_mode = LEDC_LOW_SPEED_MODE,
                                     .channel = (ledc_channel_t)(PWM_T0 + ch), .intr_type = LEDC_INTR_DISABLE,
                                     .timer_sel = (ledc_timer_t)(PWM_T0 + t), .duty = 0, .hpoint = 0 };
        if (ledc_channel_config(&cc) != ESP_OK) { r = PAPP_E_IO; goto out; }
        gpio_input_enable(gpio);                             /* pulse_in() / read() can watch it */
        s_ch_used[ch] = true; s_ch_tmr[ch] = (uint8_t)t; s_tmr[t].users++;
        p->pwm_ch = (int8_t)ch;
        p->use = PAPP_IO_USE_PWM;
    }
    bits = s_tmr[s_ch_tmr[ch]].bits;
    uint32_t full = 1u << bits;
    uint32_t d = (uint32_t)(((uint64_t)duty * full + 32767) / 65535);
    if (d > full) d = full;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)(PWM_T0 + ch), d);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)(PWM_T0 + ch));
out:
    unlock();
    SVC_LEAVE();
    return r;
}

static int io_release(int gpio)
{
    SVC_ENTER();
    lock();
    int r = 0;
    pin_t *p = find(gpio);
    if (!p || busy(p)) r = PAPP_E_ARG;
    else pin_free(p);
    unlock();
    SVC_LEAVE();
    return r;
}

static int32_t io_pulse_in(int gpio, int level, uint32_t timeout_us)
{
    SVC_ENTER();
    pin_t *p = find(gpio);
    int32_t r = 0;
    if (!p || busy(p) || p->use == PAPP_IO_USE_FREE || p->use == PAPP_IO_USE_ADC) {
        if (p && !busy(p) && io_mode(gpio, PAPP_IO_INPUT) == 0) goto go;
        SVC_LEAVE();
        return PAPP_E_ARG;
    }
go:
    if (timeout_us == 0 || timeout_us > 1000000) timeout_us = 1000000;
    level = level ? 1 : 0;
    int64_t t0 = esp_timer_get_time(), end = t0 + timeout_us;
    while (gpio_get_level(gpio) == level)                    /* a pulse already running: wait it out */
        if (esp_timer_get_time() > end) goto out;
    while (gpio_get_level(gpio) != level)
        if (esp_timer_get_time() > end) goto out;
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(gpio) == level)
        if (esp_timer_get_time() > end) goto out;
    r = (int32_t)(esp_timer_get_time() - start);
out:
    SVC_LEAVE();
    return r;
}

static void io_delay_us(uint32_t us)
{
    esp_rom_delay_us(us > 10000 ? 10000 : us);
}

/* ── I2C (the board's header bus) ───────────────────────────────────────── */

static i2c_master_dev_handle_t i2c_dev(uint8_t addr)
{
    if (bsp_io_i2c_reserved(addr) || !bsp_io_i2c_bus()) return NULL;
    for (int i = 0; i < I2C_MAXDEV; i++)
        if (s_i2c[i].dev && s_i2c[i].addr == addr) return s_i2c[i].dev;
    for (int i = 0; i < I2C_MAXDEV; i++) {
        if (s_i2c[i].dev) continue;
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr,
                                   .scl_speed_hz = I2C_HZ };
        if (i2c_master_bus_add_device((i2c_master_bus_handle_t)bsp_io_i2c_bus(), &dc, &s_i2c[i].dev) != ESP_OK)
            return NULL;
        s_i2c[i].addr = addr;
        return s_i2c[i].dev;
    }
    return NULL;
}

static int io_i2c_probe(uint8_t addr)
{
    if (!bsp_io_i2c_bus()) return PAPP_E_UNSUPPORTED;
    if (bsp_io_i2c_reserved(addr)) return PAPP_E_ARG;
    SVC_ENTER();
    int r = i2c_master_probe((i2c_master_bus_handle_t)bsp_io_i2c_bus(), addr, I2C_TMO_MS) == ESP_OK ? 0 : PAPP_E_IO;
    SVC_LEAVE();
    return r;
}

static int io_i2c_write(uint8_t addr, const void *buf, size_t len)
{
    SVC_ENTER();
    lock();
    i2c_master_dev_handle_t d = i2c_dev(addr);
    int r = !d ? PAPP_E_ARG : i2c_master_transmit(d, buf, len, I2C_TMO_MS) == ESP_OK ? (int)len : PAPP_E_IO;
    unlock();
    SVC_LEAVE();
    return r;
}

static int io_i2c_read(uint8_t addr, void *buf, size_t len)
{
    SVC_ENTER();
    lock();
    i2c_master_dev_handle_t d = i2c_dev(addr);
    int r = !d ? PAPP_E_ARG : i2c_master_receive(d, buf, len, I2C_TMO_MS) == ESP_OK ? (int)len : PAPP_E_IO;
    unlock();
    SVC_LEAVE();
    return r;
}

static int io_i2c_write_read(uint8_t addr, const void *w, size_t wl, void *rd, size_t rl)
{
    SVC_ENTER();
    lock();
    i2c_master_dev_handle_t d = i2c_dev(addr);
    int r = !d ? PAPP_E_ARG
               : i2c_master_transmit_receive(d, w, wl, rd, rl, I2C_TMO_MS) == ESP_OK ? (int)rl : PAPP_E_IO;
    unlock();
    SVC_LEAVE();
    return r;
}

/* ── UART 1 / 2 ─────────────────────────────────────────────────────────── */

static void uart_close_locked(int port)
{
    if (port < 1 || port > 2 || !s_uart[port]) return;
    uart_driver_delete((uart_port_t)port);
    free_bus_pin(s_uart_pins[port][0]);
    free_bus_pin(s_uart_pins[port][1]);
    s_uart[port] = false;
}

static int io_uart_open(int port, int tx, int rx, uint32_t baud)
{
    if (port < 1 || port > 2 || (tx < 0 && rx < 0) || baud < 300 || baud > 5000000) return PAPP_E_ARG;
    SVC_ENTER();
    lock();
    int r = 0;
    uart_close_locked(port);
    if (claim_bus_pin(tx, PAPP_IO_USE_UART) < 0) { r = PAPP_E_ARG; goto out; }
    if (rx != tx && claim_bus_pin(rx, PAPP_IO_USE_UART) < 0) { free_bus_pin(tx); r = PAPP_E_ARG; goto out; }
    uart_config_t uc = { .baud_rate = (int)baud, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                         .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                         .source_clk = UART_SCLK_DEFAULT };
    if (uart_driver_install((uart_port_t)port, 2048, 0, 0, NULL, 0) != ESP_OK ||
        uart_param_config((uart_port_t)port, &uc) != ESP_OK ||
        uart_set_pin((uart_port_t)port, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        uart_driver_delete((uart_port_t)port);
        free_bus_pin(tx); free_bus_pin(rx);
        r = PAPP_E_IO;
        goto out;
    }
    if (rx >= 0) gpio_input_enable(rx);                      /* also when RX = TX (one-wire / loopback) */
    s_uart[port] = true;
    s_uart_pins[port][0] = (int8_t)tx;
    s_uart_pins[port][1] = (int8_t)rx;
out:
    unlock();
    SVC_LEAVE();
    return r;
}

static int io_uart_write(int port, const void *buf, size_t len)
{
    if (port < 1 || port > 2 || !s_uart[port]) return PAPP_E_ARG;
    SVC_ENTER();
    int r = uart_write_bytes((uart_port_t)port, buf, len);
    SVC_LEAVE();
    return r < 0 ? PAPP_E_IO : r;
}

static int io_uart_read(int port, void *buf, size_t len, uint32_t timeout_ms)
{
    if (port < 1 || port > 2 || !s_uart[port]) return PAPP_E_ARG;
    if (timeout_ms > 1000) timeout_ms = 1000;               /* stays responsive to exit */
    SVC_ENTER();
    int r = uart_read_bytes((uart_port_t)port, buf, len, pdMS_TO_TICKS(timeout_ms));
    SVC_LEAVE();
    return r < 0 ? PAPP_E_IO : r;
}

static void io_uart_close(int port)
{
    SVC_ENTER();
    lock();
    uart_close_locked(port);
    unlock();
    SVC_LEAVE();
}

/* ── SPI (SPI2_HOST) ────────────────────────────────────────────────────── */

static void spi_close_locked(void)
{
    for (int i = 0; i < SPI_MAXDEV; i++) {
        if (!s_spi_dev[i]) continue;
        spi_bus_remove_device(s_spi_dev[i]);
        s_spi_dev[i] = NULL;
        free_bus_pin(s_spi_cs[i]);
    }
    if (s_spi_bus) {
        spi_bus_free(SPI2_HOST);
        for (int i = 0; i < 3; i++) free_bus_pin(s_spi_pins[i]);
        s_spi_bus = false;
    }
}

static int io_spi_open(int sck, int mosi, int miso)
{
    if (sck < 0) return PAPP_E_ARG;
    SVC_ENTER();
    lock();
    int r = 0;
    spi_close_locked();
    int pins[3] = { sck, mosi, miso };
    if (mosi == sck || miso == sck) { r = PAPP_E_ARG; goto out; }
    for (int i = 0; i < 3; i++)
        if (!(i == 2 && miso == mosi) && claim_bus_pin(pins[i], PAPP_IO_USE_SPI) < 0) {
            for (int k = 0; k < i; k++) free_bus_pin(pins[k]);
            r = PAPP_E_ARG;
            goto out;
        }
    spi_bus_config_t bc = { .sclk_io_num = sck, .mosi_io_num = mosi, .miso_io_num = miso,
                            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = SPI_MAXLEN };
    if (spi_bus_initialize(SPI2_HOST, &bc, SPI_DMA_CH_AUTO) != ESP_OK) {
        for (int k = 0; k < 3; k++) free_bus_pin(pins[k]);
        r = PAPP_E_IO;
        goto out;
    }
    if (miso >= 0) gpio_input_enable(miso);                  /* also when MISO = MOSI (loopback) */
    s_spi_bus = true;
    for (int i = 0; i < 3; i++) s_spi_pins[i] = (int8_t)pins[i];
out:
    unlock();
    SVC_LEAVE();
    return r;
}

static int io_spi_device(int cs, uint32_t hz, int mode)
{
    if (!s_spi_bus || mode < 0 || mode > 3 || hz < 1000 || hz > 40000000) return PAPP_E_ARG;
    SVC_ENTER();
    lock();
    int r = PAPP_E_LIMIT;
    for (int i = 0; i < SPI_MAXDEV; i++) {
        if (s_spi_dev[i]) continue;
        if (claim_bus_pin(cs, PAPP_IO_USE_SPI) < 0) { r = PAPP_E_ARG; break; }
        spi_device_interface_config_t dc = { .clock_speed_hz = (int)hz, .mode = (uint8_t)mode,
                                             .spics_io_num = cs, .queue_size = 1 };
        if (spi_bus_add_device(SPI2_HOST, &dc, &s_spi_dev[i]) != ESP_OK) {
            s_spi_dev[i] = NULL;
            free_bus_pin(cs);
            r = PAPP_E_IO;
            break;
        }
        s_spi_cs[i] = (int8_t)cs;
        r = i;
        break;
    }
    unlock();
    SVC_LEAVE();
    return r;
}

static int io_spi_transfer(int dev, const void *tx, void *rx, size_t len)
{
    if (dev < 0 || dev >= SPI_MAXDEV || !s_spi_dev[dev] || len == 0 || len > SPI_MAXLEN) return PAPP_E_ARG;
    SVC_ENTER();
    spi_transaction_t t = { .length = len * 8, .tx_buffer = tx, .rx_buffer = rx };
    int r = spi_device_polling_transmit(s_spi_dev[dev], &t) == ESP_OK ? (int)len : PAPP_E_IO;
    SVC_LEAVE();
    return r;
}

static void io_spi_close(void)
{
    SVC_ENTER();
    lock();
    spi_close_locked();
    unlock();
    SVC_LEAVE();
}

/* ── teardown / emergency stop ──────────────────────────────────────────── */

static void release_all(void)
{
    lock();
    spi_close_locked();
    uart_close_locked(1);
    uart_close_locked(2);
    for (int i = 0; i < I2C_MAXDEV; i++)
        if (s_i2c[i].dev) { i2c_master_bus_rm_device(s_i2c[i].dev); s_i2c[i].dev = NULL; }
    int n = 0;
    for (int i = 0; i < s_npin; i++)
        if (s_pin[i].use != PAPP_IO_USE_FREE) { pin_free(&s_pin[i]); n++; }
    memset(s_tmr, 0, sizeof s_tmr);
    memset(s_ch_used, 0, sizeof s_ch_used);
    if (s_held) { bsp_buttons_hold(false); s_held = false; }
    unlock();
    if (n) ESP_LOGI(TAG, "%d header pin(s) released", n);
}

static void io_all_off(void)
{
    SVC_ENTER();
    release_all();
    SVC_LEAVE();
}

void lx_svc_io_end_run(void)
{
    release_all();
}

const papp_if_io_t lx_if_io = {
    .hdr = { PAPP_IF_IO, PAPP_IF_IO_VER, sizeof(papp_if_io_t) },
    .pins = io_pins,
    .mode = io_mode,
    .write = io_write,
    .read = io_read,
    .adc = io_adc,
    .pwm = io_pwm,
    .release = io_release,
    .pulse_in = io_pulse_in,
    .delay_us = io_delay_us,
    .i2c_probe = io_i2c_probe,
    .i2c_write = io_i2c_write,
    .i2c_read = io_i2c_read,
    .i2c_write_read = io_i2c_write_read,
    .uart_open = io_uart_open,
    .uart_write = io_uart_write,
    .uart_read = io_uart_read,
    .uart_close = io_uart_close,
    .spi_open = io_spi_open,
    .spi_device = io_spi_device,
    .spi_transfer = io_spi_transfer,
    .spi_close = io_spi_close,
    .all_off = io_all_off,
};
