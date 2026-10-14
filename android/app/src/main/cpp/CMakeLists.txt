cmake_minimum_required(VERSION 3.22.1)

project("qtsq")

include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../compressor/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../crypto/common
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../crypto/ml-kem-768
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../crypto/ml-dsa-65
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../crypto/argon2
)

file(GLOB CRYPTO_SRC
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../crypto/common/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../crypto/ml-kem-768/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../crypto/ml-dsa-65/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../crypto/argon2/*.c"
)

# Phase 6a: qtc compression library (pure C11, libc-only).
file(GLOB QTC_SRC
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../compressor/src/*.c"
)

# Vulkan compute used for GPU-accelerated decompression only.
# Compression uses 6-core CPU with NEON SIMD.
add_library(
        qtsq
        SHARED
        qtsq_jni.c
        qtsq_keystore_jni.c
        vulkan_compute.c
        qtsq_capture.c
        qtsq_trainer.c
        qtsq_shader_model.c
        ../../../../../src/qtsq_surfel.c
        ../../../../../src/qtsq_tsdf.c
        ../../../../../src/qtsq_seal.c
        ../../../../../src/qtsq_seal_freqdct.c
        ../../../../../src/qtsq_utils.c
        ../../../../../src/qtsq_format.c
        ../../../../../src/qtsq_types.c
        ../../../../../src/qtsq_codec.c
        ../../../../../src/qtsq_pixel.c
        ../../../../../src/qtsq_image_file.c
        ../../../../../src/qtsq_chandrasekhar.c
        ../../../../../src/qtsq_singularity.c
        ../../../../../src/qtsq_horizon.c
        ../../../../../src/qtsq_lensing.c
        ../../../../../src/qtsq_accretion.c
        ../../../../../src/qtsq_triangle.c
        ../../../../../src/qtsq_radiation.c
        ../../../../../src/qtsq_redshift.c
        ../../../../../src/qtsq_wormhole.c
        ../../../../../src/qtsq_vault.c
        ../../../../../src/qtsq_sanitize.c
        ../../../../../src/qtsq_secure.c
        ../../../../../src/qtsq_censorship.c
        ../../../../../src/qtsq_geocrypt.c
        ../../../../../src/qtsq_shamir.c
        ../../../../../src/qtsq_deniable.c
        ../../../../../src/qtsq_region_crypt.c
        ../../../../../src/qtsq_fingerprint.c
        ../../../../../src/qtsq_waves.c
        ../../../../../src/qtsq_paradox.c
        ../../../../../src/qtsq_gravitational.c
        ../../../../../src/qtsq_splat2d.c
        ../../../../../src/qtsq_splat4d.c
        ../../../../../src/qtsq_splat_only.c
        ../../../../../src/qtsq_triangle_only.c
        ../../../../../src/qtsq_video_encode.c
        ../../../../../src/qtsq_video_decode.c
        ../../../../../src/qtsq_audio_encode.c
        ../../../../../src/qtsq_audio_decode.c
        ../../../../../src/qtsq_tensor.c
        ../../../../../src/qtsq_container.c
        ../../../../../src/qtsq_keystore.c
        ${CRYPTO_SRC}
        ${QTC_SRC}
)

find_library(log-lib log)
find_library(vulkan-lib vulkan)

target_link_libraries(
        qtsq
        ${log-lib}
        ${vulkan-lib}
        m
        z
)

# GPU decompression via Vulkan compute
target_compile_definitions(qtsq PRIVATE
        HAVE_VULKAN_COMPUTE=1
)

# Phase 8: the debug build compiled the entire native codec at -O0 (no -O
# flag anywhere), which measured ~3-5x slower than necessary on the hot
# paths (see compressor/results/speed_phase8_baseline.md). Optimize the
# native library for every build type: -O2 for Debug (keeps -g and a small
# binary), -O3 for release. Output bytes are unchanged by optimization level.
target_compile_options(qtsq PRIVATE
        $<$<CONFIG:Debug>:-O2>
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:RelWithDebInfo>:-O3>
)
