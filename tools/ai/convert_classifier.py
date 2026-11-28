"""Image classifier (Keras / Teachable-Machine style .tflite or .onnx: picture -> one class) -> ESP-DL .espdl + .txt.

    python convert_classifier.py <model> <out_name> <labels a,b,c> <calib dir> [more dirs] [--title T]

Works for float and int8 .tflite (int8 is turned back to float first, then quantized for the P4).
A leading Rescaling (x*m + a) inside the model is taken out and done by the launcher (scale/offset in the .txt).
Output: [1, N] probabilities or logits, or [1, 1] = two classes with a sigmoid.
"""
import argparse, glob, os, random, subprocess, sys
import numpy as np
import onnx, onnx.helper, onnx.numpy_helper
from onnx import shape_inference
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))


def to_onnx(src, work):
    if src.lower().endswith('.onnx'):
        return src
    import tensorflow as tf
    name = tf.lite.Interpreter(model_content=open(src, 'rb').read()).get_input_details()[0]['name']
    out = os.path.join(work, 'model.onnx')
    r = subprocess.run([sys.executable, '-m', 'tf2onnx.convert', '--tflite', src, '--output', out, '--opset', '13',
                        '--inputs-as-nchw', name, '--dequantize'], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit('tf2onnx failed:\n' + r.stderr[-2000:])
    return out


def scalar(m, name):
    for i in m.graph.initializer:
        if i.name == name:
            v = onnx.numpy_helper.to_array(i)
            return float(v.ravel()[0]) if v.size == 1 else None
    for n in m.graph.node:
        if n.op_type == 'Constant' and n.output[0] == name:
            v = onnx.numpy_helper.to_array(n.attribute[0].t)
            return float(v.ravel()[0]) if v.size == 1 else None
    return None


def fold(m, path):
    """int8 / dynamic-range .tflite: drop DynamicQuantizeLinear->DequantizeLinear pairs (activations stay float)
    and let onnxruntime fold the constants (int8 weights -> float, Shape/Gather/Reshape chains of the batch-1 graph)"""
    prod = {o: n for n in m.graph.node for o in n.output}
    drop = []
    for n in m.graph.node:
        if n.op_type == 'DequantizeLinear' and prod.get(n.input[0]) is not None and prod[n.input[0]].op_type == 'DynamicQuantizeLinear':
            src = prod[n.input[0]].input[0]
            for c in m.graph.node:
                for j, i in enumerate(c.input):
                    if i == n.output[0]:
                        c.input[j] = src
            drop.append(n)
    for n in drop:
        m.graph.node.remove(n)
    used = {i for n in m.graph.node for i in n.input} | {o.name for o in m.graph.output}
    for n in [n for n in m.graph.node if n.op_type == 'DynamicQuantizeLinear' and not any(o in used for o in n.output)]:
        m.graph.node.remove(n)
    if not any(n.op_type in ('DequantizeLinear', 'Shape', 'Cast', 'ConstantOfShape') for n in m.graph.node):
        return m
    import onnxruntime as ort
    tmp = path + '.tmp.onnx'
    onnx.save(m, tmp)
    so = ort.SessionOptions()
    so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_BASIC
    so.optimized_model_filepath = path + '.fold.onnx'
    ort.InferenceSession(tmp, so, providers=['CPUExecutionProvider'])
    out = onnx.load(path + '.fold.onnx')
    for f in (tmp, path + '.fold.onnx'):
        os.remove(f)
    print('folded int8 weights / constant shapes:', len(m.graph.node), '->', len(out.graph.node), 'layers')
    return out


def prepare(src, dst):
    """batch 1, explicit Pad mode, and take a leading x*m (+a) out -> (scale, offset) for the launcher or None"""
    m = onnx.load(src)
    d = m.graph.input[0].type.tensor_type.shape.dim
    d[0].dim_value = 1
    for o in m.graph.output:
        o.type.tensor_type.shape.dim[0].dim_value = 1
    del m.graph.value_info[:]
    m = fold(shape_inference.infer_shapes(m), dst)
    for n in m.graph.node:
        if n.op_type == 'Pad' and not any(a.name == 'mode' for a in n.attribute):
            n.attribute.append(onnx.helper.make_attribute('mode', 'constant'))
        if n.op_type == 'Resize':
            for a in n.attribute:
                if a.name == 'coordinate_transformation_mode' and a.s == b'tf_half_pixel_for_nn':
                    a.s = b'asymmetric'
    cons = {}
    for n in m.graph.node:
        for i in n.input:
            cons.setdefault(i, []).append(n)
    t = m.graph.input[0].name
    while len(cons.get(t, [])) == 1 and cons[t][0].op_type == 'Transpose':
        t = cons[t][0].output[0]
    start = t
    sc, of, removed = 1.0, 0.0, []
    while len(cons.get(t, [])) == 1 and cons[t][0].op_type in ('Mul', 'Add', 'Sub', 'Div'):
        n = cons[t][0]
        k = scalar(m, n.input[1] if n.input[0] == t else n.input[0])
        if k is None:
            break
        if n.op_type == 'Mul': sc, of = sc * k, of * k
        elif n.op_type == 'Div': sc, of = sc / k, of / k
        elif n.op_type == 'Add': of += k
        else: of = of - k if n.input[0] == t else k - of
        removed.append(n)
        t = n.output[0]
    if removed:                                                # the layer after the rescale now reads the transposed input
        for n in m.graph.node:
            for j, i in enumerate(n.input):
                if i == t:
                    n.input[j] = start
        for n in removed:
            m.graph.node.remove(n)
    del m.graph.value_info[:]
    onnx.save(shape_inference.infer_shapes(m), dst)
    return (sc, of) if removed else None


def load(p, S):
    return np.asarray(Image.open(p).convert('RGB').resize((S, S), Image.BILINEAR), np.float32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('model'); ap.add_argument('name'); ap.add_argument('labels'); ap.add_argument('calib', nargs='+')
    ap.add_argument('--title', default=''); ap.add_argument('--conf', type=float, default=0.5)
    ap.add_argument('--range', default='255', choices=['255', '127', 'raw'],
                    help='how the notebook prepares a picture: 255 = /255 (0..1), 127 = /127.5-1 (-1..1), raw = 0..255')
    a = ap.parse_args()
    labels = [l.strip() for l in a.labels.split(',') if l.strip()]
    work = os.path.abspath(a.name + '_work'); os.makedirs(work, exist_ok=True)
    onx = os.path.join(work, 'cls.onnx')
    rs = prepare(to_onnx(a.model, work), onx)
    m = onnx.load(onx)
    S = m.graph.input[0].type.tensor_type.shape.dim[2].dim_value
    nout = m.graph.output[0].type.tensor_type.shape.dim[-1].dim_value
    imgs = [p for d in a.calib for e in ('*.jpg', '*.jpeg', '*.png') for p in glob.glob(os.path.join(d, '**', e), recursive=True)]
    random.seed(1); random.shuffle(imgs); imgs = imgs[:200]
    raw = [load(p, S) for p in imgs]
    import onnxruntime as ort
    sess = ort.InferenceSession(onx, providers=['CPUExecutionProvider'])
    iname = sess.get_inputs()[0].name

    def run(x):
        return sess.run(None, {iname: x.transpose(2, 0, 1)[None].astype(np.float32).copy()})[0].ravel()

    if rs:                                                     # the model had its own rescale: feed it 0..255 * sc + of
        scale, offset = rs
        print(f'model rescales its input itself: x*{scale:.6g}{offset:+.6g} (done by the launcher now)')
    else:                                                      # as in the training notebook (default: picture / 255)
        scale, offset = {'255': (1 / 255, 0.0), '127': (1 / 127.5, -1.0), 'raw': (1.0, 0.0)}[a.range]
        print(f'input: x*{scale:.6g}{offset:+g}')
    if len(labels) != max(2, nout):
        print(f'WARNING: the model has {max(2, nout)} classes but {len(labels)} names were given')
    samples = [r * scale + offset for r in raw]
    sys.path.insert(0, HERE)
    from convert_mediapipe import quantize
    ex = quantize(onx, a.name + '.espdl', samples, S)
    import torch
    agree = 0
    test = samples[-40:]
    for x in test:
        f = run(x)
        q = ex.forward(inputs=torch.from_numpy(x.transpose(2, 0, 1)[None].astype(np.float32).copy()))[0].numpy().ravel()
        agree += (f.argmax() == q.argmax()) if nout > 1 else ((f[0] > 0.5) == (q[0] > 0.5))
    print(f'int8 vs float: same class on {agree}/{len(test)} pictures')
    with open(a.name + '.txt', 'w', encoding='utf-8', newline='\n') as t:
        t.write(f"name={a.title or a.name}\ntask=cls\nlabels={','.join(labels)}\nconf={a.conf}\nscale={scale:.10g}\noffset={offset:.10g}\n")
    print('done', a.name + '.espdl', os.path.getsize(a.name + '.espdl'))


if __name__ == '__main__':
    main()
