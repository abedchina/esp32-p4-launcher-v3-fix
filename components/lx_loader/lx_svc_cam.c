/* CAM service: the USB camera (lx_cam) for apps. The stream an app opened is closed at teardown. */
#include <string.h>
#include "lx_services.h"
#include "lx_cam.h"

static bool s_opened;            /* this run opened the camera */

static uint32_t cap_wait(uint32_t ms) { return ms > 1000 ? 1000 : ms; }

static void cam_info(papp_cam_info_t *out)
{
    if (!out) return;
    SVC_ENTER();
    lx_cam_state_t st;
    lx_cam_state(&st);
    memset(out, 0, sizeof *out);
    out->connected = st.connected;
    out->streaming = st.streaming;
    out->format = st.format;
    out->width = st.width;
    out->height = st.height;
    out->frames = st.frames;
    out->fps_x10 = st.fps_x10;
    SVC_LEAVE();
}

static int cam_open(int w, int h, int fps)
{
    SVC_ENTER();
    esp_err_t e = lx_cam_open(w, h, fps);
    if (e == ESP_OK) s_opened = true;
    SVC_LEAVE();
    return e == ESP_OK ? PAPP_E_OK : e == ESP_ERR_NOT_FOUND ? PAPP_E_NOT_CONNECTED : PAPP_E_UNSUPPORTED;
}

static void cam_close(void)
{
    SVC_ENTER();
    lx_cam_close();
    s_opened = false;
    SVC_LEAVE();
}

static int cam_jpeg(void *buf, uint32_t max, uint32_t *seq, uint32_t timeout_ms)
{
    if (!buf || !seq) return PAPP_E_UNSUPPORTED;
    if (lx_svc_exit_flag) return PAPP_E_EXIT;
    SVC_ENTER();
    int r = lx_cam_jpeg(buf, max, seq, (int)cap_wait(timeout_ms));
    SVC_LEAVE();
    if (r < 0) return PAPP_E_NOMEM;
    return lx_svc_exit_flag && r == 0 ? PAPP_E_EXIT : r;
}

static int cam_frame(uint16_t *dst, int w, int h, int stride, uint32_t *seq, uint32_t timeout_ms)
{
    if (!dst || !seq) return PAPP_E_UNSUPPORTED;
    if (lx_svc_exit_flag) return PAPP_E_EXIT;
    SVC_ENTER();
    int r = lx_cam_frame(dst, w, h, stride, seq, (int)cap_wait(timeout_ms));
    SVC_LEAVE();
    if (r < 0) return PAPP_E_UNSUPPORTED;
    return lx_svc_exit_flag && r == 0 ? PAPP_E_EXIT : r;
}

void lx_svc_cam_end_run(void)
{
    if (s_opened) lx_cam_close();
    s_opened = false;
}

const papp_if_cam_t lx_if_cam = {
    .hdr = { PAPP_IF_CAM, PAPP_IF_CAM_VER, sizeof(papp_if_cam_t) },
    .info = cam_info,
    .open = cam_open,
    .close = cam_close,
    .jpeg = cam_jpeg,
    .frame = cam_frame,
};
