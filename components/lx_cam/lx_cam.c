/* lx_cam: USB (UVC) camera, see lx_cam.h */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "usb/uvc_host.h"
#include "driver/jpeg_decode.h"
#include "lx_cam.h"

static const char *TAG = "lx_cam";

#define MAX_FRAMES 32
#define STATUS_FILE "/sd/launcher/camera.txt"

enum { EV_CONNECTED = 1, EV_DISCONNECTED };
typedef struct { int type; uint8_t addr, idx; } cam_ev_t;

static QueueHandle_t s_evq;
static SemaphoreHandle_t s_lock;          /* the latest frame and the stream handle */
static SemaphoreHandle_t s_dec_lock;      /* the JPEG decoder */

static uvc_host_frame_info_t s_list[MAX_FRAMES];
static size_t s_list_n;
static volatile bool s_connected;
static uint8_t s_addr, s_idx;
static uvc_host_stream_hdl_t s_stream;

static uint8_t *s_jpg;                    /* newest MJPEG frame (PSRAM) */
static uint32_t s_jpg_len, s_jpg_cap;
static volatile uint32_t s_seq;
static uint16_t s_w, s_h;
static uint8_t s_fmt;
static volatile uint32_t s_frames, s_fps_x10;
static uint32_t s_fps_cnt;
static int64_t s_fps_t0;

static jpeg_decoder_handle_t s_dec;
static uint8_t *s_dec_in;
static size_t s_dec_in_cap;
static uint8_t *s_dec_out;
static size_t s_dec_out_cap;

static const char *fmt_name(int f)
{
    switch (f) {
    case UVC_VS_FORMAT_MJPEG: return "MJPEG";
    case UVC_VS_FORMAT_YUY2:  return "YUY2";
    case UVC_VS_FORMAT_H264:  return "H264";
    case UVC_VS_FORMAT_H265:  return "H265";
    case UVC_VS_FORMAT_NV12:  return "NV12";
    default:                  return "?";
    }
}

/* ── driver callbacks (USB driver task) ─────────────────────────────────── */
static bool frame_cb(const uvc_host_frame_t *f, void *ctx)
{
    (void)ctx;
    if (f->data_len > 0 && f->vs_format.format == UVC_VS_FORMAT_MJPEG && xSemaphoreTake(s_lock, 0) == pdTRUE) {
        if (f->data_len > s_jpg_cap) {
            size_t cap = f->data_len + 32 * 1024;
            uint8_t *n = heap_caps_realloc(s_jpg, cap, MALLOC_CAP_SPIRAM);
            if (n) { s_jpg = n; s_jpg_cap = cap; }
        }
        if (f->data_len <= s_jpg_cap) {
            memcpy(s_jpg, f->data, f->data_len);
            s_jpg_len = f->data_len;
            s_seq++;
        }
        xSemaphoreGive(s_lock);
    }
    s_frames++;
    s_fps_cnt++;
    int64_t now = esp_timer_get_time();
    if (now - s_fps_t0 >= 1000000) {
        s_fps_x10 = (uint32_t)(s_fps_cnt * 10000000LL / (now - s_fps_t0));
        s_fps_cnt = 0;
        s_fps_t0 = now;
    }
    return true;                          /* the frame buffer goes straight back to the driver */
}

static void stream_cb(const uvc_host_stream_event_data_t *e, void *ctx)
{
    (void)ctx;
    switch (e->type) {
    case UVC_HOST_DEVICE_DISCONNECTED: {
        cam_ev_t ev = { .type = EV_DISCONNECTED };
        xQueueSend(s_evq, &ev, 0);
        break;
    }
    case UVC_HOST_TRANSFER_ERROR:
        ESP_LOGW(TAG, "transfer error %s", esp_err_to_name(e->transfer_error.error));
        break;
    default:
        break;
    }
}

static void driver_cb(const uvc_host_driver_event_data_t *e, void *ctx)
{
    (void)ctx;
    if (e->type == UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        cam_ev_t ev = { .type = EV_CONNECTED, .addr = e->device_connected.dev_addr, .idx = e->device_connected.uvc_stream_index };
        xQueueSend(s_evq, &ev, 0);
    }
}

