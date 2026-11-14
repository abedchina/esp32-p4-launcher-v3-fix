/*
 * Hand Robot - by Abed Mihilia.
 *
 * A screen for the hand-gesture robot project (esp32-hand-robot-ai server):
 *  - live video of the camera page (with the AI boxes), decoded from JPEG
 *  - what the AI sees (gesture + confidence), what the robot does, mode,
 *    ESP32 online, temperature and humidity
 *  - the joystick drives the robot: moving it switches the server to manual
 *    mode, letting go stops the robot, A/B = stop, START = back to AI mode
 *  - touch: X (top right) exits, AI button returns to AI mode
 *
 * Two worker threads, each with its own kept-open HTTPS connection:
 *   control: /api/state (5x per second) and /api/control (joystick)
 *   video:   /frame (JPEG) -> TJpgDec -> RGB565
 * No printf/snprintf (they crash in apps here): own small string helpers.
 */
#include "papp/papp.h"
#include <string.h>
#include "tjpgd.h"

#if __has_include("secret.h")
#include "secret.h"
#else
#include "secret.example.h"
#endif

PAPP_MANIFEST(.id = "abed.handrobot", .name = "Hand Robot", .version = PAPP_APP_VERSION, .main_stack = 32768);

#define W 400
#define H 240

#define VID_X 6
#define VID_Y 28
#define VID_W 240
#define VID_H 180

/* ══ small string helpers (no stdio) ══════════════════════════════════════ */
static char *s_add(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }

static char *s_int(char *d, int v)
{
    char n[12]; int k = 0;
    unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
    do { n[k++] = (char)('0' + u % 10); u /= 10; } while (u && k < 11);
    if (v < 0) *d++ = '-';
    while (k) *d++ = n[--k];
    *d = 0;
    return d;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static bool starts_ci(const char *s, const char *p)
{
    while (*p) { if (lower(*s++) != lower(*p++)) return false; }
    return true;
}

static void upcase(char *s) { for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32; }

/* ══ pixels + font ════════════════════════════════════════════════════════ */
static uint16_t *px;
static int stride;

static inline uint16_t rgb(int r, int g, int b) { return PAPP_RGB565(r, g, b); }

static void fill(int x, int y, int w, int h, uint16_t c)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
    for (int j = y0; j < y1; j++) {
        uint16_t *row = &px[j * stride];
        for (int i = x0; i < x1; i++) row[i] = c;
    }
}

static void frame_rect(int x, int y, int w, int h, uint16_t c)
{
    fill(x, y, w, 1, c); fill(x, y + h - 1, w, 1, c);
    fill(x, y, 1, h, c); fill(x + w - 1, y, 1, h, c);
}

