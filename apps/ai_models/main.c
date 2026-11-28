/*
 * AI Models - by Abed Mihilia (launcher 0.31+).
 * Lists the ESP-DL models in /sd/models, the user picks one, and it runs on the P4 on the
 * live USB camera: boxes + the recognised label on the screen.
 *
 * A model = <name>.espdl + optional <name>.txt:
 *   name=Hand Robot
 *   task=yolo            (yolo: [1, 4+classes, N] or [1, N, 4+classes]; cls: [1, classes];
 *                         hands / face: MediaPipe detector + <name>.lm landmark model, see pose.c)
 *   labels=forward,left,right,stop
 *   conf=0.5
 *   scale=0.0039215686   (input = pixel * scale + offset; YOLO: /255)
 *   offset=0
 * X (top right) exits, < (top left) goes back to the list.
 */
#include "papp/papp.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PAPP_MANIFEST(.id = "abed.aimodels", .name = "AI Models", .version = PAPP_APP_VERSION, .main_stack = 32768);

#define W 640
#define H 480
#define MODELS "/sd/models"
#define MAXM 16
#define MAXL 32
#define MAXD 20

static uint16_t *px;
static int stride;

/* ══ drawing ═════════════════════════════════════════════════════════════ */
static inline uint16_t blend(uint16_t d, uint16_t s, int a)
{
    uint32_t dd = (d | ((uint32_t)d << 16)) & 0x07E0F81F, ss = (s | ((uint32_t)s << 16)) & 0x07E0F81F;
    uint32_t r = ((((ss - dd) * a) >> 5) + dd) & 0x07E0F81F;
    return (uint16_t)(r | (r >> 16));
}
static void rect(int x, int y, int w, int h, uint16_t c, int a)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
    for (int j = y0; j < y1; j++) {
        uint16_t *r = &px[j * stride];
        for (int i = x0; i < x1; i++) r[i] = a >= 32 ? c : blend(r[i], c, a);
    }
}
static void frame_rect(int x, int y, int w, int h, int t, uint16_t c)
{
    rect(x, y, w, t, c, 32); rect(x, y + h - t, w, t, c, 32);
    rect(x, y, t, h, c, 32); rect(x + w - t, y, t, h, c, 32);
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
            if (g[r] & (0x10 >> k)) rect(x + k * sc, y + r * sc, sc, sc, col, 32);
}
static int tw(const char *s, int sc) { return (int)strlen(s) * 6 * sc - sc; }
static void text(int x, int y, const char *s, uint16_t c, int sc)
{
    for (int i = 0; s[i]; i++) { ch(x + i * 6 * sc + sc, y + sc, s[i], 0x0000, sc); ch(x + i * 6 * sc, y, s[i], c, sc); }
}
static void text_c(int y, const char *s, uint16_t c, int sc) { text((W - tw(s, sc)) / 2, y, s, c, sc); }

static void btn_x(void)
{
    rect(W - 52, 8, 44, 44, PAPP_RGB565(20, 30, 50), 26);
    for (int k = 0; k < 24; k++) { rect(W - 42 + k, 18 + k, 3, 3, 0xFFFF, 32); rect(W - 42 + k, 41 - k, 3, 3, 0xFFFF, 32); }
}
static void btn_back(void)
{
    rect(8, 8, 44, 44, PAPP_RGB565(20, 30, 50), 26);
    text(19, 16, "<", 0xFFFF, 4);
}

/* ══ models ══════════════════════════════════════════════════════════════ */
typedef struct {
    char file[96], name[40], labels[MAXL][24];
    int nlabels, task;          /* 0 yolo, 1 cls, 2 hands, 3 face */
    float conf, scale, offset, closed, seconds;
} model_t;
static model_t model;

static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ')) *--e = 0;
}

