/*
 * Aquarium — a living lagoon by Abed Mihilia.
 * Fish swim, seaweed sways, bubbles rise and light shimmers from the surface.
 * Touch the water to drop food: the fish race to it and eat it.
 * Drawn in software on an 800x480 RGB565 surface.
 */
#include "papp/papp.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

PAPP_MANIFEST(.id = "abed.aquarium", .name = "Aquarium", .version = PAPP_APP_VERSION, .main_stack = 16384);

#define W 800
#define H 480
#define SAND_Y 405
#define NFISH 9
#define NBUB 48
#define NFOOD 40
#define NRIP 8
#define NWEED 9

/* ── pixels ─────────────────────────────────────────────────────────────── */
static uint16_t *px;
static int stride;

static inline uint16_t rgb(int r, int g, int b)
{
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return PAPP_RGB565(r, g, b);
}

/* a = 0..32 */
static inline uint16_t blend(uint16_t d, uint16_t s, int a)
{
    uint32_t dd = (d | ((uint32_t)d << 16)) & 0x07E0F81F;
    uint32_t ss = (s | ((uint32_t)s << 16)) & 0x07E0F81F;
    uint32_t r = ((((ss - dd) * a) >> 5) + dd) & 0x07E0F81F;
    return (uint16_t)(r | (r >> 16));
}

static inline void plot(int x, int y, uint16_t c, int a)
{
    if ((unsigned)x >= W || (unsigned)y >= H) return;
    uint16_t *p = &px[y * stride + x];
    *p = a >= 32 ? c : blend(*p, c, a);
}

static void disc(float cx, float cy, float r, uint16_t c, int a)
{
    int x0 = (int)(cx - r), x1 = (int)(cx + r) + 1, y0 = (int)(cy - r), y1 = (int)(cy + r) + 1;
    float r2 = r * r;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r2) plot(x, y, c, a);
        }
}

static void ring(float cx, float cy, float r, float t, uint16_t c, int a)
{
    int x0 = (int)(cx - r - t), x1 = (int)(cx + r + t) + 1, y0 = (int)(cy - r - t), y1 = (int)(cy + r + t) + 1;
    float lo = (r - t) * (r - t), hi = (r + t) * (r + t);
    if (r < t) lo = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float dx = x - cx, dy = y - cy, d = dx * dx + dy * dy;
            if (d >= lo && d <= hi) plot(x, y, c, a);
        }
}

/* ── random ─────────────────────────────────────────────────────────────── */
static uint32_t seed = 0x1234567;
static float frand(void) { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; }
static float frange(float a, float b) { return a + (b - a) * frand(); }

/* ── background (static part, drawn once) ──────────────────────────────── */
static uint16_t *bg;

static float sand_top(float x) { return SAND_Y + 8 * sinf(x * 0.012f) + 5 * sinf(x * 0.031f + 1.3f); }

static void make_background(void)
{
    for (int y = 0; y < H; y++) {
        float t = (float)y / H;
        uint16_t c = rgb(10 + (int)(-6 * t), 92 - (int)(66 * t), 104 - (int)(70 * t));
        for (int x = 0; x < W; x++) bg[y * W + x] = c;
    }
    /* far rocks: soft silhouettes */
    uint16_t *save = px; int save_stride = stride;
    px = bg; stride = W;
    for (int i = 0; i < 7; i++) {
        float cx = 60 + i * 120 + frange(-30, 30), r = frange(40, 80);
        disc(cx, SAND_Y + 10, r, rgb(12, 52, 60), 20);
    }
    /* sand with dunes and grain */
    for (int x = 0; x < W; x++) {
        int top = (int)sand_top(x);
        for (int y = top; y < H; y++) {
            float t = (float)(y - top) / (H - top + 1);
            int n = (int)(frand() * 18) - 9;
            bg[y * W + x] = rgb(200 - (int)(60 * t) + n, 176 - (int)(56 * t) + n, 124 - (int)(40 * t) + n);
        }
        for (int y = top; y < top + 3 && y < H; y++) bg[y * W + x] = rgb(222, 204, 150);
    }
    /* pebbles and shells */
    for (int i = 0; i < 70; i++) {
        float x = frange(0, W), y = frange(SAND_Y + 18, H - 4);
        int g = (int)frange(90, 170);
        disc(x, y, frange(1.5f, 4), rgb(g, g - 10, g - 25), 26);
    }
    /* a pink coral and a purple one */
    for (int k = 0; k < 2; k++) {
        float bx = k ? 650 : 140, by = sand_top(bx) + 4;
        uint16_t c = k ? rgb(160, 90, 220) : rgb(255, 110, 140);
        for (int b = 0; b < 5; b++) {
            float ang = -1.57f + (b - 2) * 0.38f, len = frange(50, 85);
            for (float s = 0; s < len; s += 2) {
                float x = bx + cosf(ang) * s + sinf(s * 0.15f + b) * 4, y = by + sinf(ang) * s;
                disc(x, y, 6 - s / len * 3, c, 32);
            }
            disc(bx + cosf(ang) * len, by + sinf(ang) * len, 5, rgb(255, 230, 240), 20);
        }
    }
    px = save; stride = save_stride;
}

