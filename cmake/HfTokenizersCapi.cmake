# HfTokenizersCapi.cmake
#
# Builds the Rust staticlib that exposes the HuggingFace tokenizers C ABI
# and exports it as an imported library target `hf_tokenizers_capi`.

include_guard(GLOBAL)

get_filename_component(HF_TOKENIZERS_DEFAULT_CRATE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../third_party/hf_tokenizers_capi"
    ABSOLUTE)
set(HF_TOKENIZERS_CRATE_DIR
    "${HF_TOKENIZERS_DEFAULT_CRATE_DIR}"
    CACHE PATH "Path to the hf_tokenizers_capi Rust crate")

set(HF_TOKENIZERS_INCLUDE_DIR
    "${HF_TOKENIZERS_CRATE_DIR}/include"
    CACHE PATH "Path to the hf_tokenizers_capi C header")

if(NOT EXISTS "${HF_TOKENIZERS_CRATE_DIR}/Cargo.toml" OR
        NOT EXISTS "${HF_TOKENIZERS_INCLUDE_DIR}/hf_tokenizers_capi.h")
    message(FATAL_ERROR
        "hf_tokenizers_capi source is incomplete at ${HF_TOKENIZERS_CRATE_DIR}. "
        "A clean checkout must contain Cargo.toml and include/hf_tokenizers_capi.h.")
endif()

set(HF_TOKENIZERS_CARGO_PROFILE "release"
    CACHE STRING "Cargo profile used to build hf_tokenizers_capi (debug|release)")

find_program(CARGO_EXECUTABLE cargo REQUIRED)

if(HF_TOKENIZERS_CARGO_PROFILE STREQUAL "release")
    set(HF_TOKENIZERS_TARGET_SUBDIR "release")
    set(HF_TOKENIZERS_CARGO_FLAG "--release")
else()
    set(HF_TOKENIZERS_TARGET_SUBDIR "debug")
    set(HF_TOKENIZERS_CARGO_FLAG "")
endif()

set(HF_TOKENIZERS_CARGO_TARGET_ROOT
    "${HF_TOKENIZERS_CRATE_DIR}/target"
    CACHE PATH "Cargo target root for hf_tokenizers_capi")
set(HF_TOKENIZERS_TARGET_DIR
    "${HF_TOKENIZERS_CARGO_TARGET_ROOT}/${HF_TOKENIZERS_TARGET_SUBDIR}")

if(WIN32)
    set(HF_TOKENIZERS_STATICLIB_NAME "hf_tokenizers_capi.lib")
else()
    set(HF_TOKENIZERS_STATICLIB_NAME "libhf_tokenizers_capi.a")
endif()

set(HF_TOKENIZERS_STATICLIB_PATH
    "${HF_TOKENIZERS_TARGET_DIR}/${HF_TOKENIZERS_STATICLIB_NAME}")

if(HF_TOKENIZERS_CARGO_FLAG)
    set(_hf_cargo_cmd
        "${CARGO_EXECUTABLE}" build ${HF_TOKENIZERS_CARGO_FLAG})
else()
    set(_hf_cargo_cmd "${CARGO_EXECUTABLE}" build)
endif()

file(GLOB_RECURSE HF_TOKENIZERS_RUST_SOURCES
    CONFIGURE_DEPENDS
    "${HF_TOKENIZERS_CRATE_DIR}/src/*.rs"
    "${HF_TOKENIZERS_CRATE_DIR}/include/*.h"
)
list(APPEND HF_TOKENIZERS_RUST_SOURCES
    "${HF_TOKENIZERS_CRATE_DIR}/Cargo.toml"
    "${HF_TOKENIZERS_CRATE_DIR}/Cargo.lock")

add_custom_command(
    OUTPUT "${HF_TOKENIZERS_STATICLIB_PATH}"
    COMMAND ${CMAKE_COMMAND} -E env
        "CARGO_TARGET_DIR=${HF_TOKENIZERS_CARGO_TARGET_ROOT}"
        ${_hf_cargo_cmd} --locked
    WORKING_DIRECTORY "${HF_TOKENIZERS_CRATE_DIR}"
    DEPENDS ${HF_TOKENIZERS_RUST_SOURCES}
    COMMENT "Building hf_tokenizers_capi (${HF_TOKENIZERS_CARGO_PROFILE}) via cargo"
    VERBATIM
)

add_custom_target(hf_tokenizers_capi_build
    DEPENDS "${HF_TOKENIZERS_STATICLIB_PATH}"
)

add_library(hf_tokenizers_capi STATIC IMPORTED GLOBAL)
add_dependencies(hf_tokenizers_capi hf_tokenizers_capi_build)

set_target_properties(hf_tokenizers_capi PROPERTIES
    IMPORTED_LOCATION "${HF_TOKENIZERS_STATICLIB_PATH}"
    INTERFACE_INCLUDE_DIRECTORIES "${HF_TOKENIZERS_INCLUDE_DIR}"
)

# Rust staticlib transitively requires several platform libraries.
if(WIN32)
    set_property(TARGET hf_tokenizers_capi APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES
            ws2_32 userenv advapi32 bcrypt ntdll kernel32 ole32 oleaut32
            uuid synchronization)
elseif(APPLE)
    set_property(TARGET hf_tokenizers_capi APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "-framework Security" "-framework CoreFoundation")
else()
    set_property(TARGET hf_tokenizers_capi APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES pthread dl m)
endif()
