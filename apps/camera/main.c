/*
 * Camera - live video from a USB (UVC) camera on the board's USB host port (launcher 0.30+).
 * By Abed Mihilia. X (top right) exits. Tap the picture = record: a JPEG every 0.5 s to
 * /sd/capture/IMG_nnnn.jpg (pictures for training / calibrating AI models); tap again = stop.
 */
#include "papp/papp.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

PAPP_MANIFEST(.id = "abed.camera", .name = "Camera", .version = PAPP_APP_VERSION, .main_stack = 16384);

#define W 640
#define H 480

static uint16_t *px;
static int stride;
static uint16_t last[640 * 480];
static bool have_last;

static void rect(int x, int y, int w, int h, uint16_t c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if ((unsigned)i < W && (unsigned)j < H) px[j * stride + i] = c;
}

static void draw_x(void)
{
    rect(W - 44, 8, 36, 36, PAPP_RGB565(20, 30, 50));
    for (int k = 0; k < 20; k++) {
        rect(W - 36 + k, 16 + k, 3, 3, 0xFFFF);
        rect(W - 36 + k, 35 - k, 3, 3, 0xFFFF);
    }
}

/* 3x5 digits for the fps counter */
static const uint16_t DIG[10] = { 0x7B6F, 0x2492, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF };
static void num(int x, int y, int v)
{
    char b[8]; int n = 0;
    do { b[n++] = (char)(v % 10); v /= 10; } while (v && n < 7);
    for (int i = 0; i < n; i++) {
        uint16_t g = DIG[(int)b[n - 1 - i]];
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < 3; c++)
                if (g & (1 << (14 - r * 3 - c))) rect(x + i * 16 + c * 4, y + r * 4, 4, 4, PAPP_RGB565(120, 255, 160));
    }
}

static uint8_t jpg[256 * 1024];
static int rec_count;

static int next_index(void)
{
    char path[64];
    for (int i = 1; i < 100000; i++) {
        snprintf(path, sizeof path, "/sd/capture/IMG_%04d.jpg", i);
        struct stat st;
        if (stat(path, &st) != 0) return i;
    }
    return 1;
}

static void save_jpeg(const papp_if_cam_t *cam, int *idx)
{
    uint32_t sq = 0;
    int n = cam->jpeg(jpg, sizeof jpg, &sq, 200);
    if (n <= 0) return;
    char path[64];
    snprintf(path, sizeof path, "/sd/capture/IMG_%04d.jpg", (*idx)++);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(jpg, 1, n, f);
    fclose(f);
    rec_count++;
}

static void message(papp_surface_t *s, uint16_t bg)
{
    px = papp_disp_lock(s, &stride);
    rect(0, 0, W, H, bg);
    /* a camera symbol */
    rect(240, 180, 160, 110, PAPP_RGB565(200, 210, 230));
    rect(290, 160, 60, 24, PAPP_RGB565(200, 210, 230));
    rect(290, 205, 60, 60, bg);
    draw_x();
    papp_disp_present(s);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
    papp_surface_t *s = papp_disp_surface_create(&cfg);
    if (!s) return 2;
    const papp_if_cam_t *cam = papp_cam();
    bool open = false;
    uint32_t seq = 0;
    int64_t t_try = 0, t_save = 0;
    bool rec = false, was_touch = false;
    int idx = 1;
    while (!papp_exit_requested()) {
        papp_touch_point_t tp[3];
        int n = papp_disp_touch(s, tp, 3);
        bool quit = false, tap = false;
        for (int i = 0; i < n; i++) { if (tp[i].x > W - 60 && tp[i].y < 60) quit = true; else tap = true; }
        if (quit) break;
        if (tap && !was_touch && open) {
            rec = !rec;
            if (rec) { mkdir("/sd/capture", 0777); idx = next_index(); rec_count = 0; t_save = 0; }
        }
        was_touch = n > 0;
        if (!cam) { message(s, PAPP_RGB565(90, 20, 20)); papp_delay_ms(100); continue; }   /* launcher too old */
        papp_cam_info_t info;
        cam->info(&info);
        if (!info.connected) { open = false; message(s, PAPP_RGB565(40, 40, 60)); papp_delay_ms(100); continue; }
        if (!open) {
            int64_t now = papp_time_us();
            if (now - t_try < 1000000) { papp_delay_ms(50); continue; }
            t_try = now;
            open = cam->open(640, 480, 0) >= 0;
            if (!open) { message(s, PAPP_RGB565(90, 60, 10)); continue; }
        }
        px = papp_disp_lock(s, &stride);
        int r = cam->frame(px, W, H, stride, &seq, 200);
        if (r < 0) { papp_disp_present(s); open = false; papp_delay_ms(200); continue; }   /* try again */
        /* no new frame in time: show the last one again (the back buffer holds an older one = flicker) */
        if (r == 1) for (int y = 0; y < H; y++) memcpy(&last[y * W], &px[y * stride], W * 2);
        else if (have_last) for (int y = 0; y < H; y++) memcpy(&px[y * stride], &last[y * W], W * 2);
        have_last |= r == 1;
        cam->info(&info);
        num(12, 12, (int)(info.fps_x10 / 10));
        if (rec) {
            int64_t now = papp_time_us();
            if (now - t_save >= 500000) { t_save = now; save_jpeg(cam, &idx); }
            for (int y = -9; y <= 9; y++)            /* red REC dot + number of pictures */
                for (int x = -9; x <= 9; x++)
                    if (x * x + y * y <= 81) px[(H - 30 + y) * stride + 30 + x] = PAPP_RGB565(255, 40, 40);
            num(52, H - 40, rec_count);
        }
        draw_x();
        papp_disp_present(s);
    }
    if (cam) cam->close();
    papp_disp_surface_destroy(s);
    return 0;
}
