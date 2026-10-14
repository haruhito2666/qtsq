"""
qtsq — Read, write, compress, and decompress .qtsq files in Python.

Install:
    pip install qtsq

Usage:
    import qtsq

    # ── Images ──
    qtsq.compress("photo.png", "photo.qtsq")
    qtsq.compress("photo.png", "photo.qtsq", mode="hd")
    qtsq.compress("photo.png", "photo.qtsq", mode="lossless")

    img = qtsq.decompress("photo.qtsq")        # → numpy (H, W, 3) uint8
    qtsq.render("photo.qtsq", "output.png")     # decompress + save

    # ── Text ──
    qtsq.compress_text("Hello world!", "text.qtsq")
    text = qtsq.decompress_text("text.qtsq")    # → string

    # ── JSON ──
    qtsq.compress_json(json_str, "data.qtsq")   # auto-minifies before compressing
    json_str = qtsq.decompress_text("data.qtsq") # → minified JSON string

    # ── CSV ──
    qtsq.compress_csv(csv_str, "table.qtsq")
    csv_str = qtsq.decompress_text("table.qtsq")

    # ── Tensors (AI weights/embeddings) ──
    qtsq.compress_tensor(weights, "model.qtsq", precision="f16")
    data = qtsq.decompress_tensor("model.qtsq")  # → numpy float32

    # ── AI Brain State ──
    qtsq.brain_save("brain.qtsq", weights, biases, memories, 128, metadata)
    state = qtsq.brain_load("brain.qtsq")

    # ── Info ──
    info = qtsq.info("photo.qtsq")
    print(info)

    # ── Mesh / DTCN ──
    data = qtsq.read("photo.qtsq")
    graph = data.to_graph()

Author: haruhito
License: Proprietary
"""

import os
import numpy as np
from scipy.spatial import Delaunay

from qtsq._native import (
    read_vertices as _c_read,
    compress_image as _c_compress,
    decompress_image as _c_decompress,
    compress_text as _c_compress_text,
    compress_json as _c_compress_json,
    compress_csv as _c_compress_csv,
    decompress_raw as _c_decompress_raw,
    file_info as _c_info,
    compress_tensor as _c_compress_tensor,
    compress_tensor_bytes as _c_compress_tensor_bytes,
    decompress_tensor as _c_decompress_tensor,
    brain_save as _c_brain_save,
    brain_load as _c_brain_load,
    model_save as _c_model_save,
    model_load as _c_model_load,
    model_info as _c_model_info,
    model_load_some as _c_model_load_some,
    image_probe as _c_image_probe,
    image_decode as _c_image_decode,
)

# Training-loop checkpoints (torch optional; imported lazily inside).
from qtsq.checkpoint import (  # noqa: E402
    save_checkpoint,
    load_checkpoint,
    CheckpointSaver,
)

__version__ = "2.2.0"


# Supported tensor precisions
TENSOR_F32 = "f32"   # 32-bit delta-quantized legacy (lossy ~1e-8, compat only)
TENSOR_F16 = "f16"   # 16-bit IEEE 754 half-precision (lossy)
TENSOR_F8  = "f8"    # 8-bit E4M3 (NVIDIA FP8, lossy)
TENSOR_F4  = "f4"    # 4-bit block quantization (GGUF Q4_0-like, lossy)
TENSOR_RAW = "raw"   # 32-bit BIT-EXACT (no delta, NaN/Inf preserved) — use for weights


# ─────────────────────────────────────────────────────────────
#  Image Compression
# ─────────────────────────────────────────────────────────────

def compress(input_path, output_path, mode="default", password=None,
             codec="qtc", quality=0.0):
    """
    Compress an image to .qtsq format.

    Args:
        input_path:  path to input image (PNG, JPG, etc.)
        output_path: path for .qtsq file (or directory — auto-names)
        codec:       "qtc" (default) or "zlib" (legacy opt-out)
        quality:     0.0 = lossless; > 0 enables lossy qtc paths where
                     available (ignored by zlib)
        mode:
            Triangle + Splat (gravitational):
                "default"           — balanced quality/size
                "hd"                — high quality (recommended)
                "lossless"          — near-lossless
                "upscale"           — artifact cleanup for lossy inputs

            Splat-only:
                "splat"             — compact Gaussian splats
                "splat_hd"          — HD splats
                "splat_lossless"    — lossless splats

            Triangle-only:
                "triangle"          — compact mesh (for DTCN)
                "triangle_hd"       — HD mesh
                "triangle_lossless" — lossless mesh
                "triangle_pixel"    — pixel-perfect (1 vertex/pixel, for ML training)
                "triangle_adaptive" — adaptive recursive subdivision (TRM5)

            Ionic Data Tunnel (ICE-inspired I/O acceleration):
                "hd_ionic"          — HD + ionic decomposition + cascade prefetch

    Example:
        qtsq.compress("photo.png", "photo.qtsq", mode="hd")
        qtsq.compress("photo.png", "photo.qtsq", mode="triangle_pixel")
        qtsq.compress("photo.png", "photo.qtsq", mode="hd_ionic")
    """
    from PIL import Image

    input_path = os.path.abspath(input_path)
    output_path = os.path.abspath(output_path)

    # If output is a directory, auto-name the file
    if os.path.isdir(output_path):
        base = os.path.splitext(os.path.basename(input_path))[0]
        output_path = os.path.join(output_path, base + ".qtsq")

    img = Image.open(input_path).convert("RGB")
    pixels = np.array(img, dtype=np.uint8)
    h, w = pixels.shape[:2]
    ch = pixels.shape[2] if pixels.ndim == 3 else 1

    _c_compress(pixels.tobytes(), w, h, ch, output_path, mode,
                password=password or "", codec=codec, quality=quality)

    return output_path


def compress_bytes(pixels, width, height, channels, output_path, mode="default",
                   password=None, codec="qtc", quality=0.0):
    """
    Compress raw pixel bytes to .qtsq format.

    Args:
        pixels:      bytes or numpy array of pixel data (RGB)
        width:       image width
        height:      image height
        channels:    number of channels (1, 3, or 4)
        output_path: path for output .qtsq file
        mode:        "default", "hd", "lossless", or "upscale"
        codec:       "qtc" (default) or "zlib" (legacy opt-out)
        quality:     0.0 = lossless; > 0 enables lossy qtc paths
    """
    output_path = os.path.abspath(output_path)

    if isinstance(pixels, np.ndarray):
        pixels = pixels.astype(np.uint8).tobytes()

    _c_compress(pixels, width, height, channels, output_path, mode,
                password=password or "", codec=codec, quality=quality)
    return output_path


# ─────────────────────────────────────────────────────────────
#  Image Decompression
# ─────────────────────────────────────────────────────────────

def decompress(path, password=None):
    """
    Decompress a .qtsq image file to a numpy array.

    Args:
        path: path to .qtsq file

    Returns:
        numpy array (H, W, 3) uint8, RGB

    Example:
        img = qtsq.decompress("photo.qtsq")
        print(img.shape)  # (2048, 1514, 3)
    """
    path = os.path.abspath(path)
    result = _c_decompress(path, password=password or "")

    pixels = np.frombuffer(result["pixels"], dtype=np.uint8)
    width = result["width"]
    height = result["height"]
    ch = result["channels"]

    if ch == 0:
        ch = 3

    expected = width * height * ch
    if len(pixels) != expected:
        # Try to infer if dimensions are slightly off
        if len(pixels) % (width * ch) == 0:
            height = len(pixels) // (width * ch)
        elif len(pixels) % (height * ch) == 0:
            width = len(pixels) // (height * ch)

    if ch == 1:
        return pixels.reshape(height, width)
    return pixels.reshape(height, width, ch)


def render(qtsq_path, output_path=None, password=None):
    """
    Render a .qtsq file — display it or save it.

    Without output_path: opens a window showing the image (like QTSQStudio).
    With output_path:    saves to PNG/JPG file.

    Args:
        qtsq_path:   path to .qtsq file
        output_path:  optional — save to file instead of displaying

    Example:
        qtsq.render("photo.qtsq")              # opens window
        qtsq.render("photo.qtsq", "out.png")   # saves to file
    """
    img_array = decompress(qtsq_path, password=password)

    if output_path is not None:
        # Save to file
        from PIL import Image
        if img_array.ndim == 2:
            img = Image.fromarray(img_array, mode="L")
        else:
            img = Image.fromarray(img_array, mode="RGB")
        img.save(output_path)
        return output_path

    # Display in interactive window
    import platform
    import matplotlib
    if platform.system() == 'Darwin':
        matplotlib.use('macosx')
    else:
        try:
            matplotlib.use('TkAgg')
        except Exception:
            matplotlib.use('Agg')  # headless fallback
    import matplotlib.pyplot as plt

    h, w = img_array.shape[:2]
    dpi = 100
    fig, ax = plt.subplots(1, 1, figsize=(min(w/dpi*1.2, 16), min(h/dpi*1.2, 10)))
    fig.patch.set_facecolor('#111111')

    ax.imshow(img_array, interpolation='lanczos')
    ax.axis('off')
    ax.set_position([0, 0, 1, 1])
    plt.subplots_adjust(left=0, right=1, top=1, bottom=0)

    filename = os.path.basename(qtsq_path)
    fig.canvas.manager.set_window_title(f'QTSQ — {filename}')

    # Store state for zoom/pan
    state = {'xlim': ax.get_xlim(), 'ylim': ax.get_ylim(),
             'dragging': False, 'last_xy': None}

    def on_scroll(event):
        """Scroll wheel zoom — centered on cursor."""
        if event.xdata is None or event.ydata is None:
            return
        zoom = 0.8 if event.button == 'up' else 1.25
        xl, xr = ax.get_xlim()
        yb, yt = ax.get_ylim()
        cx, cy = event.xdata, event.ydata
        ax.set_xlim(cx - (cx - xl) * zoom, cx + (xr - cx) * zoom)
        ax.set_ylim(cy - (cy - yb) * zoom, cy + (yt - cy) * zoom)
        fig.canvas.draw_idle()

    def on_press(event):
        if event.button == 1:
            state['dragging'] = True
            state['last_xy'] = (event.x, event.y)

    def on_release(event):
        state['dragging'] = False
        state['last_xy'] = None

    def on_motion(event):
        """Click-drag pan."""
        if not state['dragging'] or state['last_xy'] is None:
            return
        if event.x is None or event.y is None:
            return
        dx = event.x - state['last_xy'][0]
        dy = event.y - state['last_xy'][1]
        state['last_xy'] = (event.x, event.y)
        xl, xr = ax.get_xlim()
        yb, yt = ax.get_ylim()
        scale = (xr - xl) / fig.get_size_inches()[0] / dpi
        ax.set_xlim(xl - dx * scale, xr - dx * scale)
        ax.set_ylim(yb - dy * scale, yt - dy * scale)
        fig.canvas.draw_idle()

    def on_key(event):
        """R = reset, F = fit, Q = quit, +/- = zoom."""
        if event.key == 'r':
            ax.set_xlim(state['xlim'])
            ax.set_ylim(state['ylim'])
            fig.canvas.draw_idle()
        elif event.key == 'f':
            ax.set_xlim(-0.5, w - 0.5)
            ax.set_ylim(h - 0.5, -0.5)
            fig.canvas.draw_idle()
        elif event.key in ('+', '='):
            xl, xr = ax.get_xlim()
            yb, yt = ax.get_ylim()
            cx, cy = (xl + xr) / 2, (yb + yt) / 2
            ax.set_xlim(cx - (cx - xl) * 0.8, cx + (xr - cx) * 0.8)
            ax.set_ylim(cy - (cy - yb) * 0.8, cy + (yt - cy) * 0.8)
            fig.canvas.draw_idle()
        elif event.key in ('-', '_'):
            xl, xr = ax.get_xlim()
            yb, yt = ax.get_ylim()
            cx, cy = (xl + xr) / 2, (yb + yt) / 2
            ax.set_xlim(cx - (cx - xl) * 1.25, cx + (xr - cx) * 1.25)
            ax.set_ylim(cy - (cy - yb) * 1.25, cy + (yt - cy) * 1.25)
            fig.canvas.draw_idle()
        elif event.key == 'q':
            plt.close(fig)

    fig.canvas.mpl_connect('scroll_event', on_scroll)
    fig.canvas.mpl_connect('button_press_event', on_press)
    fig.canvas.mpl_connect('button_release_event', on_release)
    fig.canvas.mpl_connect('motion_notify_event', on_motion)
    fig.canvas.mpl_connect('key_press_event', on_key)

    plt.show()