/* ── light rays from the surface ───────────────────────────────────────── */
static void draw_rays(float t)
{
    uint16_t lc = rgb(190, 255, 240);
    for (int k = 0; k < 4; k++) {
        float x0 = 90 + k * 200 + 40 * sinf(t * 0.25f + k * 1.7f);
        for (int y = 0; y < SAND_Y; y += 1) {
            int a = (int)(7 * (1.0f - (float)y / SAND_Y) * (0.75f + 0.25f * sinf(t * 0.8f + k)));
            if (a <= 0) break;
            float cx = x0 + y * 0.45f, hw = 22 + y * 0.09f;
            int xa = (int)(cx - hw), xb = (int)(cx + hw);
            if (xa < 0) xa = 0;
            if (xb >= W) xb = W - 1;
            uint16_t *row = &px[y * stride];
            for (int x = xa; x <= xb; x++) row[x] = blend(row[x], lc, a);
        }
    }
    /* shimmering surface line */
    for (int x = 0; x < W; x++) {
        int y = (int)(4 + 3 * sinf(x * 0.03f + t * 2.0f) + 2 * sinf(x * 0.071f - t * 1.3f));
        plot(x, y, lc, 14);
        plot(x, y + 1, lc, 8);
    }
}

/* ── seaweed ────────────────────────────────────────────────────────────── */
static struct { float x, h, ph; uint16_t c; } weed[NWEED];

static void draw_weed(float t)
{
    for (int i = 0; i < NWEED; i++) {
        float base = sand_top(weed[i].x) + 6;
        int segs = (int)(weed[i].h / 6);
        for (int s = 0; s < segs; s++) {
            float f = (float)s / segs;
            float x = weed[i].x + sinf(t * 1.3f + weed[i].ph + s * 0.22f) * f * f * 26;
            float y = base - s * 6;
            disc(x, y, 6.5f - f * 3.5f, weed[i].c, 32);
        }
    }
}

/* ── fish ───────────────────────────────────────────────────────────────── */
typedef struct { float x, y, vx, by, ph, len, hgt, speed; int kind; } fish_t;
static fish_t fish[NFISH];

/* kind: 0 clownfish, 1 blue tang, 2 yellow tang, 3 purple, 4 green-gold */
static void fish_colors(int kind, uint16_t *body, uint16_t *belly, uint16_t *tail, uint16_t *stripe)
{
    switch (kind) {
    case 0: *body = rgb(255, 120, 20); *belly = rgb(255, 170, 70); *tail = rgb(255, 110, 10); *stripe = rgb(255, 255, 255); break;
    case 1: *body = rgb(30, 90, 235); *belly = rgb(80, 150, 255); *tail = rgb(255, 215, 40); *stripe = rgb(10, 20, 60); break;
    case 2: *body = rgb(255, 215, 40); *belly = rgb(255, 240, 120); *tail = rgb(255, 200, 20); *stripe = 0; break;
    case 3: *body = rgb(160, 70, 220); *belly = rgb(255, 150, 60); *tail = rgb(255, 140, 40); *stripe = 0; break;
    default: *body = rgb(40, 200, 140); *belly = rgb(230, 240, 120); *tail = rgb(30, 170, 120); *stripe = rgb(240, 250, 200); break;
    }
}

