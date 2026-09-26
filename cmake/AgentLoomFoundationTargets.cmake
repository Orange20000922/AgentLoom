# ==================== 公共库 ====================

add_library(agent_core STATIC
    src/core/blocking_queue.h
    src/core/exception.h
    src/core/keyed_serial_executor.cpp
    src/core/keyed_serial_executor.h
    src/core/logger_adapter.h
    src/core/memory_pool.cpp
    src/core/memory_pool.h
    src/core/object_pool.h
    src/core/optimizer.cpp
    src/core/optimizer.h
    src/core/ordered_bitmap_window.h
    src/core/result.h
    src/core/shared_memory_block.h
    src/core/task_group.cpp
    src/core/task_group.h
    src/core/text_validation.h
    src/core/thread_pool.cpp
    src/core/thread_pool.h
    src/core/thread_pool_scheduler.cpp
    src/core/thread_pool_scheduler.h
    src/core/unique_handle.h
)

target_include_directories(agent_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
)

target_link_libraries(agent_core PUBLIC spdlog::spdlog)

add_library(agent_net STATIC
    src/net/backpressure_queue.h
    src/net/connection_pool.cpp
    src/net/connection_pool.h
    src/net/http_request_filter.cpp
    src/net/http_request_filter.h
    src/net/http_server.cpp
    src/net/http_server.h
    src/net/http_types.h
    src/net/protocol_types.h
    src/net/request_interfaces.h
    src/net/shared_buffer.h
    src/net/static_file_handler.cpp
    src/net/static_file_handler.h
    src/net/websocket_session.cpp
    src/net/websocket_session.h
    src/net/websocket_types.h
)

target_include_directories(agent_net PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
)

target_link_libraries(agent_net PUBLIC
    agent_core
)

if(TARGET boost_asio_headers)
    target_link_libraries(agent_net PUBLIC boost_asio_headers)
endif()

# ──────────── TLS context (reusable for HTTP, WebSocket, SMTP, etc.) ────────────
add_library(agent_tls STATIC
    src/net/tls/tls_options.h
    src/net/tls/tls_context.h
    src/net/tls/tls_context.cpp
)

target_include_directories(agent_tls PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/tls
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
)

