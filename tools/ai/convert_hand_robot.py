"""Hand robot model -> ESP-DL (.espdl, int8) for the P4, calibrated on the Roboflow dataset."""
import glob, os, sys, random
import numpy as np
from PIL import Image

DS = r"C:\Users\abedm\OneDrive\Desktop\cnn-code-ai\תנועה ידים לרובוט\hand_robot_abed_mihilia.v2i.yolov8"
NAMES = ["forward", "left", "right", "stop"]
S = 512


def letterbox(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    r = min(S / w, S / h)
    nw, nh = round(w * r), round(h * r)
    im = im.resize((nw, nh), Image.NEAREST)
    canvas = Image.new("RGB", (S, S), (114, 114, 114))
    canvas.paste(im, ((S - nw) // 2, (S - nh) // 2))
    x = np.asarray(canvas, dtype=np.float32) / 255.0
    return x.transpose(2, 0, 1)[None]            # 1x3xSxS


def top(out, conf=0.25):
    o = out[0]                                    # 8 x 5376
    sc = o[4:].max(0); cl = o[4:].argmax(0)
    i = int(sc.argmax())
    return (NAMES[cl[i]], float(sc[i])) if sc[i] >= conf else ("none", float(sc[i]))


def label_of(img):
    lab = img.replace(os.sep + "images" + os.sep, os.sep + "labels" + os.sep).rsplit(".", 1)[0] + ".txt"
    try:
        line = open(lab).read().split()
        return NAMES[int(line[0])]
    except Exception:
        return "none"


def main():
    import onnxruntime as ort
    sess = ort.InferenceSession("hand.onnx", providers=["CPUExecutionProvider"])
    tests = sorted(glob.glob(os.path.join(DS, "test", "images", "*")))
    ok = 0
    for p in tests:
        name, s = top(sess.run(None, {"images": letterbox(p)})[0])
        ok += name == label_of(p)
    print(f"float ONNX on {len(tests)} test images: {ok} correct ({100 * ok / len(tests):.0f}%)")
    if len(sys.argv) > 1 and sys.argv[1] == "quant":
        import torch
        from esp_ppq.api import espdl_quantize_onnx
        trains = sorted(glob.glob(os.path.join(DS, "train", "images", "*")))
        random.seed(1)
        calib = [torch.from_numpy(letterbox(p)[0]) for p in random.sample(trains, 256)]
        loader = torch.utils.data.DataLoader(calib, batch_size=1, shuffle=False)
        graph = espdl_quantize_onnx(onnx_import_file="hand_head.onnx", espdl_export_file="hand_robot.espdl",
                                    calib_dataloader=loader, calib_steps=128, input_shape=[1, 3, S, S],
                                    target="esp32p4", num_of_bits=8, collate_fn=lambda b: b.to("cpu").reshape(1, 3, S, S),
                                    device="cpu", error_report=True, skip_export=False, verbose=0)
        print("exported hand_robot.espdl", os.path.getsize("hand_robot.espdl"))
        eval_quant(graph)


def decode_head(outs, conf=0.5):
    """YOLOv8 raw head (box 64ch + cls ch per stride, NCHW) -> best (class, score) - the app does the same."""
    best = ("none", 0.0)
    for o in outs:
        o = o[0]
        if o.shape[0] == 64: continue
        box = [b[0] for b in outs if b[0].shape[0] == 64 and b[0].shape[1] == o.shape[1]][0]
        sc = 1 / (1 + np.exp(-o))                       # nc x H x W
        c = sc.max(0); cl = sc.argmax(0)
        y, x = np.unravel_index(c.argmax(), c.shape)
        if c[y, x] > best[1]: best = (NAMES[cl[y, x]], float(c[y, x]))
    return best if best[1] >= conf else ("none", best[1])


def eval_quant(graph):
    import torch
    from esp_ppq.executor import TorchExecutor
    ex = TorchExecutor(graph=graph, device="cpu")
    tests = sorted(glob.glob(os.path.join(DS, "test", "images", "*")))
    res = {0.0: 0, 0.3: 0, 0.4: 0, 0.5: 0}
    for p in tests:
        outs = [t.numpy() for t in ex.forward(inputs=torch.from_numpy(letterbox(p)))]
        for c in res: res[c] += decode_head(outs, c)[0] == label_of(p)
    for c, ok in res.items():
        print(f"int8 conf>={c}: {ok}/{len(tests)} correct ({100 * ok / len(tests):.0f}%)")


if __name__ == "__main__":
    main()
