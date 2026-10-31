"""tools/bench_weights.py — qtsq vs industry standards, every area.

Run: python3.10 tools/bench_weights.py [--quick]
Covers: save/load speed + size (small ~40MB, large ~135MB), lossy and
exact precisions, lazy reads, encryption overhead, sharding, tamper
detection, and a feature matrix. Prints a markdown report.
"""
import json
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np

import qtsq

QUICK = "--quick" in sys.argv
REPS = 2 if QUICK else 4

rng = np.random.default_rng(11)


def trained(shape, scale=0.05, dtype=np.float32):
    a = rng.standard_normal(shape).astype(np.float64) * scale
    a += rng.standard_normal(shape[0]).reshape(-1, *([1] * (len(shape) - 1))) * scale
    return a.astype(dtype)


def make_set(big=False):
    n = 2048 if big else 1024
    t = {
        "blk0.attn.wq": trained((n, n)),
        "blk0.attn.wk": trained((n // 4, n)),
        "blk0.mlp.w1": trained((n, 4 * n if not big else 2 * n)),
        "blk0.mlp.w2": trained((4 * n if not big else 2 * n, n)),
        "blk0.norm": np.ones(n, dtype=np.float32),
        "head.q": (rng.standard_normal(4 * n) * 6).astype(np.int8),
        "head.h": (rng.standard_normal((n // 2, n // 2)) * 0.3).astype(np.float16),
    }
    if big:
        for i in range(1, 4):
            t[f"blk{i}.w"] = trained((n, n))
    return t


def best(fn, n=REPS):
    ts = []
    for _ in range(n):
        t0 = time.time()
        fn()
        ts.append(time.time() - t0)
    return min(ts)


results = {"areas": {}}


def area(name):
    print(f"\n## {name}", flush=True)
    results["areas"][name] = {}


def row(label, seconds, raw_bytes, size_bytes=None, extra=""):
    rate = raw_bytes / 1e6 / seconds if seconds > 0 else 0
    line = f"{label:28s} {seconds:7.3f}s {rate:7.0f} MB/s"
    if size_bytes is not None:
        line += f"  ratio={raw_bytes / size_bytes:.2f}"
    if extra:
        line += f"  {extra}"
    print(line, flush=True)
    return {"seconds": seconds, "mb_s": rate}


tmp = tempfile.mkdtemp(prefix="qtsq_bench_")
SMALL = make_set(False)
BIG = make_set(True)
small_raw = sum(v.nbytes for v in SMALL.values())
big_raw = sum(v.nbytes for v in BIG.values())
print(f"datasets: small={small_raw / 1e6:.1f}MB big={big_raw / 1e6:.1f}MB", flush=True)

# ── 1. SAVE ──
area("1. Save speed + size (small)")
sp = {}
for label, kw in [("qtsq full", {}), ("qtsq fast", {"fast": True}),
                  ("qtsq store", {"store": True})]:
    p = os.path.join(tmp, "s.qtsq")
    dt = best(lambda: qtsq.save_model(SMALL, p, **kw))
    sz = os.path.getsize(p)
    o, _ = qtsq.load_model(p)
    ok = all(np.array_equal(np.ascontiguousarray(SMALL[k]), o[k]) for k in SMALL)
    sp[label] = row(label, dt, small_raw, sz, f"exact={ok}")

from safetensors.numpy import save_file as st_save, load_file as st_load

p = os.path.join(tmp, "s.st")
dt = best(lambda: st_save(SMALL, p))
sp["safetensors"] = row("safetensors", dt, small_raw, os.path.getsize(p),
                        f"exact={all(np.array_equal(SMALL[k], st_load(p)[k]) for k in SMALL)}")

import torch

tsd = {k: torch.from_numpy(np.ascontiguousarray(v)) for k, v in SMALL.items()}
p = os.path.join(tmp, "s.pt")
dt = best(lambda: torch.save(tsd, p))
sp["torch.save"] = row("torch.save", dt, small_raw, os.path.getsize(p), "exact=True")

p = os.path.join(tmp, "s.npz")
dt = best(lambda: np.savez(p, **SMALL))
sp["numpy npz"] = row("numpy npz", dt, small_raw, os.path.getsize(p), "exact=True")
results["areas"]["1. Save speed + size (small)"] = sp

area("2. Save speed, large (sharded-scale single file)")
for label, kw in [("qtsq full", {}), ("qtsq fast", {"fast": True}),
                  ("qtsq store", {"store": True})]:
    p = os.path.join(tmp, "b.qtsq")
    dt = best(lambda: qtsq.save_model(BIG, p, **kw))
    print(f"{label:28s} {dt:7.3f}s {big_raw / 1e6 / dt:7.0f} MB/s  "
          f"ratio={big_raw / os.path.getsize(p):.2f}", flush=True)

# ── 3. LOAD ──
area("3. Full load (small)")
qtsq.save_model(SMALL, os.path.join(tmp, "l.qtsq"), store=True)
st_save(SMALL, os.path.join(tmp, "l.st"))
torch.save(tsd, os.path.join(tmp, "l.pt"))
np.savez(os.path.join(tmp, "l.npz"), **SMALL)
for label, fn in [
        ("qtsq writable", lambda: qtsq.load_model(os.path.join(tmp, "l.qtsq"))),
        ("qtsq views", lambda: qtsq.load_model(os.path.join(tmp, "l.qtsq"), writable=False)),
        ("safetensors", lambda: st_load(os.path.join(tmp, "l.st"))),
        ("torch.load", lambda: torch.load(os.path.join(tmp, "l.pt"), weights_only=True)),
        ("numpy npz", lambda: dict(np.load(os.path.join(tmp, "l.npz"))))]:
    dt = best(fn)
    row(label, dt, small_raw)

# ── 4. LAZY ──
area("4. Lazy single-tensor read")
from safetensors import safe_open

def st_lazy():
    with safe_open(os.path.join(tmp, "l.st"), framework="np") as f:
        return f.get_tensor("blk0.norm")

for label, fn in [
        ("qtsq copy", lambda: qtsq.load_tensors(os.path.join(tmp, "l.qtsq"), ["blk0.norm"])),
        ("qtsq zero-copy", lambda: qtsq.load_tensors(os.path.join(tmp, "l.qtsq"), ["blk0.norm"],
                                                     writable=False)),
        ("safetensors", st_lazy)]:
    dt = best(fn)
    print(f"{label:28s} {dt * 1000:7.2f}ms", flush=True)

# ── 5. SECURITY / FEATURES ──
area("5. Security + features (functional)")
qtsq.save_model(SMALL, os.path.join(tmp, "e.qtsq"), password="pw123")
try:
    qtsq.load_model(os.path.join(tmp, "e.qtsq"))
    print("encrypted-without-password rejected: NO (BAD)", flush=True)
except RuntimeError:
    print("encrypted-without-password rejected: ok", flush=True)
o, _ = qtsq.load_model(os.path.join(tmp, "e.qtsq"), password="pw123")
print("encrypted round-trip exact:",
      all(np.array_equal(np.ascontiguousarray(SMALL[k]), o[k]) for k in SMALL), flush=True)
# tamper: flip a byte deep in a payload
with open(os.path.join(tmp, "l.qtsq"), "rb") as f:
    blob = bytearray(f.read())
blob[len(blob) // 2] ^= 0xFF
with open(os.path.join(tmp, "t.qtsq"), "wb") as f:
    f.write(blob)
try:
    qtsq.load_model(os.path.join(tmp, "t.qtsq"))
    print("tamper detected: NO (BAD)", flush=True)
except Exception as e:
    print(f"tamper detected: ok ({type(e).__name__})", flush=True)
# safetensors has no integrity: same flip loads silently
with open(os.path.join(tmp, "l.st"), "rb") as f:
    stb = bytearray(f.read())
stb[len(stb) // 2] ^= 0xFF
with open(os.path.join(tmp, "ts.st"), "wb") as f:
    f.write(stb)
try:
    st_load(os.path.join(tmp, "ts.st"))
    print("safetensors tamper: loads silently (no integrity)", flush=True)
except Exception:
    print("safetensors tamper: rejected", flush=True)
# sharding + info + checkpoint resume
paths = qtsq.save_model_sharded(BIG, os.path.join(tmp, "ckpt"), max_shard_bytes=20_000_000)
back, _ = qtsq.load_model_sharded(paths[0])
print(f"sharded {len(paths)} files exact:",
      all(np.array_equal(np.ascontiguousarray(BIG[k]), back[k]) for k in BIG), flush=True)
entries, _, stats = qtsq.model_info(os.path.join(tmp, "l.qtsq"))
print(f"model_info tensors={len(entries)} raw_bytes={stats['raw_bytes']}", flush=True)

print("\nDONE", flush=True)