static void draw_fish(const fish_t *f, float t)
{
    uint16_t body, belly, tailc, stripe;
    fish_colors(f->kind, &body, &belly, &tailc, &stripe);
    float dir = f->vx >= 0 ? 1.0f : -1.0f;
    float L = f->len, Hh = f->hgt;
    float wag = sinf(t * 9.0f + f->ph) * 0.35f;
    int x0 = (int)(f->x - L * 1.8f), x1 = (int)(f->x + L * 1.2f) + 1;
    int y0 = (int)(f->y - Hh * 1.6f), y1 = (int)(f->y + Hh * 1.6f) + 1;
    uint16_t dark = blend(body, 0, 10);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float u = (x - f->x) * dir / L;          /* +1 nose, -1 tail root */
            float v = (y - f->y) / Hh;
            float e = u * u + v * v;
            if (e <= 1.0f) {
                uint16_t c = v > 0.25f ? blend(body, belly, (int)((v - 0.25f) * 40)) : body;
                if (v < -0.55f) c = blend(c, rgb(255, 255, 255), 6);      /* light on the back */
                if (stripe) {
                    float s1 = fabsf(u - 0.35f), s2 = fabsf(u + 0.25f);
                    if (f->kind == 1) { if (v < -0.1f && u > -0.6f && u < 0.5f && fabsf(v + 0.35f) < 0.18f) c = stripe; }
                    else if (s1 < 0.09f || s2 < 0.09f) c = (s1 < 0.06f || s2 < 0.06f) ? stripe : rgb(20, 20, 20);
                }
                if (e > 0.82f) c = blend(c, dark, 18);                  /* outline */
                plot(x, y, c, 32);
            } else if (u < -0.85f && u > -1.65f) {                    /* tail */
                float d = -0.85f - u;
                float vc = v - wag * d * 1.5f;
                if (fabsf(vc) < d * 1.1f && !(d > 0.55f && fabsf(vc) < (d - 0.55f) * 1.2f))
                    plot(x, y, tailc, 30);
            } else if (u > -0.5f && u < 0.35f && v < -0.75f && v > -1.45f) { /* top fin */
                float k = (u + 0.5f) / 0.85f;
                if (v > -0.75f - (1 - k) * 0.7f) plot(x, y, tailc, 26);
            }
        }
    }
    float ex = f->x + dir * L * 0.55f, ey = f->y - Hh * 0.22f;
    disc(ex, ey, Hh * 0.24f + 1, rgb(255, 255, 255), 32);
    disc(ex + dir * 1.0f, ey, Hh * 0.13f + 0.6f, rgb(10, 10, 20), 32);
    disc(ex + dir * 0.2f, ey - 1.5f, 1.1f, rgb(255, 255, 255), 32);
}

/* ── bubbles, food, ripples ─────────────────────────────────────────────── */
static struct { float x, y, r, sp, ph; } bub[NBUB];
static struct { float x, y, vy; int life; bool on; } food[NFOOD];
static struct { float x, y, r; int life; } rip[NRIP];

static void new_bubble(int i, bool anywhere)
{
    static const float src[3] = { 210, 470, 720 };
    bub[i].x = src[(int)(frand() * 3) % 3] + frange(-10, 10);
    bub[i].y = anywhere ? frange(20, SAND_Y) : SAND_Y + frange(0, 10);
    bub[i].r = frange(2, 7);
    bub[i].sp = 0.6f + bub[i].r * 0.25f;
    bub[i].ph = frange(0, 6.28f);
}

static void drop_food(float x, float y)
{
    for (int n = 0; n < 6; n++)
        for (int i = 0; i < NFOOD; i++)
            if (!food[i].on) {
                food[i].on = true;
                food[i].x = x + frange(-22, 22);
                food[i].y = y + frange(-10, 10);
                food[i].vy = frange(0.5f, 1.1f);
                food[i].life = 900;
                break;
            }
    for (int i = 0; i < NRIP; i++)
        if (rip[i].life <= 0) { rip[i].x = x; rip[i].y = y; rip[i].r = 4; rip[i].life = 40; break; }
}

