/* ══ hands / face: detector + landmark model (launcher 0.32+: AI v2 slots) ════════════════
 * Included by main.c (uses its drawing helpers and the inference state).
 *   Finger Counter    task=hands : MediaPipe palm detector (192) + hand landmarks (224, 21 points)
 *   Driver Drowsiness task=face  : MediaPipe face detector (128) + face mesh (256, 478 points), eyes EAR
 * Files: <name>.espdl = detector, <name>.lm = landmark model (both ESP-DL int8), <name>.txt.
 * Same steps as MediaPipe: detect -> rotated square crop -> landmarks. The next frame crops from the
 * landmarks (tracking), the detector runs again only when a hand / the face is lost. */

#define TASK_HANDS 2
#define TASK_FACE  3
#define MAXP   2                        /* hands at once (face: 1) */
#define MAXPTS 478
#define PI_F   3.14159265f

typedef struct { float cx, cy, size, rot; } roi_t;
typedef struct {
    int   n;
    float pt[MAXP][MAXPTS][2];          /* frame pixels */
    int   up[MAXP];                     /* hands: fingers up on each hand */
    float ear;                          /* face: eye aspect ratio (both eyes, mean) */
    int64_t t;                          /* when the frame was taken */
} pose_res_t;

static pose_res_t pres[2];
static volatile int pres_i;
static roi_t roi[MAXP];
static int n_roi, det_skip;
static uint16_t crop[256 * 256];
static float lmbuf[1434 + 16];
static float *dout[8];
static int dsz[8], dn;
static papp_ai_shape_t dsh[8];
static int det_S, lm_S, lm_out, lm_flag, pose_npts, flag_logit;
static volatile int64_t pose_frame_t;

static float wrap_pi(float a)
{
    while (a > PI_F) a -= 2 * PI_F;
    while (a < -PI_F) a += 2 * PI_F;
    return a;
}

static void ai_unload_all(void)
{
    if (!ai) return;
    if (ai->hdr.version >= 2) { ai->select(1); ai->unload(); ai->select(0); }
    ai->unload();
}

/* loads <name>.lm into slot 1 and the detector into slot 0. 0 ok, -2 launcher too old, -1 error */
static int pose_load(void)
{
    if (ai->hdr.version < 2) return -2;
    char lm[110];
    snprintf(lm, sizeof lm, "%.*s.lm", (int)strlen(cur->file) - 6, cur->file);
    papp_ai_shape_t is, s;
    ai->select(1);
    if (ai->load(lm) < 0 || ai->input(&is) < 0) { ai->select(0); return -1; }
    lm_S = is.dims[1];
    lm_out = lm_flag = -1;
    for (int o = 0; o < ai->outputs(); o++) {        /* outputs sorted by name: points, presence, ... */
        ai->output(o, &s);
        int n = 1;
        for (int i = 0; i < s.ndim; i++) n *= s.dims[i];
        if (n >= 63) { if (lm_out < 0) lm_out = o; }
        else if (lm_flag < 0) lm_flag = o;
    }
    ai->select(0);
    if (lm_out < 0 || ai->load(cur->file) < 0 || ai->input(&is) < 0) return -1;
    det_S = is.dims[1];
    dn = ai->outputs();
    if (dn > 8) dn = 8;
    for (int o = 0; o < 8; o++) { free(dout[o]); dout[o] = NULL; }
    for (int o = 0; o < dn; o++) {
        ai->output(o, &dsh[o]);
        dsz[o] = 1;
        for (int i = 0; i < dsh[o].ndim; i++) dsz[o] *= dsh[o].dims[i];
        dout[o] = malloc(sizeof(float) * dsz[o]);
    }
    pose_npts = cur->task == TASK_HANDS ? 21 : 478;
    flag_logit = cur->task == TASK_FACE;              /* face mesh gives a logit, hands a probability */
    n_roi = 0; det_skip = 0;
    memset(pres, 0, sizeof pres);
    return 0;
}

/* square ROI around a box, MediaPipe RectTransformation (shift in the rotated frame) */
static roi_t make_roi(float cx, float cy, float w, float h, float rot, float scale, float shift_y)
{
    roi_t r;
    r.rot = rot;
    r.cx = cx - h * shift_y * sinf(rot);
    r.cy = cy + h * shift_y * cosf(rot);
    r.size = fmaxf(w, h) * scale;
    return r;
}