# ─────────────────────────────────────────────────────────────
#  Text Compression
# ─────────────────────────────────────────────────────────────

def compress_text(text, output_path, password=None, codec="qtc", quality=0.0):
    """
    Compress text to .qtsq format.

    Args:
        text:        string to compress
        output_path: path for output .qtsq file
        codec:       "qtc" (default, usually smaller for text) or "zlib"
                     (legacy opt-out) — lossless either way, quality ignored

    Example:
        qtsq.compress_text("Hello world!", "hello.qtsq")
        qtsq.compress_text("Hello world!", "hello.qtsq", codec="zlib")
    """
    output_path = os.path.abspath(output_path)
    _c_compress_text(text, output_path, password=password or "",
                     codec=codec, quality=quality)
    return output_path


def compress_json(json_str, output_path, password=None, codec="qtc", quality=0.0):
    """
    Minify and compress JSON to .qtsq format.
    
    Args:
        json_str:    JSON string to compress
        output_path: path for output .qtsq file
        codec:       "qtc" (default) or "zlib" (legacy opt-out; lossless either way)
    """
    output_path = os.path.abspath(output_path)
    _c_compress_json(json_str, output_path, password=password or "",
                     codec=codec, quality=quality)
    return output_path


def compress_csv(csv_str, output_path, password=None, codec="qtc", quality=0.0):
    """
    Compress CSV to .qtsq format using column grouping.
    
    Args:
        csv_str:     CSV string to compress
        output_path: path for output .qtsq file
        codec:       "qtc" (default) or "zlib" (legacy opt-out; lossless either way)
    """
    output_path = os.path.abspath(output_path)
    _c_compress_csv(csv_str, output_path, password=password or "",
                    codec=codec, quality=quality)
    return output_path


def decompress_text(path, password=None):
    """
    Decompress a .qtsq text file to string.

    Args:
        path: path to .qtsq file

    Returns:
        string

    Example:
        text = qtsq.decompress_text("hello.qtsq")
        print(text)  # "Hello world!"
    """
    path = os.path.abspath(path)
    raw = _c_decompress_raw(path, password=password or "")
    return raw.decode("utf-8")


# ─────────────────────────────────────────────────────────────
#  Tensor Compression (AI weights, embeddings)
# ─────────────────────────────────────────────────────────────

def compress_tensor(data, output_path, dims=None, precision="f32", password=None,
                    codec="qtc", quality=0.0, fast=False, store=False):
    """
    Compress a float32 array to .qtsq tensor format.

    Supports multiple precision levels for size/accuracy tradeoff:
        "raw"   — 32-bit BIT-EXACT (no delta, NaN/Inf preserved; use for weights)
        "f32"   — 32-bit legacy delta-quantized (~1e-8 error, compat only)
        "f16"   — 16-bit IEEE half-precision (~1e-4 error, 2:1 ratio)
        "f8"    — 8-bit NVIDIA E4M3 (~1e-2 error, 6:1 ratio)
        "f4"    — 4-bit block Q4_0-like (~1e-2 error, 7:1 ratio, lossy)

    Args:
        data:        numpy array or list of floats
        output_path: path for output .qtsq file
        dims:        tuple of dimensions (optional, auto-inferred from shape)
        precision:   "f32", "f16", "f8", "f4", or "raw"
        codec:       qtc is the only weights codec (custom compressor);
                     "zlib" raises — kept for media APIs only
        fast:       False = full contest (best ratio); True = fast subset
                     (bit-exact, different — usually slightly larger — bytes)

    Returns:
        dict with count, precision, compressed_size

    Example:
        import numpy as np
        import qtsq

        weights = np.random.randn(1000000).astype(np.float32) * 0.1
        qtsq.compress_tensor(weights, "weights.qtsq", precision="f16")
    """
    output_path = os.path.abspath(output_path)

    if isinstance(data, np.ndarray):
        # Fast path: zero-copy float32 bytes (no Python-float list).
        if dims is None:
            dims = tuple(data.shape)
        a = np.ascontiguousarray(data, dtype=np.float32)
        return _c_compress_tensor_bytes(a.tobytes(), tuple(dims), output_path,
                                        precision=precision,
                                        password=password or "",
                                        codec=codec, quality=quality,
                                        fast=1 if fast else 0,
                                        store=1 if store else 0)
    if isinstance(data, (list, tuple)):
        if dims is None:
            dims = (len(data),)
        data = [float(x) for x in data]
    else:
        raise TypeError("data must be a numpy array or list of floats")

    return _c_compress_tensor(data, dims, output_path, precision=precision,
                               password=password or "",
                               codec=codec, quality=quality,
                               fast=1 if fast else 0,
                               store=1 if store else 0)


def decompress_tensor(path, password=None):
    """
    Decompress a .qtsq tensor file to numpy float32 array.

    Auto-detects the precision format (f32/f16/f8/f4).

    Args:
        path: path to .qtsq tensor file

    Returns:
        numpy float32 array, reshaped to original dimensions

    Example:
        weights = qtsq.decompress_tensor("weights.qtsq")
        print(weights.shape, weights.dtype)
    """
    path = os.path.abspath(path)
    result = _c_decompress_tensor(path, password=password or "")

    data = np.array(result["data"], dtype=np.float32)
    dims = result.get("dims", ())

    if dims and len(dims) > 0:
        try:
            data = data.reshape(dims)
        except ValueError:
            pass  # dims don't match count, keep flat

    return data


# ─────────────────────────────────────────────────────────────
#  AI Brain State (container format)
# ─────────────────────────────────────────────────────────────

def brain_save(output_path, weights, biases, memories=None,
               vector_dim=0, metadata_json=None, password=None):
    """
    Save an AI brain state to a .qtsq container file.

    Packs weights, biases, memory vectors, and metadata JSON
    into a single .qtsq file with checksums and compression.

    Args:
        output_path:    path for output .qtsq file
        weights:        numpy array or list of float32 (network weights)
        biases:         numpy array or list of float32 (neuron biases)
        memories:       numpy array or list of float32 (memory vectors, flat)
        vector_dim:     dimension of each memory vector
        metadata_json:  JSON string with metadata (personality, hormones, etc.)

    Example:
        import qtsq
        import json

        weights = model.get_weights().flatten().tolist()
        biases = model.get_biases().flatten().tolist()
        meta = json.dumps({"age": 5000, "stage": "child", "dopamine": 0.6})

        qtsq.brain_save("brain.qtsq", weights, biases, metadata_json=meta)
    """
    output_path = os.path.abspath(output_path)

    def _to_list(x):
        if x is None:
            return []
        if isinstance(x, np.ndarray):
            return x.flatten().astype(np.float32).tolist()
        return [float(v) for v in x]

    w = _to_list(weights)
    b = _to_list(biases)
    m = _to_list(memories)

    _c_brain_save(output_path, w, b, m,
                  vector_dim=int(vector_dim),
                  metadata_json=metadata_json or "{}",
                  password=password or "")


def brain_load(path, password=None):
    """
    Load an AI brain state from a .qtsq container file.

    Returns:
        dict with:
            weights:       numpy float32 array
            biases:        numpy float32 array
            memories:      numpy float32 array (num_memories × vector_dim)
            num_memories:  int
            vector_dim:    int
            metadata:      JSON string

    Example:
        state = qtsq.brain_load("brain.qtsq")
        weights = state["weights"]
        meta = json.loads(state["metadata"])
        print(f"Brain age: {meta['age']}, {len(weights)} weights")
    """
    path = os.path.abspath(path)
    result = _c_brain_load(path, password=password or "")

    result["weights"] = np.array(result["weights"], dtype=np.float32)
    result["biases"] = np.array(result["biases"], dtype=np.float32)

    mc = result.get("num_memories", 0)
    vd = result.get("vector_dim", 0)
    mem = np.array(result["memories"], dtype=np.float32)
    if mc > 0 and vd > 0:
        try:
            mem = mem.reshape(mc, vd)
        except ValueError:
            pass
    result["memories"] = mem

    return result


# ─────────────────────────────────────────────────────────────
#  File Info
# ─────────────────────────────────────────────────────────────

def info(path, password=None):
    """
    Get metadata from a .qtsq file without decompressing.

    Args:
        path: path to .qtsq file

    Returns:
        dict with strategy, data_type, width, height, channels,
        original_size, version, encrypted

    Example:
        info = qtsq.info("photo.qtsq")
        print(info)
    """
    path = os.path.abspath(path)
    return _c_info(path, password=password or "")


# ─────────────────────────────────────────────────────────────
#  Mesh / Vertex Data (for DTCN)
# ─────────────────────────────────────────────────────────────