static const char FONT_CHARS[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:-!'.x/*<>%()";
static const uint8_t FONT[][7] = {
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
    {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}, {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02},
    {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08},
};

static void draw_char(int x, int y, char ch, uint16_t c, int sc)
{
    if (ch >= 'a' && ch <= 'z' && ch != 'x') ch -= 32;
    const char *p = ch ? strchr(FONT_CHARS, ch) : NULL;
    if (!p) return;
    const uint8_t *g = FONT[p - FONT_CHARS];
    for (int r = 0; r < 7; r++)
        for (int k = 0; k < 5; k++)
            if (g[r] & (0x10 >> k)) fill(x + k * sc, y + r * sc, sc, sc, c);
}

static int text_w(const char *s, int sc) { return (int)strlen(s) * 6 * sc - sc; }

static void text(int x, int y, const char *s, uint16_t c, int sc)
{
    for (int i = 0; s[i]; i++) draw_char(x + i * 6 * sc, y, s[i], c, sc);
}

/* ══ colours ══════════════════════════════════════════════════════════════ */
#define C_BG     rgb(11, 18, 32)
#define C_CARD   rgb(22, 32, 52)
#define C_LINE   rgb(45, 60, 88)
#define C_INK    rgb(226, 232, 240)
#define C_MUTED  rgb(130, 145, 170)
#define C_GOOD   rgb(34, 197, 94)
#define C_BAD    rgb(239, 68, 68)
#define C_WARN   rgb(245, 158, 11)
#define C_TEAL   rgb(20, 184, 166)

static uint16_t cmd_color(const char *c)
{
    if (!strcmp(c, "forward"))  return rgb(34, 197, 94);
    if (!strcmp(c, "backward")) return rgb(234, 179, 8);
    if (!strcmp(c, "left"))     return rgb(59, 130, 246);
    if (!strcmp(c, "right"))    return rgb(168, 85, 247);
    if (!strcmp(c, "stop"))     return rgb(239, 68, 68);
    return C_INK;
}

/* ══ shared state (threads <-> screen) ════════════════════════════════════ */
enum { NS_START, NS_CONNECTING, NS_ONLINE, NS_WAKING, NS_TOKEN, NS_ERROR };

typedef struct {
    int  net;                 /* NS_* of the control connection */
    bool have_state;
    char gesture[16];         /* "" = no hand */
    int  conf;                /* percent */
    bool cam_live;
    char robot[16];
    char mode[12];
    bool esp_online;
    bool has_temp, has_hum;
    int  temp, hum;
    int64_t state_us;
} shared_t;

static shared_t sh;
static papp_mutex_t *mtx;
static volatile bool run = true;
static const papp_if_net_t *net;

/* video: the decoder fills vback, then it is swapped into vfront */
static uint16_t vbuf[2][VID_W * VID_H];
static uint16_t *vfront = vbuf[0], *vback = vbuf[1];
static int vw, vh;            /* size of vfront */
static int64_t vtime;         /* last good frame */
static bool v_none;           /* server has no video (camera page not sharing) */
static int vfps10;            /* frames per second x10 */

/* joystick -> control thread */
static volatile int pad_seq;          /* changes when pad_cmd changes */
static volatile const char *pad_cmd = "stop";
static volatile bool pad_active;      /* the joystick took control (manual) */
static volatile int ai_seq;           /* bumped by START / AI button */

/* ══ HTTPS with a kept-open connection ════════════════════════════════════ */
typedef struct {
    papp_sock_t *s;
    uint8_t buf[4096];
    int pos, len;
    bool insecure;
} conn_t;

static void conn_close(conn_t *c)
{
    if (c->s) net->close(c->s);
    c->s = NULL;
    c->pos = c->len = 0;
}

static int conn_open(conn_t *c)
{
    papp_tls_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_ms = 15000;
    cfg.insecure = c->insecure;
    int err = 0;
    c->s = net->tls_connect(ROBOT_HOST, 443, &cfg, &err);
    if (!c->s && err == PAPP_E_CERT && !c->insecure) {
        c->insecure = true;          /* certificate bundle without this CA: still encrypted */
        cfg.insecure = 1;
        c->s = net->tls_connect(ROBOT_HOST, 443, &cfg, &err);
    }
    c->pos = c->len = 0;
    return c->s ? 0 : (err ? err : PAPP_E_IO);
}

static int send_all(conn_t *c, const void *data, int n)
{
    const uint8_t *p = data;
    while (n > 0) {
        int k = net->send(c->s, p, (size_t)n, 10000);
        if (k <= 0) return -1;
        p += k; n -= k;
    }
    return 0;
}

static int rgetc(conn_t *c)
{
    if (c->pos >= c->len) {
        int n = net->recv(c->s, c->buf, sizeof(c->buf), 10000);
        if (n <= 0) return -1;
        c->pos = 0; c->len = n;
    }
    return c->buf[c->pos++];
}

static int rline(conn_t *c, char *out, int max)
{
    int n = 0, ch;
    while ((ch = rgetc(c)) >= 0) {
        if (ch == '\n') { out[n] = 0; return n; }
        if (ch != '\r' && n < max - 1) out[n++] = (char)ch;
    }
    return -1;
}

/* n bytes of body: into out while it fits, the rest is dropped */
static int rbody(conn_t *c, uint8_t *out, int max, int *got, int n)
{
    while (n > 0) {
        int ch = rgetc(c);
        if (ch < 0) return -1;
        if (*got < max) out[*got] = (uint8_t)ch;
        (*got)++;
        n--;
        /* copy the rest of the buffer in one go */
        int k = c->len - c->pos;
        if (k > n) k = n;
        if (k > 0) {
            int room = max - *got;
            int cp = k < room ? k : (room > 0 ? room : 0);
            if (cp) memcpy(out + *got, c->buf + c->pos, (size_t)cp);
            *got += k; c->pos += k; n -= k;
        }
    }
    return 0;
}

static int hexval(const char *s)
{
    int v = 0;
    for (; *s; s++) {
        int d = *s >= '0' && *s <= '9' ? *s - '0' : lower(*s) >= 'a' && lower(*s) <= 'f' ? lower(*s) - 'a' + 10 : -1;
        if (d < 0) break;
        v = v * 16 + d;
    }
    return v;
}

static int atoi_s(const char *s)
{
    while (*s == ' ') s++;
    int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

/* one request on the open connection; returns the HTTP status or < 0 */
static int http_once(conn_t *c, const char *method, const char *path, const char *body,
                     uint8_t *out, int max, int *outlen)
{
    char req[768];                       /* on the stack: both threads call this */
    int blen = body ? (int)strlen(body) : 0;
    char *p = req;
    p = s_add(p, method); p = s_add(p, " "); p = s_add(p, path);
    p = s_add(p, " HTTP/1.1\r\nHost: " ROBOT_HOST "\r\nX-Device-Token: " ROBOT_TOKEN
                 "\r\nUser-Agent: hand-robot-p4\r\nConnection: keep-alive\r\n");
    if (body) {
        p = s_add(p, "Content-Type: application/json\r\nContent-Length: ");
        p = s_int(p, blen);
        p = s_add(p, "\r\n");
    }
    p = s_add(p, "\r\n");
    if (send_all(c, req, (int)(p - req)) < 0) return -1;
    if (body && send_all(c, body, blen) < 0) return -1;

    char line[256];
    if (rline(c, line, sizeof(line)) < 0) return -1;
    char *sp = strchr(line, ' ');
    if (!sp) return -1;
    int status = atoi_s(sp + 1);

    int length = -1;
    bool chunked = false, must_close = false;
    for (;;) {
        int n = rline(c, line, sizeof(line));
        if (n < 0) return -1;
        if (n == 0) break;
        if (starts_ci(line, "content-length:")) length = atoi_s(line + 15);
        else if (starts_ci(line, "transfer-encoding:") && strstr(line, "hunked")) chunked = true;
        else if (starts_ci(line, "connection:") && strstr(line + 11, "lose")) must_close = true;
    }

    int got = 0;
    if (status == 204 || status == 304) {
        /* no body */
    } else if (chunked) {
        for (;;) {
            if (rline(c, line, sizeof(line)) < 0) return -1;
            int n = hexval(line);
            if (n == 0) {
                while (rline(c, line, sizeof(line)) > 0) {}
                break;
            }
            if (rbody(c, out, max, &got, n) < 0) return -1;
            if (rline(c, line, sizeof(line)) < 0) return -1;
        }
    } else if (length >= 0) {
        if (rbody(c, out, max, &got, length) < 0) return -1;
    } else {
        /* body until the server closes */
        int ch;
        while ((ch = rgetc(c)) >= 0) { if (got < max) out[got] = (uint8_t)ch; got++; }
        must_close = true;
    }
    if (must_close) conn_close(c);
    if (got > max) return -2;            /* too big for the buffer */
    if (outlen) *outlen = got;
    return status;
}

/* request with one retry on a fresh connection */
static int http(conn_t *c, const char *method, const char *path, const char *body,
                uint8_t *out, int max, int *outlen)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!c->s && conn_open(c) < 0) return -1;
        int st = http_once(c, method, path, body, out, max, outlen);
        if (st >= 0) return st;
        conn_close(c);
        if (st == -2) return -2;
        if (!run) return -1;
    }
    return -1;
}

