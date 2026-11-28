/*
 * "machine" for Developer Mode: Pin, PWM, ADC, I2C, SPI, UART, time_pulse_us, RTC, reset,
 * on the launcher's IO service (only the header pins 28-35 and 49-52; 3.3 V).
 * Written to behave like MicroPython on an ESP32, so students' code carries over.
 */
#include <string.h>
#include "py/runtime.h"
#include "py/mphal.h"
#include "py/mperrno.h"
#include "py/objstr.h"
#include "papp/papp.h"
#include "devmode.h"

static const papp_if_io_t *io(void)
{
    const papp_if_io_t *p = papp_io();
    if (!p) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("update the launcher (0.33 or newer)"));
    return p;
}

static void check(int r, int gpio)
{
    if (r >= 0) return;
    if (r == PAPP_E_ARG)
        mp_raise_msg_varg(&mp_type_ValueError,
                          MP_ERROR_TEXT("GPIO%d can't do that here (header pins: 28-35, 49-52; ADC: 49-52)"), gpio);
    if (r == PAPP_E_LIMIT) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("no free channel"));
    mp_raise_OSError(MP_EIO);
}

void dm_machine_reset(void)
{
    if (papp_io()) papp_io()->all_off();
}

/* ══ Pin ═══════════════════════════════════════════════════════════════════ */
enum { PIN_IN = 1, PIN_OUT = 3, PIN_OPEN_DRAIN = 7 };
enum { PULL_UP = 1, PULL_DOWN = 2 };

typedef struct {
    mp_obj_base_t base;
    int gpio, mode, pull;
} pin_obj_t;

const mp_obj_type_t dm_pin_type;

int dm_pin_gpio(mp_obj_t o)                       /* Pin(..) or a plain number */
{
    if (mp_obj_is_type(o, &dm_pin_type)) return ((pin_obj_t *)MP_OBJ_TO_PTR(o))->gpio;
    return mp_obj_get_int(o);
}

static void pin_apply(pin_obj_t *p)
{
    int m = PAPP_IO_INPUT;
    if (p->mode == PIN_OUT) m = PAPP_IO_OUTPUT;
    else if (p->mode == PIN_OPEN_DRAIN) m = PAPP_IO_OUTPUT_OPEN_DRAIN;
    else if (p->pull == PULL_UP) m = PAPP_IO_INPUT_PULLUP;
    else if (p->pull == PULL_DOWN) m = PAPP_IO_INPUT_PULLDOWN;
    check(io()->mode(p->gpio, m), p->gpio);
}

static mp_obj_t pin_init_helper(pin_obj_t *p, size_t n_args, const mp_obj_t *args, mp_map_t *kw)
{
    enum { ARG_mode, ARG_pull, ARG_value };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_mode, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_pull, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_value, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, args, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    if (a[ARG_mode].u_obj != mp_const_none) p->mode = mp_obj_get_int(a[ARG_mode].u_obj);
    if (a[ARG_pull].u_obj != mp_const_none) p->pull = mp_obj_get_int(a[ARG_pull].u_obj);
    if (p->mode) pin_apply(p);
    if (a[ARG_value].u_obj != mp_const_none) check(io()->write(p->gpio, mp_obj_is_true(a[ARG_value].u_obj)), p->gpio);
    return mp_const_none;
}

static mp_obj_t pin_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);
    pin_obj_t *p = mp_obj_malloc(pin_obj_t, type);
    p->gpio = mp_obj_get_int(args[0]);
    p->mode = 0; p->pull = 0;
    papp_io_pin_t pins[24];
    int n = io()->pins(pins, 24), ok = 0;
    for (int i = 0; i < n; i++) ok |= pins[i].gpio == p->gpio;
    if (!ok) check(PAPP_E_ARG, p->gpio);
    mp_map_t kw;
    mp_map_init_fixed_table(&kw, n_kw, args + n_args);
    pin_init_helper(p, n_args - 1, args + 1, &kw);
    return MP_OBJ_FROM_PTR(p);
}

static void pin_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    (void)kind;
    pin_obj_t *p = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "Pin(%d)", p->gpio);
}

static mp_obj_t pin_value(size_t n_args, const mp_obj_t *args)
{
    pin_obj_t *p = MP_OBJ_TO_PTR(args[0]);
    if (n_args == 1) {
        int r = io()->read(p->gpio);
        check(r, p->gpio);
        return MP_OBJ_NEW_SMALL_INT(r);
    }
    check(io()->write(p->gpio, mp_obj_is_true(args[1])), p->gpio);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pin_value_obj, 1, 2, pin_value);

static mp_obj_t pin_call(mp_obj_t self_in, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
    mp_arg_check_num(n_args, n_kw, 0, 1, false);
    mp_obj_t a[2] = { self_in, n_args ? args[0] : MP_OBJ_NULL };
    return pin_value(n_args + 1, a);
}

