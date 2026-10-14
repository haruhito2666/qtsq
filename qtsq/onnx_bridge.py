"""qtsq.onnx_bridge — move weights between ONNX and .qtsq.

Custom architectures trained anywhere (PyTorch → ONNX export, JAX/ORT
pipelines, hand-built graphs) store their parameters as ONNX
initializers. These helpers convert them losslessly to .qtsq and back,
so any custom inference engine (ONNX Runtime, TensorRT via ONNX, …)
can consume qtsq checkpoints:

    from qtsq.onnx_bridge import import_onnx_weights, export_onnx_weights

    import_onnx_weights("model.onnx", "model.qtsq")          # any arch
    export_onnx_weights("trained.qtsq", "arch.onnx", "out.onnx")

Requires: pip install onnx
"""

import os


def _need_onnx():
    try:
        import onnx  # noqa: F401
    except Exception as e:
        raise ImportError("onnx required: pip install onnx") from e


def _qtsq_dtype_to_onnx(dtype_str):
    import onnx

    table = {
        "float32": onnx.TensorProto.FLOAT,
        "float16": onnx.TensorProto.FLOAT16,
        "bfloat16": onnx.TensorProto.BFLOAT16,
        "int8": onnx.TensorProto.INT8,
        "uint8": onnx.TensorProto.UINT8,
        "int16": onnx.TensorProto.INT16,
        "uint16": onnx.TensorProto.UINT16,
        "int32": onnx.TensorProto.INT32,
        "uint32": onnx.TensorProto.UINT32,
        "int64": onnx.TensorProto.INT64,
        "uint64": onnx.TensorProto.UINT64,
        "float64": onnx.TensorProto.DOUBLE,
        "complex64": onnx.TensorProto.COMPLEX64,
        "complex128": onnx.TensorProto.COMPLEX128,
        "bool": onnx.TensorProto.BOOL,
        "float8_e5m2": onnx.TensorProto.FLOAT8E5M2,
        "int4": onnx.TensorProto.INT4,
        "uint4": onnx.TensorProto.UINT4,
    }
    if dtype_str not in table:
        raise ValueError(f"dtype {dtype_str!r} has no ONNX equivalent")
    return table[dtype_str]


def import_onnx_weights(onnx_path, qtsq_path, password=None, metadata=None):
    """Store an .onnx file's initializers bit-exact in .qtsq.

    Works for any custom architecture — only the parameter tensors and
    shapes travel (graph structure stays in the .onnx file).
    Returns the .qtsq path.
    """
    _need_onnx()
    import numpy as np
    import onnx
    from onnx import numpy_helper

    from . import save_model

    model = onnx.load(os.path.abspath(onnx_path))
    tensors = {}
    for init in model.graph.initializer:
        try:
            arr = numpy_helper.to_array(init)
        except Exception as e:
            raise ValueError(f"initializer {init.name!r} unreadable: {e}") from e
        if arr.dtype == object:
            raise ValueError(f"initializer {init.name!r} is strings — not storable")
        elem = onnx.TensorProto.DataType.Name(init.data_type)
        if elem in ("INT4", "UINT4"):
            # numpy_helper unpacks 4-bit to int8 → repack with qtsq tag
            tensors[init.name] = (np.asarray(arr, dtype=np.int8),
                                  "int4" if elem == "INT4" else "uint4")
        elif elem in ("FLOAT8E4M3FN", "FLOAT8E4M3FNUZ",
                      "FLOAT8E5M2", "FLOAT8E5M2FNUZ"):
            bits = np.asarray(arr)
            if bits.dtype != np.uint8:
                bits = bits.view(np.uint8)
            tag = "float8_e5m2" if "E5M2" in elem else "float8_e4m3"
            tensors[init.name] = (np.ascontiguousarray(bits), tag)
        else:
            tensors[init.name] = np.ascontiguousarray(arr)
    meta = dict(metadata or {})
    meta.setdefault("onnx_source", os.path.basename(os.path.abspath(onnx_path)))
    return save_model(tensors, qtsq_path, password=password, metadata=meta)


def export_onnx_weights(qtsq_path, template_onnx_path, out_onnx_path, password=None):
    """Write .qtsq weights into a template .onnx file's initializers.

    The template supplies the custom architecture (graph); matching
    initializers are replaced bit-exact (dtype + element count must
    match). Extra qtsq tensors not in the template are ignored.
    Returns the output path.
    """
    _need_onnx()
    import numpy as np
    import onnx
    from onnx import numpy_helper

    from . import load_model

    tensors, _meta = load_model(os.path.abspath(qtsq_path), password=password)
    model = onnx.load(os.path.abspath(template_onnx_path))

    table = {}
    for name, value in tensors.items():
        # Packed/exotic tags have no lossless ONNX mapping yet (nibble
        # order differs by spec) — refuse loudly instead of corrupting.
        if isinstance(value, tuple):
            raise ValueError(
                f"tensor {name!r} is packed ({value[1].get('dtype')}); "
                "dequantize to a standard dtype before ONNX export")
        table[name] = np.ascontiguousarray(value)

    for init in model.graph.initializer:
        if init.name not in table:
            continue
        arr = table[init.name]
        want_type = _qtsq_dtype_to_onnx(str(arr.dtype))
        if want_type != init.data_type:
            raise ValueError(
                f"dtype mismatch for {init.name!r}: file has {arr.dtype}, "
                f"template wants {onnx.TensorProto.DataType.Name(init.data_type)}")
        if arr.size != _init_numel(init):
            raise ValueError(f"shape mismatch for {init.name!r}")
        new_init = numpy_helper.from_array(
            np.ascontiguousarray(arr).reshape(tuple(init.dims)), name=init.name)
        init.CopyFrom(new_init)

    onnx.save(model, os.path.abspath(out_onnx_path))
    return os.path.abspath(out_onnx_path)


def _init_numel(init):
    n = 1
    for d in init.dims:
        n *= d
    return n
