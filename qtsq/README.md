# qtsq — Python package (`pip install qtsq`)

Read, write, and inspect `.qtsq` (Quantum Tensor Sequence) files in Python.
The C extension builds from source via `setup.py` (no prebuilt library needed).

## Install

```bash
pip install qtsq                  # core (numpy/scipy)
pip install "qtsq[checkpoint]"    # + torch, safetensors (training loops)
pip install "qtsq[ml]"            # + torch, torch-geometric, safetensors, ml_dtypes
pip install "qtsq[images]"        # + Pillow
```

Requires: Python ≥ 3.8, C compiler, zlib, `numpy`, `scipy`.

## Usage

```python
import qtsq

# ── Model checkpoint: full trained model, one file, bit-exact ──
qtsq.save_model(model.state_dict(), "llama.qtsq",
                metadata={"arch": "llama-7b"})          # no password needed
weights, meta = qtsq.load_model("llama.qtsq", framework="torch")
entries, meta, stats = qtsq.model_info("llama.qtsq")    # inspect, no decode

# Private file? Add a password (or use the device keystore).
qtsq.save_model(sd, "m.qtsq", password="secret")

# ── Training loop (auto-export weights + optimizer + files) ──
from qtsq.checkpoint import save_checkpoint, load_checkpoint

for epoch in range(epochs):
    loss = train_one_epoch(model, loader, optimizer)
    save_checkpoint(f"ckpt-e{epoch}.qtsq", model, optimizer=optimizer,
                    scheduler=scheduler, epoch=epoch,
                    metrics={"loss": float(loss.detach())},
                    extra_files={"tokenizer.json": "tok/tokenizer.json"})

info = load_checkpoint("ckpt-e5.qtsq", model, optimizer=optimizer)

# ── Single float32 tensor ──
qtsq.compress_tensor(w, "w.qtsq", precision="raw")      # bit-exact
a = qtsq.decompress_tensor("w.qtsq")                    # → numpy float32

# ── Text / JSON / CSV ──
qtsq.compress_text("hello", "t.qtsq")
qtsq.decompress_text("t.qtsq")

# ── Images ──
qtsq.compress("photo.png", "photo.qtsq", mode="hd")
img = qtsq.decompress("photo.qtsq")                     # numpy (H, W, 3)

# ── Mesh (for DTCN) ──
data = qtsq.read("photo.qtsq")
data.vertices     # (N, 5) — x, y, R, G, B
data.triangles    # (M, 3) — Delaunay indices
graph = data.to_graph(label=3)                          # needs torch-geometric
```

## Security model (weights)

Weight files default to sanitized + SHA-256-checksummed plaintext
(tamper-evident, safetensors-style) — no password friction. With a
password (or an initialized device keystore) the same files are fully
encrypted. Image/text/audio APIs remain password-required (fail-closed).

## Tests

```bash
python3.10 tests/test_model_py.py       # weights: exactness, dtypes, torch, sharding
python3.10 tests/test_checkpoint_py.py  # training loops: resume, optimizer, pruning
make test_model                         # C: RAW exact, array lens, container
```
