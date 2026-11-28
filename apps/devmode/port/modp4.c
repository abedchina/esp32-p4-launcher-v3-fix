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
 *   p4.ai.conf(0.4)                 # threshold from the next picture on; conf() reads it
 *   p4.ai.zone(160, 120, 320, 240)  # a rectangle on the picture (drawn on the screen)
 *   p4.ai.inside("pen")             # a pen in the zone now
 *   p4.ai.entered("pen")            # True ONCE each time a pen comes into the zone
 *   p4.ai.left("pen")               # True ONCE each time it goes out (missing 2 results)
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

/* zone: a rectangle on the 640 x 480 picture. A label is "inside" while the centre of one of
 * its boxes is in it; entering counts at once, leaving after it was missing from the zone in
 * 2 results in a row (one missed detection is not a departure). Python takes each event once. */
#define ZONE_OUT_RESULTS 2
static volatile int s_zx = 0, s_zy = 0, s_zw = CW, s_zh = CH;
static volatile bool s_zone_set;
static bool s_in[MAXL];
static int s_miss[MAXL];
static volatile uint32_t s_ev_in[MAXL], s_ev_out[MAXL];
static uint32_t s_took_in[MAXL], s_took_out[MAXL];
static float *s_obuf[MAXO], *s_out0;
static int s_osz[MAXO], s_nout, s_out0_n, in_w = 320, in_h = 320;
static papp_ai_shape_t s_oshape[MAXO];
static uint32_t s_seq;

bool dm_ai_active(void) { return s_loaded; }

/* ══ hands (task=hands): MediaPipe palm detector (slot 0, <name>.espdl) + hand landmarks
 * (slot 1, <name>.lm), 21 points per hand, fingers up per hand - the same pipeline as the
 * AI Models app (pose.c): detect -> rotated square crop -> landmarks -> track from points. ══ */
#define TASK_HANDS 2
#define MAXH 2
#define PI_F 3.14159265f
typedef struct { float cx, cy, size, rot; } roi_t;
typedef struct { int n; float pt[MAXH][21][2]; int up[MAXH]; } hands_res_t;
static hands_res_t s_hres[2];
static volatile int s_hres_i;
static roi_t s_roi[MAXH];
static int s_nroi, s_det_skip, s_det_S, s_lm_S, s_lm_out, s_lm_flag;
static uint16_t s_crop[256 * 256];
static float s_lmbuf[63 + 16];

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

static float wrap_pi(float a)
{
    while (a > PI_F) a -= 2 * PI_F;
    while (a < -PI_F) a += 2 * PI_F;
    return a;
}

static roi_t make_roi(float cx, float cy, float w, float h, float rot, float scale, float shift_y)
{
    roi_t r = { cx - h * shift_y * sinf(rot), cy + h * shift_y * cosf(rot), fmaxf(w, h) * scale, rot };
    return r;
}

