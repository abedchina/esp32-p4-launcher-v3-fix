"""MediaPipe .task (hand_landmarker / face_landmarker) -> ESP-DL .espdl models for the AI Models app.

    python convert_mediapipe.py hand <hand_landmarker.task> <yolo dataset dir with images/ + labels/> <out dir>
    python convert_mediapipe.py face <face_landmarker.task> <folder of face pictures> <out dir>

Each .task holds a detector + a landmark model. Both are converted to int8:
  detector  : cut before its Reshape/Concat tail -> raw per-stride maps (the app decodes the SSD anchors)
  landmarks : kept whole (points + presence score)
Writes finger_counter.* / driver_drowsiness.*: .espdl (detector), .lm (landmarks), .txt -> AI-Models / /sd/models.
Run with the Python 3.11 venv and PYTHONUTF8=1.
"""
import glob, os, random, subprocess, sys, zipfile
import numpy as np
import onnx, onnx.helper
from onnx import shape_inference, utils
from PIL import Image

CFG = {   # detector input, range scale/offset, landmark input
    'hand': dict(det='hand_detector.tflite', lm='hand_landmarks_detector.tflite', ds=192, dsc=1 / 255, dof=0.0, ls=224),
    'face': dict(det='face_detector.tflite', lm='face_landmarks_detector.tflite', ds=128, dsc=1 / 127.5, dof=-1.0, ls=256),
}


def to_onnx(tfl, out):
    import tensorflow as tf                                   # the input's real name (input_1, input_12, ...)
    name = tf.lite.Interpreter(model_content=open(tfl, 'rb').read()).get_input_details()[0]['name']
    subprocess.run([sys.executable, '-m', 'tf2onnx.convert', '--tflite', tfl, '--output', out, '--opset', '13',
                    '--inputs-as-nchw', name], check=True, capture_output=True)
    m = onnx.load(out)
    for n in m.graph.node:                                    # ESP-DL asserts on a Pad without an explicit mode
        if n.op_type == 'Pad' and not any(a.name == 'mode' for a in n.attribute):
            n.attribute.append(onnx.helper.make_attribute('mode', 'constant'))
    d = m.graph.input[0].type.tensor_type.shape.dim
    if d[0].dim_value == 0:
        d[0].dim_value = 1                                    # batch 1 (face landmarks has a free batch)
    for o in m.graph.output:
        o.type.tensor_type.shape.dim[0].dim_value = 1
    del m.graph.value_info[:]
    onnx.save(shape_inference.infer_shapes(m), out)
    return out


def cut_detector(src, dst):
    """outputs = the Conv maps that feed the Transpose->Reshape tail (regressors + classifiers per stride)"""
    m = shape_inference.infer_shapes(onnx.load(src))
    prod = {o: n for n in m.graph.node for o in n.output}
    outs = []
    for n in m.graph.node:
        if n.op_type == 'Reshape':
            t = n.input[0]
            while prod.get(t) is not None and prod[t].op_type == 'Transpose':
                t = prod[t].input[0]
            if prod.get(t) is not None and prod[t].op_type == 'Conv':
                outs.append(t)
    sh = {v.name: [d.dim_value for d in v.type.tensor_type.shape.dim] for v in m.graph.value_info}
    outs = sorted(set(outs), key=lambda t: (-sh[t][2], sh[t][1]))     # big stride map first, classifier first
    utils.extract_model(src, dst, [m.graph.input[0].name], outs)
    return [sh[t] for t in outs]


def keep_outputs(src, dst, names):
    m = onnx.load(src)
    utils.extract_model(src, dst, [m.graph.input[0].name], names)


EYES = [[33, 160, 158, 133, 153, 144], [362, 385, 387, 263, 373, 380]]   # p1..p6 of each eye (EAR)


def add_eye_outputs(path):
    """face mesh: the 478 points come out of one linear Conv with a big range (int8 step ~2 px, an eye is ~10 px).
    Add two Convs made of differences of its rows: EyeV = the 2 vertical eye vectors, EyeH = the eye width vector
    (x, y per eye). Exactly the same numbers in float, but with their own small int8 range -> a precise EAR."""
    m = onnx.load(path)
    prod = {o: n for n in m.graph.node for o in n.output}
    rs = [n for n in m.graph.node if n.op_type == 'Reshape' and n.output[0] == 'Identity'][0]
    conv = prod[rs.input[0]]
    init = {i.name: i for i in m.graph.initializer}
    Wt = onnx.numpy_helper.to_array(init[conv.input[1]]); bt = onnx.numpy_helper.to_array(init[conv.input[2]])
    rows = {'EyeV': [(e[a], e[b], c) for e in EYES for a, b in ((1, 5), (2, 4)) for c in (0, 1)],
            'EyeH': [(e[0], e[3], c) for e in EYES for c in (0, 1)]}
    for name, rr in rows.items():
        w = np.stack([Wt[a * 3 + c] - Wt[b * 3 + c] for a, b, c in rr]).astype(np.float32)
        b = np.array([bt[a * 3 + c] - bt[b * 3 + c] for a, b, c in rr], np.float32)
        m.graph.initializer.extend([onnx.numpy_helper.from_array(w, name + '_W'), onnx.numpy_helper.from_array(b, name + '_B')])
        n = onnx.helper.make_node('Conv', [conv.input[0], name + '_W', name + '_B'], [name], name=name + '_conv')
        n.attribute.extend(conv.attribute)
        m.graph.node.append(n)
        m.graph.output.append(onnx.helper.make_tensor_value_info(name, onnx.TensorProto.FLOAT, [1, len(rr), 1, 1]))
    onnx.save(m, path)


