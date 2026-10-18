# QTSQ — Quantum Tensor Sequence

A high-performance binary format for images, text, audio, video, AI tensors, and complete neural brain states.

```
pip install qtsq
```

---

## Quick Start

```python
import qtsq
```

---

## Images

```python
# Compress an image → .qtsq
qtsq.compress("photo.png", "photo.qtsq")
qtsq.compress("photo.png", "photo.qtsq", mode="hd")         # high quality
qtsq.compress("photo.png", "photo.qtsq", mode="lossless")    # near-lossless

# Decompress → numpy array
img = qtsq.decompress("photo.qtsq")    # → numpy (H, W, 3) uint8

# View in window or save
qtsq.render("photo.qtsq")              # opens interactive viewer
qtsq.render("photo.qtsq", "out.png")   # saves to file
```

### Compression Modes

| Mode | Strategy | Best For |
|------|----------|----------|
| `"default"` | Triangle + Splat | General purpose |
| `"hd"` | Triangle + Splat (dense) | High quality output |
| `"lossless"` | Triangle + Splat | Near-lossless preservation |
| `"splat"` | Gaussian Splats only | Smooth images, gradients |
| `"splat_hd"` | HD Splats | High quality smooth regions |
| `"triangle"` | Mesh only | Sharp edges, diagrams |
| `"triangle_pixel"` | 1 vertex/pixel | ML training data |
| `"hd_ionic"` | HD + Ionic I/O | Maximum speed + quality |

---

## Text

```python
# Compress
qtsq.compress_text("Hello, world!", "hello.qtsq")

# Decompress
text = qtsq.decompress_text("hello.qtsq")
print(text)  # "Hello, world!"
```

### JSON

`qtsq.compress_json` automatically minifies your JSON (stripping excess spaces and newlines) before applying dictionary compression, making it highly efficient.

```python
import json

data = {"model": "Shochi", "type": "neuromorphic", "dopamine": 0.8}
json_str = json.dumps(data, indent=4)  # 73 bytes

# Compress
qtsq.compress_json(json_str, "data.qtsq")

# Decompress (returns minified JSON string)
recovered = qtsq.decompress_text("data.qtsq")
print(recovered)  # '{"model":"Shochi","type":"neuromorphic","dopamine":0.8}'
```

### CSV

```python
csv_data = "id,name,age\n1,Alice,30\n2,Bob,25\n"
qtsq.compress_csv(csv_data, "data.qtsq")

recovered_csv = qtsq.decompress_text("data.qtsq")
```

---

## Audio

qtsq audio is **stereo-only**. Mono costs the same space as the stereo
pair — the mid/side coder needs both channels to decorrelate — so mono
streams are rejected rather than stored at half value.

### Lossless (Python)

`save_audios()` codes raw interleaved PCM with the qtc audio lens,
bit-exact, measured at roughly 7–50x on speech-grade material depending
on content. 8/16/24-bit samples are accepted; channels must be exactly 2.

```python
import numpy as np
import qtsq

# Each entry is (pcm_bytes, channels, bits_per_sample); channels must be 2.
pcm = np.zeros((44100, 2), np.int16).tobytes()        # 1 s stereo
qtsq.save_audios("clip.qtsq", {"room": (pcm, 2, 16)})

audios = qtsq.load_audios("clip.qtsq")   # {name: (pcm, channels, bits)}
```

`load_audios()` returns samples exactly as stored, so a round-trip is
bit-exact. This path does not record a sample rate in the manifest
(`"sample_rate": 0`); carry it in `metadata` if downstream code needs it.

### Perceptual, MP3-comparable (C / QTSQStudio)

`qtsq_compress_audio_float()` with `ctx.quality > 0` selects the MDCT2
perceptual coder at a target bitrate — the same path QTSQStudio uses for
capture. It measured at MP3 parity (±1 dB PSNR against LAME 3.100 at
matched bitrate) on 48 kHz stereo.