static mp_obj_t pin_on(mp_obj_t s)  { pin_obj_t *p = MP_OBJ_TO_PTR(s); check(io()->write(p->gpio, 1), p->gpio); return mp_const_none; }
static mp_obj_t pin_off(mp_obj_t s) { pin_obj_t *p = MP_OBJ_TO_PTR(s); check(io()->write(p->gpio, 0), p->gpio); return mp_const_none; }
static mp_obj_t pin_toggle(mp_obj_t s)
{
    pin_obj_t *p = MP_OBJ_TO_PTR(s);
    int v = io()->read(p->gpio);
    check(v, p->gpio);
    check(io()->write(p->gpio, !v), p->gpio);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(pin_on_obj, pin_on);
static MP_DEFINE_CONST_FUN_OBJ_1(pin_off_obj, pin_off);
static MP_DEFINE_CONST_FUN_OBJ_1(pin_toggle_obj, pin_toggle);

static mp_obj_t pin_init(size_t n_args, const mp_obj_t *args, mp_map_t *kw)
{
    return pin_init_helper(MP_OBJ_TO_PTR(args[0]), n_args - 1, args + 1, kw);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(pin_init_obj, 1, pin_init);

static const mp_rom_map_elem_t pin_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&pin_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_value), MP_ROM_PTR(&pin_value_obj) },
    { MP_ROM_QSTR(MP_QSTR_on), MP_ROM_PTR(&pin_on_obj) },
    { MP_ROM_QSTR(MP_QSTR_off), MP_ROM_PTR(&pin_off_obj) },
    { MP_ROM_QSTR(MP_QSTR_toggle), MP_ROM_PTR(&pin_toggle_obj) },
    { MP_ROM_QSTR(MP_QSTR_IN), MP_ROM_INT(PIN_IN) },
    { MP_ROM_QSTR(MP_QSTR_OUT), MP_ROM_INT(PIN_OUT) },
    { MP_ROM_QSTR(MP_QSTR_OPEN_DRAIN), MP_ROM_INT(PIN_OPEN_DRAIN) },
    { MP_ROM_QSTR(MP_QSTR_PULL_UP), MP_ROM_INT(PULL_UP) },
    { MP_ROM_QSTR(MP_QSTR_PULL_DOWN), MP_ROM_INT(PULL_DOWN) },
};
static MP_DEFINE_CONST_DICT(pin_locals, pin_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(dm_pin_type, MP_QSTR_Pin, MP_TYPE_FLAG_NONE,
    make_new, pin_make_new, print, pin_print, call, pin_call, locals_dict, &pin_locals);

/* ══ PWM ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    mp_obj_base_t base;
    int gpio;
    uint32_t freq, duty;                          /* duty 0..65535 */
} pwm_obj_t;

static void pwm_apply(pwm_obj_t *p) { check(io()->pwm(p->gpio, p->freq, p->duty), p->gpio); }

static mp_obj_t pwm_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
    enum { ARG_pin, ARG_freq, ARG_duty, ARG_duty_u16, ARG_duty_ns };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_pin, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_freq, MP_ARG_INT, { .u_int = 5000 } },
        { MP_QSTR_duty, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = -1 } },
        { MP_QSTR_duty_u16, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = -1 } },
        { MP_QSTR_duty_ns, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = -1 } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_map_t kw;
    mp_map_init_fixed_table(&kw, n_kw, args + n_args);
    mp_arg_parse_all(n_args, args, &kw, MP_ARRAY_SIZE(allowed), allowed, a);
    pwm_obj_t *p = mp_obj_malloc(pwm_obj_t, type);
    p->gpio = dm_pin_gpio(a[ARG_pin].u_obj);
    p->freq = a[ARG_freq].u_int > 0 ? (uint32_t)a[ARG_freq].u_int : 5000;
    p->duty = 32768;
    if (a[ARG_duty_u16].u_int >= 0) p->duty = (uint32_t)a[ARG_duty_u16].u_int;
    else if (a[ARG_duty].u_int >= 0) p->duty = (uint32_t)a[ARG_duty].u_int * 65535u / 1023u;
    else if (a[ARG_duty_ns].u_int >= 0) p->duty = (uint32_t)((uint64_t)a[ARG_duty_ns].u_int * p->freq * 65535u / 1000000000u);
    if (p->duty > 65535) p->duty = 65535;
    pwm_apply(p);
    return MP_OBJ_FROM_PTR(p);
}

static mp_obj_t pwm_freq(size_t n_args, const mp_obj_t *args)
{
    pwm_obj_t *p = MP_OBJ_TO_PTR(args[0]);
    if (n_args == 1) return mp_obj_new_int_from_uint(p->freq);
    mp_int_t f = mp_obj_get_int(args[1]);
    if (f <= 0) mp_raise_ValueError(MP_ERROR_TEXT("freq must be > 0"));
    p->freq = (uint32_t)f;
    pwm_apply(p);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pwm_freq_obj, 1, 2, pwm_freq);

