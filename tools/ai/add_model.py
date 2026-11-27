"""Add a new YOLO model to the P4 model shelf: convert (.tflite / .onnx -> .espdl + .txt) and upload to AI-Models.

Started from AI-Add-Model.bat on the Desktop (drag the model file onto it).
"""
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = r'C:\ecm\ai\out'


def ask(q):
    return clean(input(q))


def clean(s):
    return s.replace(chr(0xFEFF), '').strip().strip('"').strip()


def main():
    print('=== Add a model to the P4 (AI-Models on GitHub) ===\n')
    model = sys.argv[1] if len(sys.argv) > 1 else ask('Model file (.tflite / .onnx) - drag it here and press Enter:\n> ')
    model = clean(model)
    if not os.path.isfile(model) or not model.lower().endswith(('.tflite', '.onnx')):
        raise SystemExit('Not a .tflite / .onnx file: ' + model)
    imgs = ask('\nFolder with example pictures (the dataset or test pictures) - drag it here, Enter:\n> ')
    if not os.path.isdir(imgs):
        raise SystemExit('Folder not found: ' + imgs)
    name = ''
    while not re.fullmatch(r'[a-z0-9_]{2,30}', name):
        name = ask('\nShort file name, English small letters / digits / _ (example: fire_smoke):\n> ').lower()
    title = ask('\nName shown on the P4 screen (Enter = same as file name):\n> ') or name
    keep = ask('\nKeep only some classes? numbers like 2,5 (Enter = all classes):\n> ')
    os.makedirs(OUT, exist_ok=True)
    cmd = [sys.executable, os.path.join(HERE, 'convert_yolo.py'), model, name, imgs, '--title', title]
    if keep:
        cmd += ['--keep', keep]
    print('\nConverting... (5-15 minutes, do not close this window)\n')
    if subprocess.run(cmd, cwd=OUT).returncode:
        raise SystemExit('\nConversion FAILED - send Claude a picture of this window.')
    print('\nUploading to GitHub...')
    if subprocess.run([sys.executable, os.path.join(HERE, 'upload_model.py'), os.path.join(OUT, name)]).returncode:
        raise SystemExit('\nUpload FAILED - send Claude a picture of this window.')
    print('\nDONE. On the P4: AI Models -> Download models from GitHub -> Download all new models.')


if __name__ == '__main__':
    try:
        main()
    except SystemExit as e:
        if e.code not in (None, 0):
            print(e.code)
