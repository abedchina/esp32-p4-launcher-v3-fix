/*
 * "p4" for Developer Mode: the board's AI models and USB camera from Python.
 *
 *   import p4
 *   p4.ai.load("eyes")              # /sd/models/eyes.espdl (+ eyes.txt): camera + AI start
 *   p4.ai.seen("closed")            # in the latest picture?
 *   p4.ai.seen("closed", 4)         # ... for 4 seconds in a row
 *   p4.ai.top()                     # ("closed", 0.88) or None
 *   p4.ai.result()                  # [("closed", 0.88, x, y, w, h), ...]  (640 x 480 picture)
 *   p4.ai.count("car")  p4.ai.labels()  p4.ai.ms()  p4.ai.stop()
 *
 * The camera runs in the screen thread (live picture with boxes on the board's screen),
 * the model on core 1 in its own thread; Python only reads the latest result, so a
 * student's loop never waits for the AI.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include "py/runtime.h"
#include "py/mperrno.h"
#include "papp/papp.h"
#include "devmode.h"

#define CW 640
#define CH 480
#define MAXL 32
#define MAXD 20
#define MAXO 8

typedef struct { float x, y, w, h, score; int cls; } det_t;

static struct {
    char name[40], labels[MAXL][24];
    int nlabels, task;                          /* 0 yolo, 1 classifier */
    float conf, scale, offset;
} M;

static const papp_if_ai_t *ai;
static const papp_if_cam_t *cam;
/* The camera is touched by ONE thread only: the screen thread (dm_ai_draw) opens it and
 * reads frames; dm_ai_stop() first takes the screen thread out (s_loaded = false, wait
 * while s_in_draw), then closes it. Two threads opening / closing the UVC stream at the
 * same time crashed the launcher's camera task. */
static volatile bool s_loaded, s_cam_open, s_run, s_in_draw;
static papp_thread_t *s_thread;
static uint16_t *s_frame;                       /* CW x CH copy the AI works on */
static volatile int s_state;                    /* 0 wants a frame, 1 ready, 2 busy */
static det_t s_res[2][MAXD];
static volatile int s_res_n[2], s_res_i;
static volatile int64_t s_ms;
static int64_t s_since[MAXL];                   /* label present since (us), 0 = not now */
static float *s_obuf[MAXO], *s_out0;
static int s_osz[MAXO], s_nout, s_out0_n, in_w = 320, in_h = 320;
static papp_ai_shape_t s_oshape[MAXO];
static uint32_t s_seq;

bool dm_ai_active(void) { return s_loaded; }

/* ── decode (same as the AI Models app) ─────────────────────────────────── */
static float sigm(float v) { return 1.0f / (1.0f + expf(-v)); }

static float iou(const det_t *a, const det_t *b)
{
    float x0 = fmaxf(a->x - a->w / 2, b->x - b->w / 2), x1 = fminf(a->x + a->w / 2, b->x + b->w / 2);
    float y0 = fmaxf(a->y - a->h / 2, b->y - b->h / 2), y1 = fminf(a->y + a->h / 2, b->y + b->h / 2);
    float in = fmaxf(0, x1 - x0) * fmaxf(0, y1 - y0);
    return in / (a->w * a->h + b->w * b->h - in + 1e-6f);
}