/* ── stream ─────────────────────────────────────────────────────────────── */
static void close_locked(void)
{
    if (s_stream) {
        uvc_host_stream_stop(s_stream);
        uvc_host_stream_close(s_stream);
        s_stream = NULL;
    }
    s_w = s_h = 0;
    s_fmt = 0;
}

void lx_cam_close(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    close_locked();
    xSemaphoreGive(s_lock);
}

esp_err_t lx_cam_open(int w, int h, int fps)
{
    if (!s_lock || !s_connected) return ESP_ERR_NOT_FOUND;
    if (w <= 0 || h <= 0) { w = 640; h = 480; }
    /* the MJPEG size closest to the request */
    int best = -1;
    long bestd = 0;
    for (size_t i = 0; i < s_list_n; i++) {
        if (s_list[i].format != UVC_VS_FORMAT_MJPEG) continue;
        long d = labs((long)s_list[i].h_res - w) + labs((long)s_list[i].v_res - h);
        if (best < 0 || d < bestd) { best = (int)i; bestd = d; }
    }
    if (best < 0) {
        ESP_LOGE(TAG, "the camera has no MJPEG format");
        return ESP_ERR_NOT_SUPPORTED;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_stream && s_w == s_list[best].h_res && s_h == s_list[best].v_res) {   /* already running */
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    close_locked();
    uvc_host_stream_config_t cfg = {
        .event_cb = stream_cb,
        .frame_cb = frame_cb,
        .usb = { .dev_addr = s_addr, .vid = UVC_HOST_ANY_VID, .pid = UVC_HOST_ANY_PID, .uvc_stream_index = s_idx },
        .vs_format = { .h_res = s_list[best].h_res, .v_res = s_list[best].v_res, .fps = (float)fps, .format = UVC_VS_FORMAT_MJPEG },
        .advanced = { .number_of_frame_buffers = 3, .frame_size = 0, .frame_heap_caps = MALLOC_CAP_SPIRAM,
                      .number_of_urbs = 3, .urb_size = 0 },
    };
    uvc_host_stream_hdl_t hdl = NULL;
    xSemaphoreGive(s_lock);               /* frame_cb takes it: never hold it across open/start */
    esp_err_t err = uvc_host_stream_open(&cfg, pdMS_TO_TICKS(3000), &hdl);
    if (err == ESP_OK) {
        s_frames = 0; s_fps_cnt = 0; s_fps_x10 = 0; s_fps_t0 = esp_timer_get_time();
        err = uvc_host_stream_start(hdl);
        if (err != ESP_OK) uvc_host_stream_close(hdl);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (err == ESP_OK) {
        s_stream = hdl;
        s_w = s_list[best].h_res;
        s_h = s_list[best].v_res;
        s_fmt = 1;
        ESP_LOGI(TAG, "streaming MJPEG %ux%u", s_w, s_h);
    } else {
        ESP_LOGE(TAG, "stream %ux%u failed: %s", s_list[best].h_res, s_list[best].v_res, esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
    return err;
}

void lx_cam_state(lx_cam_state_t *o)
{
    memset(o, 0, sizeof *o);
    o->connected = s_connected;
    o->streaming = s_stream != NULL;
    o->width = s_w;
    o->height = s_h;
    o->format = s_fmt;
    o->frames = s_frames;
    o->fps_x10 = s_fps_x10;
}

int lx_cam_jpeg(void *buf, uint32_t max, uint32_t *seq, int timeout_ms)
{
    if (!s_lock) return 0;
    int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        if (s_seq != *seq && s_jpg_len) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            uint32_t len = s_jpg_len;
            int r;
            if (len > max) r = -(int)len;
            else { memcpy(buf, s_jpg, len); r = (int)len; }
            *seq = s_seq;
            xSemaphoreGive(s_lock);
            return r;
        }
        if (esp_timer_get_time() >= end || !s_stream) return 0;
        vTaskDelay(pdMS_TO_TICKS(4));
    }
}

int lx_cam_frame(uint16_t *dst, int w, int h, int stride, uint32_t *seq, int timeout_ms)
{
    if (!s_lock || !dst || w <= 0 || h <= 0) return -1;
    xSemaphoreTake(s_dec_lock, portMAX_DELAY);
    int ret = -1;
    if (!s_dec) {
        jpeg_decode_engine_cfg_t ec = { .intr_priority = 0, .timeout_ms = 200 };
        if (jpeg_new_decoder_engine(&ec, &s_dec) != ESP_OK) { s_dec = NULL; goto out; }
    }
    /* copy the newest frame into the decoder's input buffer */
    int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    uint32_t len = 0;
    for (;;) {
        if (s_seq != *seq && s_jpg_len) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            len = s_jpg_len;
            if (len > s_dec_in_cap) {
                if (s_dec_in) free(s_dec_in);
                jpeg_decode_memory_alloc_cfg_t ac = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
                size_t got = 0;
                s_dec_in = jpeg_alloc_decoder_mem(len + 64 * 1024, &ac, &got);
                s_dec_in_cap = s_dec_in ? got : 0;
            }
            if (len <= s_dec_in_cap) memcpy(s_dec_in, s_jpg, len);
            else len = 0;
            *seq = s_seq;
            xSemaphoreGive(s_lock);
            break;
        }
        if (esp_timer_get_time() >= end || !s_stream) { ret = 0; goto out; }
        vTaskDelay(pdMS_TO_TICKS(4));
    }
    if (!len) goto out;
    jpeg_decode_picture_info_t pi;
    if (jpeg_decoder_get_info(s_dec_in, len, &pi) != ESP_OK || !pi.width || !pi.height) goto out;
    uint32_t pw = (pi.width + 15) & ~15u, ph = (pi.height + 15) & ~15u;
    size_t need = (size_t)pw * ph * 2;
    if (need > s_dec_out_cap) {
        if (s_dec_out) free(s_dec_out);
        jpeg_decode_memory_alloc_cfg_t ac = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
        size_t got = 0;
        s_dec_out = jpeg_alloc_decoder_mem(need, &ac, &got);
        s_dec_out_cap = s_dec_out ? got : 0;
        if (!s_dec_out) goto out;
    }
    jpeg_decode_cfg_t dc = { .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
                             .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
                             .conv_std = JPEG_YUV_RGB_CONV_STD_BT601 };
    uint32_t out_size = 0;
    if (jpeg_decoder_process(s_dec, &dc, s_dec_in, len, s_dec_out, s_dec_out_cap, &out_size) != ESP_OK) goto out;
    /* nearest-neighbour scale into the app's buffer */
    const uint16_t *src = (const uint16_t *)s_dec_out;
    uint32_t sw = out_size >= pw * ph * 2 ? pw : pi.width;
    uint32_t xs = (pi.width << 16) / (uint32_t)w, ys = (pi.height << 16) / (uint32_t)h;
    for (int y = 0; y < h; y++) {
        const uint16_t *row = src + ((y * ys) >> 16) * sw;
        uint16_t *d = dst + (size_t)y * stride;
        uint32_t fx = 0;
        for (int x = 0; x < w; x++, fx += xs) d[x] = row[fx >> 16];
    }
    ret = 1;
out:
    xSemaphoreGive(s_dec_lock);
    return ret;
}


/* ── USB log capture: every log line of the USB host stack goes to /sd/launcher/usblog.txt ── */
#include <stdarg.h>
#include "esp_log.h"
static vprintf_like_t s_prev_vprintf;
static char s_logbuf[8192];
static volatile size_t s_loglen;
static portMUX_TYPE s_logmux = portMUX_INITIALIZER_UNLOCKED;

static int log_hook(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    char line[200];
    int n = vsnprintf(line, sizeof line, fmt, ap2);
    va_end(ap2);
    if (n > 0 && (strstr(line, "USB") || strstr(line, "usb") || strstr(line, "HUB") || strstr(line, "ENUM") ||
                  strstr(line, "HCD") || strstr(line, "uvc") || strstr(line, "UVC") || strstr(line, "lx_cam") || strstr(line, "lx_hid"))) {
        if (n > (int)sizeof line - 1) n = sizeof line - 1;
        portENTER_CRITICAL(&s_logmux);
        if (s_loglen + n < sizeof s_logbuf) { memcpy(s_logbuf + s_loglen, line, n); s_loglen += n; }
        portEXIT_CRITICAL(&s_logmux);
    }
    return s_prev_vprintf ? s_prev_vprintf(fmt, ap) : n;
}

static void log_flush(void)
{
    static char out[sizeof s_logbuf];
    size_t n;
    portENTER_CRITICAL(&s_logmux);
    n = s_loglen;
    memcpy(out, s_logbuf, n);
    s_loglen = 0;
    portEXIT_CRITICAL(&s_logmux);
    if (!n) return;
    FILE *f = fopen("/sd/launcher/usblog.txt", "a");
    if (f) { fwrite(out, 1, n, f); fclose(f); }
}

/* ── every USB device that connects is listed in /sd/launcher/usb.txt (diagnostics) ── */
#include "usb/usb_host.h"
static usb_host_client_handle_t s_diag;
static QueueHandle_t s_diag_q;

static void diag_cb(const usb_host_client_event_msg_t *m, void *arg)
{
    (void)arg;
    if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV) { uint8_t a = m->new_dev.address; xQueueSend(s_diag_q, &a, 0); }
}