/* ══ tiny JSON readers ════════════════════════════════════════════════════ */
/* finds "key": { ... } and returns its range (objects inside are skipped) */
static const char *obj_find(const char *js, const char *key, const char **end)
{
    char pat[32];
    char *p = pat; *p++ = '"'; p = s_add(p, key); *p++ = '"'; *p = 0;
    const char *s = strstr(js, pat);
    if (!s) return NULL;
    s += strlen(pat);
    while (*s == ' ' || *s == ':') s++;
    if (*s != '{') return NULL;
    int depth = 0; bool str = false;
    for (const char *q = s; *q; q++) {
        if (str) { if (*q == '\\' && q[1]) q++; else if (*q == '"') str = false; continue; }
        if (*q == '"') str = true;
        else if (*q == '{') depth++;
        else if (*q == '}' && --depth == 0) { *end = q; return s; }
    }
    return NULL;
}

/* value start of "key" inside [b, e), or NULL */
static const char *val_find(const char *b, const char *e, const char *key)
{
    char pat[32];
    char *p = pat; *p++ = '"'; p = s_add(p, key); *p++ = '"'; *p = 0;
    int n = (int)strlen(pat);
    for (const char *s = b; s + n < e; s++) {
        if (memcmp(s, pat, (size_t)n)) continue;
        const char *v = s + n;
        while (v < e && (*v == ' ' || *v == ':')) v++;
        return v < e ? v : NULL;
    }
    return NULL;
}

