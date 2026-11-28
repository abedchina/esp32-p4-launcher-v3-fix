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
    subprocess.run([sys.executable, '-m', 'tf2onnx.convert', '--tflite', tfl, '--output', out, '--opset', '13',
                    '--inputs-as-nchw', 'input_1,input'], check=True, capture_output=True)
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


def letter(im, S):
    w, h = im.size; r = min(S / w, S / h)
    c = Image.new('RGB', (S, S), (114, 114, 114))
    c.paste(im.resize((round(w * r), round(h * r)), Image.BILINEAR), ((S - round(w * r)) // 2, (S - round(h * r)) // 2))
    return np.asarray(c, np.float32)


def quantize(onnx_path, out, samples, S, eq=True):
    import torch
    from esp_ppq.api import espdl_quantize_onnx
    from esp_ppq.api.setting import QuantizationSettingFactory
    st = QuantizationSettingFactory.espdl_setting()
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

    ex = quantize(det_cut, os.path.join(out, kind + '_det.espdl'), det_s, c['ds'])
    def top(o):                                               # best classifier score (any stride)
        return max(float(x.max()) for x in o if x.shape[1] in (2, 6))
    pair = compare(det_cut, ex, det_s[-40:], lambda f, q: (top(f) > 0, top(q) > 0))
    print(f'detector: found float {sum(p[0] for p in pair)}, int8 {sum(p[1] for p in pair)}, both {sum(p[0] and p[1] for p in pair)} of {len(pair)}')
    ex = quantize(lm_cut, os.path.join(out, kind + '_lm.espdl'), lm_s, c['ls'])
    names = [o.name for o in onnx.load(lm_cut).graph.output]
    pi = [i for i, n in enumerate(names) if n == 'Identity'][0]
    err = compare(lm_cut, ex, lm_s[:30], lambda f, q: float(np.abs(f[pi].reshape(-1, 3)[:, :2] - q[pi].reshape(-1, 3)[:, :2]).mean()))
    print(f'landmarks int8 vs float: mean point error {np.mean(err):.2f} px of {c["ls"]}')
    fi = [i for i, n in enumerate(names) if n == 'Identity_1'][0]
    sg = (lambda v: 1 / (1 + np.exp(-v))) if kind == 'face' else (lambda v: v)
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