class QTSQData:
    """Data extracted from a .qtsq file."""

    def __init__(self, path, vertices, width, height):
        self.path = path
        self.vertices = vertices
        self.width = width
        self.height = height
        self.num_vertices = len(vertices)
        self.positions = vertices[:, :2].copy()
        self.colors = vertices[:, 2:5].copy() / 255.0
        self._tri = None

    @property
    def triangles(self):
        self._ensure_tri()
        return self._tri.simplices

    @property
    def neighbors(self):
        self._ensure_tri()
        return self._tri.neighbors

    @property
    def num_triangles(self):
        return len(self.triangles)

    def _ensure_tri(self):
        if self._tri is None:
            self._tri = Delaunay(self.positions)

    def triangle_features(self):
        """9-dim features + normalized areas per triangle."""
        self._ensure_tri()
        s = self._tri.simplices
        M = len(s)
        f = np.zeros((M, 9), dtype=np.float32)
        a = np.zeros(M, dtype=np.float32)
        for i in range(M):
            v0,v1,v2 = s[i]
            p0,p1,p2 = self.positions[v0],self.positions[v1],self.positions[v2]
            c0,c1,c2 = self.colors[v0],self.colors[v1],self.colors[v2]
            f[i,0:3] = (c0+c1+c2)/3.0
            a[i] = 0.5*abs((p1[0]-p0[0])*(p2[1]-p0[1])-(p2[0]-p0[0])*(p1[1]-p0[1]))
            e01,e12,e20 = np.linalg.norm(p1-p0),np.linalg.norm(p2-p1),np.linalg.norm(p0-p2)
            f[i,4] = min(max(e01,e12,e20)/max(min(e01,e12,e20),1e-6),10.0)/10.0
            f[i,5] = (p0[0]+p1[0]+p2[0])/(3.0*max(self.width,1))
            f[i,6] = (p0[1]+p1[1]+p2[1])/(3.0*max(self.height,1))
            f[i,7] = np.stack([c0,c1,c2]).var()
            f[i,8] = max(np.linalg.norm(c1-c0),np.linalg.norm(c2-c1),np.linalg.norm(c0-c2))
        mx = max(a.max(),1e-6)
        f[:,3]=a/mx; a/=mx
        return f,a

    def edge_features(self):
        """5-dim edge features for dual graph."""
        self._ensure_tri()
        s,n = self._tri.simplices,self._tri.neighbors
        tf,_ = self.triangle_features()
        src,dst,at = [],[],[]
        md = max(self.width,self.height,1)
        for i in range(len(s)):
            for k in range(3):
                j=n[i,k]
                if j<0: continue
                sh=set(s[i])&set(s[j])
                el=(np.linalg.norm(self.positions[list(sh)[0]]-self.positions[list(sh)[1]])/md
                    if len(sh)==2 else 0.0)
                dr,dg,db=tf[i,0]-tf[j,0],tf[i,1]-tf[j,1],tf[i,2]-tf[j,2]
                ar=min(max(tf[i,3],1e-6)/max(tf[j,3],1e-6),max(tf[j,3],1e-6)/max(tf[i,3],1e-6))
                src.append(i);dst.append(j);at.append([el,dr,dg,db,ar])
        if not src:
            return np.zeros((2,0),dtype=np.int64),np.zeros((0,5),dtype=np.float32)
        return np.array([src,dst],dtype=np.int64),np.array(at,dtype=np.float32)

    def to_graph(self, label=None):
        """Convert to PyTorch Geometric Data for DTCN."""
        import torch
        from torch_geometric.data import Data
        feats,areas = self.triangle_features()
        ei,ea = self.edge_features()
        d = Data(
            x=torch.tensor(feats,dtype=torch.float32),
            edge_index=torch.tensor(ei,dtype=torch.long),
            edge_attr=torch.tensor(ea,dtype=torch.float32),
            areas=torch.tensor(areas,dtype=torch.float32))
        if label is not None:
            d.y = torch.tensor(label,dtype=torch.long)
        return d

    def __repr__(self):
        return (f"QTSQData('{os.path.basename(self.path)}', "
                f"{self.width}×{self.height}, "
                f"{self.num_vertices:,} vertices, "
                f"{self.num_triangles:,} triangles)")


def read(path, max_vertices=100000, password=None):
    """
    Read triangle mesh data from a .qtsq file.

    Args:
        path: path to .qtsq file
        max_vertices: max vertices to extract

    Returns:
        QTSQData object

    Example:
        data = qtsq.read("photo.qtsq")
        print(data.triangles.shape)
    """
    path = os.path.abspath(path)
    if not os.path.exists(path):
        raise FileNotFoundError(f"File not found: {path}")
    result = _c_read(path, max_vertices, password=password or "")
    vertices = np.array(result['vertices'], dtype=np.float32)
    return QTSQData(path, vertices, result['width'], result['height'])


# ─────────────────────────────────────────────────────────────
#  Model Checkpoint (exact-weight, one .qtsq per trained model)
# ─────────────────────────────────────────────────────────────

_MODEL_DTYPE_ELEM = {
    "float32": 4, "float16": 2, "bfloat16": 2,
    "int8": 1, "uint8": 1, "int16": 2, "uint16": 2,
    "int32": 4, "uint32": 4, "int64": 8, "uint64": 8, "bool": 1,
    "int4": 0.5, "uint4": 0.5, "float8_e5m2": 1,
}

# Stream content kinds (qtsq_kind_t in include/qtsq_format.h). A "/1"
# manifest has no kind field at all and every entry is "tensor", so this
# is purely additive: a /1 manifest reads as all-tensor without change.
_MODEL_KIND_TENSOR = "tensor"
_MODEL_KINDS = ("tensor", "blob", "text", "image", "audio", "table")

# Manifest format versions accepted on read. /1 predates kinds and is
# read as all-tensor; /2 adds "kind" per entry and is what we write.
_MODEL_FORMAT_V1 = "qtsq-model/1"
_MODEL_FORMAT_V2 = "qtsq-model/2"
_MODEL_FORMAT = _MODEL_FORMAT_V2
_MODEL_FORMATS_ACCEPTED = (_MODEL_FORMAT_V1, _MODEL_FORMAT_V2)

# Stream name reserved for the manifest; tensor names may not use it.
_MODEL_MANIFEST_STREAM = "__manifest__"
# Container stream names are C strings capped at 256 bytes incl. NUL.
_MODEL_NAME_MAX = 250

def _model_numpy_dtype(dtype_str):
    """Map manifest dtype string → numpy dtype (bfloat16 via ml_dtypes if present)."""
    if dtype_str == "float32":
        return np.dtype("float32")
    if dtype_str == "float16":
        return np.dtype("float16")
    if dtype_str == "bfloat16":
        try:
            import ml_dtypes  # type: ignore
            return ml_dtypes.bfloat16
        except Exception:
            return None  # caller keeps raw uint16 + dtype tag
    try:
        return np.dtype(dtype_str)
    except TypeError:
        return None  # unknown to numpy (e.g. torch-only fp8) → bits + tag