static void diag_task(void *arg)
{
    (void)arg;
    for (;;) {
        usb_host_client_handle_events(s_diag, pdMS_TO_TICKS(100));
        static int tick;
        if (++tick % 20 == 0) {
            log_flush();
            if (tick % 100 == 0) {
                usb_host_lib_info_t li;
                static int last_dev = -1;
                if (usb_host_lib_info(&li) == ESP_OK && li.num_devices != last_dev) {
                    last_dev = li.num_devices;
                    FILE *f = fopen("/sd/launcher/usblog.txt", "a");
                    if (f) { fprintf(f, "[t=%ds] devices=%d clients=%d%c", tick / 10, li.num_devices, li.num_clients, 10); fclose(f); }
                }
            }
        }
        uint8_t addr;
        while (xQueueReceive(s_diag_q, &addr, 0) == pdTRUE) {
            usb_device_handle_t dev;
            if (usb_host_device_open(s_diag, addr, &dev) != ESP_OK) continue;
            const usb_device_desc_t *dd = NULL;
            const usb_config_desc_t *cd = NULL;
            usb_device_info_t info;
            usb_host_device_info(dev, &info);
            usb_host_get_device_descriptor(dev, &dd);
            usb_host_get_active_config_descriptor(dev, &cd);
            FILE *f = fopen("/sd/launcher/usb.txt", "a");
            if (f && dd) {
                fprintf(f, "addr=%u speed=%s vid=%04x pid=%04x class=%02x/%02x/%02x\n", addr,
                        info.speed == USB_SPEED_HIGH ? "high" : info.speed == USB_SPEED_FULL ? "full" : "low",
                        dd->idVendor, dd->idProduct, dd->bDeviceClass, dd->bDeviceSubClass, dd->bDeviceProtocol);
                if (cd) {
                    int off = 0;
                    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cd;
                    while ((d = usb_parse_next_descriptor_of_type(d, cd->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &off)))
                        fprintf(f, "  intf %u alt %u class=%02x/%02x eps=%u\n", ((const usb_intf_desc_t *)d)->bInterfaceNumber,
                                ((const usb_intf_desc_t *)d)->bAlternateSetting, ((const usb_intf_desc_t *)d)->bInterfaceClass,
                                ((const usb_intf_desc_t *)d)->bInterfaceSubClass, ((const usb_intf_desc_t *)d)->bNumEndpoints);
                }
            }
            if (f) fclose(f);
            usb_host_device_close(s_diag, dev);
        }
    }
}