| `ctx.quality` | Target | Payload on a 220.8 s reference |
|---|---|---|
| 1.0 | 320 kbps | 8.83 MB |
| 0.85 | 192 kbps | 5.30 MB |
| 0.5 | 128 kbps | 3.53 MB |
| 0.25 | 96 kbps | 2.65 MB |

At 128 kbps the result is 3.54 MB against a 3.59 MB source MP3, i.e. the
size you get from an MP3 encoder at that rate. Lossy streams set
`QTSQ_FLAG_LOSSY`; their SHA-256 covers the original floats, so readers
skip checksum verification for them.

```c
#include "qtsq.h"
#include "qtsq_vault.h"

qtsq_context_t ctx;
qtsq_init(&ctx);
ctx.quality = 0.5f;                      /* 128 kbps */

qtsq_compress_audio_float(&ctx, pcm_f32, nframes * 2, 44100, 2);

qtsq_vault_encrypt_password(&ctx, "passphrase");   /* writes are encrypted */
qtsq_write(&ctx, "song.qtsq");
```

Read it back with `qtsq_decompress_audio_float()` (interleaved float32,
already at the playback engine's native format) or the generic
`qtsq_decompress()`, which routes `sample_depth == 32` audio to the float
decoder. Decoding returns 32-bit floats, not int16.

A lossless spectral-dictionary codec (`qtsq_compress_audio`, strategy
`Fourier`) exists and still decodes old files, but it is not used for new
captures: it is lossless, and on already-compressed sources such as MP3
it has no redundancy left to exploit — it expanded a 3.4 MB MP3 to
16.1 MB.

---

## Tensors (AI Weights & Embeddings)

Store neural network weights with configurable precision. Smaller precision = smaller file, slightly less accuracy.
No password needed for weights: files are sanitized + SHA-256 checksummed
plaintext by default (tamper-evident, like safetensors); pass `password`
for vault encryption, or rely on the device keystore when initialized.

```python
import numpy as np
import qtsq

# Create some weights
weights = np.random.randn(10_000_000).astype(np.float32) * 0.1

# Save at different precisions (password optional)
qtsq.compress_tensor(weights, "weights_raw.qtsq", precision="raw")  # bit-exact
qtsq.compress_tensor(weights, "weights_f16.qtsq", precision="f16")  # 2x smaller, lossy
qtsq.compress_tensor(weights, "weights_f8.qtsq",  precision="f8")   # 6x smaller, lossy
qtsq.compress_tensor(weights, "weights_f4.qtsq",  precision="f4")   # 7x smaller, lossy

# Speed levels (all bit-exact): default full contest, fast=True
# (sample-gated subset), store=True (verbatim, ~600–1600 MB/s, ratio
# ~1.0). durable=False skips the fsync for max speed (power-loss window).
qtsq.compress_tensor(weights, "w_fast.qtsq", precision="raw", fast=True)
qtsq.compress_tensor(weights, "w_store.qtsq", precision="raw", store=True)

# Private file? Add a password (or use the device keystore).
qtsq.compress_tensor(weights, "weights_raw.qtsq", precision="raw", password="secret")

# Load (always returns float32 regardless of storage precision)
loaded = qtsq.decompress_tensor("weights_f16.qtsq")
print(loaded.shape, loaded.dtype)  # (10000000,) float32
```

> `"f32"` is the legacy delta-quantized path (~1e-8 error, kept for backward
> compatibility). For exact weights — training checkpoints, retraining —
> use `"raw"`, which is bit-exact (NaN/Inf bits preserved).

### Multi-dimensional tensors

```python
# Shape is preserved automatically
matrix = np.random.randn(768, 3072).astype(np.float32)
qtsq.compress_tensor(matrix, "layer.qtsq", precision="f16")

loaded = qtsq.decompress_tensor("layer.qtsq")
print(loaded.shape)  # (768, 3072)
```

### Precision Guide

| Precision | Bits | Compression | Max Error | Best For |
|-----------|------|-------------|-----------|----------|
| `"raw"` | 32 | ~1:1 | 0 (bit-exact) | Training checkpoints, retraining, critical weights |
| `"f32"` | 32 | ~1:1 | ~1e-8 (legacy delta) | Backward compat only — prefer `"raw"` |
| `"f16"` | 16 | ~2:1 | ~1e-4 | Inference, fine-tuning |
| `"f8"` | 8 | ~6:1 | ~1e-2 | Edge/mobile deployment |
| `"f4"` | 4 | ~7:1 | ~1e-2 | LLM inference (Q4_0-like, lossy) |

### Precision Constants

```python
qtsq.TENSOR_F32   # "f32" — legacy delta (lossy ~1e-8, compat only)
qtsq.TENSOR_F16   # "f16"
qtsq.TENSOR_F8    # "f8"
qtsq.TENSOR_F4    # "f4"
qtsq.TENSOR_RAW   # "raw" — bit-exact, use for weights
```

---

## Model Checkpoint (full trained model in one `.qtsq` file)

`save_model` / `load_model` store every tensor of a trained model — any
dtype (`float32`, `float16`, `bfloat16`, `float64`, `complex64/128`,
`int8`/`uint8` … `int64`/`uint64`, `bool`, packed `int4`/`uint4`,
`float8_e5m2` bits), any shape — bit-exact in a single `.qtsq` file,
with a JSON manifest and optional metadata. No password needed by
default (sanitized + checksummed plaintext, like safetensors).

```python
import qtsq

# PyTorch state_dict (or nn.Module, or dict of numpy arrays) — no password needed
qtsq.save_model(model.state_dict(), "llama.qtsq",
                metadata={"arch": "llama-7b", "epochs": 3})

# Bit-exact reload as torch tensors (or framework="numpy")
weights, meta = qtsq.load_model("llama.qtsq", framework="torch")

# Inspect without loading weights
entries, meta, stats = qtsq.model_info("llama.qtsq")

# Private file? Add a password (or use the device keystore).
qtsq.save_model(model.state_dict(), "llama.qtsq", password="secret")
```

### Training-loop checkpoints

Two lines in the loop export weights, optimizer, scheduler, metrics,
config, and related files (tokenizer, configs) straight to `.qtsq`:

```python
from qtsq.checkpoint import save_checkpoint, load_checkpoint

for epoch in range(epochs):
    loss = train_one_epoch(model, loader, optimizer)
    save_checkpoint(f"ckpt-e{epoch}.qtsq", model, optimizer=optimizer,
                    scheduler=scheduler, epoch=epoch,
                    metrics={"loss": float(loss.detach())},
                    config={"arch": "my-model"},
                    extra_files={"tokenizer.json": "tok/tokenizer.json"})

# resume in a fresh process
info = load_checkpoint("ckpt-e5.qtsq", model, optimizer=optimizer)
start_epoch = info["epoch"] + 1   # + metrics, config, files
```

Or use `qtsq.CheckpointSaver` (epoch hook with `every`/`keep_last`
pruning and auto-sharding past 1 GiB). HuggingFace Trainer
(`HFQtsqCallback`) and Lightning (`LightningQtsqCallback`) adapters are
built in when those packages are installed. Install via
`pip install qtsq` (or `pip install qtsq[checkpoint]` with torch).

### Custom architectures & inference engines

Any framework works — anything array-like (`__array__`/`__dlpack__`:
JAX, TensorFlow-eager, MLX, custom autograd) stores directly:

```python
qtsq.save_model({"w": jax_array, "b": tf_tensor}, "custom.qtsq")
```

Custom inference engines load only what they need (no full-model decode):

```python
 inflow, _ = qtsq.load_tensors("llama.qtsq", ["blk0.attn.wq", "blk0.mlp.w1"])
```

`load_checkpoint(..., include=[...])` does the same for training files
(weights-only inference skips optimizer streams). ONNX bridges move
weights between `.onnx` initializers and `.qtsq` for ORT/TensorRT flows:

```python
from qtsq.onnx_bridge import import_onnx_weights, export_onnx_weights
import_onnx_weights("model.onnx", "model.qtsq")            # any arch
export_onnx_weights("trained.qtsq", "arch.onnx", "out.onnx")  # needs onnx
```

C/C++ engines link `libqtsq.a`: `qtsq_read` →
`qtsq_container_get_stream` (by index; match names via `model_info`-style
manifest parsing) → `qtsq_decompress_tensor` / `qtsq_decompress_array`.
The `demos/` directory builds against the same public headers and shows
working call sequences — build them all with `make demos`.

Measured on Apple Silicon (25–39 MB mixed fp32/fp16/int8; varies with
load): full save ~20–45 MB/s, `fast=True` ~55–115 MB/s at matched ratio
(sample-gated single winner; usually marginally larger, always
bit-exact), `store=True` ~600–1600 MB/s at ratio ~1.0 (mmap output,
zero-copy inputs, parallel hardware hashing — still bit-exact and
tamper-evident, just not compressed); load ~300–470 MB/s, views and
lazy reads faster (no copies).
torch inputs convert via numpy bridges, not per-byte iteration.
Subset reads (`load_tensors`, `writable=False` views) fault only
requested streams via mmap (name-first TOC walk, no payload copies):
2–25 ms for one tensor out of a 134 MB file vs seconds for a full
read. Plaintext saves stream straight to disk (no pack buffer).

HuggingFace Hub: `from qtsq import hub; hub.push("user/model",
"m.qtsq", config=..., tokenizer_files=...)` and `hub.pull(...)`
(`pip install qtsq[hub]`).

Compression runs on the custom qtc engine — and nothing else (zlib is
retired on weights paths; old zlib files still read). float32 weights
use the lossless planes + XOR-delta contest with per-plane coding;
every other dtype (fp16/bf16 bits, int/uint, packed int4, fp8 bits)
runs the array lens (transpose + delta + nibble-split contest,
per-plane) — always bit-exact, never worse than raw.

Limits: ≤65,536 tensors per file (shard beyond that), stream names
≤250 bytes. Single tensors have no unit-count limit (chunked mode 9
past qtc's 2³⁰ cap — verified with 268M units, 1.57x, exact); peak RAM
scales with tensor size, so shard giant models. Shapes of any rank
round-trip via the manifest.

Single `float32` tensors still use `compress_tensor(..., precision="raw")` /
`decompress_tensor` directly.

Extra dtype tags (exact, packed): `(int8_values, "int4")` with values in
`[-8, 7]`, `(int8_values, "uint4")` in `[0, 15]`, and `(uint8_bits,
"float8_e5m2")` for pre-quantized FP8-E5M2 — e.g.
`qtsq.save_model({"q": (q_int8, "int4")}, "m.qtsq", password="secret")`.

Large models: `save_model_sharded(tensors, "ckpt", password="secret",
max_shard_bytes=1<<30)` writes `ckpt-00001-of-000NN.qtsq`… (each shard
carries the full manifest, nothing unencrypted); `load_model_sharded`
reloads from any shard or the prefix. safetensors bridges:
`export_safetensors` / `import_safetensors` (requires `pip install
safetensors`).

---

## AI Brain State

Save and load a complete AI brain — weights, biases, memory vectors, and metadata — in a single `.qtsq` file.

```python
import qtsq
import json
import numpy as np

# ── Save ──
weights  = np.random.randn(1_000_000).astype(np.float32) * 0.1
biases   = np.random.randn(1000).astype(np.float32) * 0.01
memories = np.random.randn(100, 128).astype(np.float32)  # 100 memories, 128-dim each

metadata = json.dumps({
    "age": 5000,
    "stage": "child",
    "dopamine": 0.6,
    "cortisol": 0.2,
    "curiosity": 0.8,
    "confidence": 0.7,
})

qtsq.brain_save(
    "brain.qtsq",
    weights=weights,
    biases=biases,
    memories=memories.flatten(),
    vector_dim=128,
    metadata_json=metadata,
)

# ── Load ──
state = qtsq.brain_load("brain.qtsq")

print(state["weights"].shape)      # (1000000,)    — numpy float32
print(state["biases"].shape)       # (1000,)       — numpy float32
print(state["memories"].shape)     # (100, 128)    — numpy float32
print(state["num_memories"])       # 100
print(state["vector_dim"])         # 128

meta = json.loads(state["metadata"])
print(meta["stage"])               # "child"
print(meta["dopamine"])            # 0.6
```

### Brain State Structure

```
brain.qtsq
├── network_weights    → compressed float32 tensor
├── neuron_biases      → compressed float32 tensor
├── memory_vectors     → (num_memories × vector_dim) float32
└── metadata           → JSON string (personality, hormones, age, etc.)
```

---

## File Info

Inspect any `.qtsq` file without decompressing it:

```python
info = qtsq.info("photo.qtsq")
print(info)
# {
#   'strategy': 'tensor',
#   'data_type': 'tensor',
#   'width': 0,
#   'height': 0,
#   'channels': 0,
#   'original_size': 4000000,
#   'version': 1,
#   'encrypted': 0
# }
```

---

## Mesh Data (for DTCN)

Extract triangle mesh data for graph neural networks:

```python
data = qtsq.read("photo.qtsq")
print(data)
# QTSQData('photo.qtsq', 1920×1080, 45,231 vertices, 89,104 triangles)

data.vertices     # (N, 5) — x, y, R, G, B
data.positions    # (N, 2) — normalized positions
data.colors       # (N, 3) — RGB [0, 1]
data.triangles    # (M, 3) — triangle indices

# Convert to PyTorch Geometric graph
graph = data.to_graph(label=3)
```

---

## Security

**This library is not finished security work.** An audit on 2026-10-14 found
thirteen defects that are now fixed — most seriously, per-stream encryption was
writing the raw AES-CTR keystream over the plaintext instead of encrypting, so
it destroyed the data it was applied to. That path had no test; it does now.

That remediation pass has since been completed for every finding in the audit's
container, crypto and binding scope, with regression tests for the vault,
censorship-KDF and Shamir paths. Two items are documented rather than fixed:
stream-name obfuscation XORs on a salt written in cleartext and is **not a
cipher** (see `docs/FORMAT.md` §8), and region-crypt output is unauthenticated
by design unless it is wrapped in an authenticated layer such as the vault
envelope. Roughly 25,000 lines (`compressor/src/*.c` and the engine files)
still have not been audited at all.

Treat any file produced by this library as **untrusted input**. Do not rely on
it to protect data you cannot afford to lose.

## Install

### From PyPI

```bash
pip install qtsq
```

### From source

```bash
git clone https://github.com/haruhito/qtsq.git
cd qtsq
pip install .
```

### Requirements

- **Python** ≥ 3.8
- **C compiler** — Xcode (macOS) or gcc (Linux)
- **zlib** — usually pre-installed; on Linux: `apt install zlib1g-dev`
- **numpy**, **scipy** — installed automatically

### Optional dependencies

```bash
pip install qtsq[images]    # + Pillow (for qtsq.compress from image files)
pip install qtsq[ml]        # + PyTorch, PyG (for qtsq.read().to_graph())
```

---

## C API

The underlying C library can also be used directly:

```c
#include "qtsq.h"

// Tensor with f16 precision
float weights[1000000];
qtsq_context_t ctx;
qtsq_init(&ctx);
uint32_t dims[] = {1000000};
qtsq_compress_tensor_quantized(&ctx, weights, 1000000, dims, 1, QTSQ_TENSOR_F16);
qtsq_write(&ctx, "weights.qtsq");
qtsq_free(&ctx);

// Brain state
qtsq_brain_save("brain.qtsq", weights, 1000000, biases, 1000,
                memories, 100, 128, "{\"age\": 5000}");
```

Build the C library:

```bash
make lib    # → libqtsq.a
make cli    # → bin/qtsq (command-line tool)
```

---

## License

Proprietary — © haruhito
