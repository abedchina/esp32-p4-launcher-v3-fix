#!/usr/bin/env python3
"""Developer Mode - MicroPython for Thonny on the P4 Launcher (launcher 0.33+).
    python apps/devmode/build.py   -> build/apps/devmode/devmode.papp

Needs the MicroPython source tree (v1.24.1) at ../../../micropython (C:/esp/micropython)
or in $MICROPY_TOP. Does what MicroPython's make rules do for a port (no make needed):
version header, qstr / module / root-pointer extraction with the RISC-V preprocessor,
then compiles the core + the port with papp_build."""
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "papp-sdk", "tools"))
import papp_build  # noqa: E402

MP = os.environ.get("MICROPY_TOP") or os.path.normpath(os.path.join(ROOT, "..", "micropython"))
if not os.path.isfile(os.path.join(MP, "py", "mpconfig.h")):
    sys.exit(f"MicroPython source not found at {MP} (git clone -b v1.24.1 https://github.com/micropython/micropython)")

OUT = os.path.join(ROOT, "build", "apps", "devmode")
GEN = os.path.join(OUT, "gen")
GENHDR = os.path.join(GEN, "genhdr")
os.makedirs(GENHDR, exist_ok=True)
PY = sys.executable

SOURCES = sorted(glob.glob(os.path.join(MP, "py", "*.c")))
SOURCES += [os.path.join(MP, "extmod", f) for f in (
    "modtime.c", "modbinascii.c", "modjson.c", "modre.c", "modrandom.c", "modheapq.c", "modplatform.c")
    if os.path.isfile(os.path.join(MP, "extmod", f))]
SOURCES += [os.path.join(MP, "shared", "runtime", f) for f in (
    "pyexec.c", "interrupt_char.c", "stdout_helpers.c", "sys_stdio_mphal.c", "gchelper_generic.c")]
SOURCES += [os.path.join(MP, "shared", "readline", "readline.c"),
            os.path.join(MP, "shared", "timeutils", "timeutils.c")]
SOURCES += [os.path.join(HERE, "port", f) for f in ("mphal.c", "modos.c", "modmachine.c")]
SOURCES += [os.path.join(HERE, "main.c")]

INCLUDES = [os.path.join(HERE, "port"), HERE, MP, GEN]
DEFS = ["-DNDEBUG", "-DFFCONF_H=\"lib/oofatfs/ffconf.h\""]

gcc = papp_build.find_tool("riscv32-esp-elf-gcc")
CFLAGS = papp_build.CFLAGS + ["-I" + i for i in INCLUDES] + DEFS + ["-std=gnu99"]


def run(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode:
        sys.exit(f"build: failed: {' '.join(str(c) for c in cmd[:6])} ...")
    return r


def gen_headers():
    run([PY, os.path.join(MP, "py", "makeversionhdr.py"), os.path.join(GENHDR, "mpversion.h")])
    last = os.path.join(GENHDR, "qstr.i.last")
    flags = CFLAGS + ["-DNO_QSTR"]
    run([PY, os.path.join(MP, "py", "makeqstrdefs.py"), "pp", gcc, "-E", "output", last,
         "cflags"] + flags + ["cxxflags", "sources"] + SOURCES + ["dependencies", "changed_sources"])
    for mode, out in (("qstr", "qstrdefs.collected.h"), ("module", "moduledefs.collected"),
                      ("root_pointer", "root_pointers.collected")):
        d = os.path.join(GENHDR, mode)
        os.makedirs(d, exist_ok=True)
        for f in glob.glob(os.path.join(d, "*")):
            os.remove(f)
        run([PY, os.path.join(MP, "py", "makeqstrdefs.py"), "split", mode, last, d, "_"])
        run([PY, os.path.join(MP, "py", "makeqstrdefs.py"), "cat", mode, "_", d, os.path.join(GENHDR, out)])
    with open(os.path.join(GENHDR, "moduledefs.h"), "wb") as f:
        run([PY, os.path.join(MP, "py", "makemoduledefs.py"), os.path.join(GENHDR, "moduledefs.collected")], stdout=f)
    with open(os.path.join(GENHDR, "root_pointers.h"), "wb") as f:
        run([PY, os.path.join(MP, "py", "make_root_pointers.py"), os.path.join(GENHDR, "root_pointers.collected")], stdout=f)
    # qstrdefs.generated.h: core + collected, through the preprocessor with Q(...) lines protected
    lines = []
    for p in (os.path.join(MP, "py", "qstrdefs.h"), os.path.join(GENHDR, "qstrdefs.collected.h")):
        for ln in open(p, encoding="utf-8").read().splitlines():
            lines.append('"%s"' % ln if ln.startswith("Q(") else ln)
    pre = run([gcc, "-E"] + CFLAGS + ["-"], input="\n".join(lines).encode(), capture_output=True).stdout.decode()
    out = []
    for ln in pre.splitlines():
        if ln.startswith('"Q(') and ln.endswith(')"'):
            ln = ln[1:-1]
        out.append(ln)
    prep = os.path.join(GENHDR, "qstrdefs.preprocessed.h")
    open(prep, "w", encoding="utf-8").write("\n".join(out) + "\n")
    with open(os.path.join(GENHDR, "qstrdefs.generated.h"), "wb") as f:
        run([PY, os.path.join(MP, "py", "makeqstrdata.py"), prep], stdout=f)


gen_headers()
# the core is compiled as MicroPython wants it (warnings of the upstream code are not ours)
papp_build.build("devmode", SOURCES, includes=INCLUDES, libc=True,
                 extra_cflags=DEFS + ["-std=gnu99", "-Wno-unused-function", "-Wno-unused-variable",
                                      "-Wno-maybe-uninitialized", "-fno-strict-aliasing"],
                 version_dir=HERE, icon_dir=HERE)