static void diag_start(void)
{
    s_diag_q = xQueueCreate(8, 1);
    const usb_host_client_config_t cc = { .is_synchronous = false, .max_num_event_msg = 5,
                                          .async = { .client_event_callback = diag_cb, .callback_arg = NULL } };
    if (s_diag_q && usb_host_client_register(&cc, &s_diag) == ESP_OK)
        xTaskCreatePinnedToCore(diag_task, "lx_usbdiag", 4096, NULL, 3, NULL, 1);
}

/* ── probe: frame list + a short test stream, written to the card ───────── */
static void write_status(const char *probe)
{
    FILE *f = fopen(STATUS_FILE, "w");
    if (!f) return;
    fprintf(f, "connected=%d addr=%u stream=%u formats=%u\n", s_connected, s_addr, s_idx, (unsigned)s_list_n);
    for (size_t i = 0; i < s_list_n; i++) {
        const uvc_host_frame_info_t *fi = &s_list[i];
        fprintf(f, "%s %ux%u default=%.1ffps\n", fmt_name(fi->format), fi->h_res, fi->v_res,
                fi->default_interval ? 10000000.0f / fi->default_interval : 0.0f);
    }
    if (probe) fputs(probe, f);
    fclose(f);
}

static void scan_existing(void)
{
    uint8_t addrs[8];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof addrs, addrs, &n) != ESP_OK) return;
    for (int i = 0; i < n; i++) {
        if (s_diag_q) xQueueSend(s_diag_q, &addrs[i], 0);
        size_t cnt = MAX_FRAMES;
        if (uvc_host_get_frame_list(addrs[i], 0, (uvc_host_frame_info_t (*)[])&s_list, &cnt) == ESP_OK && cnt) {
            cam_ev_t ev = { .type = EV_CONNECTED, .addr = addrs[i], .idx = 0 };
            xQueueSend(s_evq, &ev, 0);
        }
    }
}

