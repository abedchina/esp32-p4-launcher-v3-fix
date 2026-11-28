"""Upload <name>.espdl + <name>.txt to the private GitHub repo abedchina/AI-Models (needs `gh auth login`).

    python upload_model.py <folder>/<name>      (without extension)

Same file already there -> skipped; changed file -> replaced. The AI Models app on the P4 then shows it
under "Download all new models".
"""
import base64, hashlib, json, os, subprocess, sys, tempfile

REPO = 'abedchina/AI-Models'


def gh(*args, data=None):
    r = subprocess.run(['gh', 'api', *args] + (['--input', data] if data else []), capture_output=True, text=True,
                       encoding='utf-8')
    return r.returncode, r.stdout, r.stderr


def git_sha(raw):
    return hashlib.sha1(b'blob %d\0' % len(raw) + raw).hexdigest()


def upload(path):
    name = os.path.basename(path)
    raw = open(path, 'rb').read()
    rc, out, _ = gh('repos/%s/contents/%s' % (REPO, name))
    old = json.loads(out).get('sha') if rc == 0 else None
    if old == git_sha(raw):
        print('  %-28s already on GitHub (same file)' % name)
        return
    body = {'message': ('Update ' if old else 'Add ') + name, 'content': base64.b64encode(raw).decode()}
    if old:
        body['sha'] = old
    with tempfile.NamedTemporaryFile('w', suffix='.json', delete=False) as f:
        json.dump(body, f)
    rc, out, err = gh('-X', 'PUT', 'repos/%s/contents/%s' % (REPO, name), data=f.name)
    os.remove(f.name)
    if rc:
        raise SystemExit('upload failed for %s: %s' % (name, err.strip()))
    print('  %-28s %s (%d bytes)' % (name, 'replaced' if old else 'uploaded', len(raw)))


if __name__ == '__main__':
    for base in sys.argv[1:]:
        base = base[:-6] if base.endswith('.espdl') else base
        for ext in ('.espdl', '.txt'):
            if not os.path.exists(base + ext):
                raise SystemExit('missing ' + base + ext)
        for ext in ('.espdl', '.lm', '.txt'):              # .lm = landmark model of hands / face models
            if os.path.exists(base + ext):
                upload(base + ext)