static int hands_detect(roi_t *out, int maxn)
{
    const int R = 18;                                       /* box 4 + 7 keypoints x 2 */
    typedef struct { float score, cx, cy, w, h, ax, ay, bx, by; } cand_t;
    cand_t c[16];
    int nc = 0;
    float lmin = logf(M.conf / (1.0f - M.conf));
    for (int o = 0; o < s_nout; o++) {
        papp_ai_shape_t *s = &s_oshape[o];
        if (s->ndim != 4 || s->dims[3] > 8) continue;
        int G = s->dims[1], A = s->dims[3], b = -1;
        for (int q = 0; q < s_nout; q++)
            if (s_oshape[q].ndim == 4 && s_oshape[q].dims[1] == G && s_oshape[q].dims[3] == A * R) b = q;
        if (b < 0) continue;
        ai->read(o, s_obuf[o], s_osz[o]);
        bool rd = false;
        for (int i = 0; i < G * G * A; i++) {
            if (s_obuf[o][i] < lmin) continue;
            if (!rd) { ai->read(b, s_obuf[b], s_osz[b]); rd = true; }
            int cell = i / A;
            float ax = (cell % G + 0.5f) / G * s_det_S, ay = (cell / G + 0.5f) / G * s_det_S;
            const float *g = &s_obuf[b][i * R];
            cand_t d = { sigm(s_obuf[o][i]), ax + g[0], ay + g[1], g[2], g[3], ax + g[4], ay + g[5], ax + g[8], ay + g[9] };
            if (nc < 16) c[nc++] = d;
            else {
                int lo = 0;
                for (int k = 1; k < 16; k++) if (c[k].score < c[lo].score) lo = k;
                if (d.score > c[lo].score) c[lo] = d;
            }
        }
    }
    for (int a = 0; a < nc; a++)
        for (int b = a + 1; b < nc; b++)
            if (c[b].score > c[a].score) { cand_t t = c[a]; c[a] = c[b]; c[b] = t; }
    float r = fminf((float)s_det_S / CW, (float)s_det_S / CH);
    int ox = (s_det_S - (int)lroundf(CW * r)) / 2, oy = (s_det_S - (int)lroundf(CH * r)) / 2;
    int n = 0;
    for (int a = 0; a < nc && n < maxn; a++) {
        bool keep = true;
        for (int k = 0; k < a && keep; k++) {
            float dx = c[a].cx - c[k].cx, dy = c[a].cy - c[k].cy;
            if (dx * dx + dy * dy < 0.25f * c[k].w * c[k].w) keep = false;
        }
        if (!keep) continue;
        float cx = (c[a].cx - ox) / r, cy = (c[a].cy - oy) / r, w = c[a].w / r, h = c[a].h / r;
        float vx = c[a].bx - c[a].ax, vy = c[a].by - c[a].ay;
        out[n++] = make_roi(cx, cy, w, h, wrap_pi(PI_F / 2 - atan2f(-vy, vx)), 2.6f, -0.5f);
    }
    return n;
}

static bool hands_landmarks(const roi_t *ro, float (*pt)[2])
{
    const int S = s_lm_S;
    float c = cosf(ro->rot), s = sinf(ro->rot), step = ro->size / S;
    for (int v = 0; v < S; v++) {
        float ly = (v + 0.5f) * step - ro->size / 2;
        float bx = ro->cx - ly * s, by = ro->cy + ly * c, lx = 0.5f * step - ro->size / 2;
        float fx = bx + lx * c, fy = by + lx * s, dx = step * c, dy = step * s;
        uint16_t *d = &s_crop[v * S];
        for (int u = 0; u < S; u++, fx += dx, fy += dy) {
            int X = (int)fx, Y = (int)fy;
            d[u] = (X >= 0 && X < CW && Y >= 0 && Y < CH) ? s_frame[Y * CW + X] : 0;
        }
    }
    ai->select(1);
    bool ok = false;
    if (ai->run(s_crop, S, S, S, 1.0f / 255.0f, 0.0f, 0) >= 0) {
        float f = 1;
        if (s_lm_flag >= 0) ai->read(s_lm_flag, &f, 1);
        if (s_lm_flag < 0 || f >= 0.5f) {
            int n = ai->read(s_lm_out, s_lmbuf, (int)(sizeof s_lmbuf / sizeof s_lmbuf[0]));
            if (n >= 63) {
                for (int i = 0; i < 21; i++) {
                    float lx = (s_lmbuf[i * 3] / S - 0.5f) * ro->size, ly = (s_lmbuf[i * 3 + 1] / S - 0.5f) * ro->size;
                    pt[i][0] = ro->cx + lx * c - ly * s;
                    pt[i][1] = ro->cy + lx * s + ly * c;
                }
                ok = true;
            }
        }
    }
    ai->select(0);
    return ok;
}

static bool roi_from_points(float (*pt)[2], roi_t *out)
{
    static const int IDX[] = { 0, 1, 2, 3, 5, 6, 9, 10, 13, 14, 17, 18 };
    float rot = wrap_pi(PI_F / 2 - atan2f(-(pt[9][1] - pt[0][1]), pt[9][0] - pt[0][0]));
    float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f;
    for (int i = 0; i < 12; i++) {
        const float *p = pt[IDX[i]];
        mnx = fminf(mnx, p[0]); mxx = fmaxf(mxx, p[0]); mny = fminf(mny, p[1]); mxy = fmaxf(mxy, p[1]);
    }
    float cx0 = (mnx + mxx) / 2, cy0 = (mny + mxy) / 2, c = cosf(rot), s = sinf(rot);
    float a0 = 1e9f, b0 = 1e9f, a1 = -1e9f, b1 = -1e9f;
    for (int i = 0; i < 12; i++) {
        const float *p = pt[IDX[i]];
        float dx = p[0] - cx0, dy = p[1] - cy0, a = dx * c + dy * s, b = -dx * s + dy * c;
        a0 = fminf(a0, a); a1 = fmaxf(a1, a); b0 = fminf(b0, b); b1 = fmaxf(b1, b);
    }
    float ma = (a0 + a1) / 2, mb = (b0 + b1) / 2;
    *out = make_roi(cx0 + ma * c - mb * s, cy0 + ma * s + mb * c, a1 - a0, b1 - b0, rot, 2.0f, -0.1f);
    return out->size > 24 && out->size < 4 * CW;
}