def ear_of(eyev, eyeh):
    v, h = np.asarray(eyev).ravel(), np.asarray(eyeh).ravel()
    e = [(np.hypot(v[k * 4], v[k * 4 + 1]) + np.hypot(v[k * 4 + 2], v[k * 4 + 3])) / (2 * max(1e-6, np.hypot(h[k * 2], h[k * 2 + 1])))
         for k in range(2)]
    return float(np.mean(e))


def letter(im, S):
    w, h = im.size; r = min(S / w, S / h)
    c = Image.new('RGB', (S, S), (114, 114, 114))
    c.paste(im.resize((round(w * r), round(h * r)), Image.BILINEAR), ((S - round(w * r)) // 2, (S - round(h * r)) // 2))
    return np.asarray(c, np.float32)


def quantize(onnx_path, out, samples, S, eq=True, calib='kl'):
    import torch
    from esp_ppq.api import espdl_quantize_onnx
    from esp_ppq.api.setting import QuantizationSettingFactory
    st = QuantizationSettingFactory.espdl_setting()
    st.quantize_activation_setting.calib_algorithm = calib       # kl clips rare large activations (a small object)
    if eq:                                                    # depthwise MobileNet-style nets lose a lot without it
        st.equalization = True
        st.equalization_setting.opt_level = 2
        st.equalization_setting.iterations = 4
        st.equalization_setting.value_threshold = 0.4
    from esp_ppq.executor import TorchExecutor
    data = [torch.from_numpy(s.transpose(2, 0, 1).copy()) for s in samples]
    loader = torch.utils.data.DataLoader(data, batch_size=1, shuffle=False)
    g = espdl_quantize_onnx(onnx_import_file=onnx_path, espdl_export_file=out, calib_dataloader=loader,
                            calib_steps=min(128, len(data)), input_shape=[1, 3, S, S], target='esp32p4', num_of_bits=8,
                            collate_fn=lambda b: b.to('cpu').reshape(1, 3, S, S), device='cpu', error_report=False, verbose=0,
                            setting=st)
    return TorchExecutor(graph=g, device='cpu')


def best_quantize(onnx_path, out, samples, S, score, label='model'):
    """quantize with both calibrations, keep the one whose int8 model scores best (score(ex) -> number).
    kl suits most layers, minmax keeps rare large activations (a small or thin object, a face score)."""
    res = {}
    for calib in ('minmax', 'kl'):
        ex = quantize(onnx_path, out, samples, S, calib=calib)
        res[calib] = (score(ex), ex)
        print(f'  {label} calibration {calib}: score {res[calib][0]:.3f}')
    best = max(res, key=lambda k: res[k][0])
    if best != 'kl':                                          # the file on disk is the last one (kl): redo the winner
        res[best] = (res[best][0], quantize(onnx_path, out, samples, S, calib=best))
    print(f'  {label}: using {best}')
    return res[best][1]


def compare(onnx_path, ex, samples, fn):
    import onnxruntime as ort, torch
    s = ort.InferenceSession(onnx_path, providers=['CPUExecutionProvider'])
    name = s.get_inputs()[0].name
    res = []
    for x in samples:
        x = x.transpose(2, 0, 1)[None].copy()
        f = s.run(None, {name: x})
        q = [t.numpy() for t in ex.forward(inputs=torch.from_numpy(x))]
        res.append(fn(f, q))
    return res


def main():
    kind, task, src, out = sys.argv[1:5]
    c = CFG[kind]
    os.makedirs(out, exist_ok=True)
    work = os.path.join(out, kind + '_work'); os.makedirs(work, exist_ok=True)
    zipfile.ZipFile(task).extractall(work)
    det = to_onnx(os.path.join(work, c['det']), os.path.join(work, 'det_full.onnx'))
    lm = to_onnx(os.path.join(work, c['lm']), os.path.join(work, 'lm_full.onnx'))
    det_cut = os.path.join(work, 'det.onnx'); shapes = cut_detector(det, det_cut)
    print('detector maps', shapes)
    m = onnx.load(lm)
    keep = [o.name for o in m.graph.output if [d.dim_value for d in o.type.tensor_type.shape.dim][-1] != 63 or o.name == 'Identity']
    lm_cut = os.path.join(work, 'lm.onnx'); keep_outputs(lm, lm_cut, keep)
    if kind == 'face':
        add_eye_outputs(lm_cut)                               # precise EAR outputs (EyeV, EyeH)
    print('landmark outputs', keep)

    random.seed(1)
    neg = []                                                  # crops WITHOUT a hand / face: else int8 presence never drops
    if kind == 'hand':                                        # pictures + boxes of a YOLO dataset
        items = []
        for p in glob.glob(os.path.join(src, '**', 'images', '*.jpg'), recursive=True):
            lab = p.replace(os.sep + 'images' + os.sep, os.sep + 'labels' + os.sep)[:-4] + '.txt'
            if os.path.exists(lab) and open(lab).read().split():
                items.append((p, [float(v) for v in open(lab).read().split()[1:5]]))
        random.shuffle(items); items = items[:200]
        det_s, lm_s = [], []
        for p, (cx, cy, bw, bh) in items:
            im = Image.open(p).convert('RGB'); W, H = im.size
            det_s.append(letter(im, c['ds']) * c['dsc'] + c['dof'])
            s = max(bw * W, bh * H) * random.uniform(1.5, 2.0)
            crop = im.crop((cx * W - s / 2, cy * H - s / 2, cx * W + s / 2, cy * H + s / 2)).rotate(random.uniform(-25, 25))
            lm_s.append(np.asarray(crop.resize((c['ls'], c['ls']), Image.BILINEAR), np.float32) / 255)
            n = neg_crop(im, ((cx - bw / 2) * W, (cy - bh / 2) * H, (cx + bw / 2) * W, (cy + bh / 2) * H), s, c['ls'])
            if n is not None and random.random() < 0.5: neg.append(n)
    else:                                                     # face pictures: the float detector finds the faces
        import onnxruntime as ort
        pics = [p for e in ('*.jpg', '*.jpeg', '*.png') for p in glob.glob(os.path.join(src, '**', e), recursive=True)]
        random.shuffle(pics); pics = pics[:200]
        s_det = ort.InferenceSession(det, providers=['CPUExecutionProvider'])
        det_s, lm_s = [], []
        for p in pics:
            im = Image.open(p).convert('RGB'); W, H = im.size; S = c['ds']
            x = letter(im, S) * c['dsc'] + c['dof']; det_s.append(x)
            reg, sc = s_det.run(None, {s_det.get_inputs()[0].name: x.transpose(2, 0, 1)[None].copy()})
            if reg.shape[-1] != 16: reg, sc = sc, reg
            A = anchors(S, [8, 16], [2, 6]); i = int(sc[0, :, 0].argmax())
            if sc[0, i, 0] < 0: continue
            r = min(S / W, S / H); ox, oy = (S - W * r) / 2, (S - H * r) / 2
            fcx = (A[i, 0] * S + reg[0, i, 0] - ox) / r; fcy = (A[i, 1] * S + reg[0, i, 1] - oy) / r
            s = max(reg[0, i, 2], reg[0, i, 3]) / r * 1.5
            crop = im.crop((fcx - s / 2, fcy - s / 2, fcx + s / 2, fcy + s / 2))
            lm_s.append(np.asarray(crop.resize((c['ls'], c['ls']), Image.BILINEAR), np.float32) / 255)
            n = neg_crop(im, (fcx - s / 3, fcy - s / 3, fcx + s / 3, fcy + s / 3), s, c['ls'])
            if n is not None and random.random() < 0.5: neg.append(n)
    pos = list(lm_s)
    lm_s = pos + neg
    random.shuffle(lm_s)
    print('calibration: detector', len(det_s), 'landmarks', len(pos), '+', len(neg), 'without')

    def top(o):                                               # best classifier score (any stride)
        return max(float(x.max()) for x in o if x.shape[1] in (2, 6))
    held = det_s[-40:]

    def det_score(ex):                                        # share of pictures where both find the object
        pr = compare(det_cut, ex, held, lambda f, q: (top(f) > 0, top(q) > 0))
        return sum(p[0] and p[1] for p in pr) / max(1, sum(p[0] for p in pr))
    ex = best_quantize(det_cut, os.path.join(out, kind + '_det.espdl'), det_s, c['ds'], det_score, 'detector')
    pair = compare(det_cut, ex, held, lambda f, q: (top(f) > 0, top(q) > 0))
    print(f'detector: found float {sum(p[0] for p in pair)}, int8 {sum(p[1] for p in pair)}, both {sum(p[0] and p[1] for p in pair)} of {len(pair)}')
    names = [o.name for o in onnx.load(lm_cut).graph.output]
    pi = [i for i, n in enumerate(names) if n == 'Identity'][0]
    fi = [i for i, n in enumerate(names) if n == 'Identity_1'][0]
    sg = (lambda v: 1 / (1 + np.exp(-v))) if kind == 'face' else (lambda v: v)

    vi = [i for i, n in enumerate(names) if n == 'EyeV']; hi = [i for i, n in enumerate(names) if n == 'EyeH']

    def ear_err(ex):                                          # face: EAR from the eye outputs, int8 vs float
        r = compare(lm_cut, ex, pos[-30:], lambda f, q: abs(ear_of(f[vi[0]], f[hi[0]]) - ear_of(q[vi[0]], q[hi[0]])))
        return float(np.mean(r))

    def lm_score(ex):                                         # presence kept on real crops, minus the point error
        r = compare(lm_cut, ex, pos[-30:], lambda f, q: (float(sg(q[fi].ravel()[0])) > 0.5,
                                                          float(np.abs(f[pi].reshape(-1, 3)[:, :2] - q[pi].reshape(-1, 3)[:, :2]).mean())))
        sc = np.mean([a for a, _ in r]) - np.mean([e for _, e in r]) / c['ls'] * 10
        return sc - ear_err(ex) * 20 if vi else sc
    ex = best_quantize(lm_cut, os.path.join(out, kind + '_lm.espdl'), lm_s, c['ls'], lm_score, 'landmarks')
    err = compare(lm_cut, ex, lm_s[:30], lambda f, q: float(np.abs(f[pi].reshape(-1, 3)[:, :2] - q[pi].reshape(-1, 3)[:, :2]).mean()))
    print(f'landmarks int8 vs float: mean point error {np.mean(err):.2f} px of {c["ls"]}')
    if vi:
        old = compare(lm_cut, ex, pos[-30:], lambda f, q: abs(ear_of(f[vi[0]], f[hi[0]]) -
                      float(np.mean([(np.linalg.norm(P[e[1]] - P[e[5]]) + np.linalg.norm(P[e[2]] - P[e[4]])) / (2 * np.linalg.norm(P[e[0]] - P[e[3]]))
                                     for P in [q[pi].reshape(-1, 3)[:, :2]] for e in EYES]))))
        print(f'EAR error int8 vs float: eye outputs {ear_err(ex):.3f}  (from the 478 points it would be {np.mean(old):.3f})')
    for nm, smp in (('with', pos[-30:]), ('without', neg[-30:])):
        pr = compare(lm_cut, ex, smp, lambda f, q: (float(sg(f[fi].ravel()[0])), float(sg(q[fi].ravel()[0]))))
        print(f'presence on crops {nm} a {kind}: float {np.mean([a for a, _ in pr]):.2f}  int8 {np.mean([b for _, b in pr]):.2f}'
              f'  (int8 > 0.5 on {sum(b > 0.5 for _, b in pr)}/{len(pr)})')
    name = 'finger_counter' if kind == 'hand' else 'driver_drowsiness'
    os.replace(os.path.join(out, kind + '_det.espdl'), os.path.join(out, name + '.espdl'))
    os.replace(os.path.join(out, kind + '_lm.espdl'), os.path.join(out, name + '.lm'))
    if kind == 'hand':
        desc = ['name=Finger Counter', 'task=hands', 'conf=0.5', 'scale=0.0039215686', 'offset=0']
    else:
        desc = ['name=Driver Drowsiness', 'task=face', 'conf=0.5', 'scale=0.0078431373', 'offset=-1', 'closed=0.2', 'seconds=5']
    with open(os.path.join(out, name + '.txt'), 'w', encoding='utf-8', newline='') as t:
        t.write(''.join(line + chr(10) for line in desc))
    print('files', name + '.espdl', name + '.lm', name + '.txt')
    print('done', out)


def neg_crop(im, box, s, S):
    """a square of size s that does not touch box (x0, y0, x1, y1)"""
    W, H = im.size
    s = min(s, W, H)
    for _ in range(12):
        x, y = random.uniform(0, W - s), random.uniform(0, H - s)
        if x + s < box[0] or x > box[2] or y + s < box[1] or y > box[3]:
            return np.asarray(im.crop((x, y, x + s, y + s)).resize((S, S), Image.BILINEAR), np.float32) / 255
    return None


def anchors(size, strides, per):
    a = []
    for st, n in zip(strides, per):
        g = size // st
        for y in range(g):
            for x in range(g):
                for _ in range(n):
                    a.append(((x + 0.5) / g, (y + 0.5) / g))
    return np.array(a)


if __name__ == '__main__':
    main()
