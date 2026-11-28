/* TTY service: the USB-C serial port as a byte stream for apps (PAPP_IF_TTY).
 * The port belongs to DevLink, which registers its pass-through functions with
 * lx_app_set_tty(); the port an app opened is handed back at teardown. */
#include "lx_services.h"

static const lx_tty_ops_t *s_ops;
static bool s_open;

void lx_app_set_tty(const lx_tty_ops_t *ops) { s_ops = ops; }

static int tty_open(void)
{
    if (!s_ops) return PAPP_E_UNSUPPORTED;
    SVC_ENTER();
    int r = s_ops->open();
    if (r == 0) s_open = true;
    SVC_LEAVE();
    return r;
}

static void tty_close(void)
{
    if (!s_ops || !s_open) return;
    SVC_ENTER();
    s_ops->close();
    s_open = false;
    SVC_LEAVE();
}

static int tty_read(void *buf, size_t max, uint32_t timeout_ms)
{
    if (!s_ops || !s_open || !buf) return PAPP_E_ARG;
    if (lx_svc_exit_flag) return PAPP_E_EXIT;
    if (timeout_ms > 1000) timeout_ms = 1000;
    SVC_ENTER();
    int r = s_ops->read(buf, max, timeout_ms);
    SVC_LEAVE();
    return r;
}

static int tty_write(const void *buf, size_t len)
{
    if (!s_ops || !s_open || !buf) return PAPP_E_ARG;
    SVC_ENTER();
    int r = s_ops->write(buf, len);
    SVC_LEAVE();
    return r;
}

static int tty_connected(void)
{
    return s_ops ? s_ops->connected() : 0;
}

void lx_svc_tty_end_run(void)
{
    if (s_open && s_ops) s_ops->close();
    s_open = false;
}

const papp_if_tty_t lx_if_tty = {
    .hdr = { PAPP_IF_TTY, PAPP_IF_TTY_VER, sizeof(papp_if_tty_t) },
    .open = tty_open,
    .close = tty_close,
    .read = tty_read,
    .write = tty_write,
    .connected = tty_connected,
};