static int decode_raw(det_t *cand, int maxc)
{
    int n = 0;
    float logit_min = logf(M.conf / (1.0f - M.conf));
    for (int o = 0; o < s_nout; o++) {
        papp_ai_shape_t *cs = &s_oshape[o];
        if (cs->ndim != 4 || cs->dims[3] == 64) continue;
        int Hh = cs->dims[1], Ww = cs->dims[2], nc = cs->dims[3], bo = -1;
        for (int b = 0; b < s_nout; b++)
            if (s_oshape[b].ndim == 4 && s_oshape[b].dims[3] == 64 && s_oshape[b].dims[1] == Hh && s_oshape[b].dims[2] == Ww) bo = b;
        if (bo < 0) continue;
        ai->read(o, s_obuf[o], s_osz[o]);
        bool box_read = false;
        float st = (float)in_h / Hh;
        for (int y = 0; y < Hh; y++)
            for (int x = 0; x < Ww; x++) {
                const float *c = &s_obuf[o][(y * Ww + x) * nc];
                int best = 0;
                for (int k = 1; k < nc; k++) if (c[k] > c[best]) best = k;
                if (c[best] < logit_min) continue;
                if (!box_read) { ai->read(bo, s_obuf[bo], s_osz[bo]); box_read = true; }
                const float *b = &s_obuf[bo][(y * Ww + x) * 64];
                float dist[4];
                for (int side = 0; side < 4; side++) {
                    const float *v = b + side * 16;
                    float mx = v[0], sum = 0, ev = 0;
                    for (int i = 1; i < 16; i++) if (v[i] > mx) mx = v[i];
                    for (int i = 0; i < 16; i++) { float e = expf(v[i] - mx); sum += e; ev += e * i; }
                    dist[side] = ev / sum;
                }
                float cx = x + 0.5f, cy = y + 0.5f;
                float x1 = (cx - dist[0]) * st, y1 = (cy - dist[1]) * st, x2 = (cx + dist[2]) * st, y2 = (cy + dist[3]) * st;
                det_t d = { (x1 + x2) / 2, (y1 + y2) / 2, x2 - x1, y2 - y1, sigm(c[best]), best };
                if (n < maxc) cand[n++] = d;
                else {
                    int lo = 0;
                    for (int k = 1; k < maxc; k++) if (cand[k].score < cand[lo].score) lo = k;
                    if (d.score > cand[lo].score) cand[lo] = d;
                }
            }
    }
    return n;
}

static int decode(det_t *out)
{
    papp_ai_shape_t s;
    if (ai->output(0, &s) < 0) return 0;
    if (M.task == 1) {                                     /* classifier: best class */
        int n = ai->read(0, s_out0, s_out0_n);
        if (n == 1) {
            float p = s_out0[0];
            if (p < 0 || p > 1) p = sigm(p);
            out[0] = (det_t){ CW / 2, CH / 2, CW, CH, p >= 0.5f ? p : 1 - p, p >= 0.5f };
            return 1;
        }
        int best = 0;
        for (int i = 1; i < n; i++) if (s_out0[i] > s_out0[best]) best = i;
        float sum = 0, mx = s_out0[best];
        bool prob = true;
        for (int i = 0; i < n; i++) if (s_out0[i] < 0 || s_out0[i] > 1) prob = false;
        if (!prob) { for (int i = 0; i < n; i++) sum += expf(s_out0[i] - mx); mx = 1.0f / sum; }
        out[0] = (det_t){ CW / 2, CH / 2, CW, CH, prob ? s_out0[best] : mx, best };
        return out[0].score >= M.conf;
    }
    det_t cand[64];
    int nc_ = 0;
    float coord_max = 0;
    if (s_nout >= 2 && s_oshape[0].ndim == 4) {
        nc_ = decode_raw(cand, 64);
        coord_max = 1e9f;
    } else {
        ai->read(0, s_out0, s_out0_n);
        int d1 = s.ndim >= 3 ? s.dims[s.ndim - 2] : 1, d2 = s.dims[s.ndim - 1];
        bool cmaj = d1 < d2;
        int C = cmaj ? d1 : d2, N = cmaj ? d2 : d1, nc = C - 4;
        if (nc < 1) return 0;
        for (int i = 0; i < N; i++) {
            int best = 0; float bs = -1;
            for (int c = 0; c < nc; c++) {
                float v = cmaj ? s_out0[(4 + c) * N + i] : s_out0[i * C + 4 + c];
                if (v > bs) { bs = v; best = c; }
            }
            if (bs < M.conf) continue;
            det_t d = { cmaj ? s_out0[i] : s_out0[i * C], cmaj ? s_out0[N + i] : s_out0[i * C + 1],
                        cmaj ? s_out0[2 * N + i] : s_out0[i * C + 2], cmaj ? s_out0[3 * N + i] : s_out0[i * C + 3], bs, best };
            if (d.x > coord_max) coord_max = d.x;
            if (nc_ < 64) cand[nc_++] = d;
            else {
                int lo = 0;
                for (int k = 1; k < 64; k++) if (cand[k].score < cand[lo].score) lo = k;
                if (d.score > cand[lo].score) cand[lo] = d;
            }
        }
    }
    for (int a = 0; a < nc_; a++)
        for (int b = a + 1; b < nc_; b++)
            if (cand[b].score > cand[a].score) { det_t t = cand[a]; cand[a] = cand[b]; cand[b] = t; }
    int k = 0;
    for (int a = 0; a < nc_ && k < MAXD; a++) {
        bool keep = true;
        for (int b = 0; b < k; b++) if (out[b].cls == cand[a].cls && iou(&out[b], &cand[a]) > 0.45f) { keep = false; break; }
        if (keep) out[k++] = cand[a];
    }
    float sc = coord_max <= 1.5f ? (float)in_w : 1.0f;
    float r = fminf((float)in_w / CW, (float)in_h / CH), ox = (in_w - CW * r) / 2, oy = (in_h - CH * r) / 2;
    for (int i = 0; i < k; i++) {
        out[i].x = (out[i].x * sc - ox) / r; out[i].y = (out[i].y * sc - oy) / r;
        out[i].w = out[i].w * sc / r;       out[i].h = out[i].h * sc / r;
    }
    return k;
}

