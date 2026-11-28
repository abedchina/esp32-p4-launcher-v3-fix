"""Minimal DevLink client (read-mostly): info, ls, stat, bootlog, get, put, rm, reboot, shot."""
import json, struct, sys, time, zlib
import serial

SOF = b"\xA5\x44"
T = dict(PING=1, INFO=2, LS=0x10, STAT=0x11, MKDIR=0x12, RM=0x13, PUT_BEGIN=0x20, PUT_DATA=0x21,
         PUT_END=0x22, GET=0x28, STATUS=0x32, REBOOT=0x40, BOOTLOG=0x43, SHOT=0x50)


class DL:
    def __init__(self, port):
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.2
        self.s.dtr = False  # don't reset the chip on open (USB-Serial-JTAG)
        self.s.rts = False
        self.s.open()
        self.seq = 1
        time.sleep(0.3)
        self.s.reset_input_buffer()

    def req(self, t, payload=b"", timeout=8.0):
        seq = self.seq; self.seq = (self.seq % 65000) + 1
        body = struct.pack("<BBHI", t, 0, seq, len(payload)) + payload
        self.s.write(SOF + body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF))
        buf = b""; end = time.time() + timeout
        while time.time() < end:
            buf += self.s.read(4096)
            while True:
                i = buf.find(SOF)
                if i < 0 or len(buf) - i < 10: break
                rt, fl, rs, ln = struct.unpack_from("<BBHI", buf, i + 2)
                if ln > 40000: buf = buf[i + 2:]; continue
                if len(buf) - i < 10 + ln + 4: break
                pl = buf[i + 10:i + 10 + ln]
                buf = buf[i + 14 + ln:]
                if rt == (t | 0x80) and rs == seq:
                    if fl & 1: raise RuntimeError(pl.decode("utf-8", "replace"))
                    return pl
        raise TimeoutError("no reply")


def main():
    port, cmd, *args = sys.argv[1:]
    d = DL(port)
    if cmd == "info": print(d.req(T["INFO"]).decode("utf-8", "replace"))
    elif cmd == "ls": print(d.req(T["LS"], args[0].encode()).decode("utf-8", "replace"))
    elif cmd == "stat": print(d.req(T["STAT"], args[0].encode()).decode("utf-8", "replace"))
    elif cmd == "bootlog": print(d.req(T["BOOTLOG"]).decode("utf-8", "replace"))
    elif cmd == "status": print(d.req(T["STATUS"]).decode("utf-8", "replace"))
    elif cmd == "get":
        out = bytearray(); off = 0
        while True:
            r = d.req(T["GET"], struct.pack("<II", off, 16384) + args[0].encode())
            size = struct.unpack_from("<I", r)[0]; out += r[4:]; off += len(r) - 4
            if off >= size or len(r) <= 4: break
        open(args[1], "wb").write(out); print("got", len(out))
    elif cmd == "put":
        data = open(args[0], "rb").read()
        d.req(T["PUT_BEGIN"], struct.pack("<II", len(data), zlib.crc32(data) & 0xFFFFFFFF) + args[1].encode())
        for o in range(0, len(data), 16384): d.req(T["PUT_DATA"], data[o:o + 16384])
        d.req(T["PUT_END"], timeout=20); print("put", len(data))
    elif cmd == "input": print(d.req(0x51, b"\x03").decode("utf-8", "replace"))
    elif cmd == "watch":
        secs = float(args[0]) if args else 30; prev = None; end = time.time() + secs
        while time.time() < end:
            s = d.req(0x51, b"\x03").decode("utf-8", "replace")
            if s != prev: print(time.strftime("%H:%M:%S"), s, flush=True); prev = s
            time.sleep(0.05)
    elif cmd == "mkdir": d.req(T["MKDIR"], args[0].encode()); print("mkdir")
    elif cmd == "run": print(d.req(0x30, b"".join(a.encode() + bytes([0]) for a in args), timeout=15).decode("utf-8", "replace"))
    elif cmd == "stop": print(d.req(0x31, struct.pack("<I", 1000), timeout=10).decode("utf-8", "replace"))
    elif cmd == "shot":
        import array
        data = bytearray(); total = None
        while total is None or len(data) < total:
            r = d.req(T["SHOT"], struct.pack("<II", len(data), 16384), timeout=10)
            w, h, fmt, rot, total = struct.unpack_from("<HHHHI", r); data += r[12:]
            if len(r) <= 12 and len(data) < total: raise RuntimeError("truncated")
        out = array.array("H"); i = 0
        while i < len(data) and len(out) < w * h:
            (n,) = struct.unpack_from("<H", data, i); i += 2
            if n & 0x8000: (v,) = struct.unpack_from("<H", data, i); i += 2; out.extend([v] * (n & 0x7FFF))
            else: out.frombytes(bytes(data[i:i + 2 * n])); i += 2 * n
        W, H = (h, w) if rot in (90, 270) else (w, h)
        from PIL import Image
        img = Image.new("RGB", (W, H)); pxl = img.load()
        for y in range(H):
            for x in range(W):
                v = out[x * w + (H - 1 - y)] if rot == 270 else out[(W - 1 - x) * w + y] if rot == 90 else out[y * w + x]
                pxl[x, y] = (((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)
        img.save(args[0]); print("shot", W, H, rot)
    elif cmd == "rm": d.req(T["RM"], args[0].encode()); print("removed")
    elif cmd == "crash": print(d.req(0x41).decode("utf-8", "replace"))
    elif cmd == "crashclear": print(d.req(0x42).decode("utf-8", "replace"))
    elif cmd == "runlog":                      # runlog <secs> <papp> [args...]: run, then print the log frames
        secs = float(args[0])
        print(d.req(0x30, b"".join(a.encode() + bytes([0]) for a in args[1:]), timeout=15).decode("utf-8", "replace"))
        buf = b""; end = time.time() + secs
        while time.time() < end:
            buf += d.s.read(4096)
            while True:
                i = buf.find(SOF)
                if i < 0 or len(buf) - i < 10: break
                rt, fl, rs, ln = struct.unpack_from("<BBHI", buf, i + 2)
                if ln > 40000: buf = buf[i + 2:]; continue
                if len(buf) - i < 14 + ln: break
                pl = buf[i + 10:i + 10 + ln]; buf = buf[i + 14 + ln:]
                if rt in (0x70, 0x71): print(("APP " if fl & 2 else "") + pl.decode("utf-8", "replace").rstrip(), flush=True)
    elif cmd == "reboot":
        try: d.req(T["REBOOT"], timeout=2)
        except Exception: pass
        print("reboot sent")


if __name__ == "__main__":
    main()