static float pdist(const float *a, const float *b) { return hypotf(a[0] - b[0], a[1] - b[1]); }

static int fingers_up(float (*p)[2])                        /* thumb + 4 fingers, like AI Models */
{
    static const int TIPS[] = { 8, 12, 16, 20 }, PIPS[] = { 6, 10, 14, 18 };
    int n = 0;
    for (int i = 0; i < 4; i++) n += pdist(p[TIPS[i]], p[0]) > pdist(p[PIPS[i]], p[0]) * 1.1f;
    n += pdist(p[4], p[17]) > pdist(p[3], p[17]) && pdist(p[4], p[5]) > pdist(p[0], p[9]) * 0.45f;
    return n;
}

/* one hands step -> also fills det boxes (label 0 = "hand") so seen / zone / count work too */
static int hands_step(det_t *out)
{
    if (s_nroi == 0 || (s_nroi < MAXH && ++s_det_skip >= 6)) {
        s_det_skip = 0;
        if (ai->run(s_frame, CW, CH, CW, M.scale, M.offset, 1) >= 0) {
            roi_t found[MAXH];
            int nf = hands_detect(found, MAXH);
            for (int i = 0; i < nf && s_nroi < MAXH; i++) {
                bool dup = false;
                for (int k = 0; k < s_nroi; k++)
                    if (hypotf(found[i].cx - s_roi[k].cx, found[i].cy - s_roi[k].cy) < s_roi[k].size * 0.4f) dup = true;
                if (!dup) s_roi[s_nroi++] = found[i];
            }
        }
    }
    int nxt = s_hres_i ^ 1, k = 0;
    hands_res_t *hr = &s_hres[nxt];
    roi_t keep[MAXH];
    for (int i = 0; i < s_nroi; i++) {
        if (!hands_landmarks(&s_roi[i], hr->pt[k])) continue;
        hr->up[k] = fingers_up(hr->pt[k]);
        if (roi_from_points(hr->pt[k], &keep[k])) k++;
    }
    for (int i = 0; i < k; i++) s_roi[i] = keep[i];
    s_nroi = k;
    hr->n = k;
    s_hres_i = nxt;
    for (int h = 0; h < k; h++) {                           /* a box around each hand */
        float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f;
        for (int i = 0; i < 21; i++) {
            mnx = fminf(mnx, hr->pt[h][i][0]); mxx = fmaxf(mxx, hr->pt[h][i][0]);
            mny = fminf(mny, hr->pt[h][i][1]); mxy = fmaxf(mxy, hr->pt[h][i][1]);
        }
        out[h] = (det_t){ (mnx + mxx) / 2, (mny + mxy) / 2, mxx - mnx, mxy - mny, 1.0f, 0 };
    }
    return k;
}