static int ai_thread(void *arg)
{
    (void)arg;
    while (s_run) {
        if (s_state != 1) { papp_delay_ms(3); continue; }
        s_state = 2;
        int64_t us = ai->run(s_frame, CW, CH, CW, M.scale, M.offset, M.task == 0);
        int nxt = s_res_i ^ 1;
        s_res_n[nxt] = us >= 0 ? decode(s_res[nxt]) : 0;
        int64_t now = papp_time_us();
        for (int l = 0; l < M.nlabels && l < MAXL; l++) {
            bool here = false;
            for (int i = 0; i < s_res_n[nxt]; i++) here |= s_res[nxt][i].cls == l;
            if (!here) s_since[l] = 0;
            else if (!s_since[l]) s_since[l] = now;
        }
        s_res_i = nxt;
        s_ms = us / 1000;
        s_state = 0;
    }
    return 0;
}

/* ── load / stop ────────────────────────────────────────────────────────── */
static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ')) *--e = 0;
}

static void read_desc(const char *txt)
{
    M.conf = 0.5f; M.scale = 1.0f / 255.0f; M.offset = 0; M.task = 0; M.nlabels = 0;
    papp_file_t *f = papp_fs()->open(txt, "rb");
    if (!f) return;
    static char buf[2048];
    size_t n = papp_fs()->read(f, buf, sizeof buf - 1);
    papp_fs()->close(f);
    buf[n] = 0;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        trim(line);
        char *v = strchr(line, '=');
        if (!v) continue;
        *v++ = 0;
        if (!strcmp(line, "task")) M.task = !strcmp(v, "cls") ? 1 : !strcmp(v, "yolo") ? 0 : 9;
        else if (!strcmp(line, "conf")) M.conf = strtof(v, NULL);
        else if (!strcmp(line, "scale")) M.scale = strtof(v, NULL);
        else if (!strcmp(line, "offset")) M.offset = strtof(v, NULL);
        else if (!strcmp(line, "labels")) {
            char *p = v;
            while (*p && M.nlabels < MAXL) {
                char *c = strchr(p, ',');
                size_t l = c ? (size_t)(c - p) : strlen(p);
                snprintf(M.labels[M.nlabels++], 24, "%.*s", (int)(l > 23 ? 23 : l), p);
                if (!c) break;
                p = c + 1;
            }
        }
    }
}

