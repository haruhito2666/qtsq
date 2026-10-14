"""qtsq.checkpoint — training-loop checkpointing for ML weights.

Drop two lines into any training loop and every epoch (or every N steps)
exports the model, optimizer, scheduler, metrics, config, and related
files (tokenizer, configs) directly to .qtsq — bit-exact, password-free
by default, encrypted on request.

    from qtsq.checkpoint import save_checkpoint, load_checkpoint

    for epoch in range(epochs):
        loss = train_one_epoch(model, loader, optimizer)
        save_checkpoint(f"ckpt-e{epoch}.qtsq", model, optimizer=optimizer,
                        scheduler=scheduler, epoch=epoch,
                        metrics={"loss": loss}, config={"arch": "mlp"},
                        extra_files={"tokenizer.json": "tok/tokenizer.json"})

    # resume anywhere (fresh process, new objects)
    info = load_checkpoint("ckpt-e5.qtsq", model, optimizer=optimizer)
    start_epoch = info["epoch"] + 1

No hard dependency on torch at import time; torch is required only when
passing torch objects. HuggingFace Trainer and PyTorch Lightning adapters
are provided when those packages are installed.
"""

import os

_OPT_PREFIX = "__optim_"
_SCHED_PREFIX = "__sched_"
_RESERVED_META = frozenset({
    "qtsq_checkpoint", "epoch", "metrics", "config",
    "optim", "sched", "files",
})
_RESERVED_STREAMS = (_OPT_PREFIX, _SCHED_PREFIX)
_EXTRA_FILES_MAX = 8 << 20  # 8 MiB total for embedded related files


def _need_torch():
    try:
        import torch  # noqa: F401
    except Exception as e:
        raise ImportError("torch required for checkpoints: pip install torch") from e


def _is_tensor(x):
    mod = type(x).__module__ or ""
    return mod.startswith("torch") or hasattr(x, "untyped_storage")


def _ensure_jsonable(x, path=""):
    if x is None or isinstance(x, (bool, int, float, str)):
        return x
    # Training loops log numpy scalars and 0-d torch tensors — coerce.
    if type(x).__module__ == "numpy" and hasattr(x, "item"):
        try:
            return _ensure_jsonable(x.item(), path)
        except Exception:
            pass
    if _is_tensor(x):
        try:
            import torch
            if isinstance(x, torch.Tensor) and x.numel() == 1:
                return _ensure_jsonable(x.detach().to("cpu").reshape(-1)[0].item(), path)
        except Exception:
            pass
        raise TypeError(f"checkpoint metadata at {path!r} is a tensor — "
                        "pass tensors as model/optimizer state, not metadata")
    if isinstance(x, dict):
        return {str(k): _ensure_jsonable(v, f"{path}.{k}") for k, v in x.items()}
    if isinstance(x, (list, tuple)):
        return [_ensure_jsonable(v, f"{path}[]") for v in x]
    raise TypeError(f"checkpoint metadata at {path!r} is not JSON-serializable "
                    f"({type(x).__name__}); pass tensors as model/optimizer state")