static mp_obj_t pwm_duty_u16(size_t n_args, const mp_obj_t *args)
{
    pwm_obj_t *p = MP_OBJ_TO_PTR(args[0]);
    if (n_args == 1) return MP_OBJ_NEW_SMALL_INT(p->duty);
    mp_int_t d = mp_obj_get_int(args[1]);
    p->duty = d < 0 ? 0 : d > 65535 ? 65535 : (uint32_t)d;
    pwm_apply(p);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pwm_duty_u16_obj, 1, 2, pwm_duty_u16);

static mp_obj_t pwm_duty(size_t n_args, const mp_obj_t *args)        /* ESP32 style: 0..1023 */
{
    pwm_obj_t *p = MP_OBJ_TO_PTR(args[0]);
    if (n_args == 1) return MP_OBJ_NEW_SMALL_INT(p->duty * 1023u / 65535u);
    mp_int_t d = mp_obj_get_int(args[1]);
    p->duty = d < 0 ? 0 : d > 1023 ? 65535 : (uint32_t)d * 65535u / 1023u;
    pwm_apply(p);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pwm_duty_obj, 1, 2, pwm_duty);

static mp_obj_t pwm_duty_ns(size_t n_args, const mp_obj_t *args)
{
    pwm_obj_t *p = MP_OBJ_TO_PTR(args[0]);
    uint64_t period_ns = 1000000000ull / p->freq;
    if (n_args == 1) return mp_obj_new_int_from_ull(period_ns * p->duty / 65535u);
    mp_int_t ns = mp_obj_get_int(args[1]);
    uint64_t d = ns <= 0 ? 0 : (uint64_t)ns * 65535u / period_ns;
    p->duty = d > 65535 ? 65535 : (uint32_t)d;
    pwm_apply(p);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pwm_duty_ns_obj, 1, 2, pwm_duty_ns);

static mp_obj_t pwm_deinit(mp_obj_t s)
{
    pwm_obj_t *p = MP_OBJ_TO_PTR(s);
    io()->pwm(p->gpio, 0, 0);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(pwm_deinit_obj, pwm_deinit);

static void pwm_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    (void)kind;
    pwm_obj_t *p = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "PWM(Pin(%d), freq=%u, duty_u16=%u)", p->gpio, (unsigned)p->freq, (unsigned)p->duty);
}