/* the model file + its .txt description */
static bool load_desc(const char *path, model_t *m)
{
    memset(m, 0, sizeof *m);
    snprintf(m->file, sizeof m->file, "%s", path);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t l = strlen(base);
    if (l < 7 || strcasecmp(base + l - 6, ".espdl")) return false;
    snprintf(m->name, sizeof m->name, "%.*s", (int)(l - 6), base);
    m->conf = 0.5f; m->scale = 1.0f / 255.0f; m->offset = 0; m->closed = 0.2f; m->seconds = 5;
    char txt[110];
    snprintf(txt, sizeof txt, "%.*s.txt", (int)(strlen(path) - 6), path);
    FILE *f = fopen(txt, "r");
    if (!f) return true;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        char *v = strchr(line, '=');
        if (!v) continue;
        *v++ = 0;
        if (!strcmp(line, "name")) snprintf(m->name, sizeof m->name, "%s", v);
        else if (!strcmp(line, "task")) m->task = !strcmp(v, "cls") ? 1 : !strcmp(v, "hands") ? 2 : !strcmp(v, "face") ? 3 : 0;
        else if (!strcmp(line, "closed")) m->closed = strtof(v, NULL);
        else if (!strcmp(line, "seconds")) m->seconds = strtof(v, NULL);
        else if (!strcmp(line, "conf")) m->conf = strtof(v, NULL);
        else if (!strcmp(line, "scale")) m->scale = strtof(v, NULL);
        else if (!strcmp(line, "offset")) m->offset = strtof(v, NULL);
        else if (!strcmp(line, "labels"))
            for (char *t = strtok(v, ","); t && m->nlabels < MAXL; t = strtok(NULL, ","))
                snprintf(m->labels[m->nlabels++], 24, "%s", t);
    }
    fclose(f);
    return true;
}

/* ══ inference thread ════════════════════════════════════════════════════ */
typedef struct { float x, y, w, h, score; int cls; } det_t;
static const papp_if_ai_t *ai;
static model_t *cur;
static uint16_t ai_frame[W * H];
static volatile int ai_state;                    /* 0 wants a frame, 1 frame ready, 2 busy */
static volatile bool ai_run_flag;
static det_t res[2][MAXD];
static volatile int res_n[2], res_i;
static volatile int64_t ai_us;
#define MAXO 8
static float *outbuf, *obuf[MAXO];
static int out_n, n_out, osz[MAXO], in_w = 512, in_h = 512;
static papp_ai_shape_t oshape[MAXO];

static float iou(const det_t *a, const det_t *b)
{
    float x0 = fmaxf(a->x - a->w / 2, b->x - b->w / 2), x1 = fminf(a->x + a->w / 2, b->x + b->w / 2);
    float y0 = fmaxf(a->y - a->h / 2, b->y - b->h / 2), y1 = fminf(a->y + a->h / 2, b->y + b->h / 2);
    float in = fmaxf(0, x1 - x0) * fmaxf(0, y1 - y0);
    return in / (a->w * a->h + b->w * b->h - in + 1e-6f);
}


static float sigm(float v) { return 1.0f / (1.0f + expf(-v)); }

/* raw YOLOv8 / YOLO11 head: per stride a box map [1,H,W,64] (DFL, 4 sides x 16 bins) and a
 * class map [1,H,W,classes] (logits). Boxes come out in input pixels. */