def _flatten(prefix, obj, tensors, path=""):
    """Split nested optim/scheduler state into (clean, tensors).

    Tensors are lifted into `tensors` keyed by dotted path; the returned
    structure mirrors `obj` with {"__tensor": path} markers. Everything
    left must be JSON-serializable.
    """
    if _is_tensor(obj):
        tensors[prefix.rstrip(".")] = obj
        return {"__tensor": prefix.rstrip(".")}
    if isinstance(obj, dict):
        return {str(k): _flatten(f"{prefix}{k}.", v, tensors, f"{path}.{k}")
                for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [_flatten(f"{prefix}{i}.", v, tensors, f"{path}[]")
                for i, v in enumerate(obj)]
    return _ensure_jsonable(obj, path)


def _materialize(node, table):
    """Inverse of _flatten: swap {"__tensor": name} markers for tensors."""
    if isinstance(node, dict):
        if set(node.keys()) == {"__tensor"}:
            name = node["__tensor"]
            if name not in table:
                raise ValueError(f"checkpoint corrupt: tensor {name!r} missing")
            return table[name]
        return {k: _materialize(v, table) for k, v in node.items()}
    if isinstance(node, list):
        return [_materialize(v, table) for v in node]
    return node


def _optim_tables(optimizer):
    """torch optimizer → (tensors, params) split for storage."""
    sd = optimizer.state_dict()
    tensors = {}
    state = _flatten(_OPT_PREFIX + "state.", sd.get("state", {}), tensors, "state")
    groups = _flatten(_OPT_PREFIX + "groups.", sd.get("param_groups", []), tensors, "groups")
    return tensors, {"state": state, "param_groups": groups}


def _sched_tables(scheduler):
    if scheduler is None:
        return {}, None
    sd = scheduler.state_dict()
    tensors = {}
    return tensors, _flatten(_SCHED_PREFIX, sd, tensors, "sched")


def _read_extra_files(extra_files):
    """{stream_name: path} → manifest-ready {name: {encoding, content}}."""
    import base64

    if not extra_files:
        return {}
    total, out = 0, {}
    for name, path in extra_files.items():
        sname = str(name)
        with open(path, "rb") as f:
            data = f.read()
        total += len(data)
        if total > _EXTRA_FILES_MAX:
            raise ValueError(f"extra_files exceed {_EXTRA_FILES_MAX // (1 << 20)} MiB; "
                             "store large files separately")
        try:
            out[sname] = {"encoding": "utf8", "content": data.decode("utf-8")}
        except UnicodeDecodeError:
            out[sname] = {"encoding": "base64",
                          "content": base64.b64encode(data).decode("ascii")}
    return out


def _write_extra_files(files, directory):
    import base64

    paths = {}
    os.makedirs(directory, exist_ok=True)
    for name, spec in (files or {}).items():
        data = spec["content"].encode("utf-8") if spec["encoding"] == "utf8" \
            else base64.b64decode(spec["content"])
        p = os.path.join(directory, os.path.basename(str(name)))
        with open(p, "wb") as f:
            f.write(data)
        paths[str(name)] = p
    return paths


def _estimate_bytes(tensors):
    total = 0
    for v in tensors.values():
        arr = v[0] if isinstance(v, (tuple, list)) and len(v) == 2 and isinstance(v[1], str) else v
        if _is_tensor(arr):
            total += arr.nelement() * arr.element_size()
        else:
            import numpy as np
            a = np.asarray(arr)
            total += a.nbytes
    return total


def save_checkpoint(path, model, optimizer=None, scheduler=None, epoch=None,
                    metrics=None, config=None, extra_files=None, password=None,
                    metadata=None, shard_above=1 << 30, fast=False, store=False,
                    durable=True):
    """Export a training checkpoint directly to .qtsq.

    Stores model weights (+ optimizer/scheduler state, metrics, config,
    related files) bit-exact in one file — or sharded files when large.

    Args:
        path: output `.qtsq` path (sharded: used as prefix).
        model: nn.Module or state_dict.
        optimizer: torch optimizer or None.
        scheduler: torch scheduler or None.
        epoch: int epoch/step number.
        metrics: dict of JSON scalars (loss, accuracy…).
        config: dict (arch, hyperparams…).
        extra_files: {name: path} small related files (tokenizer, configs;
            ≤8 MiB total; text or binary).
        password: optional encryption (else keystore-or-plaintext, as save_model).
        metadata: optional extra user dict (must not clash with reserved keys).
        shard_above: shard when raw bytes exceed this (default 1 GiB).

    Returns:
        str path, or list[str] shards when sharded.
    """
    from . import save_model, save_model_sharded

    if hasattr(model, "state_dict") and callable(model.state_dict):
        state = model.state_dict()
    elif isinstance(model, dict):
        state = model
    else:
        raise TypeError("model must be nn.Module or state_dict")

    tensors = {}
    for k, v in state.items():
        if str(k).startswith(_OPT_PREFIX) or str(k).startswith(_SCHED_PREFIX):
            raise ValueError(f"tensor name {k!r} uses a reserved prefix")
        tensors[str(k)] = v

    meta = dict(metadata or {})
    for reserved in _RESERVED_META:
        if reserved in meta:
            raise ValueError(f"metadata key {reserved!r} is reserved")
    meta["qtsq_checkpoint"] = 1
    if epoch is not None:
        meta["epoch"] = int(epoch)
    if metrics is not None:
        meta["metrics"] = _ensure_jsonable(dict(metrics), "metrics")
    if config is not None:
        meta["config"] = _ensure_jsonable(dict(config), "config")

    if optimizer is not None:
        _need_torch()
        ot, op = _optim_tables(optimizer)
        for k, v in ot.items():
            if k in tensors:
                raise ValueError(f"optimizer tensor collides: {k!r}")
        tensors.update(ot)
        meta["optim"] = op
    if scheduler is not None:
        _need_torch()
        st, sp = _sched_tables(scheduler)
        for k, v in st.items():
            if k in tensors:
                raise ValueError(f"scheduler tensor collides: {k!r}")
        tensors.update(st)
        meta["sched"] = sp
    files = _read_extra_files(extra_files)
    if files:
        meta["files"] = files

    if _estimate_bytes(tensors) > shard_above:
        prefix = os.path.abspath(path)
        if prefix.endswith(".qtsq"):
            prefix = prefix[:-5]
        from . import save_model_sharded as _sharded
        return _sharded(tensors, prefix, password=password,
                        max_shard_bytes=shard_above, metadata=meta, fast=fast,
                        store=store, durable=durable)
    save_model(tensors, path, password=password, metadata=meta, fast=fast,
               store=store, durable=durable)
    return os.path.abspath(path)


def load_checkpoint(path, model=None, optimizer=None, scheduler=None,
                    password=None, strict=True, extra_dir=None, framework=None,
                    include=None, writable=True):
    """Restore a checkpoint written by save_checkpoint.

    Args:
        path: `.qtsq` file, any shard, or a sharded prefix.
        model: nn.Module or None (state_dict returned in info if None).
        optimizer/scheduler: objects to restore into, or None.
        password: if the file is encrypted.
        strict: strict load_state_dict for the model.
        extra_dir: if given, related files are written there.
        framework: "torch" default when torch importable, else "numpy".
        include: optional subset of tensor names to load (weights-only
            inference; optimizer restore requires its streams included).

    Returns:
        dict {epoch, metrics, config, files, metadata, state_dict, path}.
        `files` maps names to bytes (written to extra_dir when given).
    """
    import base64

    from . import load_model, load_model_sharded, model_info, load_tensors

    if framework is None:
        try:
            import torch  # noqa: F401
            framework = "torch"
        except Exception:
            framework = "numpy"

    given = os.path.abspath(path)
    if not given.endswith(".qtsq") or not os.path.exists(given):
        if include is not None:
            raise ValueError("include= needs a file or shard path, not a bare prefix")
        tensors, meta = load_model_sharded(given, password=password, framework=framework,
                                         writable=writable)
        at = given
    else:
        entries, _m, stats = model_info(given, password=password)
        sharded = (stats.get("shards") or [None])[0] and len(stats["shards"]) > 1
        if include is not None:
            tensors, meta = load_tensors(given, include, password=password,
                                         framework=framework, writable=writable)
        elif sharded:
            tensors, meta = load_model_sharded(given, password=password, framework=framework,
                                         writable=writable)
        else:
            tensors, meta = load_model(given, password=password, framework=framework,
                                  writable=writable)
        at = given

    if not isinstance(meta, dict) or meta.get("qtsq_checkpoint") != 1:
        raise ValueError("not a qtsq checkpoint (missing checkpoint manifest)")

    if include is not None:
        if optimizer is not None and not any(
                k.startswith(_OPT_PREFIX) for k in tensors):
            raise ValueError("include= excludes optimizer streams but optimizer was passed")
        if scheduler is not None and not any(
                k.startswith(_SCHED_PREFIX) for k in tensors):
            raise ValueError("include= excludes scheduler streams but scheduler was passed")

    model_sd = {k: v for k, v in tensors.items()
                if not k.startswith(_OPT_PREFIX) and not k.startswith(_SCHED_PREFIX)}
    info = {"epoch": meta.get("epoch"), "metrics": meta.get("metrics", {}),
            "config": meta.get("config", {}), "files": {}, "metadata": {},
            "state_dict": model_sd, "path": at}
    for k, v in meta.items():
        if k not in _RESERVED_META:
            info["metadata"][k] = v

    if model is not None:
        _need_torch()
        import torch
        table = {k: (v if isinstance(v, torch.Tensor) else torch.as_tensor(v))
                 for k, v in model_sd.items()}
        if hasattr(model, "load_state_dict"):
            model.load_state_dict(table, strict=strict)
        elif isinstance(model, dict):
            model.update(table)
        else:
            raise TypeError("model must be nn.Module, dict, or None")
    if optimizer is not None:
        if "optim" not in meta:
            raise ValueError("checkpoint has no optimizer state")
        table = {k: v for k, v in tensors.items() if k.startswith(_OPT_PREFIX)}
        # strip prefix root: "__optim_state." / "__optim_groups."
        st = _materialize(meta["optim"]["state"], table)
        gr = _materialize(meta["optim"]["param_groups"], table)
        optimizer.load_state_dict({"state": _int_keys(st), "param_groups": gr})
    if scheduler is not None:
        if "sched" not in meta or meta["sched"] is None:
            raise ValueError("checkpoint has no scheduler state")
        table = {k: v for k, v in tensors.items() if k.startswith(_SCHED_PREFIX)}
        scheduler.load_state_dict(_materialize(meta["sched"], table))

    raws = {}
    for name, spec in (meta.get("files") or {}).items():
        data = spec["content"].encode("utf-8") if spec["encoding"] == "utf8" \
            else base64.b64decode(spec["content"])
        raws[str(name)] = data
    if extra_dir and raws:
        _write_extra_files({k: {"encoding": "utf8", "content": v.decode("utf-8", "replace")}
                              for k, v in raws.items()}, extra_dir)
    info["files"] = raws
    return info


def _int_keys(state):
    """state keys are param ids saved as strings — restore ints."""
    out = {}
    for k, v in state.items():
        try:
            out[int(k)] = v
        except (TypeError, ValueError):
            out[k] = v
    return out


class CheckpointSaver:
    """Per-epoch checkpoint helper for hand-rolled training loops.

        saver = qtsq.CheckpointSaver("ckpts/run1", every=1, keep_last=3)
        for epoch in range(epochs):
            ...train...
            saver.on_epoch_end(epoch, model, optimizer=opt, metrics={"loss": l},
                               config=cfg, extra_files={"tokenizer.json": p})

    Shards automatically past `shard_above` bytes; prunes to `keep_last`
    checkpoints (each checkpoint = one file or one shard set).
    """

    def __init__(self, directory, every=1, keep_last=None, password=None,
                 shard_above=1 << 30, name="ckpt-e{epoch}.qtsq", fast=False,
                 store=False, durable=True):
        self.directory = os.path.abspath(directory)
        os.makedirs(self.directory, exist_ok=True)
        self.every = max(1, int(every))
        self.keep_last = keep_last
        self.password = password
        self.shard_above = shard_above
        self.fast = fast
        self.store = store
        self.durable = durable
        self.name = name
        self._kept = []

    def on_epoch_end(self, epoch, model, optimizer=None, scheduler=None,
                     metrics=None, config=None, extra_files=None, metadata=None):
        if int(epoch) % self.every != 0:
            return None
        path = os.path.join(self.directory, self.name.format(epoch=int(epoch)))
        out = save_checkpoint(path, model, optimizer=optimizer, scheduler=scheduler,
                              epoch=int(epoch), metrics=metrics, config=config,
                              extra_files=extra_files, password=self.password,
                              metadata=metadata, shard_above=self.shard_above,
                              fast=self.fast, store=self.store,
                              durable=self.durable)
        self._kept.append(out if isinstance(out, list) else [out])
        if self.keep_last:
            while len(self._kept) > self.keep_last:
                for p in self._kept.pop(0):
                    try:
                        os.remove(p)
                    except OSError:
                        pass
        return out


# ── Framework adapters (only defined when the framework is installed) ──

try:  # HuggingFace Trainer
    from transformers import TrainerCallback as _HFTrainerCallback

    class HFQtsqCallback(_HFTrainerCallback):
        """Save a .qtsq checkpoint on every Trainer save.

        trainer.add_callback(HFQtsqCallback("ckpts", every=1))
        """

        def __init__(self, directory, every=1, password=None, metadata=None):
            self.saver = CheckpointSaver(directory, every=1, keep_last=None,
                                         password=password)
            self.every = max(1, int(every))
            self._saves = 0
            self.metadata = metadata

        def on_save(self, args, state, control, **kwargs):
            self._saves += 1
            if self._saves % self.every != 0:
                return
            self.saver.on_epoch_end(
                state.global_step, kwargs.get("model"),
                optimizer=kwargs.get("optimizer"),
                scheduler=kwargs.get("lr_scheduler"),
                metrics=dict(state.log_history[-1]) if state.log_history else None,
                metadata=self.metadata)
except Exception:  # transformers not installed
    HFQtsqCallback = None

try:  # PyTorch Lightning
    try:
        from lightning.pytorch.callbacks import Callback as _PLCallback
    except Exception:
        from pytorch_lightning.callbacks import Callback as _PLCallback

    class LightningQtsqCallback(_PLCallback):
        """Save a .qtsq checkpoint at each epoch end.

        trainer = Trainer(callbacks=[LightningQtsqCallback("ckpts")])
        """

        def __init__(self, directory, every=1, password=None, metadata=None):
            super().__init__()
            self.saver = CheckpointSaver(directory, every=1, keep_last=None,
                                         password=password)
            self.every = max(1, int(every))
            self.metadata = metadata

        def on_train_epoch_end(self, trainer, pl_module, *a, **k):
            epoch = trainer.current_epoch
            if epoch % self.every != 0:
                return
            opt = trainer.optimizers[0] if trainer.optimizers else None
            sched = None
            sched_cfgs = getattr(trainer, "lr_scheduler_configs", None) or []
            if sched_cfgs:
                sched = sched_cfgs[0].scheduler
            self.saver.on_epoch_end(epoch, pl_module, optimizer=opt,
                                    scheduler=sched, metadata=self.metadata)
except Exception:  # lightning not installed
    LightningQtsqCallback = None