static const mp_rom_map_elem_t pwm_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_freq), MP_ROM_PTR(&pwm_freq_obj) },
    { MP_ROM_QSTR(MP_QSTR_duty_u16), MP_ROM_PTR(&pwm_duty_u16_obj) },
    { MP_ROM_QSTR(MP_QSTR_duty), MP_ROM_PTR(&pwm_duty_obj) },
    { MP_ROM_QSTR(MP_QSTR_duty_ns), MP_ROM_PTR(&pwm_duty_ns_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&pwm_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(pwm_locals, pwm_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(dm_pwm_type, MP_QSTR_PWM, MP_TYPE_FLAG_NONE,
    make_new, pwm_make_new, print, pwm_print, locals_dict, &pwm_locals);

/* ══ ADC ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    mp_obj_base_t base;
    int gpio;
} adc_obj_t;

static mp_obj_t adc_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
    mp_arg_check_num(n_args, n_kw, 1, 1, true);
    adc_obj_t *a = mp_obj_malloc(adc_obj_t, type);
    a->gpio = dm_pin_gpio(args[0]);
    int mv;
    check(io()->adc(a->gpio, &mv), a->gpio);
    return MP_OBJ_FROM_PTR(a);
}

static int adc_raw(adc_obj_t *a, int *mv)
{
    int r = io()->adc(a->gpio, mv);
    check(r, a->gpio);
    return r;
}

static mp_obj_t adc_read_u16(mp_obj_t s) { int mv; return MP_OBJ_NEW_SMALL_INT(adc_raw(MP_OBJ_TO_PTR(s), &mv) * 65535 / 4095); }
static mp_obj_t adc_read(mp_obj_t s)     { int mv; return MP_OBJ_NEW_SMALL_INT(adc_raw(MP_OBJ_TO_PTR(s), &mv)); }
static mp_obj_t adc_read_uv(mp_obj_t s)  { int mv; adc_raw(MP_OBJ_TO_PTR(s), &mv); return mp_obj_new_int(mv * 1000); }
static mp_obj_t adc_noop(size_t n_args, const mp_obj_t *args) { (void)n_args; (void)args; return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_1(adc_read_u16_obj, adc_read_u16);
static MP_DEFINE_CONST_FUN_OBJ_1(adc_read_obj, adc_read);
static MP_DEFINE_CONST_FUN_OBJ_1(adc_read_uv_obj, adc_read_uv);
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(adc_noop_obj, 1, 2, adc_noop);

static const mp_rom_map_elem_t adc_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read_u16), MP_ROM_PTR(&adc_read_u16_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&adc_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_uv), MP_ROM_PTR(&adc_read_uv_obj) },
    { MP_ROM_QSTR(MP_QSTR_atten), MP_ROM_PTR(&adc_noop_obj) },
    { MP_ROM_QSTR(MP_QSTR_width), MP_ROM_PTR(&adc_noop_obj) },
    { MP_ROM_QSTR(MP_QSTR_ATTN_0DB), MP_ROM_INT(0) },
    { MP_ROM_QSTR(MP_QSTR_ATTN_2_5DB), MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_ATTN_6DB), MP_ROM_INT(2) },
    { MP_ROM_QSTR(MP_QSTR_ATTN_11DB), MP_ROM_INT(3) },
    { MP_ROM_QSTR(MP_QSTR_WIDTH_12BIT), MP_ROM_INT(12) },
};
static MP_DEFINE_CONST_DICT(adc_locals, adc_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(dm_adc_type, MP_QSTR_ADC, MP_TYPE_FLAG_NONE,
    make_new, adc_make_new, locals_dict, &adc_locals);

/* ══ I2C (the header's SDA/SCL pins: the board's own bus, 100 kHz) ══════════ */
typedef struct { mp_obj_base_t base; } i2c_obj_t;

static mp_obj_t i2c_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
    (void)n_args; (void)n_kw; (void)args;      /* id / scl / sda / freq accepted and ignored */
    return MP_OBJ_FROM_PTR(mp_obj_malloc(i2c_obj_t, type));
}

static mp_obj_t i2c_scan(mp_obj_t s)
{
    (void)s;
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (int a = 0x08; a < 0x78; a++)
        if (io()->i2c_probe((uint8_t)a) == 0) mp_obj_list_append(list, MP_OBJ_NEW_SMALL_INT(a));
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_1(i2c_scan_obj, i2c_scan);

static void i2c_check(int r, int addr)
{
    if (r >= 0) return;
    if (r == PAPP_E_ARG) mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("I2C address 0x%02x is used by the board"), addr);
    mp_raise_OSError(MP_ENODEV);                 /* no ACK, like MicroPython on an ESP32 */
}

static mp_obj_t i2c_writeto(size_t n_args, const mp_obj_t *args)
{
    int addr = mp_obj_get_int(args[1]);
    mp_buffer_info_t b;
    mp_get_buffer_raise(args[2], &b, MP_BUFFER_READ);
    i2c_check(io()->i2c_write((uint8_t)addr, b.buf, b.len), addr);
    return MP_OBJ_NEW_SMALL_INT(b.len);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(i2c_writeto_obj, 3, 4, i2c_writeto);

static mp_obj_t i2c_readfrom(size_t n_args, const mp_obj_t *args)
{
    int addr = mp_obj_get_int(args[1]);
    vstr_t v;
    vstr_init_len(&v, mp_obj_get_int(args[2]));
    i2c_check(io()->i2c_read((uint8_t)addr, v.buf, v.len), addr);
    return mp_obj_new_bytes_from_vstr(&v);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(i2c_readfrom_obj, 3, 4, i2c_readfrom);

static mp_obj_t i2c_readfrom_into(size_t n_args, const mp_obj_t *args)
{
    int addr = mp_obj_get_int(args[1]);
    mp_buffer_info_t b;
    mp_get_buffer_raise(args[2], &b, MP_BUFFER_WRITE);
    i2c_check(io()->i2c_read((uint8_t)addr, b.buf, b.len), addr);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(i2c_readfrom_into_obj, 3, 4, i2c_readfrom_into);

static size_t memaddr_bytes(uint8_t *out, mp_int_t mem, mp_int_t addrsize)
{
    size_t n = addrsize > 8 ? 2 : 1;
    if (n == 2) { out[0] = (uint8_t)(mem >> 8); out[1] = (uint8_t)mem; }
    else out[0] = (uint8_t)mem;
    return n;
}

static mp_obj_t i2c_readfrom_mem(size_t n_args, const mp_obj_t *args, mp_map_t *kw)
{
    enum { ARG_self, ARG_addr, ARG_memaddr, ARG_n, ARG_addrsize };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_addr, MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_memaddr, MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_n, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_addrsize, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 8 } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, args, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    uint8_t ma[2];
    size_t ml = memaddr_bytes(ma, a[ARG_memaddr].u_int, a[ARG_addrsize].u_int);
    int addr = (int)a[ARG_addr].u_int;
    if (mp_obj_is_int(a[ARG_n].u_obj)) {                         /* readfrom_mem(addr, mem, n) */
        vstr_t v;
        vstr_init_len(&v, mp_obj_get_int(a[ARG_n].u_obj));
        i2c_check(io()->i2c_write_read((uint8_t)addr, ma, ml, v.buf, v.len), addr);
        return mp_obj_new_bytes_from_vstr(&v);
    }
    mp_buffer_info_t b;                                         /* readfrom_mem_into(addr, mem, buf) */
    mp_get_buffer_raise(a[ARG_n].u_obj, &b, MP_BUFFER_WRITE);
    i2c_check(io()->i2c_write_read((uint8_t)addr, ma, ml, b.buf, b.len), addr);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(i2c_readfrom_mem_obj, 4, i2c_readfrom_mem);

static mp_obj_t i2c_writeto_mem(size_t n_args, const mp_obj_t *args, mp_map_t *kw)
{
    enum { ARG_self, ARG_addr, ARG_memaddr, ARG_buf, ARG_addrsize };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_addr, MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_memaddr, MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_buf, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_addrsize, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 8 } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, args, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    mp_buffer_info_t b;
    mp_get_buffer_raise(a[ARG_buf].u_obj, &b, MP_BUFFER_READ);
    uint8_t *tmp = m_new(uint8_t, b.len + 2);
    size_t ml = memaddr_bytes(tmp, a[ARG_memaddr].u_int, a[ARG_addrsize].u_int);
    memcpy(tmp + ml, b.buf, b.len);
    int addr = (int)a[ARG_addr].u_int;
    int r = io()->i2c_write((uint8_t)addr, tmp, ml + b.len);
    m_del(uint8_t, tmp, b.len + 2);
    i2c_check(r, addr);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(i2c_writeto_mem_obj, 4, i2c_writeto_mem);

static const mp_rom_map_elem_t i2c_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_scan), MP_ROM_PTR(&i2c_scan_obj) },
    { MP_ROM_QSTR(MP_QSTR_writeto), MP_ROM_PTR(&i2c_writeto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readfrom), MP_ROM_PTR(&i2c_readfrom_obj) },
    { MP_ROM_QSTR(MP_QSTR_readfrom_into), MP_ROM_PTR(&i2c_readfrom_into_obj) },
    { MP_ROM_QSTR(MP_QSTR_readfrom_mem), MP_ROM_PTR(&i2c_readfrom_mem_obj) },
    { MP_ROM_QSTR(MP_QSTR_readfrom_mem_into), MP_ROM_PTR(&i2c_readfrom_mem_obj) },
    { MP_ROM_QSTR(MP_QSTR_writeto_mem), MP_ROM_PTR(&i2c_writeto_mem_obj) },
};
static MP_DEFINE_CONST_DICT(i2c_locals, i2c_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(dm_i2c_type, MP_QSTR_I2C, MP_TYPE_FLAG_NONE,
    make_new, i2c_make_new, locals_dict, &i2c_locals);

/* ══ SPI (one bus; chip select = a Pin the student drives) ═══════════════════ */
typedef struct {
    mp_obj_base_t base;
    int dev;
} spi_obj_t;

static mp_obj_t spi_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
    enum { ARG_id, ARG_baudrate, ARG_polarity, ARG_phase, ARG_bits, ARG_firstbit, ARG_sck, ARG_mosi, ARG_miso };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_id, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_baudrate, MP_ARG_INT, { .u_int = 1000000 } },
        { MP_QSTR_polarity, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_phase, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_bits, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 8 } },
        { MP_QSTR_firstbit, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_sck, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_mosi, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_miso, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_map_t kw;
    mp_map_init_fixed_table(&kw, n_kw, args + n_args);
    mp_arg_parse_all(n_args, args, &kw, MP_ARRAY_SIZE(allowed), allowed, a);
    if (a[ARG_sck].u_obj == mp_const_none)
        mp_raise_ValueError(MP_ERROR_TEXT("give the pins: SPI(1, sck=Pin(..), mosi=Pin(..), miso=Pin(..))"));
    int sck = dm_pin_gpio(a[ARG_sck].u_obj);
    int mosi = a[ARG_mosi].u_obj == mp_const_none ? -1 : dm_pin_gpio(a[ARG_mosi].u_obj);
    int miso = a[ARG_miso].u_obj == mp_const_none ? -1 : dm_pin_gpio(a[ARG_miso].u_obj);
    check(io()->spi_open(sck, mosi, miso), sck);
    spi_obj_t *s = mp_obj_malloc(spi_obj_t, type);
    s->dev = io()->spi_device(-1, (uint32_t)a[ARG_baudrate].u_int,
                              (int)((a[ARG_polarity].u_int & 1) << 1 | (a[ARG_phase].u_int & 1)));
    check(s->dev, sck);
    return MP_OBJ_FROM_PTR(s);
}

static void spi_xfer(spi_obj_t *s, const void *tx, void *rx, size_t len)
{
    const uint8_t *t = tx;
    uint8_t *r = rx;
    while (len) {
        size_t n = len > 4096 ? 4096 : len;
        if (io()->spi_transfer(s->dev, t, r, n) < 0) mp_raise_OSError(MP_EIO);
        if (t) t += n;
        if (r) r += n;
        len -= n;
    }
}

static mp_obj_t spi_write(mp_obj_t self, mp_obj_t buf)
{
    mp_buffer_info_t b;
    mp_get_buffer_raise(buf, &b, MP_BUFFER_READ);
    spi_xfer(MP_OBJ_TO_PTR(self), b.buf, NULL, b.len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(spi_write_obj, spi_write);

static mp_obj_t spi_read(size_t n_args, const mp_obj_t *args)
{
    vstr_t v;
    vstr_init_len(&v, mp_obj_get_int(args[1]));
    memset(v.buf, n_args > 2 ? mp_obj_get_int(args[2]) : 0, v.len);
    spi_xfer(MP_OBJ_TO_PTR(args[0]), v.buf, v.buf, v.len);
    return mp_obj_new_bytes_from_vstr(&v);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(spi_read_obj, 2, 3, spi_read);

static mp_obj_t spi_readinto(size_t n_args, const mp_obj_t *args)
{
    mp_buffer_info_t b;
    mp_get_buffer_raise(args[1], &b, MP_BUFFER_WRITE);
    memset(b.buf, n_args > 2 ? mp_obj_get_int(args[2]) : 0, b.len);
    spi_xfer(MP_OBJ_TO_PTR(args[0]), b.buf, b.buf, b.len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(spi_readinto_obj, 2, 3, spi_readinto);

static mp_obj_t spi_write_readinto(mp_obj_t self, mp_obj_t w, mp_obj_t r)
{
    mp_buffer_info_t bw, br;
    mp_get_buffer_raise(w, &bw, MP_BUFFER_READ);
    mp_get_buffer_raise(r, &br, MP_BUFFER_WRITE);
    if (bw.len != br.len) mp_raise_ValueError(MP_ERROR_TEXT("buffers must be the same length"));
    spi_xfer(MP_OBJ_TO_PTR(self), bw.buf, br.buf, bw.len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(spi_write_readinto_obj, spi_write_readinto);

static mp_obj_t spi_deinit(mp_obj_t self) { (void)self; io()->spi_close(); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_1(spi_deinit_obj, spi_deinit);

static const mp_rom_map_elem_t spi_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&spi_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&spi_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&spi_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_write_readinto), MP_ROM_PTR(&spi_write_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&spi_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_MSB), MP_ROM_INT(0) },
    { MP_ROM_QSTR(MP_QSTR_LSB), MP_ROM_INT(1) },
};
static MP_DEFINE_CONST_DICT(spi_locals, spi_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(dm_spi_type, MP_QSTR_SPI, MP_TYPE_FLAG_NONE,
    make_new, spi_make_new, locals_dict, &spi_locals);

/* ══ UART 1 / 2 ════════════════════════════════════════════════════════════ */
typedef struct {
    mp_obj_base_t base;
    int id;
    uint8_t buf[256];
    int n;                                     /* bytes waiting in buf (read ahead by any()) */
} uart_obj_t;

static mp_obj_t uart_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
    enum { ARG_id, ARG_baudrate, ARG_bits, ARG_parity, ARG_stop, ARG_tx, ARG_rx };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_id, MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = 1 } },
        { MP_QSTR_baudrate, MP_ARG_INT, { .u_int = 115200 } },
        { MP_QSTR_bits, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 8 } },
        { MP_QSTR_parity, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_stop, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 1 } },
        { MP_QSTR_tx, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_rx, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_map_t kw;
    mp_map_init_fixed_table(&kw, n_kw, args + n_args);
    mp_arg_parse_all(n_args, args, &kw, MP_ARRAY_SIZE(allowed), allowed, a);
    int id = (int)a[ARG_id].u_int;
    if (id != 1 && id != 2) mp_raise_ValueError(MP_ERROR_TEXT("UART 1 or 2 (UART 0 is the system console)"));
    if (a[ARG_tx].u_obj == mp_const_none && a[ARG_rx].u_obj == mp_const_none)
        mp_raise_ValueError(MP_ERROR_TEXT("give the pins: UART(1, 9600, tx=Pin(..), rx=Pin(..))"));
    int tx = a[ARG_tx].u_obj == mp_const_none ? -1 : dm_pin_gpio(a[ARG_tx].u_obj);
    int rx = a[ARG_rx].u_obj == mp_const_none ? -1 : dm_pin_gpio(a[ARG_rx].u_obj);
    check(io()->uart_open(id, tx, rx, (uint32_t)a[ARG_baudrate].u_int), tx >= 0 ? tx : rx);
    uart_obj_t *u = mp_obj_malloc(uart_obj_t, type);
    u->id = id;
    u->n = 0;
    return MP_OBJ_FROM_PTR(u);
}

static void uart_fill(uart_obj_t *u, uint32_t timeout_ms)
{
    if (u->n >= (int)sizeof u->buf) return;
    int r = io()->uart_read(u->id, u->buf + u->n, sizeof u->buf - u->n, timeout_ms);
    if (r > 0) u->n += r;
}

static mp_obj_t uart_any(mp_obj_t self)
{
    uart_obj_t *u = MP_OBJ_TO_PTR(self);
    uart_fill(u, 0);
    return MP_OBJ_NEW_SMALL_INT(u->n);
}
static MP_DEFINE_CONST_FUN_OBJ_1(uart_any_obj, uart_any);

static mp_obj_t uart_take(uart_obj_t *u, int n)
{
    if (n > u->n) n = u->n;
    if (n == 0) return mp_const_none;                 /* like MicroPython: None on timeout */
    mp_obj_t b = mp_obj_new_bytes(u->buf, n);
    memmove(u->buf, u->buf + n, u->n - n);
    u->n -= n;
    return b;
}

static mp_obj_t uart_read(size_t n_args, const mp_obj_t *args)
{
    uart_obj_t *u = MP_OBJ_TO_PTR(args[0]);
    int want = n_args > 1 ? mp_obj_get_int(args[1]) : (int)sizeof u->buf;
    if (want > (int)sizeof u->buf) want = sizeof u->buf;
    uint64_t end = papp_time_us() + 50000;          /* 50 ms, like the default timeout on a board */
    while (u->n < want && (uint64_t)papp_time_us() < end) uart_fill(u, 10);
    return uart_take(u, want);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(uart_read_obj, 1, 2, uart_read);

static mp_obj_t uart_readline(mp_obj_t self)
{
    uart_obj_t *u = MP_OBJ_TO_PTR(self);
    uint64_t end = papp_time_us() + 1000000;
    for (;;) {
        for (int i = 0; i < u->n; i++) if (u->buf[i] == '\n') return uart_take(u, i + 1);
        if (u->n >= (int)sizeof u->buf || (uint64_t)papp_time_us() > end) return uart_take(u, u->n);
        uart_fill(u, 10);
        mp_handle_pending(true);
    }
}
static MP_DEFINE_CONST_FUN_OBJ_1(uart_readline_obj, uart_readline);

static mp_obj_t uart_write(mp_obj_t self, mp_obj_t buf)
{
    uart_obj_t *u = MP_OBJ_TO_PTR(self);
    mp_buffer_info_t b;
    mp_get_buffer_raise(buf, &b, MP_BUFFER_READ);
    int r = io()->uart_write(u->id, b.buf, b.len);
    if (r < 0) mp_raise_OSError(MP_EIO);
    return MP_OBJ_NEW_SMALL_INT(r);
}
static MP_DEFINE_CONST_FUN_OBJ_2(uart_write_obj, uart_write);

static mp_obj_t uart_deinit(mp_obj_t self)
{
    uart_obj_t *u = MP_OBJ_TO_PTR(self);
    io()->uart_close(u->id);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(uart_deinit_obj, uart_deinit);

static const mp_rom_map_elem_t uart_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_any), MP_ROM_PTR(&uart_any_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&uart_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&uart_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&uart_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&uart_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(uart_locals, uart_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(dm_uart_type, MP_QSTR_UART, MP_TYPE_FLAG_NONE,
    make_new, uart_make_new, locals_dict, &uart_locals);

/* ══ RTC (Thonny sets the clock on connect: accepted, the launcher keeps time) ══ */
typedef struct { mp_obj_base_t base; } rtc_obj_t;

static mp_obj_t rtc_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
    (void)n_args; (void)n_kw; (void)args;
    return MP_OBJ_FROM_PTR(mp_obj_malloc(rtc_obj_t, type));
}

static mp_obj_t rtc_datetime(size_t n_args, const mp_obj_t *args)
{
    (void)args;
    if (n_args > 1) return mp_const_none;
    papp_datetime_t t = { .year = 2000, .month = 1, .day = 1 };
    papp_local_time(&t);
    mp_obj_t tu[8] = { mp_obj_new_int(t.year), mp_obj_new_int(t.month), mp_obj_new_int(t.day),
                       mp_obj_new_int(t.weekday ? t.weekday - 1 : 6), mp_obj_new_int(t.hour),
                       mp_obj_new_int(t.minute), mp_obj_new_int(t.second), mp_obj_new_int(0) };
    return mp_obj_new_tuple(8, tu);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(rtc_datetime_obj, 1, 2, rtc_datetime);

static const mp_rom_map_elem_t rtc_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_datetime), MP_ROM_PTR(&rtc_datetime_obj) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&rtc_datetime_obj) },
};
static MP_DEFINE_CONST_DICT(rtc_locals, rtc_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(dm_rtc_type, MP_QSTR_RTC, MP_TYPE_FLAG_NONE,
    make_new, rtc_make_new, locals_dict, &rtc_locals);

/* ══ module functions ══════════════════════════════════════════════════════ */
static mp_obj_t machine_time_pulse_us(size_t n_args, const mp_obj_t *args)
{
    int gpio = dm_pin_gpio(args[0]);
    int level = mp_obj_get_int(args[1]);
    mp_int_t to = n_args > 2 ? mp_obj_get_int(args[2]) : 1000000;
    int32_t r = io()->pulse_in(gpio, level, (uint32_t)(to > 0 ? to : 1000000));
    check(r, gpio);
    return MP_OBJ_NEW_SMALL_INT(r == 0 ? -2 : r);      /* -2 = timeout, as on a board */
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_time_pulse_us_obj, 2, 3, machine_time_pulse_us);

static mp_obj_t machine_freq(size_t n_args, const mp_obj_t *args)
{
    (void)args;
    return n_args ? mp_const_none : mp_obj_new_int(400000000);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_freq_obj, 0, 1, machine_freq);

static mp_obj_t machine_reset(void)
{
    mp_raise_type(&mp_type_SystemExit);              /* the REPL does a soft reset */
}
static MP_DEFINE_CONST_FUN_OBJ_0(machine_reset_obj, machine_reset);

static mp_obj_t machine_unique_id(void)
{
    static const uint8_t id[6] = { 0x80, 0xf1, 0xb2, 0x50, 0x34, 0x01 };
    return mp_obj_new_bytes(id, sizeof id);
}
static MP_DEFINE_CONST_FUN_OBJ_0(machine_unique_id_obj, machine_unique_id);

static mp_obj_t machine_idle(void) { mp_hal_delay_ms(1); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_0(machine_idle_obj, machine_idle);

static mp_obj_t machine_lightsleep(size_t n_args, const mp_obj_t *args)
{
    if (n_args) mp_hal_delay_ms(mp_obj_get_int(args[0]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_lightsleep_obj, 0, 1, machine_lightsleep);

static mp_obj_t machine_disable_irq(void) { return MP_OBJ_NEW_SMALL_INT(0); }
static MP_DEFINE_CONST_FUN_OBJ_0(machine_disable_irq_obj, machine_disable_irq);
static mp_obj_t machine_enable_irq(mp_obj_t s) { (void)s; return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_1(machine_enable_irq_obj, machine_enable_irq);

static const mp_rom_map_elem_t machine_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_machine) },
    { MP_ROM_QSTR(MP_QSTR_Pin), MP_ROM_PTR(&dm_pin_type) },
    { MP_ROM_QSTR(MP_QSTR_PWM), MP_ROM_PTR(&dm_pwm_type) },
    { MP_ROM_QSTR(MP_QSTR_ADC), MP_ROM_PTR(&dm_adc_type) },
    { MP_ROM_QSTR(MP_QSTR_I2C), MP_ROM_PTR(&dm_i2c_type) },
    { MP_ROM_QSTR(MP_QSTR_SoftI2C), MP_ROM_PTR(&dm_i2c_type) },
    { MP_ROM_QSTR(MP_QSTR_SPI), MP_ROM_PTR(&dm_spi_type) },
    { MP_ROM_QSTR(MP_QSTR_SoftSPI), MP_ROM_PTR(&dm_spi_type) },
    { MP_ROM_QSTR(MP_QSTR_UART), MP_ROM_PTR(&dm_uart_type) },
    { MP_ROM_QSTR(MP_QSTR_RTC), MP_ROM_PTR(&dm_rtc_type) },
    { MP_ROM_QSTR(MP_QSTR_time_pulse_us), MP_ROM_PTR(&machine_time_pulse_us_obj) },
    { MP_ROM_QSTR(MP_QSTR_freq), MP_ROM_PTR(&machine_freq_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&machine_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_soft_reset), MP_ROM_PTR(&machine_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_unique_id), MP_ROM_PTR(&machine_unique_id_obj) },
    { MP_ROM_QSTR(MP_QSTR_idle), MP_ROM_PTR(&machine_idle_obj) },
    { MP_ROM_QSTR(MP_QSTR_lightsleep), MP_ROM_PTR(&machine_lightsleep_obj) },
    { MP_ROM_QSTR(MP_QSTR_deepsleep), MP_ROM_PTR(&machine_lightsleep_obj) },
    { MP_ROM_QSTR(MP_QSTR_disable_irq), MP_ROM_PTR(&machine_disable_irq_obj) },
    { MP_ROM_QSTR(MP_QSTR_enable_irq), MP_ROM_PTR(&machine_enable_irq_obj) },
};
static MP_DEFINE_CONST_DICT(machine_globals, machine_globals_table);

const mp_obj_module_t dm_module_machine = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&machine_globals,
};
MP_REGISTER_MODULE(MP_QSTR_machine, dm_module_machine);
