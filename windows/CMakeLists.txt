# CMakeLists.txt — QTSQ Windows (MSVC x64) build of libqtsq.
#
# Builds the FULL-featured QTSQ static library (qtsq.lib) on Windows:
# every core module, the qtc compressor, the crypto suite, and the turbo/
# ionic engines — feature parity with the Darwin/Linux Makefile builds.
# Excluded sources are Apple-only (Metal, CoreAudio) or optional-dependency
# (Opus: audio falls back to the in-house spectral encoder when absent,
# exactly like a POSIX build without libopus installed).
#
# Layout: windows/shim/ holds the POSIX-compatibility layer (force-included
# master header + pthread/dirent/unistd shims + Win32-backed mmap backend).
# See windows/README.md for the desktop-app integration story.
#
# Usage (x64 Native Tools prompt, or cmake with a VS generator):
#   cmake -S windows -B windows/build-win64
#   cmake --build windows/build-win64 --config Release
# Output: windows/build-win64/Release/qtsq.lib
#
cmake_minimum_required(VERSION 3.20)
project(qtsq LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT MSVC)
    message(FATAL_ERROR
        "This build targets the MSVC toolchain (cl.exe or clang-cl). "
        "Use the top-level Makefile on Darwin/Linux.")
endif()

find_package(ZLIB REQUIRED) # qtsq core uses zlib; qtc compressor does not

set(SHIM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shim)

# ── Core sources (mirror top-level Makefile SRC_CORE, minus Darwin-only
#    .mm/.m and the Opus-optional audio path) ────────────────────────────
set(QTSQ_CORE_SOURCES
    ../src/qtsq_utils.c
    ../src/qtsq_format.c
    ../src/qtsq_types.c
    ../src/qtsq_codec.c
    ../src/qtsq_pixel.c
    ../src/qtsq_image_file.c
    ../src/qtsq_chandrasekhar.c
    ../src/qtsq_singularity.c
    ../src/qtsq_horizon.c
    ../src/qtsq_lensing.c
    ../src/qtsq_accretion.c
    ../src/qtsq_triangle.c
    ../src/qtsq_radiation.c
    ../src/qtsq_redshift.c
    ../src/qtsq_wormhole.c
    ../src/qtsq_vault.c
    ../src/qtsq_sanitize.c
    ../src/qtsq_secure.c
    ../src/qtsq_waves.c
    ../src/qtsq_paradox.c
    ../src/qtsq_gravitational.c
    ../src/qtsq_splat2d.c
    ../src/qtsq_splat_only.c
    ../src/qtsq_triangle_only.c
    ../src/qtsq_video_encode.c
    ../src/qtsq_video_decode.c
    ../src/qtsq_audio_encode.c
    ../src/qtsq_audio_decode.c
    ../src/qtsq_tensor.c
    ../src/qtsq_container.c
    ../src/qtsq_photon_sonar.c
    ../src/qtsq_photon_tdoa.c
    ../src/qtsq_photon_wifi.c
    ../src/qtsq_photon_fusion.c
    ../src/qtsq_splat4d.c
    ../src/qtsq_splat_tsubu.c
    ../src/qtsq_4dgs.c
    ../src/qtsq_geocrypt.c
    ../src/qtsq_shamir.c
    ../src/qtsq_deniable.c
    ../src/qtsq_region_crypt.c
    ../src/qtsq_fingerprint.c
    ../src/qtsq_censorship.c
    ../src/qtsq_keystore.c
    ../src/qtsq_seal.c
    ../src/qtsq_surfel.c
    ../src/qtsq_tsdf.c
    ../src/qtsq_seal_freqdct.c
    ../src/qtsq_turbo.c
    ../src/qtsq_ionic.c
    ../src/qtsq_mf_merge.c
    ../src/qtsq_c2pa.c
)

# ── Crypto suite (same wildcards as the Makefile) ────────────────────────
file(GLOB QTSQ_CRYPTO_SOURCES
    ../crypto/common/*.c
    ../crypto/ml-kem-768/*.c
    ../crypto/ml-dsa-65/*.c
    ../crypto/argon2/*.c
)

# ── qtc compressor (mirror compressor/Makefile SRC) ──────────────────────
set(QTC_SOURCES
    ../compressor/src/qtc_model.c
    ../compressor/src/qtc_ans.c
    ../compressor/src/qtc_order1.c
    ../compressor/src/qtc_lz.c
    ../compressor/src/qtc_image.c
    ../compressor/src/qtc_audio.c
    ../compressor/src/qtc_detect.c
    ../compressor/src/qtc_mdct.c
    ../compressor/src/qtc_audio_mdct.c
    ../compressor/src/qtc_audio_mdct2.c
    ../compressor/src/qtc_audio_float.c
    ../compressor/src/qtc_delaunay.c
    ../compressor/src/qtc_image_mesh.c
    ../compressor/src/qtc_accretion.c
    ../compressor/src/qtc_image_dct.c
    ../compressor/src/qtc_tensor.c
    ../compressor/src/qtc_text.c
    ../compressor/src/qtc_table.c
    ../compressor/src/qtc_video.c
    ../compressor/src/qtc_core.c
)

# ── Windows shim + Win32 mmap backend ────────────────────────────────────
set(QTSQ_WIN_SHIM_SOURCES
    shim/qtsq_win_compat.c
    shim/qtsq_mmap_win.c
)

add_library(qtsq STATIC
    ${QTSQ_CORE_SOURCES}
    ${QTSQ_CRYPTO_SOURCES}
    ${QTC_SOURCES}
    ${QTSQ_WIN_SHIM_SOURCES}
)

target_include_directories(qtsq PRIVATE
    # shim FIRST — its pthread.h/unistd.h/dirent.h shadow the POSIX ones.
    ${SHIM_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../compressor/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../crypto/common
    ${CMAKE_CURRENT_SOURCE_DIR}/../crypto/ml-kem-768
    ${CMAKE_CURRENT_SOURCE_DIR}/../crypto/ml-dsa-65
    ${CMAKE_CURRENT_SOURCE_DIR}/../crypto/argon2
)

target_compile_definitions(qtsq PRIVATE
    _USE_MATH_DEFINES
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    NOGDI
    QTSQ_MMAP_WIN        # selects the map_handle field in qtsq_mmap_t
)

# Force-include the compat master header in every TU, in any include order.
target_compile_options(qtsq PRIVATE /FIqtsq_win_compat.h)

# bcrypt: BCryptGenRandom (qtsq_censorship.c); advapi32: CryptAcquireContext
# (crypto/common/randombytes.c); zlib: container payload compression.
target_link_libraries(qtsq PUBLIC ZLIB::ZLIB bcrypt.lib advapi32.lib)

message(STATUS "QTSQ Windows build: ${QTSQ_CORE_SOURCES} core + "
               "${QTSQ_CRYPTO_SOURCES} crypto + ${QTC_SOURCES} qtc + shim")
