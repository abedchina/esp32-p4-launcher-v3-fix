"""Add any model to the P4 model shelf: drag it onto AI-Add-Model.bat (Desktop).

Finds the kind by itself:
  YOLO detector (Ultralytics .tflite / .onnx)       -> convert_yolo.py       (names come from the file)
  image classifier (Keras / transfer learning)      -> convert_classifier.py (asks for the class names)
  MediaPipe hand_landmarker / face_landmarker .task -> convert_mediapipe.py
Pictures are optional (Enter = the general pictures in C:\\ecm\\ai\\calib_general). Then uploads to AI-Models.
"""
import io, json, os, re, subprocess, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = r'C:\ecm\ai\out'
GENERAL = r'C:\ecm\ai\calib_general'
HAND_DS = r'C:\Users\abedm\OneDrive\Desktop\cnn-code-ai\תנועה ידים לרובוט\hand_robot_abed_mihilia.v2i.yolov8\train'


def clean(s):
    return s.replace(chr(0xFEFF), '').strip().strip('"').strip()


def ask(q, default=''):
    a = clean(input(q))
    return a or default


def kind_of(path):
    """('yolo'|'cls'|'hand'|'face', names or None)"""
    low = path.lower()
    if low.endswith('.task'):
        files = zipfile.ZipFile(path).namelist()
        return ('hand' if any('hand' in f for f in files) else 'face'), None
    if low.endswith('.onnx'):
        import onnx
        m = onnx.load(path, load_external_data=False)
        mp = {p.key: p.value for p in m.metadata_props}
        if 'names' in mp:
            import ast
            return 'yolo', list(ast.literal_eval(mp['names']).values())
        dims = [d.dim_value for d in m.graph.output[0].type.tensor_type.shape.dim]
        return ('cls' if len(dims) == 2 else 'yolo'), None
    try:                                                      # Ultralytics .tflite carries metadata.json
        meta = json.loads(zipfile.ZipFile(io.BytesIO(open(path, 'rb').read())).read('metadata.json'))
        return 'yolo', list(meta['names'].values())
    except Exception:
        pass
    import tensorflow as tf
    o = tf.lite.Interpreter(model_content=open(path, 'rb').read()).get_output_details()
    return ('cls' if len(o) == 1 and len(o[0]['shape']) == 2 else 'yolo'), None


def keras_range(model):
    """the .keras saved next to the .tflite tells whether the model rescales by itself (layers.Rescaling inside)"""
    d = os.path.dirname(model)
    for f in os.listdir(d):
        if f.lower().endswith('.keras'):
            try:
                cfg = zipfile.ZipFile(os.path.join(d, f)).read('config.json').decode('utf-8', 'ignore')
            except Exception:
                continue
            return 'raw' if '"class_name": "Rescaling"' in cfg else '255'
    return None


def labels_beside(model):
    d = os.path.dirname(model)
    for n in ('labels.txt', 'classes.txt', 'class_names.txt'):
        p = os.path.join(d, n)
        if os.path.exists(p):
            names = [l.strip() for l in open(p, encoding='utf-8', errors='ignore').read().replace(',', '\n').splitlines() if l.strip()]
            names = [re.sub(r'^\d+\s+', '', n) for n in names]          # Teachable Machine: "0 cat"
            if names:
                return names
    return None


def main():
    print('=== Add a model to the P4 (AI-Models on GitHub) ===\n')
    model = clean(sys.argv[1]) if len(sys.argv) > 1 else ask('Model file - drag it here and press Enter:\n> ')
    if not os.path.isfile(model) or not model.lower().endswith(('.tflite', '.onnx', '.task')):
        raise SystemExit('Not a .tflite / .onnx / .task file: ' + model)
    print('\nChecking the model...')
    kind, names = kind_of(model)
    print({'yolo': 'YOLO detector (boxes)', 'cls': 'Image classifier (one answer for the picture)',
           'hand': 'MediaPipe hands (Finger Counter)', 'face': 'MediaPipe face (Driver Drowsiness)'}[kind])
    if names:
        print('Classes:', ', '.join(names))

    base = re.sub(r'[^a-z0-9_]+', '_', os.path.splitext(os.path.basename(model))[0].lower()).strip('_')[:30] or 'model'
    if kind in ('hand', 'face'):
        name = 'finger_counter' if kind == 'hand' else 'driver_drowsiness'
    else:
        name = ''
        while not re.fullmatch(r'[a-z0-9_]{2,30}', name):
            name = ask(f'\nShort file name, English small letters / digits / _ (Enter = {base}):\n> ', base).lower()
    title = ask('\nName shown on the P4 screen (Enter = same):\n> ', name)

    extra = []
    if kind == 'cls':
        names = labels_beside(model)
        if names:
            print('Class names from labels.txt:', ', '.join(names))
        while not names:
            t = ask('\nClass names in the training order, with commas (example: cat,dog):\n> ')
            names = [n.strip() for n in t.split(',') if n.strip()]
        rng = keras_range(model)
        if rng:
            print('From the .keras file:', 'the model divides by 255 itself' if rng == 'raw' else 'picture / 255')
        else:
            r = ask('\nHow does your notebook prepare a picture?\n'
                    '  Enter = picture / 255   (most notebooks)\n  2 = picture / 127.5 - 1\n  3 = no division\n> ', '1')
            rng = {'2': '127', '3': 'raw'}.get(r, '255')
        extra = ['--range', rng]
    if kind == 'yolo' and not names:
        t = ask('\nClass names with commas (example: fire,smoke):\n> ')
        extra = ['--labels', t]

    pics = ask('\nFolder with pictures (dataset / test pictures) - drag it here.\n'
               '  Enter = no pictures (general pictures, a bit less exact):\n> ')
    if pics and not os.path.isdir(pics):
        raise SystemExit('Folder not found: ' + pics)
    os.makedirs(OUT, exist_ok=True)
    py = sys.executable
    if kind == 'yolo':
        cmd = [py, os.path.join(HERE, 'convert_yolo.py'), model, name] + ([pics] if pics else []) + [GENERAL, '--title', title] + extra
    elif kind == 'cls':
        cmd = [py, os.path.join(HERE, 'convert_classifier.py'), model, name, ','.join(names)] + ([pics] if pics else []) + [GENERAL, '--title', title] + extra
    else:
        src = pics or (HAND_DS if kind == 'hand' else GENERAL)
        cmd = [py, os.path.join(HERE, 'convert_mediapipe.py'), kind, model, src, OUT]
    print('\nConverting... (5-15 minutes, do not close this window)\n')
    if subprocess.run(cmd, cwd=OUT).returncode:
        raise SystemExit('\nConversion FAILED - send Claude a picture of this window.')
    if kind in ('hand', 'face') and title != name:
        p = os.path.join(OUT, name + '.txt')
        lines = [l for l in open(p, encoding='utf-8').read().splitlines() if not l.startswith('name=')]
        open(p, 'w', encoding='utf-8', newline='').write(''.join(l + chr(10) for l in ['name=' + title] + lines))
    if os.environ.get('AI_NO_UPLOAD'):                       # tests
        print('\n(test: not uploaded)', os.path.join(OUT, name))
        return
    print('\nUploading to GitHub...')
    if subprocess.run([py, os.path.join(HERE, 'upload_model.py'), os.path.join(OUT, name)]).returncode:
        raise SystemExit('\nUpload FAILED - send Claude a picture of this window.')
    print('\nDONE. On the P4: AI Models -> Download models from GitHub -> Download all new models.')


if __name__ == '__main__':
    try:
        main()
    except SystemExit as e:
        if e.code not in (None, 0):
            print(e.code)