static bool get_str(const char *b, const char *e, const char *key, char *out, int max)
{
    const char *v = val_find(b, e, key);
    out[0] = 0;
    if (!v || *v != '"') return false;
    v++;
    int n = 0;
    while (v < e && *v != '"' && n < max - 1) out[n++] = *v++;
    out[n] = 0;
    return true;
}

static bool get_bool(const char *b, const char *e, const char *key)
{
    const char *v = val_find(b, e, key);
    return v && *v == 't';
}

/* number x100 (0.85 -> 85, 24.5 -> 2450); false for null / missing */
static bool get_num100(const char *b, const char *e, const char *key, int *out)
{
    const char *v = val_find(b, e, key);
    if (!v || !((*v >= '0' && *v <= '9') || *v == '-')) return false;
    int sign = 1;
    if (*v == '-') { sign = -1; v++; }
    int ip = 0, fp = 0, fd = 0;
    while (v < e && *v >= '0' && *v <= '9') ip = ip * 10 + (*v++ - '0');
    if (v < e && *v == '.') {
        v++;
        while (v < e && *v >= '0' && *v <= '9') { if (fd < 2) { fp = fp * 10 + (*v - '0'); fd++; } v++; }
    }
    while (fd < 2) { fp *= 10; fd++; }
    *out = sign * (ip * 100 + fp);
    return true;
}

static void parse_state(const char *js)
{
    shared_t st;
    papp_mutex_lock(mtx);
    st = sh;
    papp_mutex_unlock(mtx);

    const char *e, *b;
    if ((b = obj_find(js, "robot", &e))) {
        get_str(b, e, "command", st.robot, sizeof(st.robot));
        get_str(b, e, "mode", st.mode, sizeof(st.mode));
    }
    if ((b = obj_find(js, "camera", &e))) {
        st.cam_live = get_bool(b, e, "live");
        if (!get_str(b, e, "gesture", st.gesture, sizeof(st.gesture))) st.gesture[0] = 0;
        int c100 = 0;
        st.conf = get_num100(b, e, "confidence", &c100) ? c100 : 0;   /* 0.85 x100 = 85 % */
    }
    if ((b = obj_find(js, "esp", &e))) {
        int v = 0;
        st.esp_online = get_bool(b, e, "online");
        st.has_temp = get_num100(b, e, "temperature", &v); st.temp = v / 100;
        st.has_hum  = get_num100(b, e, "humidity", &v);    st.hum = v / 100;
    }
    st.have_state = true;
    st.net = NS_ONLINE;
    st.state_us = papp_time_us();

    papp_mutex_lock(mtx);
    sh = st;
    papp_mutex_unlock(mtx);
}

static void set_net(int ns)
{
    papp_mutex_lock(mtx);
    sh.net = ns;
    papp_mutex_unlock(mtx);
}

