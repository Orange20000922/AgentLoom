add_library(server_runtime STATIC
    src/server/runtime/logger.cpp
    src/server/runtime/logger.h
    src/server/runtime/server_common.cpp
    src/server/runtime/server_common.h
)

target_include_directories(server_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/runtime
)

target_link_libraries(server_runtime PUBLIC spdlog::spdlog)

add_library(agent_bert_models STATIC
    src/models/onnx_model.cpp
    src/models/onnx_model.h
    src/models/onnx_session_utils.cpp
    src/models/onnx_session_utils.h
)

target_include_directories(agent_bert_models PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/models
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${ONNXRUNTIME_ROOT}/include
)

target_link_libraries(agent_bert_models PUBLIC
    agent_core
    onnxruntime
    spdlog::spdlog
)

if(AGENTLOOM_BUILD_LOCAL_LLM)
add_library(agent_models STATIC
    src/models/llama_handles.h
    src/models/llama_runner.cpp
    src/models/llama_runner.h
)

target_include_directories(agent_models PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/models
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${LLAMA_CPP_INCLUDE_DIRS}
)

target_link_libraries(agent_models PUBLIC
    agent_bert_models
    agentloom_llama_cpp_dependency
)

if(TARGET agentloom_llama_abi_guard)
    add_dependencies(agent_models agentloom_llama_abi_guard)
endif()

add_library(agent_cache STATIC
    src/cache/vlm_cache.cpp
    src/cache/vlm_cache.h
)

target_include_directories(agent_cache PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cache
)

target_link_libraries(agent_cache PUBLIC
    agent_models
    spdlog::spdlog
)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/HfTokenizersCapi.cmake")

add_library(agent_vector STATIC
    src/vector/vector_cache.cpp
    src/vector/vector_cache.h
    src/vector/hf_tokenizer.cpp
    src/vector/hf_tokenizer.h
    src/vector/tokenizer_pool.cpp
    src/vector/tokenizer_pool.h
    src/vector/text_embedding_model.h
    src/vector/text_embedding_pooling.cpp
    src/vector/onnx_text_embedding_model.cpp
    src/vector/onnx_text_embedding_model.h
    src/vector/embedding_pipeline.cpp
    src/vector/embedding_pipeline.h
    src/vector/embedding_batch_coordinator.cpp
    src/vector/embedding_batch_coordinator.h
    src/vector/vector_index.h
    src/vector/exact_vector_index.cpp
    src/vector/exact_vector_index.h
    src/vector/faiss_vector_index.cpp
    src/vector/faiss_vector_index.h
    src/vector/vector_index_manager.cpp
    src/vector/vector_index_manager.h
)

target_include_directories(agent_vector PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vector
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/models
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage
    ${ONNXRUNTIME_ROOT}/include
)

target_link_libraries(agent_vector PUBLIC
    agent_core
    agent_bert_models
    agent_vector_storage
    hf_tokenizers_capi
    faiss
    spdlog::spdlog
)

if(BERT_BUILD_TESTS)
    add_executable(vector_tests
        tests/vector/hf_tokenizer_test.cpp
        tests/vector/tokenizer_pool_test.cpp
        tests/vector/text_embedding_test.cpp
        tests/vector/embedding_batch_coordinator_test.cpp
        tests/vector/vlm_vector_cache_test.cpp
        tests/vector/vector_index_test.cpp
        tests/vector/vector_index_manager_test.cpp
    )

    target_link_libraries(vector_tests PRIVATE
        agent_vector
        agent_storage
        GTest::gtest_main
    )

    copy_runtime_files(vector_tests ${BERT_FAISS_RUNTIME_FILES})
    copy_runtime_files(vector_tests "${BERT_SQLITE_DLL}")

    file(GLOB VECTOR_TESTS_ONNXRUNTIME_LIBS "${ONNXRUNTIME_ROOT}/${BERT_ONNXRUNTIME_RUNTIME_GLOB}")
    foreach(_ort_lib ${VECTOR_TESTS_ONNXRUNTIME_LIBS})
        copy_runtime_files(vector_tests "${_ort_lib}")
    endforeach()

    gtest_discover_tests(vector_tests DISCOVERY_MODE PRE_TEST)
endif()

if(AGENTLOOM_BUILD_TOOLS)
    add_executable(embedding_batch_coordinator_bench
        tests/vector/embedding_batch_coordinator_bench.cpp
    )
    target_link_libraries(embedding_batch_coordinator_bench PRIVATE
        agent_vector
        agent_core
    )
endif()

add_library(agent_config STATIC
    src/config/config_section.cpp
    src/config/config_section.h
    src/config/option_parser.cpp
    src/config/option_parser.h
    src/config/request_options.h
    src/config/sections/agent_gateway_config_section.cpp
    src/config/sections/auth_config_section.cpp
    src/config/sections/config_path_section.cpp
    src/config/sections/embedding_config_section.cpp
    src/config/sections/emotion_config_section.cpp
    src/config/sections/grpc_config_section.cpp
    src/config/sections/gateway_auth_config_section.cpp
    src/config/sections/http_config_section.cpp
    src/config/sections/limits_config_section.cpp
    src/config/sections/llm_config_section.cpp
    src/config/sections/model_config_section.cpp
    src/config/sections/persona_gateway_config_section.cpp
    src/config/sections/skill_session_config_section.cpp
    src/config/sections/skill_registry_config_section.cpp
    src/config/sections/vlm_cache_config_section.cpp
    src/config/sections/vram_config_section.cpp
    src/config/server_config.cpp
    src/config/server_config.h
    src/config/server_options.h
)

target_include_directories(agent_config PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config
)

target_link_libraries(agent_config PUBLIC
    agent_net
    server_runtime
    nlohmann_json::nlohmann_json
)

if(BERT_BUILD_TESTS)
    add_executable(config_tests
        tests/config/config_parser_test.cpp
    )

    target_link_libraries(config_tests PRIVATE
        GTest::gtest_main
    )
    if(WIN32)
        target_link_libraries(config_tests PRIVATE agent_config)
    endif()
    link_whole_archive(config_tests agent_config)

    gtest_discover_tests(config_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS "ci;unit;config")
endif()