target_link_libraries(agent_tls PUBLIC
    agent_core
    boost_asio_headers
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(BERT_BUILD_TESTS)
    include(GoogleTest)
    add_executable(tls_tests
        tests/net/tls/tls_context_test.cpp
    )
    target_link_libraries(tls_tests PRIVATE
        agent_tls
        GTest::gtest_main
    )
    gtest_discover_tests(tls_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS "ci;unit;tls")
endif()

# ──────────── Outbound HTTP/HTTPS client (reusable for LLM, RAG, webhooks) ────────────
add_library(agent_http_client STATIC
    src/net/http_client/http_client.h
    src/net/http_client/url_parser.h
    src/net/http_client/url_parser.cpp
    src/net/http_client/retry_policy.h
    src/net/http_client/retry_policy.cpp
    src/net/http_client/beast_http_client.h
    src/net/http_client/beast_http_client.cpp
    src/net/http_client/async_beast_http_client.h
    src/net/http_client/async_beast_http_client.cpp
    src/net/http_client/concurrent_http_client.h
)

target_include_directories(agent_http_client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/http_client
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/tls
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
)

target_link_libraries(agent_http_client PUBLIC
    agent_core
    agent_tls
    boost_asio_headers
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(BERT_BUILD_TESTS)
    add_executable(http_client_tests
        tests/net/http_client/url_parser_test.cpp
        tests/net/http_client/retry_policy_test.cpp
        tests/net/http_client/beast_http_client_test.cpp
        tests/net/http_client/async_beast_http_client_test.cpp
    )
    target_link_libraries(http_client_tests PRIVATE
        agent_http_client
        GTest::gtest_main
    )
    gtest_discover_tests(http_client_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS "ci;unit;http-client")
endif()

# ──────────── LLM client (OpenAI-compatible) ────────────
add_library(agent_llm STATIC
    src/llm/openai_llm_client.h
    src/llm/openai_llm_client.cpp
    src/llm/cloud_task_coordinator.h
    src/llm/cloud_task_coordinator.cpp
)
if(AGENTLOOM_BUILD_LOCAL_LLM)
    target_sources(agent_llm PRIVATE
        src/llm/local_llm_client.h
        src/llm/local_llm_client.cpp)
    target_compile_definitions(agent_llm PUBLIC AGENTLOOM_HAS_LOCAL_LLM=1)
endif()

target_include_directories(agent_llm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/llm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/http_client
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
)

target_link_libraries(agent_llm PUBLIC
    agent_http_client
)
if(AGENTLOOM_BUILD_LOCAL_LLM)
    target_link_libraries(agent_llm PUBLIC multimodal_proto gRPC::grpc++)
endif()

if(BERT_BUILD_TESTS)
    add_executable(llm_tests
        tests/llm/openai_llm_client_test.cpp
        tests/llm/cloud_task_coordinator_test.cpp
    )
    target_link_libraries(llm_tests PRIVATE
        agent_llm
        GTest::gtest_main
    )
    if(AGENTLOOM_BUILD_LOCAL_LLM)
        target_compile_definitions(llm_tests PRIVATE AGENTLOOM_TEST_LOCAL_LLM=1)
    endif()
    gtest_discover_tests(llm_tests DISCOVERY_MODE PRE_TEST)

    add_executable(llm_integration_tests
        tests/llm/llm_integration_e2e_test.cpp
    )
    target_link_libraries(llm_integration_tests PRIVATE
        agent_llm
        GTest::gtest_main
    )
    if(WIN32)
        target_link_libraries(llm_integration_tests PRIVATE agent_config)
    endif()
    link_whole_archive(llm_integration_tests agent_config)
    gtest_discover_tests(llm_integration_tests DISCOVERY_MODE PRE_TEST)
endif()

if(AGENTLOOM_BUILD_TOOLS)
# ──────────── OpenAI-compatible C++ quota mock (manual E2E benchmark peer) ────────────
add_executable(openai_compatible_quota_mock_server
    tools/openai_compatible_quota_mock_server.cpp
)
target_link_libraries(openai_compatible_quota_mock_server PRIVATE
    agent_net
)

# ──────────── Manual smoke test against a real LLM API (not part of CTest) ────────────
add_executable(llm_smoke_test
    tools/llm_smoke_test.cpp
)
target_link_libraries(llm_smoke_test PRIVATE
    agent_llm
    agent_tls
)
if(WIN32)
    target_link_libraries(llm_smoke_test PRIVATE agent_config)
endif()
link_whole_archive(llm_smoke_test agent_config)

# ──────────── Manual real-provider Skill tool-call E2E (not part of CTest) ────────────
add_executable(skill_llm_e2e_test tools/skill_llm_e2e_test.cpp)
target_include_directories(skill_llm_e2e_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/skill
    ${CMAKE_CURRENT_SOURCE_DIR}/src/llm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/vector
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/sqlite
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vector
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/persona
)
target_link_libraries(skill_llm_e2e_test PRIVATE
    agent_agent_runtime agent_tls agent_config agent_vector agent_vector_storage agent_storage)
link_whole_archive(skill_llm_e2e_test agent_config)

# ──────────── L3 compression E2E test (not part of CTest) ────────────
add_executable(l3_compression_e2e_test
    tools/l3_compression_e2e_test.cpp
)
target_include_directories(l3_compression_e2e_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/memory
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/sqlite
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/vector
    ${CMAKE_CURRENT_SOURCE_DIR}/src/semantic_cache
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vector
    ${CMAKE_CURRENT_SOURCE_DIR}/src/llm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
)
target_link_libraries(l3_compression_e2e_test PRIVATE
    agent_memory
    agent_llm
    agent_semantic_cache
    agent_storage
    agent_vector
    agent_vector_storage
    agent_config
    agent_tls
    nlohmann_json::nlohmann_json
)
if(WIN32)
    link_whole_archive(l3_compression_e2e_test agent_config)
endif()
copy_runtime_files(l3_compression_e2e_test "${BERT_SQLITE_DLL}")

# ──────────── Persona Gateway frontend E2E server (not part of CTest) ────────────
add_executable(document_analysis_e2e_test
    tools/document_analysis_e2e_test.cpp
)
target_include_directories(document_analysis_e2e_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/document
    ${CMAKE_CURRENT_SOURCE_DIR}/src/document/ooxml
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/gateway
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/persona
    ${CMAKE_CURRENT_SOURCE_DIR}/src/semantic_cache
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vector
    ${CMAKE_CURRENT_SOURCE_DIR}/src/llm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
)
target_link_libraries(document_analysis_e2e_test PRIVATE
    agent_service
    agent_document
    agent_vector
    agent_llm
    agent_tls
    agent_config
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
)
if(WIN32)
    link_whole_archive(document_analysis_e2e_test agent_config)
endif()
copy_runtime_files(document_analysis_e2e_test "${BERT_SQLITE_DLL}")
copy_runtime_files(document_analysis_e2e_test ${VCPKG_RUNTIME_DLLS})

function(agentloom_copy_gateway_runtime_files target)
    # Gateway 需支持单独构建，不能依赖其他测试目标顺带复制运行时库。
    copy_runtime_files(${target} "${BERT_SQLITE_DLL}")
    copy_runtime_files(${target} ${BERT_FAISS_RUNTIME_FILES})
    copy_runtime_files(${target} ${VCPKG_RUNTIME_DLLS})

    file(GLOB _gateway_onnxruntime_files
        "${ONNXRUNTIME_ROOT}/${BERT_ONNXRUNTIME_RUNTIME_GLOB}")
    copy_runtime_files(${target} ${_gateway_onnxruntime_files})

    if(ONNXRUNTIME_ROOT STREQUAL ONNXRUNTIME_GPU_ROOT AND
            EXISTS "${CUDA_RUNTIME_DLL_ROOT}")
        if(WIN32)
            file(GLOB _gateway_cuda_runtime_files
                "${CUDA_RUNTIME_DLL_ROOT}/*.dll")
        elseif(UNIX AND NOT APPLE)
            file(GLOB _gateway_cuda_runtime_files
                "${CUDA_RUNTIME_DLL_ROOT}/*.so*")
        endif()
        copy_runtime_files(${target} ${_gateway_cuda_runtime_files})
    endif()

    if(UNIX AND NOT APPLE)
        set_target_properties(${target} PROPERTIES
            BUILD_RPATH "$ORIGIN"
            INSTALL_RPATH "$ORIGIN")
    endif()
endfunction()

add_executable(persona_gateway_e2e_server
    tools/persona_gateway_e2e_server.cpp
)
target_include_directories(persona_gateway_e2e_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/sqlite
)
target_link_libraries(persona_gateway_e2e_server PRIVATE
    server_runtime
    agent_service
    agent_llm
    agent_tls
    agent_config
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
)
if(WIN32)
    link_whole_archive(persona_gateway_e2e_server agent_config)
endif()
agentloom_copy_gateway_runtime_files(persona_gateway_e2e_server)

add_executable(agent_gateway_server
    src/server/main/agent_gateway_server.cpp
)
target_include_directories(agent_gateway_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/sqlite
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/vector
)
if(WIN32)
    target_include_directories(agent_gateway_server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    target_link_libraries(agent_gateway_server PRIVATE Dbghelp)
endif()
target_link_libraries(agent_gateway_server PRIVATE
    server_runtime
    agent_service
    agent_document
    agent_vector
    agent_vector_storage
    agent_llm
    agent_tls
    agent_config
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
)
if(WIN32)
    link_whole_archive(agent_gateway_server agent_config)
endif()
if(TARGET agent_skill_media)
    link_whole_archive(agent_gateway_server agent_skill_media)
endif()
agentloom_copy_gateway_runtime_files(agent_gateway_server)

add_executable(persona_config_migration
    tools/persona_config_migration.cpp
)
target_link_libraries(persona_config_migration PRIVATE
    nlohmann_json::nlohmann_json
)

add_executable(emotion_fusion_map_calibrator
    tools/emotion_fusion_map_calibrator.cpp
)
target_link_libraries(emotion_fusion_map_calibrator PRIVATE
    agent_service
    agent_models
    agent_vector
    agent_llm
    agent_http_client
    nlohmann_json::nlohmann_json
)
copy_runtime_files(emotion_fusion_map_calibrator "${BERT_ONNXRUNTIME_SHARED_LIB}")
copy_runtime_files(emotion_fusion_map_calibrator ${VCPKG_RUNTIME_DLLS})

# ──────────── Bare boost::redis smoke test (not part of CTest) ────────────
add_executable(redis_bare_connection_test
    tools/redis_bare_connection_test.cpp
)
target_link_libraries(redis_bare_connection_test PRIVATE
    agent_semantic_cache
)

add_executable(redis_stress_test
    tools/redis_stress_test.cpp
)
target_link_libraries(redis_stress_test PRIVATE
    agent_semantic_cache
)

# ──────────── Semantic cache pipeline (skeleton — Phase 5) ────────────
endif()