static int decode_raw(det_t *cand, int maxc)
{
    int n = 0;
    float logit_min = logf(cur->conf / (1.0f - cur->conf));
    for (int o = 0; o < n_out; o++) {
        papp_ai_shape_t *cs = &oshape[o];
        if (cs->ndim != 4 || cs->dims[3] == 64) continue;
        int Hh = cs->dims[1], Ww = cs->dims[2], nc = cs->dims[3], bo = -1;
        for (int b = 0; b < n_out; b++)
            if (oshape[b].ndim == 4 && oshape[b].dims[3] == 64 && oshape[b].dims[1] == Hh && oshape[b].dims[2] == Ww) bo = b;
        if (bo < 0) continue;
        ai->read(o, obuf[o], osz[o]);
        bool box_read = false;
        float st = (float)in_h / Hh;
        for (int y = 0; y < Hh; y++)
            for (int x = 0; x < Ww; x++) {
                const float *c = &obuf[o][(y * Ww + x) * nc];
                int best = 0;
                for (int k = 1; k < nc; k++) if (c[k] > c[best]) best = k;
                if (c[best] < logit_min) continue;
                if (!box_read) { ai->read(bo, obuf[bo], osz[bo]); box_read = true; }
                const float *b = &obuf[bo][(y * Ww + x) * 64];
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
    if (cur->task == 1) {                        /* classification: best class */
        int n = ai->read(0, outbuf, out_n);
        int best = 0;
        for (int i = 1; i < n; i++) if (outbuf[i] > outbuf[best]) best = i;
        float sum = 0, mx = outbuf[best];
        bool prob = true;
        for (int i = 0; i < n; i++) if (outbuf[i] < 0 || outbuf[i] > 1) prob = false;
        if (!prob) { for (int i = 0; i < n; i++) sum += expf(outbuf[i] - mx); mx = 1.0f / sum; }
        out[0] = (det_t){ 0.5f, 0.5f, 0, 0, prob ? outbuf[best] : mx, best };
        return out[0].score >= cur->conf;
    }
    det_t cand[64];
    int nc_ = 0;
    float coord_max = 0;
    if (n_out >= 2 && oshape[0].ndim == 4) {      /* raw head, boxes in input pixels */
        nc_ = decode_raw(cand, 64);
        coord_max = 1e9f;
        goto nms;
    }
    /* YOLO: [1, C, N] (channel-major) or [1, N, C]; C = 4 + classes */
    ai->read(0, outbuf, out_n);
    int d1 = s.ndim >= 3 ? s.dims[s.ndim - 2] : 1, d2 = s.dims[s.ndim - 1];
    bool cmaj = d1 < d2;
    int C = cmaj ? d1 : d2, N = cmaj ? d2 : d1, nc = C - 4;
    if (nc < 1) return 0;
    for (int i = 0; i < N; i++) {
        int best = 0; float bs = -1;
        for (int c = 0; c < nc; c++) {
            float v = cmaj ? outbuf[(4 + c) * N + i] : outbuf[i * C + 4 + c];
            if (v > bs) { bs = v; best = c; }
        }
        if (bs < cur->conf) continue;
        det_t d;
        d.x = cmaj ? outbuf[i] : outbuf[i * C];
        d.y = cmaj ? outbuf[N + i] : outbuf[i * C + 1];
        d.w = cmaj ? outbuf[2 * N + i] : outbuf[i * C + 2];
        d.h = cmaj ? outbuf[3 * N + i] : outbuf[i * C + 3];
        d.score = bs; d.cls = best;
        if (d.x > coord_max) coord_max = d.x;
        if (nc_ < 64) cand[nc_++] = d;
        else {                                   /* keep the best 64 */
            int lo = 0;
            for (int k = 1; k < 64; k++) if (cand[k].score < cand[lo].score) lo = k;
            if (d.score > cand[lo].score) cand[lo] = d;
        }
    }
nms:
    /* sort by score, NMS */
    for (int a = 0; a < nc_; a++)
        for (int b = a + 1; b < nc_; b++)
            if (cand[b].score > cand[a].score) { det_t t = cand[a]; cand[a] = cand[b]; cand[b] = t; }
    int k = 0;
    for (int a = 0; a < nc_ && k < MAXD; a++) {
        bool keep = true;
        for (int b = 0; b < k; b++) if (out[b].cls == cand[a].cls && iou(&out[b], &cand[a]) > 0.45f) { keep = false; break; }
        if (keep) out[k++] = cand[a];
    }
    /* to frame pixels: normalised (<= ~1.5) or input pixels; undo the letterbox */
    float sc = coord_max <= 1.5f ? (float)in_w : 1.0f;
    float r = fminf((float)in_w / W, (float)in_h / H), ox = (in_w - W * r) / 2, oy = (in_h - H * r) / 2;
    for (int i = 0; i < k; i++) {
        out[i].x = (out[i].x * sc - ox) / r; out[i].y = (out[i].y * sc - oy) / r;
        out[i].w = out[i].w * sc / r;       out[i].h = out[i].h * sc / r;
    }
    return k;
}

static int ai_thread(void *arg)
{
    (void)arg;
    while (ai_run_flag) {
        if (ai_state != 1) { papp_delay_ms(3); continue; }
        ai_state = 2;
        int64_t us = ai->run(ai_frame, W, H, W, cur->scale, cur->offset, cur->task == 0);
        int nxt = res_i ^ 1;
        res_n[nxt] = us >= 0 ? decode(res[nxt]) : 0;
        res_i = nxt;
        ai_us = us;
        ai_state = 0;
    }
    return 0;
}

#include "pose.c"

/* ══ screens ═════════════════════════════════════════════════════════════ */
static const uint16_t COLS[] = { 0x07E0, 0xF81F, 0xFFE0, 0x07FF, 0xFD20, 0x801F, 0xF800, 0x041F };

static void label_of(const det_t *d, char *b, int n)
{
    const char *l = d->cls < cur->nlabels ? cur->labels[d->cls] : "CLASS";
    char tmp[8];
    if (d->cls >= cur->nlabels) { snprintf(tmp, sizeof tmp, "%d", d->cls); l = tmp; }
    snprintf(b, n, "%s %d%%", l, (int)(d->score * 100 + 0.5f));
}

static void draw_message(const char *a, const char *b)
{
    rect(0, 0, W, H, PAPP_RGB565(12, 18, 34), 32);
    text_c(190, a, 0xFFFF, 4);
    if (b) text_c(250, b, PAPP_RGB565(200, 210, 230), 2);
    btn_x();
}

/* ══ download models from GitHub ═════════════════════════════════════════ */
/* Every <name>.espdl (+ <name>.txt) in the GitHub repository abedchina/AI-Models; it
 * is read with the GitHub API, so a model is added by uploading its files there. Models already
 * on the card (same size) are not downloaded again. */
#define MODELS_API "https://api.github.com/repos/abedchina/AI-Models/contents"
#define MAXC 16
#define TOKEN_FILE "/sd/config/github_token.txt"
static uint8_t dlbuf[16384];
static char listing[48 * 1024];
static char hdr_json[300], hdr_raw[300];         /* "Authorization: Bearer <key>" + Accept */

/* the private repository needs a read-only GitHub key, kept on the card */
static bool load_token(void)
{
    char tok[200] = "";
    /* also the names Windows makes with hidden extensions, and the card's main folder */
    static const char *const paths[] = { TOKEN_FILE, TOKEN_FILE ".txt", "/sd/github_token.txt", "/sd/github_token.txt.txt",
                                         "/sd/config/github_token", "/sd/github_token" };
    for (unsigned i = 0; i < sizeof paths / sizeof paths[0] && !tok[0]; i++) {
        FILE *f = fopen(paths[i], "r");
        if (f) { if (!fgets(tok, sizeof tok, f)) tok[0] = 0; fclose(f); }
    }
    if ((unsigned char)tok[0] == 0xEF && (unsigned char)tok[1] == 0xBB && (unsigned char)tok[2] == 0xBF)   /* UTF-8 BOM (Notepad) */
        memmove(tok, tok + 3, strlen(tok + 3) + 1);
    trim(tok);
    char *t = tok;
    while (*t == ' ' || *t == '\t') t++;
    const char *ua = "User-Agent: p4-ai-models\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (*t) {
        snprintf(hdr_json, sizeof hdr_json, "%sAuthorization: Bearer %s\r\nAccept: application/vnd.github+json\r\n", ua, t);
        snprintf(hdr_raw, sizeof hdr_raw, "%sAuthorization: Bearer %s\r\nAccept: application/vnd.github.raw\r\n", ua, t);
    } else {
        snprintf(hdr_json, sizeof hdr_json, "%sAccept: application/vnd.github+json\r\n", ua);
        snprintf(hdr_raw, sizeof hdr_raw, "%sAccept: application/vnd.github.raw\r\n", ua);
    }
    return *t != 0;
}

static void progress(papp_surface_t *s, const char *title, long got, long total)
{
    px = papp_disp_lock(s, &stride);
    rect(0, 0, W, H, PAPP_RGB565(12, 18, 34), 32);
    text_c(150, title, 0xFFFF, 3);
    rect(60, 230, W - 120, 34, PAPP_RGB565(40, 50, 80), 32);
    if (total > 0) rect(64, 234, (int)((W - 128) * (double)got / total), 26, PAPP_RGB565(90, 220, 255), 32);
    char b[48];
    if (total > 0) snprintf(b, sizeof b, "%ld.%ld / %ld.%ld MB", got / 1000000, got / 100000 % 10, total / 1000000, total / 100000 % 10);
    else snprintf(b, sizeof b, "%ld KB", got / 1000);
    text_c(290, b, PAPP_RGB565(200, 210, 230), 2);
    papp_disp_present(s);
}

/* GET url -> file (or into mem when path is NULL). Returns bytes, < 0 on error */
static long http_get(const papp_if_net_t *net, const char *url, const char *path, char *mem, long memlen,
                     papp_surface_t *s, const char *title)
{
    papp_http_req_t rq = { .url = url, .timeout_ms = 20000, .headers = path ? hdr_raw : hdr_json };
    int st = 0;
    papp_http_t *h = net->http_open(&rq, &st);
    if (!h || st != 200) { if (h) net->http_close(h); return -1; }
    long total = net->http_length(h), got = 0, shown = -1;
    FILE *f = path ? fopen(path, "wb") : NULL;
    if (path && !f) { net->http_close(h); return -2; }
    int n;
    while ((n = net->http_read(h, dlbuf, sizeof dlbuf)) > 0) {
        if (f) fwrite(dlbuf, 1, n, f);
        else if (got + n < memlen) memcpy(mem + got, dlbuf, n);
        got += n;
        if (s && got - shown >= 64 * 1024) { progress(s, title, got, total); shown = got; }
        if (papp_exit_requested()) { n = -1; break; }
    }
    net->http_close(h);
    if (f) fclose(f);
    if (!path) mem[got < memlen ? got : memlen - 1] = 0;
    if (n < 0 || (total > 0 && got != total)) { if (path) remove(path); return -3; }
    return got;
}

typedef struct { char base[48], url[200], txt_url[200]; long size; bool have; } remote_t;
static remote_t rm[MAXC];

/* the "models" folder listing (JSON array): every .espdl with its size and raw download URL */
static int parse_listing(void)
{
    int n = 0;
    char *p = listing;
    while ((p = strstr(p, "\"name\":\"")) && n < MAXC) {
        p += 8;
        char *e = strchr(p, '"');
        if (!e) break;
        int len = (int)(e - p);
        char name[96];
        snprintf(name, sizeof name, "%.*s", len < 95 ? len : 95, p);
        char *obj_end = strstr(e, "\"name\":\"");
        char *sz = strstr(e, "\"size\":"), *du = strstr(e, "\"url\":\"");
        p = e;
        if (!sz || !du || (obj_end && du > obj_end)) continue;
        size_t nl = strlen(name);
        if (nl < 7 || strcasecmp(name + nl - 6, ".espdl")) continue;
        remote_t *r = &rm[n];
        snprintf(r->base, sizeof r->base, "%.*s", (int)(nl - 6), name);
        r->size = atol(sz + 7);
        du += 7;                                  /* "url":"https://api.github.com/repos/.../contents/x.espdl?ref=main" */
        char *de = strchr(du, '"');
        if (!de) continue;
        snprintf(r->url, sizeof r->url, "%.*s", (int)(de - du), du);
        char *ext = strstr(r->url, ".espdl");
        if (ext) snprintf(r->txt_url, sizeof r->txt_url, "%.*s.txt%s", (int)(ext - r->url), r->url, ext + 6);
        else r->txt_url[0] = 0;
        char path[128];
        snprintf(path, sizeof path, MODELS "/%s.espdl", r->base);
        r->have = papp_fs()->size(path) == r->size;
        n++;
    }
    return n;
}

static bool fetch_model(const papp_if_net_t *net, const remote_t *r, papp_surface_t *s, const char *title)
{
    char path[128];
    snprintf(path, sizeof path, MODELS "/%s.txt", r->base);
    if (r->txt_url[0]) http_get(net, r->txt_url, path, NULL, 0, NULL, NULL);   /* the description (optional) */
    char lm_name[80], lm_url[200];                    /* hands / face models: the landmark model <base>.lm */
    snprintf(lm_name, sizeof lm_name, "\"name\":\"%s.lm\"", r->base);
    char *ext = strstr(r->url, ".espdl");
    if (strstr(listing, lm_name) && ext) {
        snprintf(lm_url, sizeof lm_url, "%.*s.lm%s", (int)(ext - r->url), r->url, ext + 6);
        snprintf(path, sizeof path, MODELS "/%s.lm", r->base);
        char t[80];
        snprintf(t, sizeof t, "%s (2/2)", title);
        if (http_get(net, lm_url, path, NULL, 0, s, t) <= 0) return false;
    }
    snprintf(path, sizeof path, MODELS "/%s.espdl", r->base);
    return http_get(net, r->url, path, NULL, 0, s, title) == r->size;
}

static void download_models(papp_surface_t *s)
{
    const papp_if_net_t *net = papp_net();
    if (!net) { papp_ui_message("AI Models", "Update the launcher (Settings - Update) to download models."); return; }
    px = papp_disp_lock(s, &stride);
    draw_message("CONNECTING...", "WI-FI AND GITHUB");
    papp_disp_present(s);
    if (net->up(15000) < 0 && papp_ui_wifi() != 1) return;
    bool key = load_token();
    if (http_get(net, MODELS_API, NULL, listing, sizeof listing, NULL, NULL) < 0) {
        papp_ui_message("AI Models", key ? "Could not read the models on GitHub. Check the Wi-Fi and the key in /sd/config/github_token.txt."
                                         : "The models are private: put your GitHub key in /sd/config/github_token.txt (one line).");
        return;
    }
    papp_fs()->mkdir(MODELS);
    for (;;) {
        int n = parse_listing(), missing = 0;
        if (!n) { papp_ui_message("AI Models", "The models folder on GitHub is empty."); return; }
        static char item[MAXC + 1][72];
        const char *items[MAXC + 1];
        for (int i = 0; i < n; i++) missing += !rm[i].have;
        int k0 = 0;
        if (missing) { snprintf(item[0], sizeof item[0], "Download all new models (%d)", missing); items[0] = item[0]; k0 = 1; }
        for (int i = 0; i < n && k0 + i < 7; i++) {
            snprintf(item[k0 + i], sizeof item[0], "%s (%ld.%ld MB) - %s", rm[i].base, rm[i].size / 1000000,
                     rm[i].size / 100000 % 10, rm[i].have ? "on the card" : "new");
            items[k0 + i] = item[k0 + i];
        }
        int cnt = k0 + n > 7 ? 7 : k0 + n;
        int k = papp_ui_menu(missing ? "Models on GitHub" : "All models are on the card", items, cnt, 0);
        if (k < 0) return;
        if (missing && k == 0) {
            int done = 0, i_new = 0;
            for (int i = 0; i < n; i++) {
                if (rm[i].have) continue;
                char t[80];
                snprintf(t, sizeof t, "%d/%d %s", ++i_new, missing, rm[i].base);
                done += fetch_model(net, &rm[i], s, t);
            }
            char msg[96];
            snprintf(msg, sizeof msg, "%d of %d new models are on the card now.", done, missing);
            papp_ui_message("AI Models", msg);
            continue;
        }
        remote_t *r = &rm[k - k0];
        if (r->have) { papp_ui_message("AI Models", "This model is already on the card."); continue; }
        papp_ui_message("AI Models", fetch_model(net, r, s, r->base) ? "The model is on the card. Choose \"Run a model\" to use it."
                                                                     : "The download failed. Check the Wi-Fi and try again.");
    }
}

int main(int argc, char **argv)
{
    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565, .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
    papp_surface_t *s = papp_disp_surface_create(&cfg);
    if (!s) return 2;
    ai = papp_ai();
    const papp_if_cam_t *cam = papp_cam();
    enum { LIST, RUN } screen = LIST;
    const char *auto_path = argc > 1 ? argv[1] : NULL;   /* "-- /sd/models/x.espdl": start it (DevLink tests) */
    bool was_touch = false, cam_open = false;
    uint32_t seq = 0;
    papp_thread_t *th = NULL;
    int64_t t_fps = papp_time_us();
    int frames = 0, fps = 0;
    while (!papp_exit_requested()) {
        papp_touch_point_t tp[3];
        int n = papp_disp_touch(s, tp, 3);
        bool tap = n > 0 && !was_touch;
        was_touch = n > 0;
        int tx = n ? tp[0].x : 0, ty = n ? tp[0].y : 0;
        if (tap && tx > W - 60 && ty < 60) break;

        if (!ai || !cam) {
            px = papp_disp_lock(s, &stride);
            draw_message("UPDATE THE LAUNCHER", "SETTINGS - UPDATE (0.31 OR NEWER)");
            papp_disp_present(s);
            papp_delay_ms(100);
            continue;
        }
        if (screen == LIST) {
            char path[256] = "";
            if (auto_path) { snprintf(path, sizeof path, "%s", auto_path); auto_path = NULL; }
            else {
                static const char *const menu[] = { "Run a model", "Download models from GitHub", "Exit" };
                px = papp_disp_lock(s, &stride);
                draw_message("AI MODELS", "BY ABED MIHILIA");
                papp_disp_present(s);
                int m = papp_ui_menu("AI Models", menu, 3, 0);
                if (m < 0 || m == 2) break;
                if (m == 1) { download_models(s); continue; }
                papp_fs()->mkdir(MODELS);
                papp_pick_cfg_t pc = { .title = "AI Models", .root = MODELS, .extensions = ".espdl" };
                papp_pick_result_t pr;
                if (papp_ui_pick_file(&pc, &pr) == PAPP_PICK_CANCEL) continue;  /* back to the menu */
                snprintf(path, sizeof path, "%s", pr.path);
            }
            if (!load_desc(path, &model)) continue;
            cur = &model;
            px = papp_disp_lock(s, &stride);
            draw_message("LOADING...", cur->name);
            papp_disp_present(s);
            if (cur->task >= TASK_HANDS) {              /* detector + landmark model */
                int e = pose_load();
                if (e < 0) {
                    ai_unload_all();
                    px = papp_disp_lock(s, &stride);
                    draw_message(e == -2 ? "UPDATE THE LAUNCHER" : "CANNOT LOAD MODEL", e == -2 ? "SETTINGS - UPDATE (0.32 OR NEWER)" : cur->file);
                    papp_disp_present(s);
                    papp_delay_ms(2500);
                    continue;
                }
                ai_us = 0; ai_state = 0; ai_run_flag = true;
                papp_thread_cfg_t tc = { .name = "ai", .stack_size = 16384, .prio = PAPP_PRIO_LOW, .core = 1 };
                th = papp_thread_create(pose_thread, NULL, &tc);
                if (!th) { tc.flags |= PAPP_THREAD_STACK_PSRAM; th = papp_thread_create(pose_thread, NULL, &tc); }   /* internal RAM full */
                screen = RUN;
                continue;
            }
            if (ai->load(cur->file) < 0) {
                px = papp_disp_lock(s, &stride);
                draw_message("CANNOT LOAD MODEL", cur->file);
                papp_disp_present(s);
                papp_delay_ms(2500);
                continue;
            }
            papp_ai_shape_t is, os;
            ai->input(&is);
            if (is.ndim == 4) { in_h = is.dims[1]; in_w = is.dims[2]; }
            ai->output(0, &os);
            out_n = 1;
            for (int i = 0; i < os.ndim; i++) out_n *= os.dims[i];
            free(outbuf);
            outbuf = malloc(sizeof(float) * out_n);
            n_out = ai->outputs();
            if (n_out > MAXO) n_out = MAXO;
            for (int o = 0; o < MAXO; o++) { free(obuf[o]); obuf[o] = NULL; }
            for (int o = 0; o < n_out; o++) {
                ai->output(o, &oshape[o]);
                osz[o] = 1;
                for (int i = 0; i < oshape[o].ndim; i++) osz[o] *= oshape[o].dims[i];
                obuf[o] = malloc(sizeof(float) * osz[o]);
            }
            res_n[0] = res_n[1] = 0; ai_us = 0; ai_state = 0;
            ai_run_flag = true;
            papp_thread_cfg_t tc = { .name = "ai", .stack_size = 16384, .prio = PAPP_PRIO_LOW, .core = 1 };   /* core 0 = screen loop */
            th = papp_thread_create(ai_thread, NULL, &tc);
            if (!th) { tc.flags |= PAPP_THREAD_STACK_PSRAM; th = papp_thread_create(ai_thread, NULL, &tc); }
            screen = RUN;
            continue;
        }
        /* RUN */
        if (tap && tx < 60 && ty < 60) {
            ai_run_flag = false;
            if (th) papp_thread_join(th, 5000, NULL);
            th = NULL;
            ai_unload_all();
            cam->close(); cam_open = false;
            screen = LIST;
            continue;
        }
        papp_cam_info_t ci;
        cam->info(&ci);
        px = papp_disp_lock(s, &stride);
        if (!ci.connected) {
            cam_open = false;
            draw_message("CONNECT A USB CAMERA", "TO THE HIGH-SPEED USB-C PORT");
            btn_back();
            papp_disp_present(s);
            papp_delay_ms(100);
            continue;
        }
        if (!cam_open) cam_open = cam->open(W, H, 0) >= 0;
        int r = cam->frame(px, W, H, stride, &seq, 200);
        /* no new frame: present nothing, the screen keeps showing the last good frame (no flicker, no copy) */
        if (r != 1) continue;
        if (ai_state == 0) {                     /* hand the AI thread a copy of this frame */
            for (int y = 0; y < H; y++) memcpy(&ai_frame[y * W], &px[y * stride], W * 2);
            pose_frame_t = papp_time_us();
            ai_state = 1;
        }
        /* results */
        int ri = res_i, k = res_n[ri];
        det_t *d = res[ri];
        char b[48];
        for (int i = 0; i < k && cur->task == 0; i++) {
            uint16_t c = COLS[d[i].cls % 8];
            int bx = (int)(d[i].x - d[i].w / 2), by = (int)(d[i].y - d[i].h / 2);
            frame_rect(bx, by, (int)d[i].w, (int)d[i].h, 3, c);
            label_of(&d[i], b, sizeof b);
            rect(bx, by - 26 < 0 ? by : by - 26, tw(b, 3) + 10, 26, c, 26);
            text(bx + 5, (by - 26 < 0 ? by : by - 26) + 3, b, 0x0000, 3);
        }
        if (cur->task >= TASK_HANDS) {
            uint16_t lc;
            pose_draw(b, sizeof b, &lc);
            rect(0, H - 64, W, 64, 0x0000, 16);
            text_c(H - 52, b, lc, strlen(b) > 16 ? 3 : 5);
        } else {
            rect(0, H - 64, W, 64, 0x0000, 16);
            if (k > 0) { label_of(&d[0], b, sizeof b); text_c(H - 52, b, COLS[d[0].cls % 8], 5); }
            else text_c(H - 44, "...", PAPP_RGB565(200, 200, 200), 4);
        }
        if (++frames, papp_time_us() - t_fps > 1000000) { fps = frames; frames = 0; t_fps = papp_time_us(); }
        snprintf(b, sizeof b, "%s  AI %dMS  %dFPS", cur->name, (int)(ai_us / 1000), fps);
        rect(60, 8, W - 120, 30, 0x0000, 14);
        text(70, 15, b, PAPP_RGB565(170, 255, 200), 2);
        btn_back();
        btn_x();
        papp_disp_present(s);
    }
    ai_run_flag = false;
    if (th) papp_thread_join(th, 5000, NULL);
    ai_unload_all();
    if (cam) cam->close();
    free(outbuf);
    for (int o = 0; o < MAXO; o++) free(obuf[o]);
    papp_disp_surface_destroy(s);
    return 0;
}