/* ══ control thread: state + joystick commands ════════════════════════════ */
static conn_t cconn;
static char cbody[8192];

static int handle_reply(int st, int len)
{
    if (st == 200) {
        cbody[len < (int)sizeof(cbody) ? len : (int)sizeof(cbody) - 1] = 0;
        parse_state(cbody);
    } else if (st == 401) set_net(NS_TOKEN);
    else if (st >= 500) set_net(NS_WAKING);
    else set_net(NS_ERROR);
    return st;
}

static int control_thread(void *arg)
{
    (void)arg;
    int sent_pad = -1, sent_ai = ai_seq;
    int64_t last_ctl = 0, last_state = 0;
    set_net(NS_CONNECTING);

    while (run && !papp_exit_requested()) {
        int64_t now = papp_time_us();
        int len = 0, st;
        char body[96];

        if (ai_seq != sent_ai) {
            sent_ai = ai_seq;
            st = http(&cconn, "POST", "/api/control", "{\"mode\":\"ai\"}", (uint8_t *)cbody, sizeof(cbody) - 1, &len);
            handle_reply(st, len);
            last_state = papp_time_us();
        } else if (pad_active && (pad_seq != sent_pad || now - last_ctl > 900000)) {
            sent_pad = pad_seq;
            char *p = s_add(body, "{\"mode\":\"manual\",\"command\":\"");
            p = s_add(p, (const char *)pad_cmd);
            s_add(p, "\"}");
            st = http(&cconn, "POST", "/api/control", body, (uint8_t *)cbody, sizeof(cbody) - 1, &len);
            handle_reply(st, len);
            last_ctl = last_state = papp_time_us();
        } else if (now - last_state > 200000) {
            /* since = far future: no log / climate lists in the reply */
            st = http(&cconn, "GET", "/api/state?since=9999999999", NULL, (uint8_t *)cbody, sizeof(cbody) - 1, &len);
            if (st < 0) { set_net(NS_ERROR); papp_delay_ms(1000); }
            else handle_reply(st, len);
            last_state = papp_time_us();
        } else {
            papp_delay_ms(10);
        }
    }
    conn_close(&cconn);
    return 0;
}

/* ══ video thread: /frame -> JPEG decode ══════════════════════════════════ */
static conn_t vconn;
static uint8_t jpg[160 * 1024];
static uint8_t jwork[16 * 1024];

typedef struct {
    const uint8_t *p;
    size_t len, pos;
    uint16_t *dst;
    int dw, dh;
} jsrc_t;

static size_t jin(JDEC *jd, uint8_t *buf, size_t n)
{
    jsrc_t *d = (jsrc_t *)jd->device;
    size_t left = d->len - d->pos;
    if (n > left) n = left;
    if (buf) memcpy(buf, d->p + d->pos, n);
    d->pos += n;
    return n;
}

static int jout(JDEC *jd, void *bmp, JRECT *r)
{
    jsrc_t *d = (jsrc_t *)jd->device;
    const uint16_t *s = (const uint16_t *)bmp;
    for (int y = r->top; y <= r->bottom; y++)
        for (int x = r->left; x <= r->right; x++, s++)
            if (x < d->dw && y < d->dh) d->dst[y * d->dw + x] = *s;
    return 1;
}

static bool decode(const uint8_t *data, int len)
{
    JDEC jd;
    jsrc_t src = { data, (size_t)len, 0, vback, 0, 0 };
    if (jd_prepare(&jd, jin, jwork, sizeof(jwork), &src) != JDR_OK) return false;
    int sc = 0;
    while (sc < 3 && ((jd.width >> sc) > VID_W || (jd.height >> sc) > VID_H)) sc++;
    int dw = (jd.width + (1 << sc) - 1) >> sc, dh = (jd.height + (1 << sc) - 1) >> sc;
    src.dw = dw > VID_W ? VID_W : dw;
    src.dh = dh > VID_H ? VID_H : dh;
    if (jd_decomp(&jd, jout, (uint8_t)sc) != JDR_OK) return false;

    papp_mutex_lock(mtx);
    uint16_t *t = vfront; vfront = vback; vback = t;
    vw = src.dw; vh = src.dh;
    vtime = papp_time_us();
    v_none = false;
    papp_mutex_unlock(mtx);
    return true;
}