/* ── main ───────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
    papp_surface_t *s = papp_disp_surface_create(&cfg);
    if (!s) { papp_log("no surface\n"); return 2; }
    bg = papp_alloc(W * H * 2, PAPP_MEM_LARGE, 64);
    if (!bg) { papp_disp_surface_destroy(s); return 3; }
    seed ^= (uint32_t)papp_time_us();
    make_background();

    static const float wx[NWEED] = { 40, 95, 260, 330, 400, 540, 590, 760, 790 };
    for (int i = 0; i < NWEED; i++) {
        weed[i].x = wx[i] + frange(-10, 10);
        weed[i].h = frange(90, 190);
        weed[i].ph = frange(0, 6.28f);
        weed[i].c = rgb((int)frange(20, 70), (int)frange(140, 200), (int)frange(60, 100));
    }
    for (int i = 0; i < NFISH; i++) {
        fish_t *f = &fish[i];
        f->kind = i % 5;
        f->len = frange(26, 40);
        f->hgt = f->len * (f->kind == 1 || f->kind == 2 ? 0.62f : 0.48f);
        f->x = frange(80, W - 80);
        f->by = f->y = frange(70, SAND_Y - 60);
        f->speed = frange(0.9f, 1.8f);
        f->vx = frand() < 0.5f ? f->speed : -f->speed;
        f->ph = frange(0, 6.28f);
    }
    for (int i = 0; i < NBUB; i++) new_bubble(i, true);

    float t = 0;
    bool was_down = false;
    int hold = 0;
    while (!papp_exit_requested()) {
        px = papp_disp_lock(s, &stride);
        for (int y = 0; y < H; y++) memcpy(&px[y * stride], &bg[y * W], W * 2);
        draw_rays(t);

        /* touch: drop food */
        papp_touch_point_t tp[2];
        int n = papp_disp_touch(s, tp, 2);
        if (n > 0 && tp[0].y < SAND_Y) {
            if (!was_down || ++hold % 12 == 0) drop_food(tp[0].x, tp[0].y);
        } else hold = 0;
        was_down = n > 0;

        draw_weed(t);

        /* food sinks, then rests on the sand and fades */
        for (int i = 0; i < NFOOD; i++) {
            if (!food[i].on) continue;
            float ground = sand_top(food[i].x) + 4;
            if (food[i].y < ground) { food[i].y += food[i].vy; food[i].x += sinf(t * 3 + i) * 0.3f; }
            if (--food[i].life <= 0) { food[i].on = false; continue; }
            int a = food[i].life < 64 ? food[i].life / 2 : 32;
            disc(food[i].x, food[i].y, 2.6f, rgb(170, 90, 40), a);
            plot((int)food[i].x - 1, (int)food[i].y - 1, rgb(240, 170, 90), a);
        }

        /* fish: wander, or chase the nearest food */
        for (int i = 0; i < NFISH; i++) {
            fish_t *f = &fish[i];
            int best = -1; float bd = 1e9f;
            for (int k = 0; k < NFOOD; k++) {
                if (!food[k].on) continue;
                float dx = food[k].x - f->x, dy = food[k].y - f->y, d = dx * dx + dy * dy;
                if (d < bd) { bd = d; best = k; }
            }
            if (best >= 0 && bd < 380.0f * 380.0f) {
                float dx = food[best].x - f->x, dy = food[best].y - f->y;
                float sp = f->speed * 2.2f;
                float want = dx > 0 ? sp : -sp;
                if (fabsf(dx) < 6) want = dx * 0.3f;
                f->vx += (want - f->vx) * 0.08f;
                f->by += dy * 0.05f;
                if (fabsf(dx) < f->len * 0.9f && fabsf(dy) < f->hgt * 0.9f) {
                    food[best].on = false;
                    for (int b = 0; b < NBUB; b++)               /* a happy bubble */
                        if (bub[b].y < 30) { bub[b].x = f->x; bub[b].y = f->y - f->hgt; bub[b].r = 3; break; }
                }
            } else {
                float want = f->vx >= 0 ? f->speed : -f->speed;
                if (f->x > W - 60) want = -f->speed;
                if (f->x < 60) want = f->speed;
                if (fabsf(f->vx) < 0.15f) want = (frand() < 0.5f ? 1 : -1) * f->speed;
                f->vx += (want - f->vx) * 0.02f;
                f->by += sinf(t * 0.4f + f->ph) * 0.15f;
            }
            if (f->by < 50) f->by = 50;
            if (f->by > SAND_Y - 30) f->by = SAND_Y - 30;
            f->x += f->vx;
            f->y = f->by + sinf(t * 1.6f + f->ph) * 4;
            draw_fish(f, t);
        }

        /* bubbles */
        uint16_t bc = rgb(200, 255, 250);
        for (int i = 0; i < NBUB; i++) {
            bub[i].y -= bub[i].sp;
            float x = bub[i].x + sinf(t * 2.5f + bub[i].ph + bub[i].y * 0.03f) * 3;
            if (bub[i].y < 8) { new_bubble(i, false); continue; }
            ring(x, bub[i].y, bub[i].r, 0.8f, bc, 22);
            disc(x - bub[i].r * 0.35f, bub[i].y - bub[i].r * 0.35f, bub[i].r * 0.28f, bc, 26);
        }

        /* ripples where food was dropped */
        for (int i = 0; i < NRIP; i++) {
            if (rip[i].life <= 0) continue;
            rip[i].r += 2.2f;
            ring(rip[i].x, rip[i].y, rip[i].r, 1.2f, rgb(220, 255, 250), rip[i].life * 20 / 40);
            rip[i].life--;
        }

        papp_disp_present(s);
        t += 1.0f / 30;
    }
    papp_free(bg);
    papp_disp_surface_destroy(s);
    return 0;
}