static void cam_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2500));        /* a camera plugged in at power-on enumerated before we listened */
    if (!s_connected) scan_existing();
    for (;;) {
        cam_ev_t ev;
        if (xQueueReceive(s_evq, &ev, portMAX_DELAY) != pdTRUE) continue;
        if (ev.type == EV_CONNECTED && s_connected && ev.addr == s_addr) continue;   /* seen by the startup scan */
        if (ev.type == EV_CONNECTED) {
            s_addr = ev.addr;
            s_idx = ev.idx;
            size_t n = MAX_FRAMES;
            if (uvc_host_get_frame_list(s_addr, s_idx, (uvc_host_frame_info_t (*)[])&s_list, &n) != ESP_OK) n = 0;
            s_list_n = n;
            s_connected = true;
            ESP_LOGI(TAG, "camera connected (addr %u, %u formats)", s_addr, (unsigned)n);
            write_status("probe=running\n");
            /* a 3 s test stream, unless an app already opened one */
            if (!s_stream && lx_cam_open(640, 480, 0) == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(3000));
                char line[160];
                snprintf(line, sizeof line, "probe=ok %ux%u frames=%lu fps=%lu.%lu last_jpeg=%lu bytes\n",
                         s_w, s_h, (unsigned long)s_frames, (unsigned long)(s_fps_x10 / 10), (unsigned long)(s_fps_x10 % 10),
                         (unsigned long)s_jpg_len);
                write_status(line);
                /* one frame on the card, to look at */
                static uint8_t *snap;
                if (!snap) snap = heap_caps_malloc(512 * 1024, MALLOC_CAP_SPIRAM);
                uint32_t sq = 0;
                int n = snap ? lx_cam_jpeg(snap, 512 * 1024, &sq, 500) : 0;
                if (n > 0) { FILE *f = fopen("/sd/launcher/camera.jpg", "wb"); if (f) { fwrite(snap, 1, n, f); fclose(f); } }
                lx_cam_close();
            } else if (!s_stream) {
                write_status("probe=failed (no MJPEG stream)\n");
            }
        } else if (ev.type == EV_DISCONNECTED) {
            ESP_LOGI(TAG, "camera disconnected");
            lx_cam_close();
            s_connected = false;
            s_list_n = 0;
            write_status("disconnected\n");
        }
    }
}


esp_err_t lx_cam_start(void)
{
    remove("/sd/launcher/usblog.txt");
    s_prev_vprintf = esp_log_set_vprintf(log_hook);
    esp_log_level_set("*", ESP_LOG_INFO);
    s_evq = xQueueCreate(4, sizeof(cam_ev_t));
    s_lock = xSemaphoreCreateMutex();
    s_dec_lock = xSemaphoreCreateMutex();
    if (!s_evq || !s_lock || !s_dec_lock) return ESP_ERR_NO_MEM;
    const uvc_host_driver_config_t cfg = {
        .driver_task_stack_size = 6 * 1024,
        .driver_task_priority = 6,
        .xCoreID = 0,
        .create_background_task = true,
        .event_cb = driver_cb,
        .user_ctx = NULL,
    };
    esp_err_t err = uvc_host_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UVC host install failed: %s", esp_err_to_name(err));
        return err;
    }
    xTaskCreatePinnedToCore(cam_task, "lx_cam", 6 * 1024, NULL, 4, NULL, 1);
    diag_start();
    ESP_LOGI(TAG, "USB camera (UVC) host ready");
    return ESP_OK;
}