static int video_thread(void *arg)
{
    (void)arg;
    int frames = 0;
    int64_t since = papp_time_us();
    while (run && !papp_exit_requested()) {
        int len = 0;
        int st = http(&vconn, "GET", "/frame", NULL, jpg, sizeof(jpg), &len);
        if (st == 200 && len > 0) {
            if (decode(jpg, len)) frames++;
            papp_delay_ms(20);
        } else if (st == 204) {
            papp_mutex_lock(mtx); v_none = true; papp_mutex_unlock(mtx);
            papp_delay_ms(500);
        } else {
            papp_delay_ms(1000);
        }
        int64_t now = papp_time_us();
        if (now - since >= 2000000) {
            vfps10 = (int)(frames * 10000000LL / (now - since));
            frames = 0; since = now;
        }
    }
    conn_close(&vconn);
    return 0;
}

/* ══ joystick ═════════════════════════════════════════════════════════════ */
static char pad_name[24];
static bool pad_connected;

static void read_pad(void)
{
    static uint32_t prev;
    uint32_t b = 0;
    pad_connected = false;
    papp_gamepad_t g;
    for (int p = 0; p < 2; p++) {
        if (!papp_input_gamepad(p, &g) || !g.connected) continue;
        if (!pad_connected) { memcpy(pad_name, g.name, sizeof(pad_name)); pad_name[23] = 0; }
        pad_connected = true;
        b |= g.buttons;
        if (g.lx < -16000) b |= PAPP_BTN_LEFT;
        if (g.lx > 16000)  b |= PAPP_BTN_RIGHT;
        if (g.ly < -16000) b |= PAPP_BTN_UP;
        if (g.ly > 16000)  b |= PAPP_BTN_DOWN;
    }
    uint32_t pressed = b & ~prev;
    prev = b;

    if (pressed & PAPP_BTN_START) {          /* back to AI (hand gestures) */
        pad_active = false;
        pad_cmd = "stop";
        ai_seq++;
        return;
    }

    const char *cmd = "stop";
    if (b & (PAPP_BTN_A | PAPP_BTN_B)) cmd = "stop";
    else if (b & PAPP_BTN_UP)    cmd = "forward";
    else if (b & PAPP_BTN_DOWN)  cmd = "backward";
    else if (b & PAPP_BTN_LEFT)  cmd = "left";
    else if (b & PAPP_BTN_RIGHT) cmd = "right";

    bool touched = strcmp(cmd, "stop") || (pressed & (PAPP_BTN_A | PAPP_BTN_B));
    if (touched && !pad_active) pad_active = true;      /* joystick takes control */
    if (pad_active && strcmp(cmd, (const char *)pad_cmd)) {
        pad_cmd = cmd;
        pad_seq++;
    }
}

/* ══ touch ════════════════════════════════════════════════════════════════ */
#define AI_BTN_X 300
#define AI_BTN_Y 214
#define AI_BTN_W 94
#define AI_BTN_H 22

static bool read_touch(papp_surface_t *s)
{
    static bool was_x, was_ai;
    papp_touch_point_t tp[3];
    int n = papp_disp_touch(s, tp, 3);
    bool on_x = false, on_ai = false;
    for (int i = 0; i < n; i++) {
        int x = tp[i].x, y = tp[i].y;
        if (x > W - 44 && y < 40) on_x = true;
        if (x >= AI_BTN_X - 4 && x < AI_BTN_X + AI_BTN_W + 4 && y >= AI_BTN_Y - 6) on_ai = true;
    }
    if (on_ai && !was_ai) { pad_active = false; pad_cmd = "stop"; ai_seq++; }
    bool exit_tap = on_x && !was_x;
    was_x = on_x; was_ai = on_ai;
    return exit_tap;
}

/* ══ drawing ══════════════════════════════════════════════════════════════ */
static void draw_x_button(void)
{
    int x = W - 28, y = 3;
    fill(x, y, 24, 18, C_BAD);
    for (int i = 0; i < 10; i++) {
        fill(x + 7 + i, y + 4 + i, 2, 1, C_INK);
        fill(x + 16 - i, y + 4 + i, 2, 1, C_INK);
    }
}