static int ai_thread(void *arg)
{
    (void)arg;
    while (s_run) {
        if (s_state != 1) { papp_delay_ms(3); continue; }
        s_state = 2;
        int64_t us;
        int nxt = s_res_i ^ 1;
        if (M.task == TASK_HANDS) {
            int64_t t0 = papp_time_us();
            s_res_n[nxt] = hands_step(s_res[nxt]);
            us = papp_time_us() - t0;
        } else {
            us = ai->run(s_frame, CW, CH, CW, M.scale, M.offset, M.task == 0);
            s_res_n[nxt] = us >= 0 ? decode(s_res[nxt]) : 0;
        }
        int64_t now = papp_time_us();
        int zx = s_zx, zy = s_zy, zw = s_zw, zh = s_zh;
        for (int l = 0; l < M.nlabels && l < MAXL; l++) {
            bool here = false, inz = false;
            for (int i = 0; i < s_res_n[nxt]; i++) {
                const det_t *d = &s_res[nxt][i];
                if (d->cls != l) continue;
                here = true;
                if (M.task == 1 || (d->x >= zx && d->x < zx + zw && d->y >= zy && d->y < zy + zh)) inz = true;
            }
            if (!here) s_since[l] = 0;
            else if (!s_since[l]) s_since[l] = now;
            if (inz) {
                s_miss[l] = 0;
                if (!s_in[l]) { s_in[l] = true; s_ev_in[l]++; }
            } else if (s_in[l] && ++s_miss[l] >= ZONE_OUT_RESULTS) {
                s_in[l] = false;
                s_ev_out[l]++;
            }
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
        if (!strcmp(line, "task")) M.task = !strcmp(v, "cls") ? 1 : !strcmp(v, "yolo") ? 0 : !strcmp(v, "hands") ? TASK_HANDS : 9;
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
    if (ai && ai->hdr.version >= 2) { ai->select(1); ai->unload(); ai->select(0); }   /* hands: landmarks */
    if (ai) ai->unload();
    s_nroi = 0; s_det_skip = 0;
    memset(s_hres, 0, sizeof s_hres);
    /* the camera stream stays open for the whole Developer Mode session (the launcher closes it
     * when the app exits): Thonny soft-resets on every connect / Run / Stop, and closing a UVC
     * stream right after it started trips an assert in the USB host stack */
    for (int o = 0; o < MAXO; o++) { papp_free(s_obuf[o]); s_obuf[o] = NULL; }
    papp_free(s_out0); s_out0 = NULL;
    papp_free(s_frame); s_frame = NULL;
    s_res_n[0] = s_res_n[1] = 0;
    memset(s_since, 0, sizeof s_since);
    memset(s_in, 0, sizeof s_in);
    memset(s_miss, 0, sizeof s_miss);
    for (int l = 0; l < MAXL; l++) { s_ev_in[l] = s_ev_out[l] = 0; s_took_in[l] = s_took_out[l] = 0; }
    s_zx = 0; s_zy = 0; s_zw = CW; s_zh = CH; s_zone_set = false;
}

/* "Hand Robot", "hand robot", "hand_robot", "hand_robot.espdl" -> "hand_robot": the file name,
 * or the name= of a model's .txt (what the AI Models app shows), compared without case,
 * spaces, '_' and '-'. */
static void squash(const char *s, char *out, size_t n)
{
    size_t o = 0;
    for (; *s && o + 1 < n; s++) {
        char c = *s;
        if (c == ' ' || c == '_' || c == '-') continue;
        out[o++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    out[o] = 0;
}

static bool model_title(const char *txt, char *title, size_t n)
{
    title[0] = 0;
    papp_file_t *f = papp_fs()->open(txt, "rb");
    if (!f) return false;
    char buf[512];
    size_t len = papp_fs()->read(f, buf, sizeof buf - 1);
    papp_fs()->close(f);
    buf[len] = 0;
    char *p = strstr(buf, "name=");
    if (!p || (p != buf && p[-1] != '\n')) return false;
    p += 5;
    size_t l = strcspn(p, "\r\n");
    snprintf(title, n, "%.*s", (int)(l < n ? l : n - 1), p);
    return true;
}

static bool resolve_model(const char *name, char *base, size_t n)
{
    char want[96], fn[96], path[160];
    const char *b = name[0] == '/' ? strrchr(name, '/') : NULL;   /* a path; "Eyes open/closed" is a name */
    snprintf(fn, sizeof fn, "%s", b ? b + 1 : name);
    size_t l = strlen(fn);
    if (l > 6 && !strcasecmp(fn + l - 6, ".espdl")) fn[l - 6] = 0;
    snprintf(path, sizeof path, "/sd/models/%s.espdl", fn);
    if (papp_fs()->size(path) >= 0) { snprintf(base, n, "%s", fn); return true; }
    squash(fn, want, sizeof want);
    const papp_if_fs_t *fs = papp_fs();
    if (fs->hdr.version < 2) return false;
    papp_dir_t *d = fs->dir_open("/sd/models");
    if (!d) return false;
    papp_dirent_t e;
    bool found = false;
    while (!found && fs->dir_read(d, &e) == 1) {
        size_t el = strlen(e.name);
        if (e.is_dir || el < 7 || strcasecmp(e.name + el - 6, ".espdl")) continue;
        char eb[96], sq[96], title[64], txt[192];
        snprintf(eb, sizeof eb, "%.*s", (int)(el - 6), e.name);
        squash(eb, sq, sizeof sq);
        if (!strcmp(sq, want)) found = true;                 /* "hand robot" -> hand_robot */
        else {
            snprintf(txt, sizeof txt, "/sd/models/%s.txt", eb);
            if (model_title(txt, title, sizeof title)) {
                squash(title, sq, sizeof sq);
                found = !strcmp(sq, want);                    /* "Hand Robot" (its name=) */
            }
        }
        if (found) snprintf(base, n, "%s", eb);
    }
    fs->dir_close(d);
    return found;
}

/* models() -> [("hand_robot", "Hand Robot"), ...]: what p4.ai.load() can open */
static mp_obj_t ai_models(void)
{
    mp_obj_t list = mp_obj_new_list(0, NULL);
    const papp_if_fs_t *fs = papp_fs();
    if (fs->hdr.version < 2) return list;
    papp_dir_t *d = fs->dir_open("/sd/models");
    if (!d) return list;
    papp_dirent_t e;
    while (fs->dir_read(d, &e) == 1) {
        size_t el = strlen(e.name);
        if (e.is_dir || el < 7 || strcasecmp(e.name + el - 6, ".espdl")) continue;
        char eb[96], title[64], txt[192];
        snprintf(eb, sizeof eb, "%.*s", (int)(el - 6), e.name);
        snprintf(txt, sizeof txt, "/sd/models/%s.txt", eb);
        if (!model_title(txt, title, sizeof title)) snprintf(title, sizeof title, "%s", eb);
        mp_obj_t t[2] = { mp_obj_new_str(eb, strlen(eb)), mp_obj_new_str(title, strlen(title)) };
        mp_obj_list_append(list, mp_obj_new_tuple(2, t));
    }
    fs->dir_close(d);
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ai_models_obj, ai_models);

static mp_obj_t ai_load(size_t n_args, const mp_obj_t *args)
{
    ai = papp_ai();
    cam = papp_cam();
    if (!ai || !cam) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("update the launcher (0.33 or newer)"));
    int zx = s_zx, zy = s_zy, zw = s_zw, zh = s_zh;          /* a zone set before load() stays */
    bool zset = s_zone_set;
    dm_ai_stop();
    s_zx = zx; s_zy = zy; s_zw = zw; s_zh = zh; s_zone_set = zset;
    const char *name = mp_obj_str_get_str(args[0]);
    char base[96], path[128], txt[128];
    if (!resolve_model(name, base, sizeof base))
        mp_raise_msg_varg(&mp_type_OSError,
                          MP_ERROR_TEXT("no model '%s' on the card - see p4.ai.models() (or download it in AI Models)"), name);
    snprintf(path, sizeof path, "/sd/models/%s.espdl", base);
    snprintf(txt, sizeof txt, "/sd/models/%s.txt", base);
    read_desc(txt);
    if (M.task > TASK_HANDS) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("face models are not supported in Python yet"));
    if (n_args > 1) M.conf = mp_obj_get_float(args[1]);
    snprintf(M.name, sizeof M.name, "%s", base);
    papp_ai_shape_t is;
    if (M.task == TASK_HANDS) {                               /* landmarks <name>.lm in slot 1 */
        if (ai->hdr.version < 2) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("update the launcher (0.32 or newer)"));
        char lm[128];
        snprintf(lm, sizeof lm, "/sd/models/%s.lm", base);
        ai->select(1);
        if (ai->load(lm) < 0 || ai->input(&is) < 0) {
            ai->select(0);
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("the hand landmark model (.lm) could not be loaded"));
        }
        s_lm_S = is.dims[1];
        s_lm_out = s_lm_flag = -1;
        for (int o = 0; o < ai->outputs(); o++) {
            papp_ai_shape_t sh;
            ai->output(o, &sh);
            int n = 1;
            for (int i = 0; i < sh.ndim; i++) n *= sh.dims[i];
            if (n >= 63) { if (s_lm_out < 0) s_lm_out = o; }
            else if (s_lm_flag < 0) s_lm_flag = o;
        }
        ai->select(0);
        if (s_lm_out < 0 || s_lm_S > 256) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("unexpected hand landmark model"));
        if (M.nlabels == 0) { snprintf(M.labels[0], 24, "hand"); M.nlabels = 1; }
        s_nroi = 0; s_det_skip = 0;
    } else if (ai->hdr.version >= 2) ai->select(0);
    if (ai->load(path) < 0) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("the model could not be loaded"));
    ai->input(&is);
    if (is.ndim == 4) { in_h = is.dims[1]; in_w = is.dims[2]; }
    s_det_S = in_h;
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