void dm_ai_stop(void)
{
    __atomic_store_n(&s_loaded, false, __ATOMIC_SEQ_CST);     /* the screen thread stops drawing */
    for (int i = 0; i < 200 && __atomic_load_n(&s_in_draw, __ATOMIC_SEQ_CST); i++) papp_delay_ms(5);
    s_run = false;
    if (s_thread) papp_thread_join(s_thread, 5000, NULL);
    s_thread = NULL;
    if (ai) ai->unload();
    /* the camera stream stays open for the whole Developer Mode session (the launcher closes it
     * when the app exits): Thonny soft-resets on every connect / Run / Stop, and closing a UVC
     * stream right after it started trips an assert in the USB host stack */
    for (int o = 0; o < MAXO; o++) { papp_free(s_obuf[o]); s_obuf[o] = NULL; }
    papp_free(s_out0); s_out0 = NULL;
    papp_free(s_frame); s_frame = NULL;
    s_res_n[0] = s_res_n[1] = 0;
    memset(s_since, 0, sizeof s_since);
}

static mp_obj_t ai_load(size_t n_args, const mp_obj_t *args)
{
    ai = papp_ai();
    cam = papp_cam();
    if (!ai || !cam) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("update the launcher (0.33 or newer)"));
    dm_ai_stop();
    const char *name = mp_obj_str_get_str(args[0]);
    char base[96], path[128], txt[128];
    const char *b = strrchr(name, '/');
    snprintf(base, sizeof base, "%s", b ? b + 1 : name);
    size_t l = strlen(base);
    if (l > 6 && !strcasecmp(base + l - 6, ".espdl")) base[l - 6] = 0;
    snprintf(path, sizeof path, "/sd/models/%s.espdl", base);
    snprintf(txt, sizeof txt, "/sd/models/%s.txt", base);
    if (papp_fs()->size(path) < 0)
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("no model %s (download it in the AI Models app)"), path);
    read_desc(txt);
    if (M.task > 1) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("hand / face models are not supported in Python yet"));
    if (n_args > 1) M.conf = mp_obj_get_float(args[1]);
    snprintf(M.name, sizeof M.name, "%s", base);
    if (ai->hdr.version >= 2) ai->select(0);
    if (ai->load(path) < 0) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("the model could not be loaded"));
    papp_ai_shape_t is;
    ai->input(&is);
    if (is.ndim == 4) { in_h = is.dims[1]; in_w = is.dims[2]; }
    s_nout = ai->outputs();
    if (s_nout > MAXO) s_nout = MAXO;
    for (int o = 0; o < s_nout; o++) {
        ai->output(o, &s_oshape[o]);
        s_osz[o] = 1;
        for (int i = 0; i < s_oshape[o].ndim; i++) s_osz[o] *= s_oshape[o].dims[i];
        s_obuf[o] = papp_alloc(sizeof(float) * s_osz[o], PAPP_MEM_ANY, 0);
    }
    s_out0_n = s_osz[0];
    s_out0 = papp_alloc(sizeof(float) * s_out0_n, PAPP_MEM_ANY, 0);
    s_frame = papp_alloc(CW * CH * 2, PAPP_MEM_LARGE, 64);
    if (!s_frame || !s_out0) { dm_ai_stop(); mp_raise_OSError(MP_ENOMEM); }
    s_cam_open = false;                               /* the screen thread opens the camera */
    s_state = 0;
    s_run = true;
    papp_thread_cfg_t tc = { .name = "ai", .stack_size = 16384, .prio = PAPP_PRIO_LOW, .core = 1 };
    s_thread = papp_thread_create(ai_thread, NULL, &tc);
    if (!s_thread) { tc.flags |= PAPP_THREAD_STACK_PSRAM; s_thread = papp_thread_create(ai_thread, NULL, &tc); }
    s_loaded = true;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ai_load_obj, 1, 2, ai_load);

static mp_obj_t ai_stop_py(void) { dm_ai_stop(); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_0(ai_stop_obj, ai_stop_py);

static void need_model(void)
{
    if (!s_loaded) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("load a model first: p4.ai.load(\"eyes\")"));
}

