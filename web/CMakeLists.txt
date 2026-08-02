# coBroc Web — Emscripten CMakeLists
#
# This compiles core/ + model to WASM for use in the web frontend.
# Requires Emscripten SDK (emcmake + emmake).
#
# Build:
#   source ~/emsdk/emsdk_env.sh
#   emcmake cmake -B build-emscripten -S . && emmake cmake --build build-emscripten
# Outputs: public/coBrocWeb.js + public/coBrocWeb.wasm

cmake_minimum_required(VERSION 3.20)
project(coBrocWeb)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Core sources (platform-independent) + C ABI for JS
set(CORE_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/../core/coBroc_core.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../core/wasm_api.cpp
)

set(CORE_HEADERS
    ${CMAKE_CURRENT_LIST_DIR}/../core/coBroc_core.h
    ${CMAKE_CURRENT_LIST_DIR}/../core/model.h
    ${CMAKE_CURRENT_LIST_DIR}/../core/wasm_api.h
    ${CMAKE_CURRENT_LIST_DIR}/../external/ml/coBroc_regression_model.h
)

add_executable(coBrocWeb
    ${CORE_SOURCES}
    ${CORE_HEADERS}
)

target_include_directories(coBrocWeb PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/..
)

# emcc note: "-s NAME=VALUE" (space) is silently dropped by emcc when passed
# as a single argument, so we use the "-sNAME=VALUE" form (no space) and keep
# each flag as one list element (no spaces inside).
set(EMSCRIPTEN_COMPILE_FLAGS
    "-O3"
)

set(EMSCRIPTEN_LINK_FLAGS
    "-sWASM=1"
    "-sMODULARIZE=1"
    "-sEXPORT_NAME=CoBrocModule"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sINITIAL_MEMORY=16MB"
    "-sEXPORTED_FUNCTIONS=['_cobroc_new_game','_cobroc_free','_cobroc_cycle_type','_cobroc_cycle_param','_cobroc_add_block','_cobroc_set_input_color','_cobroc_select_input_color','_cobroc_ai_turn','_cobroc_run','_cobroc_undo','_cobroc_get_turn','_cobroc_get_move_count','_cobroc_get_selected_param','_cobroc_snapshot','_cobroc_free_snapshot','_cobroc_serialize','_cobroc_deserialize','_malloc','_free']"
    "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','getValue','setValue','UTF8ToString','stringToUTF8','lengthBytesUTF8']"
    "--no-entry"
)

target_compile_options(coBrocWeb PRIVATE ${EMSCRIPTEN_COMPILE_FLAGS})
target_link_options(coBrocWeb PRIVATE ${EMSCRIPTEN_LINK_FLAGS})

# Output to web/public/ for Vite (which copies public/ into dist/)
set_target_properties(coBrocWeb PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/public"
)
