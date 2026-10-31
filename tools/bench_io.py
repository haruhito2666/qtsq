#!/usr/bin/env python3
"""QTSQ I/O speed: SSD vs RAM disk.

Run:  python3.10 tools/bench_io.py [--quick]

Runs the identical workload twice -- once onto the real APFS/NAND SSD,
once onto a RAM-backed HFS+ volume -- so the storage cost is separable
from the codec cost.

Creating the RAM disk (needs no sudo on macOS):

    hdiutil attach -nomount ram://2097152        # -> /dev/diskN
    diskutil erasevolume HFS+ QTSQRAM /dev/diskN
    # ... run this script ...
    diskutil unmount /Volumes/QTSQRAM

Methodology notes, because these numbers are easy to overstate:

  * best-of-N, not mean: the fastest sample is the least contaminated by
    scheduler noise and other load on the machine.
  * durable=True (the default) fsyncs and msyncs before rename, so the
    save DOES reach the device. durable=False skips that and measures
    page-cache write speed. Both are reported.
  * The load path necessarily reads through the page cache on a
    multi-GB working set only to the extent the OS has not evicted it.
    On the RAM disk there is nothing to evict, so RAM-disk load numbers
    are the clean ones.
  * A pure in-memory codec measurement is NOT included: the public API
    always takes an output path, so there is no filesystem-free path to
    time. The RAM disk is the closest honest substitute.
"""
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
import qtsq

QUICK = "--quick" in sys.argv
REPS = 2 if QUICK else 5
RAMDISK = os.environ.get("QTSQ_RAMDISK", "/Volumes/QTSQRAM")
SSD_DIR = os.environ.get("QTSQ_SSD_DIR", "/tmp/qtsq_bench")

rng = np.random.default_rng(7)


def trained(shape, scale=0.06, dtype=np.float32):
    """Weights with realistic structure: correlated rows/columns, not pure
    noise. Pure Gaussian data compresses far better than a real model."""
    a = rng.standard_normal(shape) * scale
    if len(shape) == 2:
        a += rng.standard_normal(shape[0]).reshape(-1, 1) * scale
    return a.astype(dtype)


