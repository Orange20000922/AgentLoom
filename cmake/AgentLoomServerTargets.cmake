# ==================== 推理服务端 ====================

add_library(agent_grpc_runtime STATIC
    src/server/grpc/async_grpc_runtime.cpp
    src/server/grpc/async_grpc_runtime.h
    src/server/grpc/grpc_status.cpp
    src/server/grpc/grpc_status.h
)

target_include_directories(agent_grpc_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/grpc
)

target_link_libraries(agent_grpc_runtime PUBLIC
    agent_core
    gRPC::grpc++
    spdlog::spdlog
)

add_library(agent_emotion_server STATIC
    src/server/grpc/grpc_error.cpp
    src/server/grpc/grpc_error.h
    src/server/grpc/async_emotion_grpc_service.cpp
    src/server/grpc/async_emotion_grpc_service.h
    src/server/grpc/async_emotion_inference_handler.cpp
    src/server/grpc/async_emotion_inference_handler.h
    src/server/grpc/emotion_grpc_service.cpp
    src/server/grpc/emotion_grpc_service.h
    src/service/inference/emotion_inference_service.cpp
    src/service/inference/emotion_inference_service.h
    src/service/inference/request_validation.cpp
    src/service/inference/request_validation.h
)

target_include_directories(agent_emotion_server PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/grpc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/inference
)

if(UNIX AND NOT APPLE)
    target_include_directories(agent_emotion_server PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/config
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
endif()

target_link_libraries(agent_emotion_server PUBLIC
    agent_grpc_runtime
    multimodal_proto
    agent_core
    agent_bert_models
    server_runtime
    gRPC::grpc++
    spdlog::spdlog
)
if(WIN32)
    target_link_libraries(agent_emotion_server PUBLIC agent_config)
else()
    target_link_libraries(agent_emotion_server PUBLIC agent_net)
endif()

if(AGENTLOOM_BUILD_LOCAL_LLM AND AGENTLOOM_BUILD_MEDIA)
add_library(agent_server STATIC
    src/server/grpc/grpc_error.cpp
    src/server/grpc/grpc_error.h
    src/server/grpc/multimodal_grpc_service.cpp
    src/server/grpc/multimodal_grpc_service.h
    src/service/inference/multimodal_service.cpp
    src/service/inference/multimodal_service.h
    src/service/inference/request_validation.cpp
    src/service/inference/request_validation.h
    src/service/inference/shared_memory_media_runtime.cpp
    src/service/inference/shared_memory_media_runtime.h
)

target_include_directories(agent_server PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/grpc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/inference
)

target_link_libraries(agent_server PUBLIC
    agent_grpc_runtime
    multimodal_proto
    agent_core
    agent_ipc
    agent_media_inference
    agent_skill_media
    agent_service
    agent_models
    agent_cache
    agent_semantic_cache
    agent_vector
    server_runtime
    agent_config
    gRPC::grpc++
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)
endif()

if(BERT_BUILD_EMOTION_INFERENCE_SERVER)
    add_executable(emotion_inference_server
        src/server/main/emotion_inference_server.cpp
    )

    target_link_libraries(emotion_inference_server PRIVATE agent_emotion_server)
    link_whole_archive(emotion_inference_server agent_config)
endif()

if(BERT_BUILD_MULTIMODAL_INFERENCE_SERVER AND TARGET agent_server)
    add_executable(multimodal_inference_server
        src/server/main/multimodal_inference_server.cpp
    )

    target_link_libraries(multimodal_inference_server PRIVATE agent_server)
    if(WIN32)
        link_whole_archive(multimodal_inference_server agent_config)
    endif()

    copy_runtime_files(multimodal_inference_server ${LLAMA_CPP_RUNTIME_FILES})
endif()

if(BERT_BUILD_TESTS)
    add_executable(inference_grpc_tests
        tests/server/inference_grpc_service_test.cpp
        src/server/grpc/grpc_error.cpp
        src/server/grpc/emotion_grpc_service.cpp
        src/server/grpc/multimodal_grpc_service.cpp
        src/service/inference/request_validation.cpp
    )

    target_include_directories(inference_grpc_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/server/grpc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/service/inference
    )

    target_link_libraries(inference_grpc_tests PRIVATE
        agent_grpc_runtime
        multimodal_proto
        agent_core
        agent_ipc_grpc
        agent_media_inference
        server_runtime
        agent_config
        gRPC::grpc++
        spdlog::spdlog
        GTest::gtest_main
    )

    copy_runtime_files(inference_grpc_tests ${VCPKG_RUNTIME_DLLS})
    gtest_discover_tests(inference_grpc_tests DISCOVERY_MODE PRE_TEST)

    add_executable(async_grpc_runtime_tests
        tests/server/async_grpc_runtime_test.cpp
        src/server/grpc/async_emotion_grpc_service.cpp
    )

    target_include_directories(async_grpc_runtime_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/server/grpc
    )

    target_link_libraries(async_grpc_runtime_tests PRIVATE
        agent_grpc_runtime
        multimodal_proto
        GTest::gtest_main
    )

    copy_runtime_files(async_grpc_runtime_tests ${VCPKG_RUNTIME_DLLS})
    gtest_discover_tests(async_grpc_runtime_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS "ci;unit;grpc-runtime")

    add_executable(shared_media_runtime_tests
        tests/server/shared_memory_media_runtime_test.cpp
        src/service/inference/shared_memory_media_runtime.cpp
    )

    target_include_directories(shared_media_runtime_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/service/inference
    )

    target_link_libraries(shared_media_runtime_tests PRIVATE
        multimodal_proto
        agent_core
        agent_ipc
        agent_media_inference
        agent_skill_media
        agent_config
        nlohmann_json::nlohmann_json
        GTest::gtest_main
    )

    copy_runtime_files(shared_media_runtime_tests ${VCPKG_RUNTIME_DLLS})
    gtest_discover_tests(shared_media_runtime_tests DISCOVERY_MODE PRE_TEST)
endif()

# ==================== 客户端工具（BERT 协议，用于向后兼容测试） ====================

if(AGENTLOOM_BUILD_LEGACY_BERT_PROTO)
    add_executable(bert_inference_client
        src/client/client_test.cpp
    )

    target_include_directories(bert_inference_client PRIVATE ${GENERATED_DIR})
    target_link_libraries(bert_inference_client PRIVATE bert_proto gRPC::grpc++)

    add_executable(bert_benchmark_client
        src/client/benchmark_client.cpp
    )

    target_include_directories(bert_benchmark_client PRIVATE ${GENERATED_DIR})
    target_link_libraries(bert_benchmark_client PRIVATE bert_proto gRPC::grpc++)
endif()
