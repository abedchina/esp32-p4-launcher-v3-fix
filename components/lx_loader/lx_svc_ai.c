/* AI service: one ESP-DL model for apps (lx_ai). The model is unloaded at teardown. */
#include <string.h>
#include "lx_services.h"
#include "lx_ai.h"

static bool s_loaded;

static void to_shape(const lx_ai_shape_t *a, papp_ai_shape_t *o)
{
    memset(o, 0, sizeof *o);
    o->ndim = a->ndim;
    for (int i = 0; i < 4; i++) o->dims[i] = a->dims[i];
    o->exponent = a->exponent;
    o->dtype = a->dtype;
}

static int ai_load(const char *path)
{
    if (!path) return PAPP_E_UNSUPPORTED;
    SVC_ENTER();
    esp_err_t e = lx_ai_load(path);
    s_loaded = e == ESP_OK;
    SVC_LEAVE();
    return e == ESP_OK ? PAPP_E_OK : e == ESP_ERR_NOT_FOUND ? PAPP_E_NOT_CONNECTED : PAPP_E_UNSUPPORTED;
}

static void ai_unload(void)
{
    SVC_ENTER();
    lx_ai_unload();
    s_loaded = false;
    SVC_LEAVE();
}

static int ai_input(papp_ai_shape_t *out)
{
    if (!out) return PAPP_E_UNSUPPORTED;
    SVC_ENTER();
    lx_ai_shape_t a;
    int r = lx_ai_input(&a);
    if (r == 0) to_shape(&a, out);
    SVC_LEAVE();
    return r == 0 ? PAPP_E_OK : PAPP_E_UNSUPPORTED;
}

static int ai_outputs(void)
{
    SVC_ENTER();
    int n = lx_ai_outputs();
    SVC_LEAVE();
    return n;
}

static int ai_output(int idx, papp_ai_shape_t *out)
{
    if (!out) return PAPP_E_UNSUPPORTED;
    SVC_ENTER();
    lx_ai_shape_t a;
    int r = lx_ai_output(idx, &a);
    if (r == 0) to_shape(&a, out);
    SVC_LEAVE();
    return r == 0 ? PAPP_E_OK : PAPP_E_UNSUPPORTED;
}

static int64_t ai_run(const uint16_t *img, int w, int h, int stride, float scale, float offset, int letterbox)
{
    if (lx_svc_exit_flag) return PAPP_E_EXIT;
    SVC_ENTER();
    int64_t r = lx_ai_run(img, w, h, stride, scale, offset, letterbox);
    SVC_LEAVE();
    return r < 0 ? PAPP_E_UNSUPPORTED : r;
}

static int ai_read(int idx, float *dst, int max)
{
    SVC_ENTER();
    int n = lx_ai_read(idx, dst, max);
    SVC_LEAVE();
    return n;
}

void lx_svc_ai_end_run(void)
{
    if (s_loaded) lx_ai_unload();
    s_loaded = false;
}

const papp_if_ai_t lx_if_ai = {
    .hdr = { PAPP_IF_AI, PAPP_IF_AI_VER, sizeof(papp_if_ai_t) },
    .load = ai_load,
    .unload = ai_unload,
    .input = ai_input,
    .outputs = ai_outputs,
    .output = ai_output,
    .run = ai_run,
    .read = ai_read,
};
