cmake_minimum_required(VERSION 3.10)
project(qtsq_wasm)

set(CMAKE_C_STANDARD 11)

# Include directories
include_directories(../include ../compressor/include ../crypto/common ../crypto/ml-kem-768 ../crypto/ml-dsa-65)

# Emscripten flags for WASM output
# -sUSE_ZLIB=1: Link built-in zlib
# -sEXPORTED_RUNTIME_METHODS=ccall,cwrap: Expose runtime methods to JS wrapper
# -sALLOW_MEMORY_GROWTH=1: Allow memory growth for large files
# -sMALLOC=emmalloc: Smaller malloc implementation
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -flto -s USE_ZLIB=1")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s USE_ZLIB=1 -s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','getValue','HEAPU8'] -s ALLOW_MEMORY_GROWTH=1 -s MALLOC=emmalloc -s EXPORTED_FUNCTIONS=['_malloc','_free'] -s EXPORT_ES6=1")

# Source files (decoder subset)
set(SOURCES
    qtsq_wasm_api.c
    ../src/qtsq_format.c
    ../src/qtsq_types.c
    ../src/qtsq_utils.c
    ../src/qtsq_codec.c
    ../src/qtsq_pixel.c
    ../src/qtsq_image_file.c
    ../src/qtsq_radiation.c
    ../src/qtsq_gravitational.c
    ../src/qtsq_triangle.c
    ../src/qtsq_triangle_only.c
    ../src/qtsq_splat2d.c
    ../src/qtsq_splat_only.c
    ../src/qtsq_horizon.c
    ../src/qtsq_lensing.c
    ../src/qtsq_accretion.c
    ../src/qtsq_singularity.c
    ../src/qtsq_censorship.c
    ../src/qtsq_secure.c
    ../src/qtsq_sanitize.c
    ../src/qtsq_chandrasekhar.c
    ../src/qtsq_tensor.c
    ../src/qtsq_geocrypt.c
)

file(GLOB CRYPTO_SOURCES "../crypto/common/*.c" "../crypto/ml-kem-768/*.c" "../crypto/ml-dsa-65/*.c")
list(APPEND SOURCES ${CRYPTO_SOURCES})

# Phase 6a: qtc compression library (needed to decode qtc payloads).
file(GLOB QTC_SOURCES "../compressor/src/*.c")
list(APPEND SOURCES ${QTC_SOURCES})

# Build the WASM module
add_executable(qtsq-decoder ${SOURCES})
set_target_properties(qtsq-decoder PROPERTIES SUFFIX ".js")
set_target_properties(qtsq-decoder PROPERTIES LINK_FLAGS "-s MODULARIZE=1 -s EXPORT_NAME=QTSQModule")
