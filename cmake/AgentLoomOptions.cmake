option(BERT_BUILD_TESTS "Build C++ unit tests" OFF)
option(BERT_BUILD_MULTIMODAL_INFERENCE_SERVER "Build full multimodal inference server with VLM/llama.cpp support" ON)
option(BERT_BUILD_EMOTION_INFERENCE_SERVER "Build CPU-only BERT emotion inference server" ON)
option(AGENTLOOM_BUILD_MEDIA "Build media, WebRTC, and shared-memory frame targets" ON)
option(AGENTLOOM_BUILD_LOCAL_LLM "Build llama.cpp/mtmd local LLM and VLM targets" ON)
option(AGENTLOOM_BUILD_REFERENCE_GATEWAY "Build AgentLoom's reference Gateway implementation" ON)
option(AGENTLOOM_BUILD_LEGACY_BERT_PROTO "Build the legacy BERT protocol and clients" ON)
option(AGENTLOOM_BUILD_TOOLS "Build smoke, benchmark, and reference executable targets" ON)
option(AGENT_LLAMA_STRICT_TOOLSET_ABI
    "Treat a prebuilt llama.cpp MSVC toolset mismatch as a configuration error" OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

if(BERT_BUILD_TESTS)
    enable_testing()
endif()

if(WIN32)
    set(BERT_VCPKG_TRIPLET_DEFAULT "x64-windows")
elseif(UNIX AND NOT APPLE)
    set(BERT_VCPKG_TRIPLET_DEFAULT "x64-linux")
else()
    set(BERT_VCPKG_TRIPLET_DEFAULT "")
endif()

set(BERT_VCPKG_TRIPLET "${BERT_VCPKG_TRIPLET_DEFAULT}" CACHE STRING "vcpkg target triplet for dependency lookup")
if(BERT_VCPKG_TRIPLET)
    list(APPEND CMAKE_PREFIX_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/${BERT_VCPKG_TRIPLET}"
        "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/${BERT_VCPKG_TRIPLET}/share")
endif()

function(require_path path label)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "${label} not found: ${path}")
    endif()
endfunction()

function(copy_runtime_files target)
    foreach(runtime_file IN LISTS ARGN)
        if(EXISTS "${runtime_file}")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${runtime_file}"
                    $<TARGET_FILE_DIR:${target}>)
        endif()
    endforeach()
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/AgentLoomWholeArchive.cmake")

function(link_whole_archive target library)
    # 源码构建和安装包 consumer 共用一个实现，避免平台分支逐渐分叉。
    if(TARGET "${library}")
        agentloom_link_whole_archive("${target}" "${library}")
    else()
        # Foundation 中的工具会提前引用稍后定义的 agent_config。等当前目录
        # 配置完毕再验证/应用 helper；EVAL 将当前实参固定，避免函数返回后丢失。
        cmake_language(EVAL CODE
            "cmake_language(DEFER CALL agentloom_link_whole_archive [[${target}]] [[${library}]])")
    endif()
endfunction()
