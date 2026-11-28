"""Publish the model tool to the private repo abedchina/AI-Models, folder tool/ (needs gh auth).

    python publish_tool.py          (from the development PC)

tool/ gets: the converters + add_model.py + upload_model.py, AI-Add-Model.bat, install.bat, make_shortcut.ps1,
requirements.txt, the esp-ppq wheel and calib_general.zip (general calibration pictures), README.md.
Another PC: download the repo (Code -> Download ZIP), run tool\\install.bat once.
"""
import base64, hashlib, io, json, os, subprocess, sys, tempfile, zipfile

REPO = 'abedchina/AI-Models'
HERE = os.path.dirname(os.path.abspath(__file__))
PKG = r'C:\ecm\p4tool'
CODE = ['add_model.py', 'convert_yolo.py', 'convert_classifier.py', 'convert_mediapipe.py', 'upload_model.py']
README = """# AI-Add-Model - תוכנת ההמרה של מודלים ל-P4

גוררים מודל (‎.tflite / ‎.onnx / ‎.task) על האייקון **AI-Add-Model** בשולחן העבודה:
הוא מזהה את סוג המודל, ממיר ל-‎.espdl ומעלה לספרייה AI-Models. ב-P4: AI Models ← Download all new models.

## התקנה במחשב חדש (פעם אחת)
1. בדף הראשי של AI-Models: ‏**Code ← Download ZIP**, ולפתוח את ה-ZIP.
2. בתיקייה `tool` - דאבל קליק על **install.bat** (10-20 דקות, צריך אינטרנט).
3. בסוף נפתח דפדפן להתחברות ל-GitHub (פעם אחת).
4. על שולחן העבודה מופיע **AI-Add-Model**.

## סוגי מודלים
- YOLO (מוכן או מ-YOLO Lab) - השמות נלקחים מהקובץ
- מודל שאימנת (Transfer Learning) - שואל את שמות הסוגים
- MediaPipe ‏hand_landmarker / face_landmarker

תמונות לא חובה: הוא מחפש תמונות ליד המודל (גם בתוך ZIP של דאטה סט), אחרת משתמש בתמונות כלליות.
"""


def gh(*args, data=None):
    r = subprocess.run(['gh', 'api', *args] + (['--input', data] if data else []), capture_output=True, text=True, encoding='utf-8')
    return r.returncode, r.stdout, r.stderr


def put(path, raw):
    rc, out, _ = gh('repos/%s/contents/%s' % (REPO, path))
    old = json.loads(out).get('sha') if rc == 0 else None
    if old == hashlib.sha1(b'blob %d\0' % len(raw) + raw).hexdigest():
        print('  %-40s same' % path); return
    body = {'message': ('Update ' if old else 'Add ') + path, 'content': base64.b64encode(raw).decode()}
    if old:
        body['sha'] = old
    with tempfile.NamedTemporaryFile('w', suffix='.json', delete=False) as f:
        json.dump(body, f)
    rc, _, err = gh('-X', 'PUT', 'repos/%s/contents/%s' % (REPO, path), data=f.name)
    os.remove(f.name)
    if rc:
        raise SystemExit('upload failed %s: %s' % (path, err.strip()))
    print('  %-40s %s (%d bytes)' % (path, 'replaced' if old else 'uploaded', len(raw)))


def crlf(b):
    return b.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')


def main():
    files = {}
    for n in CODE:
        files[n] = open(os.path.join(HERE, n), 'rb').read()
    for n in ('install.bat', 'AI-Add-Model.bat', 'make_shortcut.ps1', 'requirements.txt'):
        b = open(os.path.join(PKG, 'tool', n), 'rb').read()
        files[n] = crlf(b) if n.endswith(('.bat', '.ps1')) else b
    whl = [f for f in os.listdir(os.path.join(PKG, 'wheels')) if f.endswith('.whl')][0]
    files[whl] = open(os.path.join(PKG, 'wheels', whl), 'rb').read()
    zb = io.BytesIO()
    with zipfile.ZipFile(zb, 'w', zipfile.ZIP_STORED) as z:
        src = r'C:\ecm\ai\calib_general'
        for f in sorted(os.listdir(src)):
            z.write(os.path.join(src, f), 'calib_general/' + f)
    files['calib_general.zip'] = zb.getvalue()
    zb = io.BytesIO()
    with zipfile.ZipFile(zb, 'w', zipfile.ZIP_STORED) as z:
        src = os.path.join('C:' + os.sep, 'ecm', 'ai', 'calib_faces')
        for f in sorted(os.listdir(src)):
            z.write(os.path.join(src, f), 'calib_faces/' + f)
    files['calib_faces.zip'] = zb.getvalue()
    files['README.md'] = README.encode('utf-8')
    for n, b in files.items():
        put('tool/' + n, b)


if __name__ == '__main__':
    main()