/* conf()       -> the threshold now;  conf(0.4) -> new threshold from the next picture on */
static mp_obj_t ai_conf(size_t n_args, const mp_obj_t *args)
{
    need_model();
    if (n_args == 0) return mp_obj_new_float(M.conf);
    mp_float_t c = mp_obj_get_float(args[0]);
    if (c <= 0 || c >= 1) mp_raise_ValueError(MP_ERROR_TEXT("conf is between 0 and 1, e.g. 0.4"));
    M.conf = (float)c;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ai_conf_obj, 0, 1, ai_conf);

/* zone(x, y, w, h) on the 640 x 480 picture; zone() = the whole picture again */
static mp_obj_t ai_zone(size_t n_args, const mp_obj_t *args)
{
    if (n_args == 0) { s_zx = 0; s_zy = 0; s_zw = CW; s_zh = CH; s_zone_set = false; return mp_const_none; }
    if (n_args != 4) mp_raise_TypeError(MP_ERROR_TEXT("zone(x, y, w, h) - the picture is 640 x 480"));
    int x = mp_obj_get_int(args[0]), y = mp_obj_get_int(args[1]), w = mp_obj_get_int(args[2]), h = mp_obj_get_int(args[3]);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > CW) w = CW - x;
    if (y + h > CH) h = CH - y;
    if (w <= 0 || h <= 0) mp_raise_ValueError(MP_ERROR_TEXT("the zone is outside the 640 x 480 picture"));
    s_zx = x; s_zy = y; s_zw = w; s_zh = h;
    s_zone_set = true;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ai_zone_obj, 0, 4, ai_zone);

