"""qtsq.hub — HuggingFace Hub push/pull for .qtsq checkpoints.

Makes qtsq checkpoints adoptable where the ecosystem lives: push a
checkpoint (weights + config + tokenizer + model card) to a Hub repo,
and pull it back by repo id.

    from qtsq import hub

    hub.push("user/my-model", "ckpt.qtsq", config={"arch": "custom-7b"},
             tokenizer_files={"tokenizer.json": "tok/tokenizer.json"})
    hub.pull("user/my-model", "my-model.qtsq")   # then qtsq.load_model(...)

Requires: pip install huggingface_hub (and a login token for private repos).
"""

import os

QTSQ_CARD_TAG = "qtsq-checkpoint"


def _need_hub():
    try:
        import huggingface_hub  # noqa: F401
    except Exception as e:
        raise ImportError("huggingface_hub required: pip install huggingface_hub") from e


def _card(repo_id, config, metrics):
    lines = ["---", f"tags:\n- {QTSQ_CARD_TAG}", "---",
             f"# {repo_id} (qtsq checkpoint)", ""]
    if config:
        lines += ["## Config", "", "```json"]
        import json as _json
        lines += [_json.dumps(config, indent=2)[:4000], "```", ""]
    if metrics:
        lines += ["## Metrics"] + [f"- **{k}**: {v}" for k, v in list(metrics.items())[:20]]
        lines += [""]
    lines += ["Loaded with:",
              "```python",
              "import qtsq",
              'weights, meta = qtsq.load_model("model.qtsq", framework="torch")',
              "```"]
    return "\n".join(lines)


def push(repo_id, qtsq_path=None, state_dict=None, config=None,
         tokenizer_files=None, metrics=None, password=None, metadata=None,
         private=False, commit_message=None):
    """Push a checkpoint to the Hub.

    Either `qtsq_path` (an existing .qtsq file) or `state_dict`
    (saved on the fly) must be given, plus optional config/tokenizer.
    Layout in repo: model.qtsq (+ config.json, tokenizer files,
    README.md card). Returns the repo URL.
    """
    _need_hub()
    from huggingface_hub import HfApi

    import tempfile

    if (qtsq_path is None) == (state_dict is None):
        raise ValueError("pass exactly one of qtsq_path or state_dict")
    if qtsq_path is None:
        from . import save_model
        fd, tmp = tempfile.mkstemp(suffix=".qtsq")
        os.close(fd)
        save_model(state_dict, tmp, password=password,
                   metadata={"hub_repo": repo_id, **(metadata or {})})
        qtsq_path = tmp
    files = {"model.qtsq": os.path.abspath(qtsq_path)}
    if config is not None:
        import json as _json
        fd, cp = tempfile.mkstemp(suffix=".json")
        with os.fdopen(fd, "w") as f:
            _json.dump(config, f)
        files["config.json"] = cp
    for name, src in (tokenizer_files or {}).items():
        files[os.path.basename(str(name))] = os.path.abspath(src)
    fd, card = tempfile.mkstemp(suffix=".md")
    with os.fdopen(fd, "w") as f:
        f.write(_card(repo_id, config, metrics))
    files["README.md"] = card

    api = HfApi()
    api.create_repo(repo_id, private=private, exist_ok=True)
    for dst, src in files.items():
        api.upload_file(path_or_fileobj=src, path_in_repo=dst,
                        repo_id=repo_id,
                        commit_message=commit_message or f"qtsq checkpoint ({dst})")
    return f"https://huggingface.co/{repo_id}"


def pull(repo_id, output_path="model.qtsq", filename="model.qtsq",
         password=None, revision=None):
    """Download a .qtsq checkpoint from the Hub. Returns the local path."""
    _need_hub()
    from huggingface_hub import hf_hub_download

    local = hf_hub_download(repo_id=repo_id, filename=filename,
                            revision=revision)
    dest = os.path.abspath(output_path)
    if os.path.abspath(local) != dest:
        import shutil
        shutil.copyfile(local, dest)
    # Validate immediately: manifest must parse.
    from . import model_info
    model_info(dest, password=password)
    return dest
