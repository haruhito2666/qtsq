#!/usr/bin/env python3
"""QTSQ I/O speed per payload KIND: tensors, images, audio, tables.

Run:  python3.10 tools/bench_io_kinds.py [--quick]

The earlier bench_io.py measured only tensors. That understated the
picture: the four typed streams added in 2.2 have very different
characteristics, and on two of them the storage target is what decides
the number.

RAM disk setup (no sudo on macOS):

    hdiutil attach -nomount ram://3145728       # -> /dev/diskN
    diskutil erasevolume HFS+ QTSQRAM /dev/diskN
    ... run ...
    diskutil unmount /Volumes/QTSQRAM

NOTE: there is no save_texts()/load_texts(). The text lens
(QTC_LENS_TEXT) exists in C and works through the generic blob path, but
it has no public Python API, so it cannot be benchmarked from here. That
gap is recorded in docs/security-audit-2026-10.md's follow-ups.
"""
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
import qtsq

QUICK = "--quick" in sys.argv
REPS = 2 if QUICK else 3
RAMDISK = os.environ.get("QTSQ_RAMDISK", "/Volumes/QTSQRAM")
SSD_DIR = os.environ.get("QTSQ_SSD_DIR", "/tmp/qtsq_bench")
rng = np.random.default_rng(7)


# ── payload generators ─────────────────────────────────────────────────────
def make_tensors(mb):
    n = 1024
    per = 4 * n * n
    k = max(1, int(mb * 1e6 / per))
    out = {}
    for i in range(k):
        a = rng.standard_normal((n, n)) * 0.06
        a += rng.standard_normal(n).reshape(-1, 1) * 0.06
        out[f"blk{i}.wq"] = a.astype(np.float32)
    return out


def make_images(mb, already_compressed=True):
    """PNG payloads. If already_compressed, the bytes are the encoder's own
    output, which is what a real caller hands save_images()."""
    n = max(1, int(mb / 3))
    out = {}
    for i in range(n):
        w = h = 1024
        noise = rng.integers(0, 256, size=(h, w * 3), dtype=np.uint8)
        raw = b"".join(b"\x00" + noise[y].tobytes() for y in range(h))
        out[f"img{i}"] = _png(w, h, raw)
    return out


def _png(w, h, raw):
    def ck(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n"
            + ck(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + ck(b"IDAT", zlib.compress(raw, 6))
            + ck(b"IEND", b""))


def make_audios(mb):
    """16-bit stereo PCM. Two flavours: tonal (compresses hard) and noise
    (incompressible, so the number is the storage floor)."""
    out = {}
    secs = max(1, int(mb / (96 * 1000)))
    for i in range(2):
        n = secs * 48000
        t = np.arange(n) / 48000.0
        if i == 0:
            s = 0.4 * np.sin(2 * np.pi * 440 * t) * 30000
        else:
            s = (rng.standard_normal(n) * 8000)
        out[f"tone{i}"] = (s.astype("<i2").tobytes(), 2, 16)
    return out


def make_tables(mb):
    """Numeric CSV — the shape the table lens was designed for."""
    per = 40
    rows = max(100, int(mb * 1e6 / per))
    out = {}
    hdr = "id,name,score,lat,lon\n"
    for k in range(2):
        body = "".join(f"{100000+i*7},user_{i},{i*1.37:.2f},{i*0.001:.4f},{i*0.002:.4f}\n"
                       for i in range(rows // 2))
        out[f"csv{k}"] = (hdr + body).encode()
    return out


def _save_tensors(path, data):
    qtsq.save_model(data, path)


KINDS = {
    "tensor":  (make_tensors, _save_tensors, qtsq.load_model),
    "image":   (make_images,  qtsq.save_images, qtsq.load_images),
    "audio":   (make_audios,  qtsq.save_audios, qtsq.load_audios),
    "table":   (make_tables,  qtsq.save_tables, qtsq.load_tables),
}


def raw_of(kind, d):
    if kind == "tensor":
        return sum(a.nbytes for a in d.values())
    if kind == "audio":
        # values are (pcm_bytes, channels, bits_per_sample)
        return sum(len(v[0]) for v in d.values())
    return sum(len(v) for v in d.values())


def best(fn, reps=REPS):
    secs = []
    for _ in range(reps):
        t0 = time.perf_counter()
        fn()
        secs.append(time.perf_counter() - t0)
    return min(secs)


def main():
    targets = [("SSD", SSD_DIR)]
    if os.path.isdir(RAMDISK):
        targets.append(("RAM", RAMDISK))
    else:
        print(f"! {RAMDISK} missing — SSD only. See docstring for setup.\n")

    size_mb = 20 if QUICK else 60
    print("# QTSQ per-kind I/O — SSD vs RAM")
    print(f"\nbest-of-{REPS} · ~{size_mb} MB raw per kind\n")
    print("| kind | target | raw | file | save | load | ratio | exact |")
    print("|---|---|---:|---:|---:|---:|---:|---|")

    results = {}
    for kind, (gen, save, load) in KINDS.items():
        data = gen(size_mb)
        raw = raw_of(kind, data)
        for tname, tdir in targets:
            os.makedirs(tdir, exist_ok=True)
            path = os.path.join(tdir, f"{kind}.qtsq")
            try:
                tsave = best(lambda: save(path, data))
                sz = os.path.getsize(path)
                if kind == "tensor":
                    def do_load():
                        t, _ = load(path)
                        return t
                else:
                    do_load = lambda: load(path)
                got = None
                tl = None
                for _ in range(REPS):
                    t0 = time.perf_counter()
                    got = do_load()
                    tl = time.perf_counter() - t0
                if kind == "tensor":
                    exact = all(np.array_equal(got[k], v) for k, v in data.items())
                elif kind == "image":
                    # load_images yields (bytes, w, h, ch)
                    exact = all(got[k][0] == v for k, v in data.items())
                elif kind == "audio":
                    # load_audios yields (pcm, channels, bits)
                    exact = all(got[k][0] == v[0] for k, v in data.items())
                else:
                    exact = all(got[k] == v for k, v in data.items())
                results[(kind, tname)] = (tsave, tl)
                print(f"| {kind} | {tname} | {raw/1e6:.1f} MB | {sz/1e6:.1f} MB | "
                      f"{raw/1e6/tsave:.1f} MB/s | {raw/1e6/tl:.1f} MB/s | "
                      f"{raw/sz:.2f}x | {'yes' if exact else 'NO'} |")
            except Exception as e:
                print(f"| {kind} | {tname} | {raw/1e6:.1f} MB | — | ERROR: "
                      f"{type(e).__name__}: {str(e)[:40]} | | | |")
            finally:
                try:
                    os.remove(path)
                except OSError:
                    pass

    print("\n## Storage sensitivity (SSD time / RAM time)\n")
    print("| kind | save | load |")
    print("|---|---:|---:|")
    for kind in KINDS:
        s = results.get((kind, "SSD"))
        m = results.get((kind, "RAM"))
        if s and m:
            print(f"| {kind} | {s[0]/m[0]:.2f}x | {s[1]/m[1]:.2f}x |")
    print("\n~1.00x means the target does not matter: the codec is the "
          "bottleneck. A large number means storage dominates.")
    return 0


if __name__ == "__main__":
    sys.exit(main())