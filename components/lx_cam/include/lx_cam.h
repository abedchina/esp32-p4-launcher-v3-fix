#pragma once
/* lx_cam: USB (UVC) camera on the board's USB host port.
 * The UVC class driver shares the USB host library installed by lx_input. A camera that plugs in
 * is probed (its frame list goes to /sd/launcher/camera.txt); apps open a stream through the CAM
 * service. MJPEG frames are kept as they arrive (the latest one) and decoded on request with the
 * P4's hardware JPEG decoder. */
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool     connected;
    bool     streaming;
    uint16_t width, height;          /* of the running stream (0 when not streaming) */
    uint8_t  format;                 /* 1 MJPEG, 2 YUY2 */
    uint32_t frames;                 /* frames received since the stream started */
    uint32_t fps_x10;                /* measured over the last second */
} lx_cam_state_t;

esp_err_t lx_cam_start(void);                                   /* after lx_input (USB host installed) */
void      lx_cam_state(lx_cam_state_t *out);
/* start streaming: the size closest to w x h (0 x 0 = 640x480), fps 0 = camera default */
esp_err_t lx_cam_open(int w, int h, int fps);
void      lx_cam_close(void);
/* the newest MJPEG frame whose sequence number is above *seq: copies up to max bytes, sets *seq.
 * Returns its length, 0 on timeout, < 0 when the frame does not fit (-length) */
int       lx_cam_jpeg(void *buf, uint32_t max, uint32_t *seq, int timeout_ms);
/* the newest frame decoded to RGB565 and scaled (nearest) into dst (w x h, stride in pixels).
 * Returns 1 = new frame, 0 = timeout, < 0 = error */
int       lx_cam_frame(uint16_t *dst, int w, int h, int stride, uint32_t *seq, int timeout_ms);