static void label_value(int x, int y, const char *label, const char *value, uint16_t c, int sc)
{
    text(x, y, label, C_MUTED, 1);
    text(x, y + 10, value, c, sc);
}

static void render(void)
{
    /* background + top bar first (2 buffers: the whole frame is drawn every time) */
    fill(0, 0, W, VID_Y - 4, C_CARD);
    fill(0, VID_Y - 4, W, H - VID_Y + 4, C_BG);
    fill(VID_X, VID_Y, VID_W, VID_H, rgb(0, 0, 0));

    shared_t st;
    papp_mutex_lock(mtx);
    st = sh;
    int64_t now = papp_time_us();
    bool fresh = vw > 0 && now - vtime < 3000000;
    bool none = v_none;
    if (fresh) {
        int ox = VID_X + (VID_W - vw) / 2, oy = VID_Y + (VID_H - vh) / 2;
        for (int y = 0; y < vh; y++)
            memcpy(&px[(oy + y) * stride + ox], &vfront[y * vw], (size_t)vw * 2);
    }
    papp_mutex_unlock(mtx);

    text(8, 8, "HAND ROBOT AI", C_TEAL, 1);
    {
        const char *ns = st.net == NS_ONLINE ? "ONLINE" : st.net == NS_WAKING ? "SERVER WAKING UP..." :
                         st.net == NS_TOKEN ? "WRONG TOKEN" : st.net == NS_ERROR ? "NO CONNECTION" : "CONNECTING...";
        uint16_t nc = st.net == NS_ONLINE ? C_GOOD : st.net == NS_CONNECTING || st.net == NS_START ? C_WARN : C_BAD;
        fill(98, 9, 6, 6, nc);
        text(108, 8, ns, nc, 1);
    }
    draw_x_button();

    /* video box */
    if (!fresh) {
        const char *m1 = none ? "NO VIDEO" : "WAITING FOR VIDEO...";
        const char *m2 = none ? "OPEN THE CAMERA PAGE" : "";
        text(VID_X + (VID_W - text_w(m1, 1)) / 2, VID_Y + 80, m1, C_MUTED, 1);
        if (*m2) text(VID_X + (VID_W - text_w(m2, 1)) / 2, VID_Y + 94, m2, C_MUTED, 1);
    }
    frame_rect(VID_X - 1, VID_Y - 1, VID_W + 2, VID_H + 2, C_LINE);
    {
        char fps[16];
        char *p = s_add(fps, "FPS ");
        p = s_int(p, vfps10 / 10); p = s_add(p, "."); s_int(p, vfps10 % 10);
        text(VID_X + 2, VID_Y + VID_H + 6, fps, C_MUTED, 1);
    }

    /* right panel */
    int x = 256;
    char tmp[32];
    if (st.gesture[0]) {
        strcpy(tmp, st.gesture); upcase(tmp);
        label_value(x, 30, "AI SEES", tmp, cmd_color(st.gesture), 2);
        char cf[16]; char *p = s_add(cf, "CONFIDENCE "); p = s_int(p, st.conf); s_add(p, "%");
        text(x, 56, cf, C_MUTED, 1);
    } else {
        label_value(x, 30, "AI SEES", st.cam_live ? "NO HAND" : "CAMERA OFF", C_MUTED, 2);
    }

    strcpy(tmp, st.robot[0] ? st.robot : "-"); upcase(tmp);
    label_value(x, 74, "ROBOT DOES", tmp, cmd_color(st.robot), 2);

    {
        bool manual = !strcmp(st.mode, "manual");
        label_value(x, 104, "MODE", manual ? "JOYSTICK / MANUAL" : "AI HAND GESTURES",
                    manual ? C_WARN : C_TEAL, 1);
    }
    label_value(x, 128, "ESP32 ROBOT", st.esp_online ? "ONLINE" : "OFFLINE", st.esp_online ? C_GOOD : C_BAD, 1);
    {
        char th[32]; char *p = th;
        p = s_add(p, "T:");
        if (st.has_temp) { p = s_int(p, st.temp); p = s_add(p, "C"); } else p = s_add(p, "--");
        p = s_add(p, "  H:");
        if (st.has_hum) { p = s_int(p, st.hum); p = s_add(p, "%"); } else p = s_add(p, "--");
        label_value(x, 152, "CLIMATE", th, C_INK, 1);
    }
    {
        const char *pn = pad_connected ? pad_name : "NO JOYSTICK";
        strncpy(tmp, pn, 23); tmp[23] = 0; upcase(tmp);
        label_value(x, 176, "JOYSTICK", tmp, pad_connected ? C_GOOD : C_MUTED, 1);
    }

    /* bottom: hint + AI button */
    text(8, 226, "STICK: DRIVE  A/B: STOP  START: AI", C_MUTED, 1);
    fill(AI_BTN_X, AI_BTN_Y, AI_BTN_W, AI_BTN_H, C_TEAL);
    text(AI_BTN_X + (AI_BTN_W - text_w("AI MODE", 1)) / 2, AI_BTN_Y + 8, "AI MODE", C_BG, 1);
}