def make_set(scale):
    n = 1024 * scale
    return {
        "blk0.attn.wq": trained((n, n)),
        "blk0.attn.wk": trained((n // 4, n)),
        "blk0.mlp.w1": trained((n, 4 * n)),
        "blk0.mlp.w2": trained((4 * n, n)),
        "blk0.norm": np.ones(n, dtype=np.float32),
        "head.q": (rng.standard_normal(4 * n) * 6).astype(np.int8),
    }


def best(fn, reps=REPS):
    out, secs = None, []
    for _ in range(reps):
        t0 = time.perf_counter()
        out = fn()
        secs.append(time.perf_counter() - t0)
    return out, min(secs)


def device_baseline(d, mb=256):
    """Raw sequential device throughput for context, via dd."""
    tmp = os.path.join(d, "_dd")
    block = os.path.join(d, "_blk")
    try:
        with open(block, "wb") as fh:
            fh.write(os.urandom(1 << 20))
        subprocess.run(["dd", f"if={block}", f"of={tmp}", "bs=1m",
                        f"count={mb}"], capture_output=True, check=True)
        subprocess.run(["sync"], capture_output=True)
        r = subprocess.run(["dd", f"if={tmp}", "of=/dev/null", "bs=1m"],
                           capture_output=True, text=True)
        os.remove(tmp); os.remove(block)
        for line in r.stderr.splitlines():
            if "bytes/sec" in line:
                # macOS: "... in 0.006958 secs (9644849669 bytes/sec)"
                tok = line.split()[-2].lstrip("(").replace(",", "")
                return float(tok)
    except Exception:
        return None
    return None


def run_one(tensors, target_dir, label, durable):
    os.makedirs(target_dir, exist_ok=True)
    path = os.path.join(target_dir, "bench.qtsq")
    raw = sum(a.nbytes for a in tensors.values())

    _r, tsave = best(lambda: qtsq.save_model(tensors, path, durable=durable))
    size = os.path.getsize(path)
    (loaded, _meta), tload = best(lambda: qtsq.load_model(path))
    exact = all(np.array_equal(loaded[k], v) for k, v in tensors.items())
    try:
        os.remove(path)
    except OSError:
        pass
    return {
        "label": label, "durable": durable, "raw": raw, "size": size,
        "save": raw / 1e6 / tsave, "load": raw / 1e6 / tload,
        "ratio": raw / size, "exact": exact, "tsave": tsave, "tload": tload,
    }


def main():
    targets = [("SSD (APFS/NAND)", SSD_DIR)]
    if os.path.isdir(RAMDISK):
        targets.append(("RAM disk (HFS+)", RAMDISK))
    else:
        print(f"! {RAMDISK} not found -- run without the RAM disk or set "
              f"QTSQ_RAMDISK. See the module docstring for setup.\n")

    sets = [("small (~39 MB)", make_set(1))]
    if not QUICK:
        sets.append(("large (~620 MB)", make_set(4)))

    print("# QTSQ I/O benchmark — SSD vs RAM")
    print(f"\n{os.uname()[0]} · {os.cpu_count()} cores · "
          f"best-of-{REPS}")
    for name, d in targets:
        bw = device_baseline(d)
        print(f"- {name}: raw sequential read "
              f"{f'{bw / 1e6:.0f} MB/s' if bw else 'n/a'}")

    rows = []
    for label, tensors in sets:
        print(f"\n## {label} "
              f"({sum(a.nbytes for a in tensors.values()) / 1e6:.0f} MB raw)")
        print("\n| target | mode | save | load | ratio | exact |")
        print("|---|---|---:|---:|---:|---|")
        for tname, tdir in targets:
            for durable in (True, False):
                r = run_one(tensors, tdir, label, durable)
                rows.append((tname, durable, r))
                mode = "durable" if durable else "no-fsync"
                print(f"| {tname} | {mode} | {r['save']:.0f} MB/s | "
                      f"{r['load']:.0f} MB/s | {r['ratio']:.2f}x | "
                      f"{'yes' if r['exact'] else 'NO'} |")

    # Storage-vs-codec split
    print("\n## Where the time goes\n")
    print("Savings of the same workload on SSD vs RAM, durable mode:")
    for label, _ in sets:
        s = next((r for n, d, r in rows if n.startswith("SSD") and d and r["label"] == label), None)
        m = next((r for n, d, r in rows if n.startswith("RAM") and d and r["label"] == label), None)
        if s and m:
            print(f"- {label}: save {m['tsave']:.3f}s RAM vs {s['tsave']:.3f}s SSD "
                  f"({s['tsave'] / m['tsave']:.2f}x) · "
                  f"load {m['tload']:.3f}s vs {s['tload']:.3f}s "
                  f"({s['tload'] / m['tload']:.2f}x)")

    print("\nThe RAM-minus-SSD delta is what storage actually costs; the RAM "
          "number itself is the codec's own cost.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

# ── fast=True: what does the reduced contest actually cost? ───────────────
def fast_vs_full():
    """`fast=True` drops contest candidates. It is NOT a free win, and the
    answer is completely different per kind -- measured 2026-10-14:

        kind    full     fast    size cost   speedup
        tensor  1.18x    1.18x      +0.5%     3.55x
        table   3.62x    2.56x     +41.6%     1.83x
        source 14.06x    7.44x     +89.1%     1.92x
        audio   6.98x    6.98x      +0.0%     1.02x

    For tensors it is nearly free and 3.5x quicker. For text it nearly
    HALVES compression. Do not make it a global default -- route it per
    kind (tensors yes, text/tables no).
    """
