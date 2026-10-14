# QTSQ Windows Port

Full-featured `qtsq.lib` build for Windows x64 (MSVC / clang-cl), with
feature parity to the Darwin and Linux Makefile builds: all core modules,
the qtc compressor, the crypto suite (ML-KEM-768, ML-DSA-65, Argon2id), and
the turbo/ionic I/O engines.

## Layout

- `shim/` — POSIX compatibility layer:
  - `qtsq_win_compat.h` — force-included master header (`/FI` on MSVC).
    Maps the unistd surface (`fsync`→`_commit`, `mkdir`, `access`, `chmod`,
    `unlink`, `fileno`), `strcasecmp`/`strncasecmp`, `rename` (atomic
    replace via `MoveFileExA`), `sysconf`, `clock_gettime`/`gettimeofday`,
    `gmtime_r`, `sincosf`, `M_PI`. Contains **no** `windows.h` include so
    it is safe in any include order.
  - `pthread.h` — header-only Win32-backed shim (`HANDLE` /
    `CRITICAL_SECTION` / `CONDITION_VARIABLE`). `include/qtsq_ionic.h`
    embeds these types **by value** in the public `qtsq_ionic_t` struct, so
    real Win32 types (not stubs) are required.
  - `dirent.h` — `opendir`/`readdir`/`closedir` via `FindFirstFileA`
    (implemented in `qtsq_win_compat.c`).
  - `unistd.h`, `strings.h`, `sys/time.h` — route to the master header.
  - `qtsq_win_compat.c` — implementations for everything above.
  - `qtsq_mmap_win.c` — turbo Layer 1 memory-mapped I/O on
    `CreateFileMappingA`/`MapViewOfFile`, compiled with `QTSQ_MMAP_WIN`
    (the `map_handle` field in `qtsq_mmap_t`).
- `CMakeLists.txt` — static-library build entry (see below).

Not ported (same as POSIX builds without system deps): Opus audio (falls
back to the in-house spectral encoder), Metal (Apple-only), CoreAudio
(Apple-only).

## Build

From an **x64 Native Tools Command Prompt for VS 2022** (or any MSVC
environment with cmake + zlib available, e.g. `vcpkg install zlib`):

```
cmake -S windows -B windows\build-win64 -DCMAKE_TOOLCHAIN_FILE=[vcpkg]/scripts/buildsystems/vcpkg.cmake
cmake --build windows\build-win64 --config Release
```

Output: `windows/build-win64/Release/qtsq.lib`

## Linking a consumer

Link `qtsq.lib` plus: `zlib` (`ZLIB::ZLIB`), `bcrypt.lib`
(`BCryptGenRandom`), `advapi32.lib` (`CryptAcquireContext`), and the CRT.
The desktop app's `crates/sofuu-ffi/build.rs` picks the library up from
`windows/build-win64/Release/qtsq.lib` (override with `SOFUU_QTSQ_LIB`) —
see that file for the exact link lines.

## Verification status

- Full source compile + 21-symbol FFI link closure verified via
  `zig cc -target x86_64-windows-gnu` (mingw target compiles the same shim
  code paths; MSVC-only `#pragma pack` guards exercised in parallel).
- Final proof on native MSVC happens on a Windows machine — no MSVC in this
  dev environment.