/* hands models: fingers() -> raised fingers on all hands (0..10), None with no hand; hands() -> count */
static mp_obj_t ai_fingers(void)
{
    need_model();
    if (M.task != TASK_HANDS) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("fingers() needs a hands model: p4.ai.load(\"finger_counter\")"));
    const hands_res_t *hr = &s_hres[s_hres_i];
    if (!hr->n) return mp_const_none;
    int t = 0;
    for (int h = 0; h < hr->n; h++) t += hr->up[h];
    return MP_OBJ_NEW_SMALL_INT(t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ai_fingers_obj, ai_fingers);

static mp_obj_t ai_hands(void)
{
    need_model();
    return MP_OBJ_NEW_SMALL_INT(M.task == TASK_HANDS ? s_hres[s_hres_i].n : 0);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ai_hands_obj, ai_hands);

static mp_obj_t ai_inside(mp_obj_t lab)
{
    need_model();
    return mp_obj_new_bool(s_in[label_index(lab)]);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ai_inside_obj, ai_inside);

static mp_obj_t ai_entered(mp_obj_t lab)                   /* True once per entry */
{
    need_model();
    int l = label_index(lab);
    if (s_took_in[l] == s_ev_in[l]) return mp_const_false;
    s_took_in[l]++;
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ai_entered_obj, ai_entered);