static int label_index(mp_obj_t lab)
{
    const char *s = mp_obj_str_get_str(lab);
    for (int i = 0; i < M.nlabels; i++) if (!strcasecmp(s, M.labels[i])) return i;
    mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("the model has no label '%s' - see p4.ai.labels()"), s);
}

static mp_obj_t label_str(int cls)
{
    if (cls >= 0 && cls < M.nlabels) return mp_obj_new_str(M.labels[cls], strlen(M.labels[cls]));
    return MP_OBJ_NEW_SMALL_INT(cls);
}

static mp_obj_t ai_result(void)
{
    need_model();
    int i0 = s_res_i, n = s_res_n[i0];
    det_t d[MAXD];
    memcpy(d, s_res[i0], sizeof(det_t) * n);
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (int i = 0; i < n; i++) {
        mp_obj_t t[6] = { label_str(d[i].cls), mp_obj_new_float(d[i].score),
                          MP_OBJ_NEW_SMALL_INT((int)(d[i].x - d[i].w / 2)), MP_OBJ_NEW_SMALL_INT((int)(d[i].y - d[i].h / 2)),
                          MP_OBJ_NEW_SMALL_INT((int)d[i].w), MP_OBJ_NEW_SMALL_INT((int)d[i].h) };
        mp_obj_list_append(list, mp_obj_new_tuple(6, t));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ai_result_obj, ai_result);

static mp_obj_t ai_top(void)
{
    need_model();
    int i0 = s_res_i;
    if (!s_res_n[i0]) return mp_const_none;
    det_t d = s_res[i0][0];
    mp_obj_t t[2] = { label_str(d.cls), mp_obj_new_float(d.score) };
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ai_top_obj, ai_top);

static mp_obj_t ai_seen(size_t n_args, const mp_obj_t *args)
{
    need_model();
    int l = label_index(args[0]);
    int64_t since = s_since[l];
    if (!since) return mp_const_false;
    float sec = n_args > 1 ? mp_obj_get_float(args[1]) : 0;
    return mp_obj_new_bool(papp_time_us() - since >= (int64_t)(sec * 1e6f));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ai_seen_obj, 1, 2, ai_seen);

static mp_obj_t ai_count(mp_obj_t lab)
{
    need_model();
    int l = label_index(lab), i0 = s_res_i, c = 0;
    for (int i = 0; i < s_res_n[i0]; i++) c += s_res[i0][i].cls == l;
    return MP_OBJ_NEW_SMALL_INT(c);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ai_count_obj, ai_count);

static mp_obj_t ai_labels(void)
{
    need_model();
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (int i = 0; i < M.nlabels; i++) mp_obj_list_append(list, label_str(i));
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ai_labels_obj, ai_labels);

static mp_obj_t ai_ms(void) { return MP_OBJ_NEW_SMALL_INT((int)s_ms); }
static MP_DEFINE_CONST_FUN_OBJ_0(ai_ms_obj, ai_ms);

static const mp_rom_map_elem_t ai_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ai) },
    { MP_ROM_QSTR(MP_QSTR_load), MP_ROM_PTR(&ai_load_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&ai_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_result), MP_ROM_PTR(&ai_result_obj) },
    { MP_ROM_QSTR(MP_QSTR_top), MP_ROM_PTR(&ai_top_obj) },
    { MP_ROM_QSTR(MP_QSTR_seen), MP_ROM_PTR(&ai_seen_obj) },
    { MP_ROM_QSTR(MP_QSTR_count), MP_ROM_PTR(&ai_count_obj) },
    { MP_ROM_QSTR(MP_QSTR_labels), MP_ROM_PTR(&ai_labels_obj) },
    { MP_ROM_QSTR(MP_QSTR_ms), MP_ROM_PTR(&ai_ms_obj) },
};
static MP_DEFINE_CONST_DICT(ai_globals, ai_globals_table);
static const mp_obj_module_t p4_ai_module = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&ai_globals };

