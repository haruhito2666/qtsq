"""
setup.py — Compile qtsq C code into a Python extension module.

After: pip install .
Anyone: import qtsq
"""

import os
import platform
import subprocess
from setuptools import setup, Extension

SRC_DIR = 'src'
INCLUDE_DIR = 'include'


def pkg_config(name, flag):
    """Return pkg-config output for a package, or [] if unavailable."""
    try:
        out = subprocess.check_output(['pkg-config', flag, name],
                                      stderr=subprocess.DEVNULL)
        return out.decode().split()
    except (OSError, subprocess.CalledProcessError):
        return []


# Collect C source files (skip .mm — replaced by stubs)
c_sources = [os.path.join(SRC_DIR, f)
             for f in sorted(os.listdir(SRC_DIR))
             if f.endswith('.c')]

# PRODUCT.md §7: video is parked — src/qtsq_video_encode.c does not compile
# (pre-existing break) and is excluded until video is unparked. The decoder
# still builds (old files stay readable); src/qtsq_video_stubs.c satisfies
# the encoder symbols, returning QTSQ_ERR_STRATEGY if called.
c_sources = [s for s in c_sources
             if os.path.basename(s) != 'qtsq_video_encode.c']

# Add post-quantum crypto implementations
for root, dirs, files in os.walk('crypto'):
    for f in files:
        if f.endswith('.c'):
            c_sources.append(os.path.join(root, f))

# Phase 6a: qtc compression library (pure C, compiled in-tree like the rest)
for f in sorted(os.listdir(os.path.join('compressor', 'src'))):
    if f.endswith('.c'):
        c_sources.append(os.path.join('compressor', 'src', f))

# Add Python C extension wrapper + Metal stubs
c_sources.append('qtsq/_native.c')
c_sources.append('qtsq/_metal_stubs.c')

# Compiler flags
compile_args = ['-O2', '-std=c11',
                '-Wno-unused-parameter', '-Wno-sign-compare',
                '-Wno-missing-braces', '-Wno-unused-function',
                '-Wno-misleading-indentation']
link_args = []
libs = ['z']

if platform.system() == 'Darwin':
    link_args += ['-framework', 'Metal', '-framework', 'Foundation']
elif platform.system() == 'Linux':
    compile_args += ['-D_GNU_SOURCE']  # enables CLOCK_MONOTONIC, strnlen, etc.
    libs += ['m', 'pthread']

# Opus audio (Phase 9) — conditional, mirrors the Makefile. qtsq_audio_opus.c
# includes <opus/opus.h> unguarded; it is only compilable when libopus is
# present, so skip it (and fall back to the in-house spectral encoder) otherwise.
opus_cflags = pkg_config('opus', '--cflags')
opus_libs = pkg_config('opus', '--libs')
if opus_libs:
    compile_args += ['-DQTSQ_HAVE_OPUS'] + opus_cflags
    # pkg-config returns .../include/opus (the subdir); add the parent too so
    # the portable '#include <opus/opus.h>' form resolves.
    for fl in opus_cflags:
        if fl.startswith('-I'):
            parent = os.path.dirname(fl[2:].rstrip('/'))
            if parent:
                compile_args.append('-I' + parent)
    link_args += opus_libs
else:
    c_sources = [s for s in c_sources if s != os.path.join(SRC_DIR, 'qtsq_audio_opus.c')]

ext = Extension(
    'qtsq._native',
    sources=c_sources,
    include_dirs=[INCLUDE_DIR, 'compressor/include', 'crypto', 'crypto/ml-dsa-65', 'crypto/ml-kem-768', 'crypto/common', 'crypto/argon2'],
    libraries=libs,
    extra_compile_args=compile_args,
    extra_link_args=link_args,
)

# All metadata (name, version, deps, classifiers) lives in pyproject.toml.
# setup.py only defines the C extension which pyproject.toml can't do.
setup(ext_modules=[ext])
