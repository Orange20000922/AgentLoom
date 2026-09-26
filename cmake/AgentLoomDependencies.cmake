# 查找依赖
if(BERT_VCPKG_TRIPLET)
    set(BERT_PROTOC_EXECUTABLE "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/${BERT_VCPKG_TRIPLET}/tools/protobuf/protoc")
    if(WIN32)
        string(APPEND BERT_PROTOC_EXECUTABLE ".exe")
    endif()
    if(EXISTS "${BERT_PROTOC_EXECUTABLE}")
        set(Protobuf_PROTOC_EXECUTABLE "${BERT_PROTOC_EXECUTABLE}")
    endif()
endif()
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(OpenSSL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(hiredis CONFIG REQUIRED)
find_package(redis++ CONFIG REQUIRED)
if(NOT TARGET redis++::redis++)
    if(TARGET redis++::redis++_static)
        add_library(redis++::redis++ ALIAS redis++::redis++_static)
    else()
        message(FATAL_ERROR
            "redis-plus-plus did not export redis++::redis++ or redis++::redis++_static")
    endif()
endif()
find_package(libzip CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)
find_package(Threads REQUIRED)

if(WIN32 AND BERT_VCPKG_TRIPLET)
    file(GLOB VCPKG_RUNTIME_DLLS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/${BERT_VCPKG_TRIPLET}/bin/*.dll")
endif()

# ONNX Runtime 预编译二进制
if(WIN32)
    set(BERT_DEFAULT_ONNXRUNTIME_CPU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/onnxruntime-win-x64-1.17.1")
    set(BERT_DEFAULT_ONNXRUNTIME_GPU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/onnxruntime-win-x64-gpu-1.20.1")
    set(BERT_DEFAULT_CUDA_RUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/cuda-runtime/bin")
    set(BERT_ONNXRUNTIME_SHARED_LIB_RELATIVE "lib/onnxruntime.dll")
    set(BERT_ONNXRUNTIME_IMPORT_LIB_RELATIVE "lib/onnxruntime.lib")
    set(BERT_ONNXRUNTIME_RUNTIME_GLOB "lib/onnxruntime*.dll")
elseif(UNIX AND NOT APPLE)
    set(BERT_DEFAULT_ONNXRUNTIME_CPU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/onnxruntime-linux-x64-1.17.1")
    set(BERT_DEFAULT_ONNXRUNTIME_GPU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/onnxruntime-linux-x64-gpu-1.20.1")
    set(BERT_DEFAULT_CUDA_RUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/cuda-runtime/lib64")
    set(BERT_ONNXRUNTIME_SHARED_LIB_RELATIVE "lib/libonnxruntime.so")
    set(BERT_ONNXRUNTIME_IMPORT_LIB_RELATIVE "")
    set(BERT_ONNXRUNTIME_RUNTIME_GLOB "lib/libonnxruntime*.so*")
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

set(ONNXRUNTIME_CPU_ROOT "${BERT_DEFAULT_ONNXRUNTIME_CPU_ROOT}" CACHE PATH "Path to the CPU ONNX Runtime package")
set(ONNXRUNTIME_GPU_ROOT "${BERT_DEFAULT_ONNXRUNTIME_GPU_ROOT}" CACHE PATH "Path to the GPU ONNX Runtime package")
set(CUDA_RUNTIME_DLL_ROOT "${BERT_DEFAULT_CUDA_RUNTIME_ROOT}" CACHE PATH "Directory containing CUDA/cuDNN runtime libraries")
set(BERT_EXTRA_RUNTIME_DLL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/build/Release" CACHE PATH "Optional directory containing local runtime DLL/SO overrides")
set(BERT_USE_ONNXRUNTIME_GPU "AUTO" CACHE STRING "Use the GPU ONNX Runtime package: AUTO, ON, or OFF")
set_property(CACHE BERT_USE_ONNXRUNTIME_GPU PROPERTY STRINGS AUTO ON OFF)

set(BERT_ONNXRUNTIME_CPU_LIB "${ONNXRUNTIME_CPU_ROOT}/${BERT_ONNXRUNTIME_SHARED_LIB_RELATIVE}")
set(BERT_ONNXRUNTIME_GPU_LIB "${ONNXRUNTIME_GPU_ROOT}/${BERT_ONNXRUNTIME_SHARED_LIB_RELATIVE}")

if(BERT_USE_ONNXRUNTIME_GPU STREQUAL "ON")
    if(NOT EXISTS "${BERT_ONNXRUNTIME_GPU_LIB}")
        message(FATAL_ERROR "BERT_USE_ONNXRUNTIME_GPU=ON but GPU package was not found at ${ONNXRUNTIME_GPU_ROOT}")
    endif()
    set(ONNXRUNTIME_ROOT "${ONNXRUNTIME_GPU_ROOT}")
elseif(BERT_USE_ONNXRUNTIME_GPU STREQUAL "AUTO")
    if(EXISTS "${BERT_ONNXRUNTIME_GPU_LIB}")
        set(ONNXRUNTIME_ROOT "${ONNXRUNTIME_GPU_ROOT}")
    else()
        set(ONNXRUNTIME_ROOT "${ONNXRUNTIME_CPU_ROOT}")
    endif()
else()
    set(ONNXRUNTIME_ROOT "${ONNXRUNTIME_CPU_ROOT}")
endif()

set(BERT_ONNXRUNTIME_SHARED_LIB "${ONNXRUNTIME_ROOT}/${BERT_ONNXRUNTIME_SHARED_LIB_RELATIVE}")
require_path("${BERT_ONNXRUNTIME_SHARED_LIB}" "ONNX Runtime package")

message(STATUS "Using ONNX Runtime from ${ONNXRUNTIME_ROOT}")

add_library(onnxruntime SHARED IMPORTED)
set_target_properties(onnxruntime PROPERTIES
    IMPORTED_LOCATION "${BERT_ONNXRUNTIME_SHARED_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_ROOT}/include"
)

if(WIN32)
    set_target_properties(onnxruntime PROPERTIES
        IMPORTED_IMPLIB "${ONNXRUNTIME_ROOT}/${BERT_ONNXRUNTIME_IMPORT_LIB_RELATIVE}"
    )
endif()

# llama.cpp 预编译路径
if(NOT DEFINED LLAMA_CPP_ROOT)
    set(LLAMA_CPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/llama.cpp" CACHE PATH "Path to llama.cpp repository")
endif()
if(NOT DEFINED LLAMA_CPP_BUILD)
    set(LLAMA_CPP_BUILD "${LLAMA_CPP_ROOT}/build" CACHE PATH "Path to llama.cpp build directory")
endif()
set(LLAMA_CPP_PREBUILT_CONFIG "" CACHE STRING
    "Configuration used to build prebuilt llama.cpp binaries; inferred from its CMakeCache.txt when empty")

function(agentloom_read_cmake_cache_entry cache_file entry_name output_variable)
    if(NOT EXISTS "${cache_file}")
        set(${output_variable} "" PARENT_SCOPE)
        return()
    endif()

    file(STRINGS "${cache_file}" cache_entry
        REGEX "^${entry_name}(:[^=]*)?=" LIMIT_COUNT 1)
    if(cache_entry)
        string(REGEX REPLACE "^[^=]*=" "" cache_value "${cache_entry}")
        set(${output_variable} "${cache_value}" PARENT_SCOPE)
    else()
        set(${output_variable} "" PARENT_SCOPE)
    endif()
endfunction()

set(LLAMA_CPP_CACHE_FILE "${LLAMA_CPP_BUILD}/CMakeCache.txt")
agentloom_read_cmake_cache_entry(
    "${LLAMA_CPP_CACHE_FILE}" CMAKE_BUILD_TYPE LLAMA_CPP_CACHED_BUILD_TYPE)
agentloom_read_cmake_cache_entry(
    "${LLAMA_CPP_CACHE_FILE}" CMAKE_CXX_COMPILER LLAMA_CPP_CACHED_CXX_COMPILER)

if(NOT LLAMA_CPP_PREBUILT_CONFIG)
    set(LLAMA_CPP_PREBUILT_CONFIG "${LLAMA_CPP_CACHED_BUILD_TYPE}")
endif()

set(LLAMA_CPP_INCLUDE_DIRS
    "${LLAMA_CPP_ROOT}/include"
    "${LLAMA_CPP_ROOT}/tools/mtmd"
    "${LLAMA_CPP_ROOT}/common"
    "${LLAMA_CPP_ROOT}/ggml/include"
)

if(WIN32)
    if(EXISTS "${LLAMA_CPP_BUILD}/src/Release/llama.lib")
        set(LLAMA_CPP_LIB_DIR "${LLAMA_CPP_BUILD}/src/Release")
        set(LLAMA_CPP_COMMON_LIB_DIR "${LLAMA_CPP_BUILD}/common/Release")
        set(LLAMA_CPP_MTMD_LIB_DIR "${LLAMA_CPP_BUILD}/tools/mtmd/Release")
        set(LLAMA_CPP_GGML_LIB_DIR "${LLAMA_CPP_BUILD}/ggml/src/Release")
        set(LLAMA_CPP_BIN_DIR "${LLAMA_CPP_BUILD}/bin/Release")
        if(NOT LLAMA_CPP_PREBUILT_CONFIG)
            set(LLAMA_CPP_PREBUILT_CONFIG "Release")
        endif()
    else()
        set(LLAMA_CPP_LIB_DIR "${LLAMA_CPP_BUILD}/src")
        set(LLAMA_CPP_COMMON_LIB_DIR "${LLAMA_CPP_BUILD}/common")
        set(LLAMA_CPP_MTMD_LIB_DIR "${LLAMA_CPP_BUILD}/tools/mtmd")
        set(LLAMA_CPP_GGML_LIB_DIR "${LLAMA_CPP_BUILD}/ggml/src")
        set(LLAMA_CPP_BIN_DIR "${LLAMA_CPP_BUILD}/bin")
    endif()
    set(LLAMA_CPP_LIBS
        "${LLAMA_CPP_LIB_DIR}/llama.lib"
        "${LLAMA_CPP_MTMD_LIB_DIR}/mtmd.lib"
        "${LLAMA_CPP_COMMON_LIB_DIR}/llama-common.lib"
        "${LLAMA_CPP_GGML_LIB_DIR}/ggml.lib"
        "${LLAMA_CPP_GGML_LIB_DIR}/ggml-base.lib"
    )
    set(LLAMA_CPP_RUNTIME_FILES
        "${LLAMA_CPP_BIN_DIR}/llama.dll"
        "${LLAMA_CPP_BIN_DIR}/ggml.dll"
        "${LLAMA_CPP_BIN_DIR}/ggml-base.dll"
        "${LLAMA_CPP_BIN_DIR}/ggml-cpu.dll"
        "${LLAMA_CPP_BIN_DIR}/ggml-cuda.dll"
        "${LLAMA_CPP_BIN_DIR}/mtmd.dll"
        "${LLAMA_CPP_BIN_DIR}/llama-common.dll"
    )
elseif(UNIX AND NOT APPLE)
    set(LLAMA_CPP_LIB_DIR "${LLAMA_CPP_BUILD}/src")
    set(LLAMA_CPP_COMMON_LIB_DIR "${LLAMA_CPP_BUILD}/common")
    set(LLAMA_CPP_MTMD_LIB_DIR "${LLAMA_CPP_BUILD}/tools/mtmd")
    set(LLAMA_CPP_GGML_LIB_DIR "${LLAMA_CPP_BUILD}/ggml/src")
    set(LLAMA_CPP_BIN_DIR "${LLAMA_CPP_BUILD}/bin")
    set(LLAMA_CPP_LIBS
        "${LLAMA_CPP_LIB_DIR}/libllama.so"
        "${LLAMA_CPP_MTMD_LIB_DIR}/libmtmd.so"
        "${LLAMA_CPP_COMMON_LIB_DIR}/libllama-common.a"
        "${LLAMA_CPP_GGML_LIB_DIR}/libggml.so"
        "${LLAMA_CPP_GGML_LIB_DIR}/libggml-base.so"
    )
    file(GLOB LLAMA_CPP_RUNTIME_FILES
        "${LLAMA_CPP_BIN_DIR}/libllama.so*"
        "${LLAMA_CPP_BIN_DIR}/libggml*.so*"
        "${LLAMA_CPP_BIN_DIR}/libmtmd.so*"
        "${LLAMA_CPP_LIB_DIR}/libllama.so*"
        "${LLAMA_CPP_MTMD_LIB_DIR}/libmtmd.so*"
        "${LLAMA_CPP_GGML_LIB_DIR}/libggml*.so*"
    )
endif()

if(WIN32 AND AGENTLOOM_BUILD_LOCAL_LLM)
    if(NOT LLAMA_CPP_PREBUILT_CONFIG)
        message(FATAL_ERROR
            "Cannot determine the ABI configuration of prebuilt llama.cpp at ${LLAMA_CPP_BUILD}. "
            "Set LLAMA_CPP_PREBUILT_CONFIG to Debug, Release, RelWithDebInfo, or MinSizeRel.")
    endif()

    add_custom_target(agentloom_llama_abi_guard
        COMMAND "${CMAKE_COMMAND}"
            "-DAGENT_CONFIG=$<CONFIG>"
            "-DLLAMA_CONFIG=${LLAMA_CPP_PREBUILT_CONFIG}"
            "-DLLAMA_BUILD=${LLAMA_CPP_BUILD}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AgentLoomLlamaAbiGuard.cmake"
        VERBATIM
        COMMENT "Checking AgentLoom/llama.cpp ABI configuration")

    if(MSVC AND LLAMA_CPP_CACHED_CXX_COMPILER)
        string(REGEX MATCH
            "MSVC[/\\\\]([0-9]+\\.[0-9]+)"
            LLAMA_CPP_TOOLSET_MATCH "${LLAMA_CPP_CACHED_CXX_COMPILER}")
        set(LLAMA_CPP_TOOLSET_DIRECTORY_VERSION "${CMAKE_MATCH_1}")
        string(REGEX MATCH
            "MSVC[/\\\\]([0-9]+\\.[0-9]+)"
            AGENT_TOOLSET_MATCH "${CMAKE_CXX_COMPILER}")
        set(AGENT_TOOLSET_DIRECTORY_VERSION "${CMAKE_MATCH_1}")

        if(LLAMA_CPP_TOOLSET_DIRECTORY_VERSION
                AND AGENT_TOOLSET_DIRECTORY_VERSION
                AND NOT LLAMA_CPP_TOOLSET_DIRECTORY_VERSION
                    STREQUAL AGENT_TOOLSET_DIRECTORY_VERSION)
            string(CONCAT LLAMA_CPP_TOOLSET_MESSAGE
                "MSVC toolset mismatch: AgentLoom uses ${AGENT_TOOLSET_DIRECTORY_VERSION} "
                "(${CMAKE_CXX_COMPILER}), while llama.cpp uses "
                "${LLAMA_CPP_TOOLSET_DIRECTORY_VERSION} (${LLAMA_CPP_CACHED_CXX_COMPILER}).")
            if(AGENT_LLAMA_STRICT_TOOLSET_ABI)
                message(FATAL_ERROR "${LLAMA_CPP_TOOLSET_MESSAGE}")
            else()
                message(WARNING
                    "${LLAMA_CPP_TOOLSET_MESSAGE} Matching toolsets are recommended; "
                    "set AGENT_LLAMA_STRICT_TOOLSET_ABI=ON to reject this configuration.")
            endif()
        endif()
    endif()

    message(STATUS
        "llama.cpp prebuilt ABI: config=${LLAMA_CPP_PREBUILT_CONFIG}, "
        "build=${LLAMA_CPP_BUILD}")
endif()

if(AGENTLOOM_BUILD_LOCAL_LLM)
    foreach(LLAMA_CPP_INCLUDE_DIR ${LLAMA_CPP_INCLUDE_DIRS})
        require_path("${LLAMA_CPP_INCLUDE_DIR}" "llama.cpp include directory")
    endforeach()

    foreach(LLAMA_CPP_LIB ${LLAMA_CPP_LIBS})
        require_path("${LLAMA_CPP_LIB}" "llama.cpp library")
    endforeach()
endif()

if(AGENTLOOM_BUILD_LOCAL_LLM)
    add_library(agentloom_llama_cpp_dependency INTERFACE)
    target_include_directories(agentloom_llama_cpp_dependency INTERFACE
        "$<BUILD_INTERFACE:${LLAMA_CPP_INCLUDE_DIRS}>")
    target_link_libraries(agentloom_llama_cpp_dependency INTERFACE
        "$<BUILD_INTERFACE:${LLAMA_CPP_LIBS}>"
        "$<INSTALL_INTERFACE:AgentLoom::llama_cpp_external>")
endif()

if(AGENTLOOM_BUILD_MEDIA)
    if(WIN32)
        set(BERT_DEFAULT_OPENCV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/opencv-4.10.0-windows/build")
        set(OpenCV_DIR "${BERT_DEFAULT_OPENCV_ROOT}/x64/vc16/lib" CACHE PATH "Path to OpenCVConfig.cmake")
        set(BERT_OPENCV_RUNTIME_DIR "${BERT_DEFAULT_OPENCV_ROOT}/x64/vc16/bin")
        set(BERT_OPENCV_RUNTIME_GLOB "${BERT_OPENCV_RUNTIME_DIR}/opencv_world*.dll")
        set(BERT_OPENCV_EXTRA_RUNTIME_GLOB "${BERT_OPENCV_RUNTIME_DIR}/opencv_videoio_*.dll")
    elseif(UNIX AND NOT APPLE)
        set(BERT_DEFAULT_OPENCV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/opencv-4.10.0-linux")
        set(OpenCV_DIR "${BERT_DEFAULT_OPENCV_ROOT}/lib/cmake/opencv4" CACHE PATH "Path to OpenCVConfig.cmake")
        set(BERT_OPENCV_RUNTIME_DIR "${BERT_DEFAULT_OPENCV_ROOT}/lib")
        set(BERT_OPENCV_RUNTIME_GLOB "${BERT_OPENCV_RUNTIME_DIR}/libopencv*.so*")
        set(BERT_OPENCV_EXTRA_RUNTIME_GLOB "")
    endif()

    find_package(OpenCV CONFIG REQUIRED COMPONENTS core imgproc videoio)
    file(GLOB BERT_OPENCV_RUNTIME_FILES
        "${BERT_OPENCV_RUNTIME_GLOB}"
        "${BERT_OPENCV_EXTRA_RUNTIME_GLOB}")
endif()

set(BERT_BOOST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/boost_1_85_0" CACHE PATH "Path to Boost header package")
require_path("${BERT_BOOST_ROOT}/boost/asio.hpp" "Boost.Asio header")
require_path("${BERT_BOOST_ROOT}/boost/beast.hpp" "Boost.Beast header")
require_path("${BERT_BOOST_ROOT}/boost/redis.hpp" "Boost.Redis header")
require_path("${BERT_BOOST_ROOT}/boost/redis/src.hpp" "Boost.Redis source header")

add_library(boost_asio_headers INTERFACE)
file(STRINGS "${BERT_BOOST_ROOT}/boost/version.hpp" _agentloom_boost_version_line
    REGEX "^#define BOOST_VERSION [0-9]+$" LIMIT_COUNT 1)
file(STRINGS "${BERT_BOOST_ROOT}/boost/version.hpp" _agentloom_boost_lib_version_line
    REGEX "^#define BOOST_LIB_VERSION \"[0-9_]+\"$" LIMIT_COUNT 1)
string(REGEX REPLACE ".*BOOST_VERSION ([0-9]+).*" "\\1"
    AGENTLOOM_BOOST_VERSION "${_agentloom_boost_version_line}")
string(REGEX REPLACE ".*BOOST_LIB_VERSION \"([0-9_]+)\".*" "\\1"
    AGENTLOOM_BOOST_LIB_VERSION "${_agentloom_boost_lib_version_line}")
string(REPLACE "_" "." AGENTLOOM_BOOST_VERSION_STRING
    "${AGENTLOOM_BOOST_LIB_VERSION}")
if(NOT AGENTLOOM_BOOST_VERSION MATCHES "^[0-9]+$" OR
        NOT AGENTLOOM_BOOST_VERSION_STRING MATCHES "^[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "Unable to determine Boost version from ${BERT_BOOST_ROOT}")
endif()
target_include_directories(boost_asio_headers BEFORE INTERFACE
    "$<BUILD_INTERFACE:${BERT_BOOST_ROOT}>")
target_compile_definitions(boost_asio_headers INTERFACE
    "AGENTLOOM_BUILT_BOOST_VERSION=${AGENTLOOM_BOOST_VERSION}")
target_link_libraries(boost_asio_headers INTERFACE "$<INSTALL_INTERFACE:Boost::headers>")
if(WIN32)
    target_compile_definitions(boost_asio_headers INTERFACE _WIN32_WINNT=0x0602)
    target_link_libraries(boost_asio_headers INTERFACE ws2_32 mswsock)
endif()

add_library(boost_redis_headers INTERFACE)
target_link_libraries(boost_redis_headers INTERFACE boost_asio_headers OpenSSL::SSL OpenSSL::Crypto)

add_library(boost_interprocess_headers INTERFACE)
target_include_directories(boost_interprocess_headers INTERFACE "${BERT_BOOST_ROOT}")
target_link_libraries(boost_interprocess_headers INTERFACE "$<INSTALL_INTERFACE:Boost::headers>")

set(BERT_EIGEN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/eigen-5.0.1" CACHE PATH "Path to Eigen header package")
require_path("${BERT_EIGEN_ROOT}/Eigen/Dense" "Eigen headers")

add_library(eigen_headers INTERFACE)
target_include_directories(eigen_headers INTERFACE "${BERT_EIGEN_ROOT}")

set(BERT_SQLITE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/sqlite-amalgamation-3530100" CACHE PATH "Path to SQLite headers")
if(WIN32)
    set(BERT_SQLITE_DLL "${CMAKE_CURRENT_SOURCE_DIR}/deps/sqlite3.dll" CACHE FILEPATH "Path to sqlite3.dll")
    set(BERT_SQLITE_DEF "${CMAKE_CURRENT_SOURCE_DIR}/deps/sqlite3.def" CACHE FILEPATH "Path to sqlite3.def")
else()
    set(BERT_SQLITE_DLL "" CACHE FILEPATH "SQLite runtime library")
    set(BERT_SQLITE_DEF "" CACHE FILEPATH "SQLite module definition")
endif()
set(BERT_SQLITE_BUILD_DEF "${CMAKE_CURRENT_BINARY_DIR}/deps/sqlite3.def")
set(BERT_SQLITE_IMPLIB "${CMAKE_CURRENT_BINARY_DIR}/deps/sqlite3.lib")

require_path("${BERT_SQLITE_INCLUDE_DIR}/sqlite3.h" "SQLite header")

if(WIN32)
    require_path("${BERT_SQLITE_DLL}" "SQLite runtime")
    require_path("${BERT_SQLITE_DEF}" "SQLite module definition")
    add_custom_command(
        OUTPUT "${BERT_SQLITE_IMPLIB}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/deps"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BERT_SQLITE_DEF}" "${BERT_SQLITE_BUILD_DEF}"
        COMMAND lib /def:sqlite3.def /machine:x64 /out:sqlite3.lib
        DEPENDS "${BERT_SQLITE_DEF}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/deps"
        VERBATIM
    )
    add_custom_target(sqlite_import_lib DEPENDS "${BERT_SQLITE_IMPLIB}")

    add_library(sqlite3 SHARED IMPORTED)
    add_dependencies(sqlite3 sqlite_import_lib)
    set_target_properties(sqlite3 PROPERTIES
        IMPORTED_LOCATION "${BERT_SQLITE_DLL}"
        IMPORTED_IMPLIB "${BERT_SQLITE_IMPLIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${BERT_SQLITE_INCLUDE_DIR}"
    )
elseif(UNIX AND NOT APPLE)
    require_path("${BERT_SQLITE_INCLUDE_DIR}/sqlite3.c" "SQLite amalgamation source")
    add_library(sqlite3 STATIC "${BERT_SQLITE_INCLUDE_DIR}/sqlite3.c")
    target_include_directories(sqlite3 PUBLIC "${BERT_SQLITE_INCLUDE_DIR}")
    target_link_libraries(sqlite3 PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
endif()

if(WIN32)
    set(BERT_FAISS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/faiss-1.14.1-cpu-win64" CACHE PATH "Path to Faiss CPU package")
    set(BERT_MKL_RUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/mkl-2023.1.0-win64" CACHE PATH "Path to MKL runtime package")
    require_path("${BERT_FAISS_ROOT}/include/faiss/IndexFlat.h" "Faiss headers")
    require_path("${BERT_FAISS_ROOT}/lib/faiss.lib" "Faiss import library")
    require_path("${BERT_FAISS_ROOT}/bin/faiss.dll" "Faiss runtime")
    require_path("${BERT_MKL_RUNTIME_ROOT}/Library/bin/mkl_rt.2.dll" "MKL runtime")
    set(faiss_DIR "${BERT_FAISS_ROOT}/share/faiss" CACHE PATH "Path to Faiss config package")
    find_package(faiss CONFIG REQUIRED)
    set(BERT_FAISS_RUNTIME_FILES
        "${BERT_FAISS_ROOT}/bin/faiss.dll"
        "${BERT_MKL_RUNTIME_ROOT}/Library/bin/mkl_rt.2.dll")
elseif(UNIX AND NOT APPLE)
    set(BERT_FAISS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/faiss-1.14.1-cpu-linux" CACHE PATH "Path to Faiss CPU package")
    set(BERT_MKL_RUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/mkl-2023.1.0-linux" CACHE PATH "Path to MKL runtime package")
    require_path("${BERT_FAISS_ROOT}/include/faiss/IndexFlat.h" "Faiss headers")
    require_path("${BERT_FAISS_ROOT}/lib/libfaiss.so" "Faiss shared library")
    find_library(BERT_OPENBLAS_LIBRARY NAMES openblas libopenblas.so.0 REQUIRED)
    add_library(faiss SHARED IMPORTED)
    set_target_properties(faiss PROPERTIES
        IMPORTED_LOCATION "${BERT_FAISS_ROOT}/lib/libfaiss.so"
        INTERFACE_INCLUDE_DIRECTORIES "${BERT_FAISS_ROOT}/include"
        INTERFACE_LINK_LIBRARIES "${BERT_OPENBLAS_LIBRARY}")
    set(BERT_FAISS_RUNTIME_FILES "${BERT_FAISS_ROOT}/lib/libfaiss.so")
    if(EXISTS "${BERT_MKL_RUNTIME_ROOT}/lib/libmkl_rt.so")
        list(APPEND BERT_FAISS_RUNTIME_FILES "${BERT_MKL_RUNTIME_ROOT}/lib/libmkl_rt.so")
    endif()
endif()

# GStreamer prebuilt package for media/WebRTC probes and future media pipeline work.
set(GSTREAMER_ROOT "D:/Program Files/gstreamer/1.0/msvc_x86_64" CACHE PATH "Path to GStreamer MSVC x64 package")
if(AGENTLOOM_BUILD_MEDIA AND EXISTS "${GSTREAMER_ROOT}/bin/gst-inspect-1.0.exe")
    add_library(gstreamer_headers INTERFACE)
    target_include_directories(gstreamer_headers INTERFACE
        "${GSTREAMER_ROOT}/include/gstreamer-1.0"
        "${GSTREAMER_ROOT}/include/glib-2.0"
        "${GSTREAMER_ROOT}/lib/glib-2.0/include"
    )

    add_library(gstreamer_core INTERFACE)
    target_link_libraries(gstreamer_core INTERFACE
        gstreamer_headers
        "${GSTREAMER_ROOT}/lib/gstreamer-1.0.lib"
        "${GSTREAMER_ROOT}/lib/gobject-2.0.lib"
        "${GSTREAMER_ROOT}/lib/glib-2.0.lib"
    )

    add_library(agentloom_gstreamer_dependency INTERFACE)
    target_include_directories(agentloom_gstreamer_dependency INTERFACE
        "$<BUILD_INTERFACE:${GSTREAMER_ROOT}/include/gstreamer-1.0>"
        "$<BUILD_INTERFACE:${GSTREAMER_ROOT}/include/glib-2.0>"
        "$<BUILD_INTERFACE:${GSTREAMER_ROOT}/lib/glib-2.0/include>")
    target_link_libraries(agentloom_gstreamer_dependency INTERFACE
        "$<BUILD_INTERFACE:${GSTREAMER_ROOT}/lib/gstreamer-1.0.lib>"
        "$<BUILD_INTERFACE:${GSTREAMER_ROOT}/lib/gobject-2.0.lib>"
        "$<BUILD_INTERFACE:${GSTREAMER_ROOT}/lib/glib-2.0.lib>"
        "$<BUILD_INTERFACE:${GSTREAMER_ROOT}/lib/gstapp-1.0.lib>"
        "$<BUILD_INTERFACE:${GSTREAMER_ROOT}/lib/gstvideo-1.0.lib>"
        "$<BUILD_INTERFACE:${GSTREAMER_ROOT}/lib/gstsdp-1.0.lib>"
        "$<BUILD_INTERFACE:${GSTREAMER_ROOT}/lib/gstwebrtc-1.0.lib>"
        "$<INSTALL_INTERFACE:AgentLoom::gstreamer_external>")
elseif(AGENTLOOM_BUILD_MEDIA)
    message(STATUS "GStreamer package not found at ${GSTREAMER_ROOT}; media_gstreamer_probe will not be built")
endif()