static const mp_rom_map_elem_t p4_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_p4) },
    { MP_ROM_QSTR(MP_QSTR_ai), MP_ROM_PTR(&p4_ai_module) },
};
static MP_DEFINE_CONST_DICT(p4_globals, p4_globals_table);
const mp_obj_module_t dm_module_p4 = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&p4_globals };
MP_REGISTER_MODULE(MP_QSTR_p4, dm_module_p4);

/* ── the screen thread: live camera + boxes (called instead of the console view) ── */
static const uint16_t COLS[] = { 0x07E0, 0xF81F, 0xFFE0, 0x07FF, 0xFD20, 0x801F, 0xF800, 0x041F };

/* -> 1 drew a new frame, 0 nothing new (don't present), -1 no camera (draw a message) */
static int64_t s_t_open;

static int ai_draw(uint16_t *px, int stride, void (*rect)(int, int, int, int, uint16_t),
                   void (*text)(int, int, const char *, uint16_t, int));

int dm_ai_draw(uint16_t *px, int stride, void (*rect)(int, int, int, int, uint16_t),
               void (*text)(int, int, const char *, uint16_t, int))
{
    __atomic_store_n(&s_in_draw, true, __ATOMIC_SEQ_CST);
    int r = 0;
    if (__atomic_load_n(&s_loaded, __ATOMIC_SEQ_CST)) r = ai_draw(px, stride, rect, text);
    __atomic_store_n(&s_in_draw, false, __ATOMIC_SEQ_CST);
    return r;
}

static int ai_draw(uint16_t *px, int stride, void (*rect)(int, int, int, int, uint16_t),
                   void (*text)(int, int, const char *, uint16_t, int))
{
    if (!cam) return -1;
    papp_cam_info_t ci;
    cam->info(&ci);
    if (!ci.connected) { s_cam_open = false; return -1; }
    if (!s_cam_open) {                                    /* (re)open, at most once a second */
        int64_t now = papp_time_us();
        if (now - s_t_open < 1000000) return -1;
        s_t_open = now;
        s_cam_open = cam->open(CW, CH, 0) >= 0;
        if (!s_cam_open) return -1;
    }
    int r = cam->frame(px, CW, CH, stride, &s_seq, 100);
    if (r < 0) { cam->close(); s_cam_open = false; return -1; }
    if (r != 1) return 0;
    if (s_state == 0 && s_frame) {                        /* hand the AI a copy of this frame */
        for (int y = 0; y < CH; y++) memcpy(&s_frame[y * CW], &px[y * stride], CW * 2);
        s_state = 1;
    }
    int i0 = s_res_i, n = s_res_n[i0];
    for (int i = 0; i < n && M.task == 0; i++) {
        det_t *d = &s_res[i0][i];
        uint16_t c = COLS[d->cls % 8];
        int x = (int)(d->x - d->w / 2), y = (int)(d->y - d->h / 2), w = (int)d->w, h = (int)d->h;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > CW) w = CW - x;
        if (y + h > CH) h = CH - y;
        if (w <= 0 || h <= 0) continue;
        rect(x, y, w, 3, c); rect(x, y + h - 3, w, 3, c); rect(x, y, 3, h, c); rect(x + w - 3, y, 3, h, c);
        char b[40];
        snprintf(b, sizeof b, "%s %d%%", d->cls < M.nlabels ? M.labels[d->cls] : "?", (int)(d->score * 100 + 0.5f));
        rect(x, y > 20 ? y - 20 : y, (int)strlen(b) * 12 + 6, 20, c);
        text(x + 3, (y > 20 ? y - 20 : y) + 3, b, 0x0000, 2);
    }
    char b[64];
    if (M.task == 1 && n) snprintf(b, sizeof b, "%s %d%%", M.labels[s_res[i0][0].cls % MAXL], (int)(s_res[i0][0].score * 100));
    else snprintf(b, sizeof b, "%s  AI %dMS", M.name, (int)s_ms);
    rect(0, CH - 26, CW, 26, 0x0000);
    text(8, CH - 20, b, PAPP_RGB565(170, 255, 200), 2);
    return 1;
}