/* detector on the whole frame -> ROIs (frame pixels) */
static int pose_detect(roi_t *out, int maxn)
{
    const int R = cur->task == TASK_HANDS ? 18 : 16;     /* box 4 + keypoints */
    typedef struct { float score, cx, cy, w, h, ax, ay, bx, by; } cand_t;
    cand_t c[16];
    int nc = 0;
    float lmin = logf(cur->conf / (1.0f - cur->conf));
    for (int o = 0; o < dn; o++) {
        papp_ai_shape_t *s = &dsh[o];
        if (s->ndim != 4 || s->dims[3] > 8) continue;      /* classifier map [1,G,G,anchors] */
        int G = s->dims[1], A = s->dims[3], b = -1;
        for (int q = 0; q < dn; q++)
            if (dsh[q].ndim == 4 && dsh[q].dims[1] == G && dsh[q].dims[3] == A * R) b = q;
        if (b < 0) continue;
        ai->read(o, dout[o], dsz[o]);
        bool rd = false;
        for (int i = 0; i < G * G * A; i++) {
            if (dout[o][i] < lmin) continue;
            if (!rd) { ai->read(b, dout[b], dsz[b]); rd = true; }
            int cell = i / A;
            float ax = (cell % G + 0.5f) / G * det_S, ay = (cell / G + 0.5f) / G * det_S;
            const float *g = &dout[b][i * R];
            int k2 = cur->task == TASK_HANDS ? 8 : 6;      /* hands: wrist -> middle finger; face: right -> left eye */
            cand_t d = { sigm(dout[o][i]), ax + g[0], ay + g[1], g[2], g[3], ax + g[4], ay + g[5], ax + g[k2], ay + g[k2 + 1] };
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
    /* undo the letterbox exactly like lx_ai_run */
    float r = fminf((float)det_S / W, (float)det_S / H);
    int ox = (det_S - (int)lroundf(W * r)) / 2, oy = (det_S - (int)lroundf(H * r)) / 2;
    int n = 0;
    for (int a = 0; a < nc && n < maxn; a++) {
        bool keep = true;
        for (int k = 0; k < a && keep; k++) {               /* simple NMS on the centres */
            float dx = c[a].cx - c[k].cx, dy = c[a].cy - c[k].cy;
            if (dx * dx + dy * dy < 0.25f * c[k].w * c[k].w) keep = false;
        }
        if (!keep) continue;
        float cx = (c[a].cx - ox) / r, cy = (c[a].cy - oy) / r, w = c[a].w / r, h = c[a].h / r;
        float vx = c[a].bx - c[a].ax, vy = c[a].by - c[a].ay;
        if (cur->task == TASK_HANDS)
            out[n++] = make_roi(cx, cy, w, h, wrap_pi(PI_F / 2 - atan2f(-vy, vx)), 2.6f, -0.5f);
        else
            out[n++] = make_roi(cx, cy, w, h, wrap_pi(-atan2f(-vy, vx)), 1.5f, 0.0f);
    }
    return n;
}

/* rotated square crop -> landmark model -> points in frame pixels; false = no hand / face */
static bool pose_landmarks(const roi_t *ro, float (*pt)[2])
{
    const int S = lm_S;
    float c = cosf(ro->rot), s = sinf(ro->rot), step = ro->size / S;
    for (int v = 0; v < S; v++) {
        float ly = (v + 0.5f) * step - ro->size / 2;
        float bx = ro->cx - ly * s, by = ro->cy + ly * c;      /* row start, then step along the rotated x */
        float lx = 0.5f * step - ro->size / 2;
        float fx = bx + lx * c, fy = by + lx * s, dx = step * c, dy = step * s;
        uint16_t *d = &crop[v * S];
        for (int u = 0; u < S; u++, fx += dx, fy += dy) {
            int X = (int)fx, Y = (int)fy;
            d[u] = (X >= 0 && X < W && Y >= 0 && Y < H) ? ai_frame[Y * W + X] : 0;
        }
    }
    ai->select(1);
    bool ok = false;
    if (ai->run(crop, S, S, S, 1.0f / 255.0f, 0.0f, 0) >= 0) {
        float f = 0;
        if (lm_flag >= 0) ai->read(lm_flag, &f, 1);
        if (flag_logit) f = sigm(f);
        if (lm_flag < 0 || f >= 0.5f) {
            int n = ai->read(lm_out, lmbuf, (int)(sizeof lmbuf / sizeof lmbuf[0]));
            if (n >= pose_npts * 3) {
                for (int i = 0; i < pose_npts; i++) {
                    float lx = (lmbuf[i * 3] / S - 0.5f) * ro->size, ly = (lmbuf[i * 3 + 1] / S - 0.5f) * ro->size;
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

/* next frame's ROI from the points (MediaPipe *_landmarks_to_roi) */
static bool roi_from_points(float (*pt)[2], roi_t *out)
{
    static const int HAND_IDX[] = { 0, 1, 2, 3, 5, 6, 9, 10, 13, 14, 17, 18 };
    const bool hand = cur->task == TASK_HANDS;
    int n = hand ? 12 : pose_npts;
    float rot = hand ? wrap_pi(PI_F / 2 - atan2f(-(pt[9][1] - pt[0][1]), pt[9][0] - pt[0][0]))
                     : wrap_pi(-atan2f(-(pt[263][1] - pt[33][1]), pt[263][0] - pt[33][0]));
    float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f;
    for (int i = 0; i < n; i++) {
        const float *p = pt[hand ? HAND_IDX[i] : i];
        mnx = fminf(mnx, p[0]); mxx = fmaxf(mxx, p[0]); mny = fminf(mny, p[1]); mxy = fmaxf(mxy, p[1]);
    }
    float cx0 = (mnx + mxx) / 2, cy0 = (mny + mxy) / 2, c = cosf(rot), s = sinf(rot);
    float a0 = 1e9f, b0 = 1e9f, a1 = -1e9f, b1 = -1e9f;
    for (int i = 0; i < n; i++) {                            /* extents in the rotated frame */
        const float *p = pt[hand ? HAND_IDX[i] : i];
        float dx = p[0] - cx0, dy = p[1] - cy0, a = dx * c + dy * s, b = -dx * s + dy * c;
        a0 = fminf(a0, a); a1 = fmaxf(a1, a); b0 = fminf(b0, b); b1 = fmaxf(b1, b);
    }
    float ma = (a0 + a1) / 2, mb = (b0 + b1) / 2;
    float cx = cx0 + ma * c - mb * s, cy = cy0 + ma * s + mb * c;
    *out = hand ? make_roi(cx, cy, a1 - a0, b1 - b0, rot, 2.0f, -0.1f) : make_roi(cx, cy, a1 - a0, b1 - b0, rot, 1.5f, 0.0f);
    return out->size > 24 && out->size < 4 * W;
}

static float pdist(const float *a, const float *b) { return hypotf(a[0] - b[0], a[1] - b[1]); }

/* the Finger Count project's rule: [thumb, index, middle, ring, pinky] */
static int fingers_up(float (*p)[2])
{
    static const int TIPS[] = { 8, 12, 16, 20 }, PIPS[] = { 6, 10, 14, 18 };
    int n = 0;
    for (int i = 0; i < 4; i++) n += pdist(p[TIPS[i]], p[0]) > pdist(p[PIPS[i]], p[0]) * 1.1f;
    float palm = pdist(p[0], p[9]);
    n += pdist(p[4], p[17]) > pdist(p[3], p[17]) && pdist(p[4], p[5]) > palm * 0.45f;
    return n;
}

/* the Driver Drowsiness project's EAR: eye height / eye width, both eyes */
static float eye_ear(float (*p)[2])
{
    static const int E[2][6] = { { 33, 160, 158, 133, 153, 144 }, { 362, 385, 387, 263, 373, 380 } };
    float sum = 0;
    for (int e = 0; e < 2; e++) {
        const int *k = E[e];
        float wdt = pdist(p[k[0]], p[k[3]]);
        sum += wdt > 1 ? (pdist(p[k[1]], p[k[5]]) + pdist(p[k[2]], p[k[4]])) / (2 * wdt) : 0;
    }
    return sum / 2;
}

static int pose_thread(void *arg)
{
    (void)arg;
    const int maxn = cur->task == TASK_HANDS ? MAXP : 1;
    while (ai_run_flag) {
        if (ai_state != 1) { papp_delay_ms(3); continue; }
        ai_state = 2;
        int64_t t0 = papp_time_us();
        /* detector: nothing tracked, or now and then while a second hand could appear */
        if (n_roi == 0 || (n_roi < maxn && ++det_skip >= 6)) {
            det_skip = 0;
            if (ai->run(ai_frame, W, H, W, cur->scale, cur->offset, 1) >= 0) {
                roi_t found[MAXP];
                int nf = pose_detect(found, maxn);
                for (int i = 0; i < nf && n_roi < maxn; i++) {
                    bool dup = false;                        /* already tracked? */
                    for (int k = 0; k < n_roi; k++)
                        if (hypotf(found[i].cx - roi[k].cx, found[i].cy - roi[k].cy) < roi[k].size * 0.4f) dup = true;
                    if (!dup) roi[n_roi++] = found[i];
                }
            }
        }
        int nxt = pres_i ^ 1, k = 0;
        pose_res_t *pr = &pres[nxt];
        roi_t keep[MAXP];
        for (int i = 0; i < n_roi; i++) {
            if (!pose_landmarks(&roi[i], pr->pt[k])) continue;
            if (cur->task == TASK_HANDS) pr->up[k] = fingers_up(pr->pt[k]);
            else pr->ear = eye_ear(pr->pt[k]);
            if (roi_from_points(pr->pt[k], &keep[k])) k++;
        }
        for (int i = 0; i < k; i++) roi[i] = keep[i];
        n_roi = k;
        pr->n = k;
        pr->t = pose_frame_t;
        pres_i = nxt;
        ai_us = papp_time_us() - t0;
        ai_state = 0;
    }
    return 0;
}

/* ── drawing ── */
static void dot(float x, float y, int r, uint16_t c) { rect((int)x - r, (int)y - r, 2 * r + 1, 2 * r + 1, c, 32); }

static void line(const float *a, const float *b, uint16_t c)
{
    float dx = b[0] - a[0], dy = b[1] - a[1];
    int n = (int)(fmaxf(fabsf(dx), fabsf(dy)) / 2) + 1;
    for (int i = 0; i <= n; i++) dot(a[0] + dx * i / n, a[1] + dy * i / n, 1, c);
}

static int64_t eyes_closed_since;
static int alarm_on;

/* draws the points, writes the bottom label */
static void pose_draw(char *lab, int labn, uint16_t *lab_col)
{
    static const uint8_t BONES[][2] = { {0,1},{1,2},{2,3},{3,4},{0,5},{5,6},{6,7},{7,8},{5,9},{9,10},{10,11},{11,12},
                                        {9,13},{13,14},{14,15},{15,16},{13,17},{17,18},{18,19},{19,20},{0,17} };
    const pose_res_t *pr = &pres[pres_i];
    if (cur->task == TASK_HANDS) {
        int total = 0;
        for (int h = 0; h < pr->n; h++) {
            uint16_t c = h ? PAPP_RGB565(255, 120, 220) : PAPP_RGB565(90, 255, 160);
            for (unsigned b = 0; b < sizeof BONES / 2; b++) line(pr->pt[h][BONES[b][0]], pr->pt[h][BONES[b][1]], c);
            for (int i = 0; i < 21; i++) dot(pr->pt[h][i][0], pr->pt[h][i][1], 3, 0xFFFF);
            total += pr->up[h];
        }
        if (pr->n) snprintf(lab, labn, "FINGERS: %d", total);
        else snprintf(lab, labn, "SHOW YOUR HAND");
        *lab_col = pr->n ? PAPP_RGB565(90, 255, 160) : PAPP_RGB565(200, 200, 200);
        return;
    }
    /* face: eyes + EAR + the closed-eyes timer */
    static const int EYES[] = { 33, 160, 158, 133, 153, 144, 362, 385, 387, 263, 373, 380 };
    int64_t now = papp_time_us();
    bool closed = false;
    if (pr->n) {
        closed = pr->ear < cur->closed;
        for (int i = 0; i < pose_npts; i += 6) dot(pr->pt[0][i][0], pr->pt[0][i][1], 1, PAPP_RGB565(120, 200, 255));
        for (unsigned i = 0; i < sizeof EYES / sizeof EYES[0]; i++)
            dot(pr->pt[0][EYES[i]][0], pr->pt[0][EYES[i]][1], 3, closed ? PAPP_RGB565(255, 80, 80) : PAPP_RGB565(90, 255, 160));
    }
    if (closed) { if (!eyes_closed_since) eyes_closed_since = now; }
    else { eyes_closed_since = 0; alarm_on = 0; }
    float secs = eyes_closed_since ? (now - eyes_closed_since) / 1e6f : 0;
    if (secs >= cur->seconds) alarm_on = 1;
    if (alarm_on && (now / 300000) % 2) {
        rect(0, 0, W, H, PAPP_RGB565(255, 0, 0), 12);
        text_c(200, "WAKE UP!", 0xFFFF, 8);
    }
    int ear100 = (int)(pr->ear * 100 + 0.5f);
    if (!pr->n) snprintf(lab, labn, "NO FACE");
    else if (closed) snprintf(lab, labn, "CLOSED %d.%dS  EAR 0.%02d", (int)secs, (int)(secs * 10) % 10, ear100);
    else snprintf(lab, labn, "EYES OPEN  EAR 0.%02d", ear100);
    *lab_col = !pr->n ? PAPP_RGB565(200, 200, 200) : closed ? PAPP_RGB565(255, 90, 90) : PAPP_RGB565(90, 255, 160);
}