static mp_obj_t ai_left(mp_obj_t lab)                      /* True once per exit */
{
    need_model();
    int l = label_index(lab);
    if (s_took_out[l] == s_ev_out[l]) return mp_const_false;
    s_took_out[l]++;
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ai_left_obj, ai_left);

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
    { MP_ROM_QSTR(MP_QSTR_conf), MP_ROM_PTR(&ai_conf_obj) },
    { MP_ROM_QSTR(MP_QSTR_models), MP_ROM_PTR(&ai_models_obj) },
    { MP_ROM_QSTR(MP_QSTR_fingers), MP_ROM_PTR(&ai_fingers_obj) },
    { MP_ROM_QSTR(MP_QSTR_hands), MP_ROM_PTR(&ai_hands_obj) },
    { MP_ROM_QSTR(MP_QSTR_zone), MP_ROM_PTR(&ai_zone_obj) },
    { MP_ROM_QSTR(MP_QSTR_inside), MP_ROM_PTR(&ai_inside_obj) },
    { MP_ROM_QSTR(MP_QSTR_entered), MP_ROM_PTR(&ai_entered_obj) },
    { MP_ROM_QSTR(MP_QSTR_left), MP_ROM_PTR(&ai_left_obj) },
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
    if (M.task == TASK_HANDS) {                               /* skeleton + fingers */
        static const uint8_t BONES[][2] = { {0,1},{1,2},{2,3},{3,4},{0,5},{5,6},{6,7},{7,8},{5,9},{9,10},{10,11},{11,12},
                                            {9,13},{13,14},{14,15},{15,16},{13,17},{17,18},{18,19},{19,20},{0,17} };
        const hands_res_t *hr = &s_hres[s_hres_i];
        int total = 0;
        for (int h = 0; h < hr->n; h++) {
            uint16_t c = h ? PAPP_RGB565(255, 120, 220) : PAPP_RGB565(90, 255, 160);
            for (unsigned b = 0; b < sizeof BONES / 2; b++) {
                const float *a = hr->pt[h][BONES[b][0]], *e = hr->pt[h][BONES[b][1]];
                float dx = e[0] - a[0], dy = e[1] - a[1];
                int steps = (int)(fmaxf(fabsf(dx), fabsf(dy)) / 2) + 1;
                for (int i = 0; i <= steps; i++) {
                    int x = (int)(a[0] + dx * i / steps), y = (int)(a[1] + dy * i / steps);
                    if (x >= 1 && x < CW - 1 && y >= 1 && y < CH - 1) rect(x - 1, y - 1, 3, 3, c);
                }
            }
            for (int i = 0; i < 21; i++) {
                int x = (int)hr->pt[h][i][0], y = (int)hr->pt[h][i][1];
                if (x >= 3 && x < CW - 3 && y >= 3 && y < CH - 3) rect(x - 3, y - 3, 7, 7, 0xFFFF);
            }
            total += hr->up[h];
        }
        char fb[32];
        snprintf(fb, sizeof fb, hr->n ? "FINGERS: %d" : "SHOW YOUR HAND", total);
        rect(0, 0, (int)strlen(fb) * 24 + 16, 40, 0x0000);
        text(8, 6, fb, hr->n ? PAPP_RGB565(90, 255, 160) : PAPP_RGB565(200, 200, 200), 4);
    }
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
    if (s_zone_set) {                                        /* the zone: yellow, green with something in it */
        bool any = false;
        for (int l = 0; l < M.nlabels && l < MAXL; l++) any |= s_in[l];
        uint16_t zc = any ? PAPP_RGB565(60, 255, 90) : PAPP_RGB565(255, 220, 40);
        int x = s_zx, y = s_zy, w = s_zw, h = s_zh;
        for (int t = 0; t < 4; t += 2) {                     /* dashed-looking double line */
            rect(x + t, y + t, w - 2 * t, 2, zc); rect(x + t, y + h - 2 - t, w - 2 * t, 2, zc);
            rect(x + t, y + t, 2, h - 2 * t, zc); rect(x + w - 2 - t, y + t, 2, h - 2 * t, zc);
        }
        text(x + 6, y + 6, any ? "ZONE - INSIDE" : "ZONE", zc, 2);
    }
    char b[64];
    if (M.task == 1 && n) snprintf(b, sizeof b, "%s %d%%", M.labels[s_res[i0][0].cls % MAXL], (int)(s_res[i0][0].score * 100));
    else snprintf(b, sizeof b, "%s  AI %dMS  CONF %d%%", M.name, (int)s_ms, (int)(M.conf * 100 + 0.5f));
    rect(0, CH - 26, CW, 26, 0x0000);
    text(8, CH - 20, b, PAPP_RGB565(170, 255, 200), 2);
    return 1;
}