def _pack_nibbles(a, signed):
    """Pack int8 values into nibbles (high-nibble first), exact.

    int4: values in [-8, 7]; uint4: values in [0, 15].
    Odd counts pad the last low nibble with 0.
    """
    flat = np.ascontiguousarray(a, dtype=np.int8).reshape(-1)
    if signed:
        if flat.min() < -8 or flat.max() > 7:
            raise ValueError("int4 values must be in [-8, 7]")
        q = (flat.astype(np.int16) + 8).astype(np.uint8)
    else:
        if flat.min() < 0 or flat.max() > 15:
            raise ValueError("uint4 values must be in [0, 15]")
        q = flat.astype(np.uint8)
    n = len(q)
    out = np.zeros((n + 1) // 2, dtype=np.uint8)
    out[:] = (q[0::2].astype(np.uint16) << 4)
    if n > 1:
        second = q[1::2].astype(np.uint16)
        out[:len(second)] |= second
    return out.tobytes(), n


def _unpack_nibbles(raw, count, signed):
    """Inverse of _pack_nibbles → int8 array of length count."""
    q = np.frombuffer(raw, dtype=np.uint8).copy()
    vals = np.empty(len(q) * 2, dtype=np.uint8)
    vals[0::2] = q >> 4
    vals[1::2] = q & 0x0F
    vals = vals[:count]
    if signed:
        return (vals.astype(np.int16) - 8).astype(np.int8)
    return vals.astype(np.int8)


def _model_tensor_to_bytes(arr, as_view=False):
    """Return (dtype_str, shape, raw_bytes, is_f32, elem_size, nibble).

    With as_view=True (and no packing needed) raw_bytes is a memoryview
    over the source array — no copy. 0-d arrays still return bytes.


    Values may also be (array, dtype_tag) tuples to force an exact packed
    tag: "int4", "uint4" (from int8 values), or "float8_e5m2" (from uint8
    bits of already-quantized FP8-E5M2 weights). elem_size drives the qtc
    array lens (1/2/4/8); nibble selects the packed-nibble candidate.
    """
    tag = None
    if isinstance(arr, (tuple, list)) and len(arr) == 2 and isinstance(arr[1], str):
        arr, tag = arr[0], arr[1]
    if tag in ("int4", "uint4"):
        mod = type(arr).__module__ or ""
        if mod.startswith("torch") or hasattr(arr, "untyped_storage"):
            raise ValueError(f"{tag} packing needs a numpy int8 array "
                             "(convert with .cpu().to(torch.int8).numpy())")
        a0 = np.asarray(arr, dtype=np.int8)
        shape = tuple(a0.shape)  # capture before: ascontiguousarray flattens 0-d (numpy 2.x)
        a = a0 if a0.ndim == 0 else np.ascontiguousarray(a0)
        raw, n = _pack_nibbles(a, signed=(tag == "int4"))
        return tag, shape, raw, 0, 1, 1
    if tag == "float8_e5m2":
        mod = type(arr).__module__ or ""
        if mod.startswith("torch") or hasattr(arr, "untyped_storage"):
            import torch as _torch
            t = arr.detach().to("cpu").contiguous()
            if str(t.dtype) not in ("uint8", "float8_e5m2"):
                raise ValueError("float8_e5m2 needs uint8 bits "
                                 "(e.g. tensor.view(torch.uint8))")
            # NOTE: bytes(untyped_storage) is ~10_000x slower (per-byte
            # iteration in torch 2.x); go through numpy (memcpy).
            raw = t.view(_torch.uint8).numpy().tobytes()
            return tag, tuple(t.shape), raw, 0, 1, 0
        a0 = np.asarray(arr, dtype=np.uint8)
        shape = tuple(a0.shape)
        a = a0 if a0.ndim == 0 else np.ascontiguousarray(a0)
        raw = memoryview(a) if (as_view and a.ndim > 0) else a.tobytes()
        return tag, shape, raw, 0, 1, 0
    if tag is not None:
        raise ValueError(f"unknown dtype tag {tag!r} "
                         "(use 'int4', 'uint4', 'float8_e5m2')")
    # Torch tensor?
    mod = type(arr).__module__ or ""
    if mod.startswith("torch") or hasattr(arr, "untyped_storage"):
        import torch  # local import, optional dep
        t = arr.detach().to("cpu").contiguous()
        d = str(t.dtype).replace("torch.", "")  # e.g. float32, bfloat16
        shape = tuple(t.shape)
        # NOTE: bytes(t.untyped_storage()) iterates per byte (~1 MB/s);
        # .numpy().tobytes() memcpys (~GB/s). bf16 goes via uint16 view
        # (numpy() rejects bf16 without ml_dtypes).
        try:
            na = t.view(torch.uint16).numpy() if d == "bfloat16" else t.numpy()
        except Exception:
            na = None
        if na is None:
            raw = bytes(t.untyped_storage())  # slow last resort
        elif as_view and na.ndim > 0:
            raw = memoryview(na)
        else:
            raw = na.tobytes()
        return d, shape, raw, 1 if d == "float32" else 0, t.element_size(), 0
    a0 = None
    try:
        # Works for numpy, JAX/TF-eager/MLX host arrays, and anything
        # implementing __array__ (custom-arch frameworks included).
        probe = np.asarray(arr)
        # NOTE: np.asarray does NOT consume __dlpack__ — dlpack-only
        # objects come back as 0-d object arrays. Detect and reroute.
        if probe.dtype != object:
            a0 = probe
    except Exception:
        a0 = None
    if a0 is None:
        try:
            # __dlpack__-only objects (no __array__).
            a0 = np.from_dlpack(arr)
        except Exception as e:
            raise TypeError(
                f"tensor of type {type(arr).__name__} is not array-like "
                f"(need __array__/__dlpack__ of numeric data): {e}") from e
    if a0.dtype == object:
        raise TypeError(f"tensor of type {type(arr).__name__} has object dtype — "
                        "convert to a numeric array first")
    # numpy 2.x ascontiguousarray flattens 0-d → capture shape first
    shape = tuple(a0.shape)
    a = a0 if a0.ndim == 0 else np.ascontiguousarray(a0)
    dtype_str = str(a.dtype)
    # numpy 2.x may report bfloat16 via ml_dtypes as 'bfloat16'
    if dtype_str not in _MODEL_DTYPE_ELEM:
        # Fallback: store raw bytes, keep original tag
        dtype_str = str(a.dtype)
    if as_view and a.ndim > 0:
        raw = memoryview(a)
    else:
        raw = a.tobytes()
    return dtype_str, shape, raw, 1 if dtype_str == "float32" else 0, a.itemsize, 0


def save_images(output_path, images, tensors=None, password=None,
                metadata=None, fast=False, store=False, durable=True,
                max_pixels=0):
    """Save encoded image FILES (PNG/JPEG/BMP/TGA/PPM/GIF) into one .qtsq.

    Each entry is (name, file_bytes) or (name, file_bytes, policy), where
    policy is one of:

      "keep"    (default) store the encoded bytes verbatim — byte-for-byte
                identical to the input. Right for anything already
                compressed: a JPEG re-coded losslessly comes out ~6x
                BIGGER, and a PNG is roughly a tie.
      "pixels"  decode to 8-bit raster and apply the qtc image lens
                (2D MED prediction). Worth it for UNCOMPRESSED inputs —
                a BMP/TGA/PPM of the same image shrinks substantially —
                and lossless either way.
      Lossy "photo" compression is not offered yet. The block-DCT
      codec is implemented and measured, but it is not wired into the
      BLB1 image path; until a lossy stream carries QTSQ_BLOB_FLAG_LOSSY
      and reads back approximately, offering it would be a silent lie.

    Pixels are optional alongside: pass tensors= to keep weights and
    images in one file. Geometry and a SHA-256 of the original bytes are
    recorded per stream, so a reader can inventory the file from the
    directory without decoding a pixel.

    max_pixels: refuse images larger than this many pixels (0 = no limit).
    Probing is cheap and happens before decode, so an untrusted file's
    declared dimensions are bounded before anything is allocated.

    Returns:
        output_path

    Example:
        qtsq.save_images("assets.qtsq", [
            ("logo.png", open("logo.png","rb").read()),
            ("scan.bmp", scan_bytes, "pixels"),
        ], tensors={"w": weights})
    """
    import json as _json
    import struct as _struct

    if not isinstance(images, dict) or not images:
        raise ValueError("images must be a non-empty dict {name: bytes}")

    blob_names, blob_kinds, blob_ctypes = [], [], []
    blob_params, blob_depths, blob_policies, blob_payloads = [], [], [], []
    blob_channels = []
    _KIND_IMAGE, _CT_FILE, _CT_PIXELS = 3, 16, 17
    # "photo" (lossy) is deliberately NOT offered. The block-DCT codec
    # exists and was measured (it beats JPEG by ~14% bpp at matched
    # PSNR), but it is not wired into the BLB1 image path: a lossy
    # stream needs QTSQ_BLOB_FLAG_LOSSY propagation plus an
    # approximate-decode contract on read, and without both, "photo"
    # would silently behave as lossless -- worse than not offering it.
    # See compressor/results/phase55b_report.md.
    _POLICY = {"keep": 0, "pixels": 1}

    for name, val in images.items():
        sname = str(name)
        if sname == _MODEL_MANIFEST_STREAM:
            raise ValueError(f"stream name {sname!r} is reserved")
        if len(sname.encode("utf-8")) > _MODEL_NAME_MAX:
            raise ValueError(f"name too long (>{_MODEL_NAME_MAX} bytes): {sname!r}")
        policy_name = "keep"
        if isinstance(val, (tuple, list)) and len(val) == 2 and isinstance(val[1], str):
            data, policy_name = val[0], val[1]
        else:
            data = val
        if policy_name not in _POLICY:
            raise ValueError(f"unknown image policy {policy_name!r}; "
                             f"expected one of {sorted(_POLICY)}")
        data = bytes(data) if not isinstance(data, (bytes, bytearray)) else bytes(data)
        if not data:
            raise ValueError(f"image {sname!r} is empty")

        policy = _POLICY[policy_name]
        if policy == 0:
            w, h, ch = _c_image_probe(data)
            px = data
            ctype = _CT_FILE
        else:
            px, w, h, ch = _c_image_decode(data)
            ctype = _CT_PIXELS

        if max_pixels and w * h > max_pixels:
            raise ValueError(f"image {sname!r} is {w}x{h} = {w * h} pixels, "
                             f"over the max_pixels limit of {max_pixels}")

        blob_channels.append(int(ch))
        blob_names.append(sname)
        blob_kinds.append(_KIND_IMAGE)
        blob_ctypes.append(ctype)
        blob_params.append((w, h))
        blob_depths.append(8)
        blob_policies.append(policy)
        blob_payloads.append(px)

    # Tensors (if any) share the container; the manifest lists both.
    tensor_entries = []
    names, payloads, dims_list = [], [], []
    is_f32_list, elem_list, nibble_list = [], [], []
    if tensors:
        for name, arr in tensors.items():
            sname = str(name)
            if sname in blob_names:
                raise ValueError(f"duplicate stream name: {sname!r}")
            dtype_str, shape, raw, is_f32, elem_size, nibble = _model_tensor_to_bytes(
                arr, as_view=bool(store))
            rlen = _raw_nbytes(raw)
            if rlen == 0:
                raise ValueError(f"tensor {sname!r} is empty")
            elem = _MODEL_DTYPE_ELEM.get(dtype_str, elem_size)
            if dtype_str in ("int4", "uint4"):
                count = 1
                for d in shape:
                    count *= d
            elif elem_size in (1, 2, 4, 8) and rlen % elem_size == 0:
                count = rlen // elem_size
            else:
                count, elem = rlen, 1
            names.append(sname)
            payloads.append(raw)
            dims_list.append(tuple(shape) if len(shape) <= 8 and
                             dtype_str not in ("int4", "uint4") else ())
            is_f32_list.append(is_f32)
            elem_list.append(elem_size if elem_size in (1, 2, 4, 8) else 1)
            nibble_list.append(nibble)
            tensor_entries.append({
                "name": sname, "dtype": dtype_str, "shape": list(shape),
                "stream": sname, "elem_size": elem, "count": count,
                "kind": _MODEL_KIND_TENSOR,
            })

    image_entries = []
    for i, sname in enumerate(blob_names):
        image_entries.append({
            "name": sname, "kind": "image", "dtype": "uint8",
            "shape": [blob_params[i][1], blob_params[i][0], 1]
                     if blob_ctypes[i] == _CT_PIXELS else [],
            "stream": sname, "codec": _KIND_IMAGE,
            "content_type": blob_ctypes[i],
            "width": blob_params[i][0], "height": blob_params[i][1],
            "channels": blob_channels[i],
            "original_size": len(blob_payloads[i]),
            "policy": blob_policies[i],
        })

    manifest = _json.dumps({
        "format": _MODEL_FORMAT,
        "tensors": tensor_entries + image_entries,
        "metadata": metadata or {},
    }, separators=(",", ":"))

    output_path = os.path.abspath(output_path)
    _c_model_save(output_path, manifest, names, payloads, dims_list,
                  is_f32_list, elem_list, nibble_list,
                  password=password or "", fast=1 if fast else 0,
                  store=1 if store else 0, durable=1 if durable else 0,
                  blob_names=blob_names, blob_kinds=blob_kinds,
                  blob_ctypes=blob_ctypes, blob_params=blob_params,
                  blob_depths=blob_depths, blob_policies=blob_policies,
                  blob_payloads=blob_payloads)
    return output_path


def load_images(path, password=None):
    """Read back every image stream in a file → {name: (pixels, w, h, ch)}.

    Streams stored with policy "keep" return the ORIGINAL encoded file
    bytes; streams transcoded to "pixels" return the decoded raster. Use
    model_info(path) to see which is which without decoding anything.
    """
    entries, _meta, _stats = model_info(os.path.abspath(path), password=password)
    names = [n for n, t in entries.items() if t.get("kind") == "image"]
    if not names:
        return {}

    # A raw (decompressed) blob stream still carries its 64-byte BLB1
    # header, but the two zero-copy mmap paths serve VERBATIM payloads
    # only — they return the inner bytes with the header stripped. Detect
    # which shape we got from the magic rather than assuming, so the
    # geometry read below works on both paths.
    res = _c_model_load_some(os.path.abspath(path), names,
                             password=password or "")["tensors"]
    out = {}
    for n in names:
        raw = res[n]
        # load_some already strips the 64-byte BLB1 header and returns the
        # decoded body, so geometry comes from the manifest (recorded when
        # the stream was written) rather than from the payload.
        info = entries[n]
        w = int(info.get("width") or 0)
        h = int(info.get("height") or 0)
        if info.get("content_type") == 17:      # pixels
            ch = (len(raw) // (w * h)) if w and h else 0
            out[n] = (raw, w, h, ch)
        else:                                    # keep: encoded file bytes
            out[n] = (raw, w, h, 0)
    return out


def save_audios(output_path, audios, tensors=None, password=None,
                metadata=None, fast=False, store=False, durable=True):
    """Save interleaved PCM audio into one .qtsq, alongside optional tensors.

    Each entry is name -> raw interleaved PCM bytes, or
    (name, (pcm, channels, bits_per_sample)) to state the layout. Audio
    that is already an encoded file (wav/mp3/flac) should be stored with
    policy "keep" via the generic blob path instead; this function codes
    raw samples with the qtc audio lens, which is bit-exact and measured at
    roughly 7-50x on speech-grade material depending on content.

    channels must be 1..16 and bits_per_sample 8, 16 or 24; anything else
    is rejected rather than written. Tensors may be passed alongside so a
    model and its sample data share one file.

    Returns:
        output_path

    Example:
        pcm = np.zeros((48000, 2), np.int16).tobytes()   # 1s stereo
        qtsq.save_audios("clip.qtsq", {"room": (pcm, 2, 16)},
                         tensors=model.state_dict())
    """
    import json as _json

    if not isinstance(audios, dict) or not audios:
        raise ValueError("audios must be a non-empty dict {name: pcm}")

    blob_names, blob_kinds, blob_ctypes = [], [], []
    blob_params, blob_depths, blob_policies = [], [], []
    blob_payloads, blob_channels = [], []
    _KIND_AUDIO, _CT_PCM = 4, 32

    for name, val in audios.items():
        sname = str(name)
        if sname == _MODEL_MANIFEST_STREAM:
            raise ValueError(f"stream name {sname!r} is reserved")
        if len(sname.encode("utf-8")) > _MODEL_NAME_MAX:
            raise ValueError(f"name too long (>{_MODEL_NAME_MAX} bytes): {sname!r}")
        if isinstance(val, tuple) and len(val) == 3:
            pcm, channels, bps = val[0], int(val[1]), int(val[2])
        else:
            raise ValueError(
                f"audio {sname!r} must be (pcm_bytes, channels, bits_per_sample)")
        pcm = bytes(pcm) if not isinstance(pcm, (bytes, bytearray)) else bytes(pcm)
        if not pcm:
            raise ValueError(f"audio {sname!r} is empty")
        if not (1 <= channels <= 16):
            raise ValueError(f"audio {sname!r}: channels must be 1..16, got {channels}")
        if bps not in (8, 16, 24):
            raise ValueError(f"audio {sname!r}: bits_per_sample must be 8/16/24, got {bps}")
        frame_bytes = channels * (bps // 8)
        if len(pcm) % frame_bytes:
            raise ValueError(
                f"audio {sname!r}: {len(pcm)} bytes is not a whole number of "
                f"{channels}-channel {bps}-bit frames")
        frames = len(pcm) // frame_bytes

        blob_names.append(sname)
        blob_kinds.append(_KIND_AUDIO)
        blob_ctypes.append(_CT_PCM)
        blob_params.append((0, frames))       # sample_rate, frame count
        blob_depths.append(bps)
        blob_policies.append(1)                # code it (not "keep")
        blob_payloads.append(pcm)
        blob_channels.append(channels)

    tensor_entries, names, payloads = [], [], []
    dims_list, is_f32_list, elem_list, nibble_list = [], [], [], []
    if tensors:
        for name, arr in tensors.items():
            sname = str(name)
            if sname in blob_names:
                raise ValueError(f"duplicate stream name: {sname!r}")
            dtype_str, shape, raw, is_f32, elem_size, nibble = _model_tensor_to_bytes(
                arr, as_view=bool(store))
            if _raw_nbytes(raw) == 0:
                raise ValueError(f"tensor {sname!r} is empty")
            elem = _MODEL_DTYPE_ELEM.get(dtype_str, elem_size)
            count = (_raw_nbytes(raw) // elem_size
                     if elem_size in (1, 2, 4, 8) and _raw_nbytes(raw) % elem_size == 0
                     else _raw_nbytes(raw))
            if elem_size not in (1, 2, 4, 8):
                elem = 1
            names.append(sname); payloads.append(raw)
            dims_list.append(tuple(shape) if len(shape) <= 8 else ())
            is_f32_list.append(is_f32)
            elem_list.append(elem_size if elem_size in (1, 2, 4, 8) else 1)
            nibble_list.append(nibble)
            tensor_entries.append({
                "name": sname, "dtype": dtype_str, "shape": list(shape),
                "stream": sname, "elem_size": elem, "count": count,
                "kind": _MODEL_KIND_TENSOR})

    audio_entries = [{
        "name": n, "kind": "audio", "dtype": "uint8",
        "shape": [blob_params[i][1], blob_channels[i]],
        "stream": n, "content_type": _CT_PCM,
        "channels": blob_channels[i], "bits": blob_depths[i],
        "frames": blob_params[i][1], "sample_rate": blob_params[i][0],
    } for i, n in enumerate(blob_names)]

    manifest = _json.dumps({
        "format": _MODEL_FORMAT,
        "tensors": tensor_entries + audio_entries,
        "metadata": metadata or {},
    }, separators=(",", ":"))

    output_path = os.path.abspath(output_path)
    _c_model_save(output_path, manifest, names, payloads, dims_list,
                  is_f32_list, elem_list, nibble_list,
                  password=password or "", fast=1 if fast else 0,
                  store=1 if store else 0, durable=1 if durable else 0,
                  blob_names=blob_names, blob_kinds=blob_kinds,
                  blob_ctypes=blob_ctypes, blob_params=blob_params,
                  blob_depths=blob_depths, blob_policies=blob_policies,
                  blob_payloads=blob_payloads, blob_channels=blob_channels)
    return output_path


def load_audios(path, password=None):
    """Read every audio stream in a file -> {name: (pcm, channels, bits)}.

    Samples come back exactly as stored, so a round-trip is bit-exact.
    Use model_info(path) to see channels/bits/frames without decoding.
    """
    entries, _meta, _stats = model_info(os.path.abspath(path), password=password)
    names = [n for n, t in entries.items() if t.get("kind") == "audio"]
    if not names:
        return {}
    res = _c_model_load_some(os.path.abspath(path), names,
                             password=password or "")["tensors"]
    return {n: (res[n], entries[n].get("channels", 0),
                entries[n].get("bits", 0)) for n in names}


def save_tables(output_path, tables, tensors=None, password=None,
                metadata=None, fast=False, store=False, durable=True):
    """Save delimited text (CSV/TSV) into one .qtsq, alongside tensors.

    Each entry is name -> the file's text. Comma, semicolon, tab and pipe
    are detected automatically (only when one clearly dominates), so the
    caller does not have to declare a delimiter.

    The table lens contests row-major against column-major ordering and
    keeps whichever encodes smaller. Column-major wins large on numeric
    tables (measured -34% on a 165kB CSV) because the bytes are already
    compressible and only their ORDER defeats LZ; row-major wins on
    categorical tables (-26%), which repeat whole rows that front-coding
    already collapses. Contesting both means this can never lose to
    storing the text as-is.

    Transcoding is only attempted for a strict rectangle (one delimiter,
    no quoting, no CR, no empty or ragged rows). Anything else — quoted
    CSV, ragged rows, free text — is stored verbatim through the text
    lens instead, so every input round-trips byte-exactly.

    Returns:
        output_path
    """
    import json as _json

    if not isinstance(tables, dict) or not tables:
        raise ValueError("tables must be a non-empty dict {name: text}")

    blob_names, blob_kinds, blob_ctypes = [], [], []
    blob_params, blob_depths, blob_policies = [], [], []
    blob_payloads, blob_channels = [], []
    _KIND_TABLE = 5

    for name, val in tables.items():
        sname = str(name)
        if sname == _MODEL_MANIFEST_STREAM:
            raise ValueError(f"stream name {sname!r} is reserved")
        if len(sname.encode("utf-8")) > _MODEL_NAME_MAX:
            raise ValueError(f"name too long (>{_MODEL_NAME_MAX} bytes): {sname!r}")
        if isinstance(val, str):
            text = val.encode("utf-8")
        elif isinstance(val, (bytes, bytearray)):
            text = bytes(val)
        else:
            raise ValueError(f"table {sname!r} must be str or bytes")
        if not text:
            raise ValueError(f"table {sname!r} is empty")
        # content_type distinguishes csv from tsv for a reader; the lens
        # itself sniffs, so this is a hint, not a requirement.
        ctype = 49 if b"\t" in text.split(b"\n", 1)[0] else 48
        blob_names.append(sname)
        blob_kinds.append(_KIND_TABLE)
        blob_ctypes.append(ctype)
        blob_params.append((0, 0))
        blob_depths.append(0)
        blob_policies.append(1)          # code it
        blob_payloads.append(text)
        blob_channels.append(0)

    tensor_entries, names, payloads = [], [], []
    dims_list, is_f32_list, elem_list, nibble_list = [], [], [], []
    if tensors:
        for name, arr in tensors.items():
            sname = str(name)
            if sname in blob_names:
                raise ValueError(f"duplicate stream name: {sname!r}")
            dtype_str, shape, raw, is_f32, elem_size, nibble = _model_tensor_to_bytes(
                arr, as_view=bool(store))
            if _raw_nbytes(raw) == 0:
                raise ValueError(f"tensor {sname!r} is empty")
            elem = _MODEL_DTYPE_ELEM.get(dtype_str, elem_size)
            count = (_raw_nbytes(raw) // elem_size
                     if elem_size in (1, 2, 4, 8) and _raw_nbytes(raw) % elem_size == 0
                     else _raw_nbytes(raw))
            if elem_size not in (1, 2, 4, 8):
                elem = 1
            names.append(sname); payloads.append(raw)
            dims_list.append(tuple(shape) if len(shape) <= 8 else ())
            is_f32_list.append(is_f32)
            elem_list.append(elem_size if elem_size in (1, 2, 4, 8) else 1)
            nibble_list.append(nibble)
            tensor_entries.append({
                "name": sname, "dtype": dtype_str, "shape": list(shape),
                "stream": sname, "elem_size": elem, "count": count,
                "kind": _MODEL_KIND_TENSOR})

    table_entries = [{
        "name": n, "kind": "table", "dtype": "uint8", "shape": [len(blob_payloads[i])],
        "stream": n, "content_type": blob_ctypes[i],
        "original_size": len(blob_payloads[i]),
    } for i, n in enumerate(blob_names)]

    manifest = _json.dumps({
        "format": _MODEL_FORMAT,
        "tensors": tensor_entries + table_entries,
        "metadata": metadata or {},
    }, separators=(",", ":"))

    output_path = os.path.abspath(output_path)
    _c_model_save(output_path, manifest, names, payloads, dims_list,
                  is_f32_list, elem_list, nibble_list,
                  password=password or "", fast=1 if fast else 0,
                  store=1 if store else 0, durable=1 if durable else 0,
                  blob_names=blob_names, blob_kinds=blob_kinds,
                  blob_ctypes=blob_ctypes, blob_params=blob_params,
                  blob_depths=blob_depths, blob_policies=blob_policies,
                  blob_payloads=blob_payloads, blob_channels=blob_channels)
    return output_path


def load_tables(path, password=None):
    """Read every table stream in a file -> {name: text}.

    The text comes back byte-identical to what was stored. Use
    model_info(path) to list table streams without reading them.
    """
    entries, _meta, _stats = model_info(os.path.abspath(path), password=password)
    names = [n for n, t in entries.items() if t.get("kind") == "table"]
    if not names:
        return {}
    res = _c_model_load_some(os.path.abspath(path), names,
                             password=password or "")["tensors"]
    return {n: res[n] for n in names}


def save_model(tensors, output_path, password=None, metadata=None, fast=False, store=False,
               durable=True):
    """
    Save a full trained model to ONE .qtsq file — every weight type,
    bit-exact so retraining/inference resumes identically.

    No password needed: without one the file is sanitized + checksummed
    plaintext (like safetensors/.bin — integrity without friction);
    with a password it is vault-encrypted, or encrypted to the device
    key when the keystore is initialized. Tampering is always detected
    on load via per-stream SHA-256.

    Args:
        tensors: dict {name: array} — numpy arrays or torch.Tensors.
            Supported dtypes: float32 (TNS2 RAW_BITEXACT), float16,
            bfloat16, float64, complex64/128, int8/uint8, int16/uint16,
            int32/uint32, int64/uint64, bool, plus exact packed tags via
            (array, tag) tuples: (int8_values, "int4") with values in
            [-8, 7], (int8_values, "uint4") in [0, 15], and
            (uint8_bits, "float8_e5m2") for pre-quantized FP8-E5M2.
            Any shape incl. 0-d scalars and >8 dims (dims preserved in
            the manifest). Also accepts a torch nn.Module (uses state_dict()).
        output_path: path for .qtsq file
        password: optional — vault encryption; None uses device keystore
            when available, else tamper-evident plaintext.
        metadata: optional dict (arch, epochs, optimizer, tokenizer, ...)
        fast: False = full contest (best ratio); True = fast subset
            (bit-exact, different — usually slightly larger — bytes).
        store: True = verbatim framing at memcpy speed (ratio ~1.0).
        durable: True (default) fsyncs before rename (crash-safe);
            False skips the sync for maximum speed (power-loss window).
            Applies to plaintext writes; encrypted writes always sync.

    Returns:
        output_path

    Example:
        import torch
        qtsq.save_model(model.state_dict(), "llama.qtsq",
                        metadata={"arch": "llama-7b"})
        # private file:
        qtsq.save_model(model.state_dict(), "llama.qtsq", password="secret")
        # numpy
        qtsq.save_model({"w": w_f32, "q": q_int8}, "m.qtsq")
    """
    import json as _json

    # nn.Module → state_dict
    if hasattr(tensors, "state_dict") and callable(getattr(tensors, "state_dict")):
        tensors = tensors.state_dict()
    if not isinstance(tensors, dict) or not tensors:
        raise ValueError("tensors must be a non-empty dict {name: array}")

    output_path = os.path.abspath(output_path)
    names, payloads, dims_list = [], [], []
    is_f32_list, elem_list, nibble_list, manifest_tensors = [], [], [], []
    seen = set()
    for name, arr in tensors.items():
        sname = str(name)
        if sname == _MODEL_MANIFEST_STREAM:
            raise ValueError(f"tensor name {sname!r} is reserved")
        if len(sname.encode("utf-8")) > _MODEL_NAME_MAX:
            raise ValueError(f"tensor name too long (>{_MODEL_NAME_MAX} bytes): {sname!r}")
        if sname in seen:
            raise ValueError(f"duplicate tensor name: {sname!r}")
        seen.add(sname)
        dtype_str, shape, raw, is_f32, elem_size, nibble = _model_tensor_to_bytes(
            arr, as_view=bool(store))
        rlen = _raw_nbytes(raw)
        if rlen == 0:
            raise ValueError(f"tensor {sname!r} is empty")
        elem = _MODEL_DTYPE_ELEM.get(dtype_str, elem_size)
        if dtype_str in ("int4", "uint4"):
            n = 1
            for d in shape:
                n *= d
            count = n  # packed ceil(n/2) bytes in raw; numel in count
        elif elem_size in (1, 2, 4, 8) and rlen % elem_size == 0:
            count = rlen // elem_size
        else:
            count = rlen
            elem = 1
        # TNS2 embeds ≤8 dims; longer shapes and packed layouts (whose
        # dims describe values, not storage units) live in the manifest.
        if len(shape) <= 8 and dtype_str not in ("int4", "uint4"):
            dims_for_c = tuple(shape)
        else:
            dims_for_c = ()
        names.append(sname)
        payloads.append(raw)
        dims_list.append(dims_for_c)
        is_f32_list.append(is_f32)
        elem_list.append(elem_size if elem_size in (1, 2, 4, 8) else 1)
        nibble_list.append(nibble)
        manifest_tensors.append({
            "name": sname, "dtype": dtype_str, "shape": list(shape),
            "stream": sname, "elem_size": elem, "count": count,
            "kind": _MODEL_KIND_TENSOR,
        })
    manifest = _json.dumps({
        "format": _MODEL_FORMAT, "tensors": manifest_tensors,
        "metadata": metadata or {},
    }, separators=(",", ":"))
    _c_model_save(output_path, manifest, names, payloads,
                  dims_list, is_f32_list, elem_list, nibble_list,
                  password=password or "", fast=1 if fast else 0,
                  store=1 if store else 0, durable=1 if durable else 0)
    return output_path



def load_model(path, password=None, framework="numpy", writable=True):
    """
    Load a model saved with save_model — bit-exact dtypes/shapes.

    Password only needed if the file was saved with one (or the device
    keystore); plaintext weight files open directly. Tampering is
    detected via per-stream SHA-256 either way.

    Args:
        path: .qtsq model file
        password: decryption password (if encrypted)
        framework: "numpy" (default) or "torch" (returns torch.Tensors)
        writable: True (default) returns owned writable arrays; False
            returns read-only views (zero Python-side copy; numpy only —
            torch always copies into writable tensors)

    Returns:
        (tensors_dict, metadata_dict)

    Example:
        weights, meta = qtsq.load_model("llama.qtsq", password="secret")
        weights_torch, meta = qtsq.load_model("llama.qtsq", password="secret",
                                              framework="torch")
    """
    import json as _json

    path = os.path.abspath(path)
    if not password:
        # mmap fast path first (views or single-copy; falls back below).
        hit = _mmap_tensors_file(path, None, framework, writable)
        if hit is not None:
            return hit
    result = _c_model_load(path, password=password or "")
    entries, metadata, _full = _model_parse_manifest(result["manifest"])
    raw_map = {k: bytes(v) for k, v in result["tensors"].items()}

    out = {}
    for name, info in entries.items():
        # Non-tensor kinds (image/audio/table/blob/text) share the file but
        # are not numeric arrays: they have no dtype/shape arithmetic and
        # must not be run through _model_check_size, which would reject
        # them against a shape the manifest never claimed to be numeric.
        # They are read with load_images()/the blob helpers instead.
        if info.get("kind") and info["kind"] != _MODEL_KIND_TENSOR:
            continue
        raw = raw_map.get(info.get("stream", name))
        if raw is None:
            raise ValueError(f"model file corrupt: stream missing for tensor {name!r}")
        _model_check_size(name, info, len(raw))
        dtype_str, shape = info["dtype"], tuple(info["shape"])
        count = int(info.get("count", 0))

        def _np(dtype, writable=writable):
            a = np.frombuffer(raw, dtype=dtype)
            a = a.reshape(shape) if len(shape) else a.reshape(())
            return a.copy() if writable else a

        if dtype_str in ("int4", "uint4"):
            vals = _unpack_nibbles(raw, count, signed=(dtype_str == "int4"))
            vals = vals.reshape(shape) if len(shape) else vals.reshape(())
            if framework == "torch":
                try:
                    import torch as _torch
                    out[name] = _torch.from_numpy(vals.copy())
                    continue
                except Exception:
                    pass
            out[name] = vals
            continue
        if dtype_str == "float8_e5m2":
            # Bits of pre-quantized FP8-E5M2; returned as uint8.
            out[name] = _np(np.uint8)
            if framework == "torch":
                try:
                    import torch as _torch
                    out[name] = _torch.from_numpy(_np(np.uint8, True))
                except Exception:
                    pass
            continue
        if framework == "torch":
            try:
                import torch
                if dtype_str == "bfloat16":
                    u16 = _np(np.uint16, True)
                    t = torch.empty(shape if len(shape) else (u16.size,),
                                    dtype=torch.bfloat16)
                    t.view(torch.uint16).copy_(torch.from_numpy(u16))
                    out[name] = t
                    continue
                _torch_dtype = getattr(torch, dtype_str, None)
                if _torch_dtype is not None:
                    np_dtype = _model_numpy_dtype(dtype_str)
                    out[name] = torch.from_numpy(_np(np_dtype, True))
                    continue
            except Exception:
                pass
        np_dtype = _model_numpy_dtype(dtype_str)
        if np_dtype is None:
            # No numpy equivalent (bfloat16 w/o ml_dtypes, torch-only
            # fp8, …): keep raw bits + dtype tag instead of raising.
            out[name] = (_np(np.uint8 if dtype_str != "bfloat16" else np.uint16),
                         {"dtype": dtype_str})
        else:
            out[name] = _np(np_dtype)
    return out, metadata


class _MmapFallback(Exception):
    """Internal: this file needs the decoding path instead."""


def _mmap_verbatim_views(path, want, framework):
    # Legacy entry point: read-only views for an explicit want list.
    return _mmap_tensors_file(path, want, framework, False)


def _mmap_tensors_file(path, want, framework, writable):
    """mmap load of one container file: views or single-copy arrays.

    want=None loads every manifest tensor. Returns (out, metadata) or
    None (caller falls back to the decoding path). Plaintext V3 only;
    every payload is SHA-256 verified.
    """
    import hashlib
    import mmap as _mmap
    import struct as _struct

    if os.name != "posix":
        return None
    try:
        f = open(path, "rb")
    except OSError:
        return None
    try:
        size = os.fstat(f.fileno()).st_size
        if size < 112:
            return None
        mm = _mmap.mmap(f.fileno(), 0, access=_mmap.ACCESS_READ)
    except (OSError, ValueError):
        return None
    finally:
        try:
            f.close()  # mapping persists; views pin mm via base chain
        except Exception:
            pass
    try:
        return _mmap_tensors_inner(mm, size, path, want, framework,
                                   writable, hashlib, _struct)
    except _MmapFallback:
        return None
    except (KeyError, ValueError):
        raise
    except Exception:
        return None


def _verbatim_range(mv, size, start, dsize, info, hashlib):
    """Locate verbatim array bytes inside one TNS2 stream.

    Returns (offset, length) or None (needs real decoding). Verifies
    the TNS2 payload checksum over the stored blob first.
    """
    import struct as _struct

    if dsize < 72 or start + dsize > size:
        return None
    if mv[start:start + 4].tobytes() != b"TNS2":
        return None
    mode = mv[start + 4]
    ndims = mv[start + 5]
    if ndims > 8:
        return None
    r0 = mv[start + 6]
    count = _struct.unpack_from("<Q", mv, start + 8)[0]
    hlen = 72 + ndims * 4
    if hlen > dsize:
        return None
    stored = mv[start + hlen - 40:start + hlen - 8].tobytes()
    (uncomp,) = _struct.unpack_from("<Q", mv, start + hlen - 8)
    blob = start + hlen
    blob_len = dsize - hlen
    if mode == 1 and r0 == 2:
        # raw payload bytes verbatim
        if blob_len != uncomp or uncomp != count * 4:
            return None
        if hashlib.sha256(mv[blob:blob + blob_len]).digest() != stored:
            raise ValueError("tensor %r checksum mismatch (corrupt?)" % (info["name"],))
        return blob, blob_len
    if mode == 1 and r0 == 1:
        # qtc lens bytes: verbatim only in mode-7 (raw) framing
        if blob_len < 6:
            return None
        if hashlib.sha256(mv[blob:blob + blob_len]).digest() != stored:
            raise ValueError("tensor %r checksum mismatch (corrupt?)" % (info["name"],))
        (icount,) = _struct.unpack_from("<I", mv, blob)
        if mv[blob + 4] != 7 or mv[blob + 5] != 0:
            return None
        if icount != count or blob_len - 6 != count * 4:
            return None
        return blob + 6, blob_len - 6
    if mode in (5, 6, 7, 8):
        # array envelope carries an extra [u64 inner_len] prefix; the
        # hash covers the inner bytes only (mirrors decompress_array).
        if blob_len < 8 + 6:
            return None
        (inner_len,) = _struct.unpack_from("<Q", mv, blob)
        inner = blob + 8
        if inner_len != blob_len - 8:
            return None
        if hashlib.sha256(mv[inner:inner + inner_len]).digest() != stored:
            raise ValueError("tensor %r checksum mismatch (corrupt?)" % (info["name"],))
        # qtc array bytes: verbatim only in mode-3 (STORED) framing
        if inner_len < 6:
            return None
        (acount,) = _struct.unpack_from("<I", mv, inner)
        aelem = mv[inner + 5]
        if mv[inner + 4] != 3 or aelem not in (1, 2, 4, 8):
            return None
        if acount * aelem != uncomp or inner_len - 6 != uncomp:
            return None
        return inner + 6, inner_len - 6
    return None


def _mmap_tensors_inner(mm, size, path, want, framework, writable,
                          hashlib, _struct):
    mv = memoryview(mm)
    if len(mv) < 112 or mv[0:4].tobytes() != b"QTS\x01" or mv[4] != 3:
        raise _MmapFallback()
    flags = _struct.unpack_from("<I", mv, 6)[0]
    if mv[10] != 0xF0 or (flags & (0x10 | 0x10000 | 0x20000000)):
        raise _MmapFallback()  # not a container, or encrypted
    horizon, accretion, sing_size, sec_size = _struct.unpack_from("<4Q", mv, 62)
    rad_size = _struct.unpack_from("<Q", mv, 86)[0]
    # Plaintext files carry a zero-filled security stub (mirroring
    # write_impl); only a salted block means keys → fall back.
    sec_off = 112 + horizon + accretion + sing_size + rad_size
    if sec_size != 0:
        if sec_off + sec_size > size or sec_off < 112 or sec_size > size:
            raise _MmapFallback()
        # salt sits at +32 (version/type/nonce/tag precede it); the name
        # key derives from the salt alone, so zero salt = cleartext.
        if sec_off + 64 > size or mv[sec_off + 32:sec_off + 64].tobytes() != bytes(32):
            raise _MmapFallback()
    if horizon > size or accretion > size or sing_size > size:
        raise _MmapFallback()
    off = 112 + horizon
    if off + accretion > size:
        raise _MmapFallback()
    acc_end = off + accretion
    while off < acc_end:
        if off + 5 > size:
            raise _MmapFallback()
        ttype = mv[off]
        (psize,) = _struct.unpack_from("<I", mv, off + 1)
        if psize == 0 and ttype == 0:
            break
        if psize > 65536:
            raise _MmapFallback()
        off += 5 + psize
    if off != acc_end:
        off = 112 + horizon + accretion
    if off + sing_size > size:
        raise _MmapFallback()
    base = off
    (nstreams,) = _struct.unpack_from("<I", mv, off)
    if nstreams > 65536:
        raise _MmapFallback()
    off += 4
    streams = {}
    for _ in range(nstreams):
        if off + 32 > size:
            raise _MmapFallback()
        _dt, _st, _fl, _rsv, doff, dsize, _os = _struct.unpack_from(
            "<BBHIQQQ", mv, off)
        off += 32
        start = base + 4 + nstreams * 32 + doff
        if start + 4 > size:
            raise _MmapFallback()
        end = start
        while end < size and mv[end] != 0:
            end += 1
            if end - start > 256:
                raise _MmapFallback()
        if end >= size:
            raise _MmapFallback()
        try:
            name = mv[start:end].tobytes().decode("utf-8", "strict")
        except UnicodeDecodeError:
            raise _MmapFallback()
        padded = ((end - start + 1 + 3) // 4) * 4
        streams[name] = (start + padded, dsize)
    # manifest still needs horizon decoding — tiny, via the C mmap path
    res = _c_model_load_some(path, [], password="")
    entries, metadata, _full = _model_parse_manifest(res["manifest"])
    if framework == "torch":
        raise _MmapFallback()  # torch needs writable tensors
    names = list(entries) if want is None else want
    for w in names:
        if w not in entries:
            raise KeyError("tensors not in checkpoint: [%r]" % (w,))

    def _one(w):
        info = entries[w]
        if info["dtype"] in ("int4", "uint4"):
            raise _MmapFallback()  # needs nibble unpack (a copy anyway)
        loc = streams.get(info.get("stream", w))
        if loc is None:
            raise _MmapFallback()
        start, dsize = loc
        found = _verbatim_range(mv, size, start, dsize, info, hashlib)
        if found is None:
            raise _MmapFallback()
        roff, rlen = found
        _model_check_size(w, info, rlen)
        import numpy as _np_mod
        np_dtype = _model_numpy_dtype(info["dtype"])
        if np_dtype is None:
            raise _MmapFallback()
        view = mv[roff:roff + rlen]
        a = _np_mod.frombuffer(view, dtype=np_dtype)
        a = a.reshape(tuple(info["shape"])) if info["shape"] else a.reshape(())
        if writable:
            a = a.copy()
        else:
            a.flags.writeable = False
        return a

    out = {}
    if len(names) > 1:
        # hashlib releases the GIL: verify slices concurrently.
        import concurrent.futures as _cf
        with _cf.ThreadPoolExecutor(max_workers=min(8, len(names))) as ex:
            futs = {ex.submit(_one, w): w for w in names}
            for fu in _cf.as_completed(futs):
                out[futs[fu]] = fu.result()
        out = {w: out[w] for w in names}  # manifest order
    else:
        for w in names:
            out[w] = _one(w)
    return out, metadata


def load_tensors(path, names, password=None, framework="numpy", writable=True):
    """Load only the named tensors — for custom inference engines.

    Reads the manifest, decompresses just the requested streams, and
    skips everything else (optimizer moments, unused layers). Shard-aware:
    pass any shard or prefix. Unknown names raise KeyError; corrupt
    streams raise ValueError. With writable=False on plaintext files,
    verbatim payloads come back as zero-copy read-only views straight
    off an mmap (no payload copy at all).

    Returns:
        (tensors_dict, metadata_dict).
    """
    import glob as _glob

    want = [str(n) for n in names]
    first = os.path.abspath(path)
    if not first.endswith(".qtsq") or not os.path.exists(first):
        cands = sorted(_glob.glob(first + "-*.qtsq"))
        if not cands:
            raise FileNotFoundError(f"no shards found for prefix: {first}")
        first = cands[0]
    if not password:
        hit = _mmap_tensors_file(first, want, framework, writable)
        if hit is not None:
            _entries, _meta, _full = _model_parse_manifest(
                _c_model_load_some(first, [], password="")["manifest"])
            if not (_full.get("shards") and len(_full["shards"]) > 1):
                return hit
    # Manifest via the lazy reader (mmap fast path serves it without
    # reading the whole file); _c_model_info does a full read.
    entries, metadata, full = _model_parse_manifest(
        _c_model_load_some(first, [], password=password or "")["manifest"])
    missing = [n for n in want if n not in entries]
    if missing:
        raise KeyError(f"tensors not in checkpoint: {missing[:5]}")
    shards = full.get("shards") or []
    if len(shards) > 1:
        directory = os.path.dirname(first)
        # group wanted streams by shard
        by_shard = {}
        for n in want:
            sh = entries[n].get("shard", shards[0])
            by_shard.setdefault(sh, []).append(n)
        out = {}
        for sh, sub in by_shard.items():
            p = os.path.join(directory, sh)
            if not os.path.exists(p):
                raise FileNotFoundError(f"shard missing: {p}")
            res = _c_model_load_some(p, sub, password=password or "")
            raw_map = {k: bytes(v) for k, v in res["tensors"].items()}
            for n in sub:
                info = entries[n]
                raw = raw_map.get(info.get("stream", n))
                if raw is None:
                    raise ValueError(f"tensor {n!r} missing from {sh} (corrupt?)")
                _model_check_size(n, info, len(raw))
                _load_one_tensor(out, n, info, raw, framework, writable)
        return out, metadata
    result = _c_model_load_some(first, want, password=password or "")
    raw_map = {k: bytes(v) for k, v in result["tensors"].items()}
    out = {}
    for n in want:
        info = entries[n]
        raw = raw_map.get(info.get("stream", n))
        if raw is None:
            raise ValueError(f"tensor {n!r} missing from file (corrupt?)")
        _model_check_size(n, info, len(raw))
        _load_one_tensor(out, n, info, raw, framework, writable)
    return out, metadata


def _model_parse_manifest(manifest_bytes):
    """Parse + validate a model manifest → (entries, metadata, full).

    Raises ValueError with a clear message on anything malformed
    (corrupt file, truncated manifest, unknown format version) instead
    of KeyError/TypeError.

    The "format" field was written but never checked before 2.2; it is
    validated now so a truncated or foreign manifest is rejected up
    front instead of surfacing later as a confusing per-tensor error.
    A /1 manifest carries no "kind" and is read as all-tensor.
    """
    import json as _json

    try:
        m = _json.loads(bytes(manifest_bytes).decode("utf-8"))
    except Exception as e:
        raise ValueError(f"model manifest corrupt: {e}") from e
    if not isinstance(m, dict) or not isinstance(m.get("tensors"), list):
        raise ValueError("model manifest corrupt: missing tensor list")

    fmt = m.get("format")
    if fmt is not None and fmt not in _MODEL_FORMATS_ACCEPTED:
        raise ValueError(
            "model manifest format %r not supported by qtsq %s (supported: %s)"
            % (fmt, __version__, ", ".join(_MODEL_FORMATS_ACCEPTED)))

    entries = {}
    for t in m["tensors"]:
        if not isinstance(t, dict):
            raise ValueError("model manifest corrupt: bad tensor entry")
        for k in ("name", "dtype", "shape"):
            if k not in t:
                raise ValueError(f"model manifest corrupt: entry missing {k!r}")
        if (not isinstance(t["shape"], list) or
                any(not isinstance(d, int) or d < 0 for d in t["shape"])):
            raise ValueError(
                f"model manifest corrupt: bad shape for {t.get('name')!r}")
        # Normalize kind: absent => tensor (/1 compatibility).
        kind = t.get("kind") or _MODEL_KIND_TENSOR
        if kind not in _MODEL_KINDS:
            raise ValueError(
                "model manifest corrupt: unknown kind %r for %r (known: %s)"
                % (kind, t.get("name"), ", ".join(_MODEL_KINDS)))
        t["kind"] = kind
        entries[str(t["name"])] = t
    meta = m.get("metadata") or {}
    if not isinstance(meta, dict):
        raise ValueError("model manifest corrupt: bad metadata")
    return entries, meta, m


def _raw_nbytes(raw):
    return raw.nbytes if isinstance(raw, memoryview) else len(raw)


def _model_check_size(name, info, raw_len):
    """Verify a stream's byte length matches its manifest entry."""
    n = 1
    for d in info["shape"]:
        n *= d
    dtype = info["dtype"]
    if dtype in ("int4", "uint4"):
        exp = (n + 1) // 2
    else:
        el = _MODEL_DTYPE_ELEM.get(dtype)
        if el is None:
            if dtype == "bfloat16":
                el = 2
            else:
                try:
                    el = np.dtype(dtype).itemsize
                except TypeError:
                    return  # unknown layout — decode will decide
        exp = n * el
    if raw_len != exp:
        raise ValueError(
            f"tensor {name!r} corrupt: {raw_len} bytes, manifest says {exp}")


def model_info(path, password=None):
    """Inspect a checkpoint without loading weights.

    Returns:
        (tensor_entries, metadata, stats) where tensor_entries is
        {name: {name, kind, dtype, shape, ...}} from the manifest, and
        stats carries {shards, raw_bytes, kinds}.

        "kind" is "tensor" for every entry in a /1 manifest (the format
        predates kinds) and one of tensor/blob/text/image/audio/table in
        a /2 manifest. "kinds" in stats counts them, so a mixed
        container can be described without decoding a payload.

    Works on any single shard of a sharded checkpoint (every shard
    carries the full manifest). Served by the lazy reader — only the
    tiny manifest stream is faulted in, never the weights.
    """
    entries, meta, full = _model_parse_manifest(
        _c_model_load_some(os.path.abspath(path), [], password=password or "")["manifest"])
    total = 0
    kinds = {}
    for t in entries.values():
        kind = t.get("kind") or _MODEL_KIND_TENSOR
        kinds[kind] = kinds.get(kind, 0) + 1
        if kind != _MODEL_KIND_TENSOR:
            # Non-tensor entries carry no shape arithmetic; their byte
            # count lives in the stream header, not the manifest.
            continue
        n = 1
        for d in t["shape"]:
            n *= d
        if t["dtype"] in ("int4", "uint4"):
            total += (n + 1) // 2
            continue
        el = _MODEL_DTYPE_ELEM.get(t["dtype"], 0) or 0
        if el == int(el):
            total += n * int(el)
    return entries, meta, {"shards": full.get("shards"), "raw_bytes": total,
                           "kinds": kinds,
                           "format": full.get("format") or _MODEL_FORMAT_V1}


# ─────────────────────────────────────────────────────────────
#  Sharded checkpoints (large models: many .qtsq files, one manifest)
# ─────────────────────────────────────────────────────────────

def save_model_sharded(tensors, output_prefix, password=None, max_shard_bytes=1 << 30,
                       metadata=None, fast=False, store=False, durable=True):
    """Save a large model as sharded .qtsq files.

    Each shard is a self-contained model file holding a subset
    of tensors plus the FULL manifest (tensor → shard assignment), so no
    unencrypted index leaks names. Greedy packs tensors in dict order.
    Password semantics match save_model (optional).

    Args:
        tensors: same as save_model (dict, nn.Module, or state_dict).
        output_prefix: e.g. "/models/llama" → "llama-00001-of-00003.qtsq"…
        password: optional (vault / keystore / plaintext, as save_model).
        max_shard_bytes: max raw payload bytes per shard (default 1 GiB).
        metadata: optional dict stored in every shard manifest.

    Returns:
        list of shard paths.
    """
    import json as _json

    if hasattr(tensors, "state_dict") and callable(getattr(tensors, "state_dict")):
        tensors = tensors.state_dict()
    if not isinstance(tensors, dict) or not tensors:
        raise ValueError("tensors must be a non-empty dict {name: array}")

    # Materialize each tensor once (validates names/dtypes first).
    items = []  # (sname, dtype_str, shape, raw, is_f32, elem_size, nibble)
    seen = set()
    for name, arr in tensors.items():
        sname = str(name)
        if sname == _MODEL_MANIFEST_STREAM:
            raise ValueError(f"tensor name {sname!r} is reserved")
        if len(sname.encode("utf-8")) > _MODEL_NAME_MAX:
            raise ValueError(f"tensor name too long: {sname!r}")
        if sname in seen:
            raise ValueError(f"duplicate tensor name: {sname!r}")
        seen.add(sname)
        dtype_str, shape, raw, is_f32, elem_size, nibble = _model_tensor_to_bytes(
            arr, as_view=bool(store))
        if _raw_nbytes(raw) == 0:
            raise ValueError(f"tensor {sname!r} is empty")
        items.append((sname, dtype_str, tuple(shape), raw, is_f32, elem_size, nibble))

    # Greedy pack by raw bytes.
    shards = [[]]
    shard_sizes = [0]
    for it in items:
        sz = _raw_nbytes(it[3])
        if shard_sizes[-1] + sz > max_shard_bytes and shards[-1]:
            shards.append([])
            shard_sizes.append(0)
        shards[-1].append(it)
        shard_sizes[-1] += sz
    n = len(shards)
    prefix = os.path.abspath(output_prefix)
    if prefix.endswith(".qtsq"):
        prefix = prefix[:-5]
    shard_names = [f"{prefix}-{i + 1:05d}-of-{n:05d}.qtsq" for i in range(n)]

    manifest_tensors = []
    for si, shard in enumerate(shards):
        for (sname, dtype_str, shape, raw, _is_f32, elem_size, _nib) in shard:
            elem = _MODEL_DTYPE_ELEM.get(dtype_str, elem_size)
            if dtype_str in ("int4", "uint4"):
                count = 1
                for d in shape:
                    count *= d
            elif elem_size in (1, 2, 4, 8) and _raw_nbytes(raw) % elem_size == 0:
                count = _raw_nbytes(raw) // elem_size
            else:
                count = _raw_nbytes(raw)
                elem = 1
            manifest_tensors.append({
                "name": sname, "dtype": dtype_str, "shape": list(shape),
                "stream": sname, "elem_size": elem, "count": count,
                "kind": _MODEL_KIND_TENSOR,
                "shard": os.path.basename(shard_names[si]),
            })
    manifest = _json.dumps({
        "format": _MODEL_FORMAT,
        "shards": [os.path.basename(p) for p in shard_names],
        "tensors": manifest_tensors, "metadata": metadata or {},
    }, separators=(",", ":"))

    for si, shard in enumerate(shards):
        names = [it[0] for it in shard]
        payloads = [it[3] for it in shard]
        dims_list = [it[2] if len(it[2]) <= 8 and it[1] not in ("int4", "uint4") else ()
                     for it in shard]
        is_f32_list = [it[4] for it in shard]
        elem_list = [it[5] if it[5] in (1, 2, 4, 8) else 1 for it in shard]
        nibble_list = [it[6] for it in shard]
        _c_model_save(shard_names[si], manifest, names, payloads,
                      dims_list, is_f32_list, elem_list, nibble_list,
                      password=password or "", fast=1 if fast else 0,
                      store=1 if store else 0, durable=1 if durable else 0)
    return shard_names


def load_model_sharded(path_or_prefix, password=None, framework="numpy", writable=True):
    """Load a sharded checkpoint written by save_model_sharded.

    Args:
        path_or_prefix: any shard path, or the output prefix — sibling
            shards are discovered from the manifest's shard list.
        password: decryption password (if the shards are encrypted).
        framework: "numpy" or "torch".

    Returns:
        (tensors_dict, metadata_dict).
    """
    import glob as _glob

    given = os.path.abspath(path_or_prefix)
    if os.path.isdir(given):
        raise ValueError("pass a shard file or the output prefix, not a directory")
    first = given
    if not first.endswith(".qtsq") or not os.path.exists(first):
        cands = sorted(_glob.glob(given + "-*.qtsq"))
        if not cands:
            raise FileNotFoundError(f"no shards found for prefix: {given}")
        first = cands[0]

    base = _c_model_load(first, password=password or "")
    entries, meta0, full = _model_parse_manifest(base["manifest"])
    shard_files = full.get("shards") or []
    directory = os.path.dirname(first)
    paths = [os.path.join(directory, s) for s in shard_files] if shard_files else [first]

    merged, meta = {}, {}
    for i, p in enumerate(paths):
        if not os.path.exists(p):
            raise FileNotFoundError(f"shard missing: {p}")
        res = base if (i == 0 and p == first) else _c_model_load(p, password=password or "")
        if i == 0:
            meta = meta0
        raw_map = {k: bytes(v) for k, v in res["tensors"].items()}
        for name, info in entries.items():
            if info.get("shard", os.path.basename(paths[0])) != os.path.basename(p):
                if "shard" in info:
                    continue
            raw = raw_map.get(info.get("stream", name))
            if raw is None:
                continue
            if name in merged:
                continue
            _model_check_size(name, info, len(raw))
            _load_one_tensor(merged, name, info, raw, framework, writable)
    # Fallback: single-file manifest without shard fields → plain load.
    if not merged:
        return load_model(first, password=password, framework=framework)
    missing = [k for k in entries if k not in merged]
    if missing:
        raise ValueError(f"sharded load incomplete, missing: {missing[:5]}")
    return merged, meta


def _load_one_tensor(out, name, info, raw, framework, writable=True):
    """Shared single-tensor decode used by load_model_sharded."""
    dtype_str, shape = info["dtype"], tuple(info["shape"])
    count = int(info.get("count", 0))

    def _np(dtype, writable=writable):
        a = np.frombuffer(raw, dtype=dtype)
        a = a.reshape(shape) if len(shape) else a.reshape(())
        return a.copy() if writable else a

    def _ro(a):
        if not writable:
            a = a.view()
            a.flags.writeable = False
        return a

    if dtype_str in ("int4", "uint4"):
        vals = _unpack_nibbles(raw, count, signed=(dtype_str == "int4"))
        vals = vals.reshape(shape) if len(shape) else vals.reshape(())
        if framework == "torch":
            try:
                import torch as _torch
                out[name] = _torch.from_numpy(vals.copy())
                return
            except Exception:
                pass
        out[name] = _ro(vals)
        return
    if dtype_str == "float8_e5m2":
        out[name] = _np(np.uint8)
        if framework == "torch":
            try:
                import torch as _torch
                out[name] = _torch.from_numpy(_np(np.uint8, True))
            except Exception:
                pass
        return
    if framework == "torch":
        try:
            import torch
            if dtype_str == "bfloat16":
                u16 = _np(np.uint16, True)
                t = torch.empty(shape if len(shape) else (u16.size,),
                                dtype=torch.bfloat16)
                t.view(torch.uint16).copy_(torch.from_numpy(u16))
                out[name] = t
                return
            _torch_dtype = getattr(torch, dtype_str, None)
            if _torch_dtype is not None:
                out[name] = torch.from_numpy(_np(_model_numpy_dtype(dtype_str), True))
                return
        except Exception:
            pass
    np_dtype = _model_numpy_dtype(dtype_str)
    if np_dtype is None:
        out[name] = (_np(np.uint8 if dtype_str != "bfloat16" else np.uint16),
                     {"dtype": dtype_str})
    else:
        out[name] = _np(np_dtype)


# ─────────────────────────────────────────────────────────────
#  safetensors interop
# ─────────────────────────────────────────────────────────────

def export_safetensors(qtsq_path, st_path, password=None, framework="numpy"):
    """Export a .qtsq model file to safetensors (requires safetensors lib)."""
    try:
        from safetensors.numpy import save_file as _st_save
    except Exception as e:
        raise ImportError("safetensors lib required: pip install safetensors") from e
    tensors, _meta = load_model(qtsq_path, password=password, framework="numpy")
    plain = {}
    for k, v in tensors.items():
        if isinstance(v, tuple):
            raise ValueError(f"tensor {k!r} needs ml_dtypes for safetensors export "
                             "(pip install ml_dtypes)")
        plain[k] = np.ascontiguousarray(v)
    _st_save(plain, os.path.abspath(st_path))
    return os.path.abspath(st_path)


def import_safetensors(st_path, qtsq_path, password=None, metadata=None):
    """Import a safetensors file to .qtsq (requires safetensors lib)."""
    try:
        from safetensors.numpy import load_file as _st_load
    except Exception as e:
        raise ImportError("safetensors lib required: pip install safetensors") from e
    tensors = _st_load(os.path.abspath(st_path))
    return save_model(dict(tensors), qtsq_path, password=password, metadata=metadata)
