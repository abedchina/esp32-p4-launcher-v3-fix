/* lx_ai: ESP-DL models for apps (up to LX_AI_SLOTS loaded at once), see lx_ai.h */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <new>
#include <vector>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "dl_model_base.hpp"
#include "lx_ai.h"

static const char *TAG = "lx_ai";
static dl::Model *s_models[LX_AI_SLOTS];
static std::vector<dl::TensorBase *> s_outv[LX_AI_SLOTS];
static int s_cur;                                    /* selected slot: all calls below work on it */
#define s_model s_models[s_cur]
#define s_outs  s_outv[s_cur]
static SemaphoreHandle_t s_lock;

static void lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_lock, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_lock); }

static void fill(dl::TensorBase *t, lx_ai_shape_t *o)
{
    memset(o, 0, sizeof *o);
    o->ndim = (int)t->shape.size() > 4 ? 4 : (int)t->shape.size();
    for (int i = 0; i < o->ndim; i++) o->dims[i] = t->shape[i];
    o->exponent = t->get_exponent();
    o->dtype = (int)t->dtype;
}

static void unload_locked(void)
{
    delete s_model;
    s_model = nullptr;
    s_outs.clear();
}

extern "C" esp_err_t lx_ai_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    fclose(f);
    lock();
    unload_locked();
    int64_t t0 = esp_timer_get_time();
    /* ESP-DL takes "internal" RAM for its buffers; the LP (RTC) RAM counts as internal but the vector
     * instructions fault there (load access fault in the kernels) -> keep it busy while the model loads */
    void *lp[4] = {};
    for (int i = 0; i < 4; i++) {
        size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_RTCRAM);
        if (big < 64) break;
        lp[i] = heap_caps_malloc(big, MALLOC_CAP_RTCRAM);
    }
    s_model = new (std::nothrow) dl::Model(path, fbs::MODEL_LOCATION_IN_SDCARD);
    for (int i = 0; i < 4; i++) heap_caps_free(lp[i]);
    if (!s_model || s_model->get_inputs().empty() || s_model->get_outputs().empty()) {
        ESP_LOGE(TAG, "cannot load %s", path);
        unload_locked();
        unlock();
        return ESP_FAIL;
    }
    for (auto &kv : s_model->get_outputs()) s_outs.push_back(kv.second);
    ESP_LOGI(TAG, "loaded %s in %lld ms (%d outputs)", path, (esp_timer_get_time() - t0) / 1000, (int)s_outs.size());
    unlock();
    return ESP_OK;
}

extern "C" void lx_ai_unload(void)
{
    lock();
    unload_locked();
    unlock();
}

extern "C" int lx_ai_select(int slot)
{
    if (slot < 0 || slot >= LX_AI_SLOTS) return -1;
    lock();
    int prev = s_cur;
    s_cur = slot;
    unlock();
    return prev;
}

extern "C" void lx_ai_unload_all(void)
{
    lock();
    for (int i = 0; i < LX_AI_SLOTS; i++) { s_cur = i; unload_locked(); }
    s_cur = 0;
    unlock();
}

extern "C" int lx_ai_input(lx_ai_shape_t *out)
{
    lock();
    int r = -1;
    if (s_model) { fill(s_model->get_input(), out); r = 0; }
    unlock();
    return r;
}

extern "C" int lx_ai_outputs(void)
{
    lock();
    int n = s_model ? (int)s_outs.size() : 0;
    unlock();
    return n;
}

extern "C" int lx_ai_output(int idx, lx_ai_shape_t *out)
{
    lock();
    int r = -1;
    if (s_model && idx >= 0 && idx < (int)s_outs.size()) { fill(s_outs[idx], out); r = 0; }
    unlock();
    return r;
}

extern "C" int64_t lx_ai_run(const uint16_t *img, int w, int h, int stride, float scale, float offset, int letterbox)
{
    if (!img || w <= 0 || h <= 0) return -1;
    lock();
    if (!s_model) { unlock(); return -1; }
    dl::TensorBase *in = s_model->get_input();
    if (in->shape.size() != 4 || (in->dtype != dl::DATA_TYPE_INT8 && in->dtype != dl::DATA_TYPE_INT16)) {
        unlock();
        return -2;
    }
    const int H = in->shape[1], W = in->shape[2], C = in->shape[3];
    const float inv = ldexpf(1.0f, -in->get_exponent());       /* q = v / 2^exponent */
    const bool i16 = in->dtype == dl::DATA_TYPE_INT16;
    int16_t lut[256];
    for (int i = 0; i < 256; i++) {
        long q = lroundf((i * scale + offset) * inv);
        long lo = i16 ? -32768 : -128, hi = i16 ? 32767 : 127;
        lut[i] = (int16_t)(q < lo ? lo : q > hi ? hi : q);
    }
    int ox = 0, oy = 0, rw = W, rh = H;
    if (letterbox) {
        float r = fminf((float)W / w, (float)H / h);
        rw = (int)lroundf(w * r); rh = (int)lroundf(h * r);
        if (rw > W) rw = W;
        if (rh > H) rh = H;
        ox = (W - rw) / 2; oy = (H - rh) / 2;
    }
    int8_t *d8 = (int8_t *)in->get_element_ptr();
    int16_t *d16 = (int16_t *)in->get_element_ptr();
    const int16_t pad = lut[114];
    for (int y = 0; y < H; y++) {
        int sy = y - oy;
        bool row_in = sy >= 0 && sy < rh;
        const uint16_t *src = row_in ? img + (size_t)(sy * h / rh) * stride : nullptr;
        for (int x = 0; x < W; x++) {
            int sx = x - ox;
            int16_t r = pad, g = pad, b = pad;
            if (row_in && sx >= 0 && sx < rw) {
                uint16_t p = src[sx * w / rw];
                int r5 = (p >> 11) & 31, g6 = (p >> 5) & 63, b5 = p & 31;
                r = lut[(r5 << 3) | (r5 >> 2)];
                g = lut[(g6 << 2) | (g6 >> 4)];
                b = lut[(b5 << 3) | (b5 >> 2)];
            }
            size_t o = ((size_t)y * W + x) * C;
            if (i16) { d16[o] = r; if (C > 1) d16[o + 1] = g; if (C > 2) d16[o + 2] = b; }
            else     { d8[o] = (int8_t)r; if (C > 1) d8[o + 1] = (int8_t)g; if (C > 2) d8[o + 2] = (int8_t)b; }
        }
    }
    int64_t t0 = esp_timer_get_time();
    s_model->run();
    int64_t dt = esp_timer_get_time() - t0;
    unlock();
    return dt;
}

extern "C" int lx_ai_read(int idx, float *dst, int max)
{
    lock();
    if (!s_model || idx < 0 || idx >= (int)s_outs.size() || !dst) { unlock(); return 0; }
    dl::TensorBase *t = s_outs[idx];
    int n = t->get_size();
    if (n > max) n = max;
    const float s = ldexpf(1.0f, t->get_exponent());
    switch (t->dtype) {
    case dl::DATA_TYPE_INT8:  { const int8_t *p = (const int8_t *)t->get_element_ptr(); for (int i = 0; i < n; i++) dst[i] = p[i] * s; break; }
    case dl::DATA_TYPE_INT16: { const int16_t *p = (const int16_t *)t->get_element_ptr(); for (int i = 0; i < n; i++) dst[i] = p[i] * s; break; }
    case dl::DATA_TYPE_FLOAT: memcpy(dst, t->get_element_ptr(), n * sizeof(float)); break;
    default: n = 0;
    }
    unlock();
    return n;
}