static void message_screen(papp_surface_t *s, const char *l1, const char *l2)
{
    px = papp_disp_lock(s, &stride);
    fill(0, 0, W, H, C_BG);
    text((W - text_w("HAND ROBOT AI", 2)) / 2, 70, "HAND ROBOT AI", C_TEAL, 2);
    text((W - text_w(l1, 1)) / 2, 120, l1, C_INK, 1);
    if (l2) text((W - text_w(l2, 1)) / 2, 136, l2, C_MUTED, 1);
    draw_x_button();
    papp_disp_present(s);
}

/* ══ main ═════════════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
    papp_surface_t *s = papp_disp_surface_create(&cfg);
    if (!s) return 2;

    mtx = papp_mutex_create(false);
    memset(&sh, 0, sizeof(sh));
    strcpy(sh.robot, "stop");
    strcpy(sh.mode, "ai");

    /* Wi-Fi */
    net = papp_net();
    if (!net) {
        message_screen(s, "THIS LAUNCHER HAS NO NETWORK", "UPDATE THE LAUNCHER (0.29+)");
        while (!papp_exit_requested() && !read_touch(s)) { papp_delay_ms(30); message_screen(s, "THIS LAUNCHER HAS NO NETWORK", "UPDATE THE LAUNCHER (0.29+)"); }
        return 0;
    }
    message_screen(s, "CONNECTING TO WI-FI...", NULL);
    if (net->up(15000) != PAPP_E_OK) {
        int r = papp_ui_wifi();
        if (r != 1) {
            for (;;) {
                message_screen(s, "NO WI-FI", "TAP X TO EXIT");
                if (papp_exit_requested() || read_touch(s)) return 0;
                papp_delay_ms(30);
            }
        }
    }
    message_screen(s, "CONNECTING TO THE SERVER...", ROBOT_HOST);

    papp_thread_cfg_t tc = { .name = "hr_ctl", .stack_size = 16384, .prio = PAPP_PRIO_NORMAL, .core = -1 };
    papp_thread_t *t_ctl = papp_thread_create(control_thread, NULL, &tc);
    papp_thread_cfg_t tv = { .name = "hr_video", .stack_size = 16384, .prio = PAPP_PRIO_NORMAL, .core = -1 };
    papp_thread_t *t_vid = papp_thread_create(video_thread, NULL, &tv);

    while (!papp_exit_requested()) {
        read_pad();
        if (read_touch(s)) break;
        px = papp_disp_lock(s, &stride);
        render();
        papp_disp_present(s);
    }

    /* let the robot stop if the joystick was driving */
    if (pad_active) { pad_cmd = "stop"; pad_seq++; papp_delay_ms(300); }
    run = false;
    if (t_ctl) papp_thread_join(t_ctl, 3000, NULL);
    if (t_vid) papp_thread_join(t_vid, 3000, NULL);
    papp_disp_surface_destroy(s);
    return 0;
}
