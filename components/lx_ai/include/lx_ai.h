#pragma once
/* lx_ai: runs one ESP-DL model (.espdl from the SD card) for apps.
 * The app hands over an RGB565 picture; the launcher resizes it (letterbox or stretch) into the
 * model's int8 input, runs the model on the P4 and gives the outputs back as floats. */
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int     ndim;
    int     dims[4];
    int     exponent;          /* real = int * 2^exponent */
    int     dtype;             /* 1 float, 3 int8, 5 int16 */
} lx_ai_shape_t;

#define LX_AI_SLOTS 4
/* models live in slots 0..3; every call works on the selected slot (default 0) */
int       lx_ai_select(int slot);                       /* returns the previous slot, -1 = bad slot */
void      lx_ai_unload_all(void);
esp_err_t lx_ai_load(const char *path);                 /* replaces the model in the selected slot */
void      lx_ai_unload(void);
int       lx_ai_input(lx_ai_shape_t *out);              /* 0 = ok */
int       lx_ai_outputs(void);                          /* number of outputs */
int       lx_ai_output(int idx, lx_ai_shape_t *out);
/* picture -> input: v = pixel(0..255) * scale + offset per channel (RGB order);
 * letterbox: keep the aspect ratio, pad with grey (114). Returns the time in us, < 0 on error */
int64_t   lx_ai_run(const uint16_t *img, int w, int h, int stride, float scale, float offset, int letterbox);
/* dequantized copy of an output; returns the element count copied */
int       lx_ai_read(int idx, float *dst, int max);

#ifdef __cplusplus
}
#endif
