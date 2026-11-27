"""Any Ultralytics YOLO detector (.tflite with metadata, or .onnx) -> ESP-DL .espdl (int8) + .txt for AI Models.

    python convert_yolo.py <model.tflite|.onnx> <out_name> <calib_dir> [more calib dirs...] [--size 640] [--labels a,b,c]

The graph is cut before the DFL/decode part (ESP-DL cannot build its Reshape), leaving per stride a box map
[1,64,H,W] and a class map [1,nc,H,W]; the AI Models app does the DFL decode and the sigmoid.
Run with the Python 3.11 venv (tf2onnx, onnx, onnxruntime, esp-ppq, torch) and PYTHONUTF8=1.
"""
import argparse, glob, io, json, os, random, subprocess, sys, zipfile
import numpy as np
import onnx, onnx.numpy_helper
from onnx import shape_inference, utils
from PIL import Image


def tflite_meta(path):
    try:
        return json.loads(zipfile.ZipFile(io.BytesIO(open(path, 'rb').read())).read('metadata.json'))
    except Exception:
        return {}


def to_onnx(src, work):
    if src.lower().endswith('.onnx'):
        return src
    out = os.path.join(work, 'model.onnx')
    subprocess.run([sys.executable, '-m', 'tf2onnx.convert', '--tflite', src, '--output', out, '--opset', '13',
                    '--inputs-as-nchw', 'images'], check=True, capture_output=True)
    return out


def cut_head(src, dst, nc):
    m = onnx.load(src)
    for n in m.graph.node:                                   # tf2onnx: old Resize mode -> same result for x2 nearest
        if n.op_type == 'Resize':
            for a in n.attribute:
                if a.name == 'coordinate_transformation_mode' and a.s == b'tf_half_pixel_for_nn':
                    a.s = b'asymmetric'
    m = shape_inference.infer_shapes(m)
    shapes = {v.name: [d.dim_value for d in v.type.tensor_type.shape.dim]
              for v in list(m.graph.value_info) + list(m.graph.output) + list(m.graph.input)}
    inp = m.graph.input[0].name
    S = shapes[inp][2]
    cons = {}
    for n in m.graph.node:
        for i in n.input:
            cons.setdefault(i, []).append(n)
    prod = {o: n for n in m.graph.node for o in n.output}
    outs = []
    for t, sh in shapes.items():
        if len(sh) != 4 or sh[0] != 1 or sh[2] not in (S // 8, S // 16, S // 32) or sh[1] not in (64, nc):
            continue
        p = prod.get(t)
        if not p or p.op_type not in ('Conv', 'Add'):
            continue
        c = cons.get(t, [])
        if c and all(x.op_type in ('Reshape', 'Concat') for x in c):
            # a Concat that only joins box+cls of one stride is fine; a Concat inside the backbone is not
            if any(x.op_type == 'Concat' and len(shapes.get(x.output[0], [])) == 4 and shapes[x.output[0]][1] not in (64 + nc,)
                   for x in c):
                continue
            outs.append(t)
    outs = sorted(set(outs), key=lambda t: (-shapes[t][2], shapes[t][1] != 64))
    if len(outs) != 6:
        raise SystemExit('expected 6 head outputs, found %d: %s' % (len(outs), [(t, shapes[t]) for t in outs]))
    onnx.save(m, dst + '.fixed.onnx')
    utils.extract_model(dst + '.fixed.onnx', dst, [inp], outs)
    os.remove(dst + '.fixed.onnx')
    return inp, S, [(shapes[t]) for t in outs]


def letterbox(path, S):
    im = Image.open(path).convert('RGB')
    w, h = im.size
    r = min(S / w, S / h)
    nw, nh = round(w * r), round(h * r)
    canvas = Image.new('RGB', (S, S), (114, 114, 114))
    canvas.paste(im.resize((nw, nh), Image.NEAREST), ((S - nw) // 2, (S - nh) // 2))
    return (np.asarray(canvas, dtype=np.float32) / 255.0).transpose(2, 0, 1)[None]


def top(outs, nc):
    best = (-1, 0.0)
    for o in outs:
        o = o[0]
        if o.shape[0] != nc or (nc == 64):
            continue
        s = 1 / (1 + np.exp(-o))
        c = s.max(0)
        if c.max() > best[1]:
            y, x = np.unravel_index(c.argmax(), c.shape)
            best = (int(s[:, y, x].argmax()), float(c.max()))
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('model'); ap.add_argument('name'); ap.add_argument('calib', nargs='+')
    ap.add_argument('--size', type=int, default=0); ap.add_argument('--labels', default='')
    ap.add_argument('--keep', default='', help='class ids to keep, e.g. 2,5')
    ap.add_argument('--title', default=''); ap.add_argument('--conf', type=float, default=0.3)
    a = ap.parse_args()
    if a.model.lower().endswith('.tflite'):
        meta = tflite_meta(a.model)
    else:                                                     # Ultralytics ONNX: names in metadata_props
        import ast
        mp = {p.key: p.value for p in onnx.load(a.model, load_external_data=False).metadata_props}
        meta = {'names': ast.literal_eval(mp['names'])} if 'names' in mp else {}
    labels = a.labels.split(',') if a.labels else list(meta.get('names', {}).values())
    nc = len(labels)
    work = os.path.abspath(a.name + '_work'); os.makedirs(work, exist_ok=True)
    head = os.path.join(work, 'head.onnx')
    inp, S, shp = cut_head(to_onnx(a.model, work), head, nc)
    if a.keep:                                                # keep only some classes: slice the last class Conv
        keep = [int(k) for k in a.keep.split(',')]
        m = onnx.load(head)
        init = {i.name: i for i in m.graph.initializer}
        outn = {o.name for o in m.graph.output}
        for n in m.graph.node:
            if n.op_type == 'Conv' and n.output[0] in outn:
                w = onnx.numpy_helper.to_array(init[n.input[1]])
                if w.shape[0] != nc:
                    continue
                init[n.input[1]].CopyFrom(onnx.numpy_helper.from_array(w[keep], n.input[1]))
                if len(n.input) > 2:
                    b = onnx.numpy_helper.to_array(init[n.input[2]])
                    init[n.input[2]].CopyFrom(onnx.numpy_helper.from_array(b[keep], n.input[2]))
        for o in m.graph.output:
            if o.type.tensor_type.shape.dim[1].dim_value == nc:
                o.type.tensor_type.shape.dim[1].dim_value = len(keep)
        del m.graph.value_info[:]
        onnx.save(shape_inference.infer_shapes(m), head)
        labels = [labels[k] for k in keep]; nc = len(keep)
    if a.size and a.size != S:                             # fully convolutional: run it at another size
        m = onnx.load(head)
        d = m.graph.input[0].type.tensor_type.shape.dim; d[2].dim_value = a.size; d[3].dim_value = a.size
        for o in m.graph.output:
            for k in (2, 3): o.type.tensor_type.shape.dim[k].dim_value = 0
        del m.graph.value_info[:]
        onnx.save(shape_inference.infer_shapes(m), head); S = a.size
    print('input', S, 'head', shp, 'classes', nc)
    imgs = [p for d in a.calib for e in ('*.jpg', '*.jpeg', '*.png') for p in glob.glob(os.path.join(d, '**', e), recursive=True)]
    random.seed(1); random.shuffle(imgs)
    imgs = imgs[:200]
    print('calibration images', len(imgs))
    import onnxruntime as ort, torch
    from esp_ppq.api import espdl_quantize_onnx
    from esp_ppq.executor import TorchExecutor
    sess = ort.InferenceSession(head, providers=['CPUExecutionProvider'])
    calib = [torch.from_numpy(letterbox(p, S)[0]) for p in imgs]
    loader = torch.utils.data.DataLoader(calib, batch_size=1, shuffle=False)
    graph = espdl_quantize_onnx(onnx_import_file=head, espdl_export_file=a.name + '.espdl', calib_dataloader=loader,
                                calib_steps=min(128, len(calib)), input_shape=[1, 3, S, S], target='esp32p4', num_of_bits=8,
                                collate_fn=lambda b: b.to('cpu').reshape(1, 3, S, S), device='cpu', error_report=False, verbose=0)
    ex = TorchExecutor(graph=graph, device='cpu')
    agree = n = 0
    for p in imgs[:40]:
        x = letterbox(p, S)
        f = top(sess.run(None, {inp: x}), nc)
        q = top([t.numpy() for t in ex.forward(inputs=torch.from_numpy(x))], nc)
        if f[1] >= a.conf:
            n += 1; agree += f[0] == q[0]
    print(f'int8 vs float: same class on {agree}/{n} images with a detection')
    with open(a.name + '.txt', 'w', encoding='utf-8', newline='\n') as t:
        t.write(f"name={a.title or a.name}\ntask=yolo\nlabels={','.join(labels)}\nconf={a.conf}\nscale=0.0039215686\noffset=0\n")
    print('done', a.name + '.espdl', os.path.getsize(a.name + '.espdl'))


if __name__ == '__main__':
    main()
