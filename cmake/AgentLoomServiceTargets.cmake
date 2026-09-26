add_library(agent_service_core INTERFACE)

target_include_directories(agent_service_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/document
    ${CMAKE_CURRENT_SOURCE_DIR}/src/llm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/semantic_cache
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/persona
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/gateway
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
)

target_link_libraries(agent_service_core INTERFACE
    agent_core
    agent_net
    nlohmann_json::nlohmann_json
)

add_library(agent_agent_runtime STATIC
    src/service/persona/persona_algorithm.cpp
    src/service/persona/persona_algorithm.h
    src/service/persona/session_manager.cpp
    src/service/persona/session_manager.h
    src/service/persona/session_persistence_contracts.h
    src/service/persona/gateway_session_affinity_scheduler.cpp
    src/service/persona/gateway_session_affinity_scheduler.h
    src/service/persona/persona_runtime.cpp
    src/service/persona/persona_runtime.h
    src/service/persona/skill_session_manager.cpp
    src/service/persona/skill_session_manager.h
    src/service/persona/tool_memory_provider.cpp
    src/service/persona/tool_memory_provider.h
    src/skill/skill_manifest.h
    src/skill/skill_manifest_json.h
    src/skill/skill_registry.h
    src/skill/skill_registry.cpp
    src/skill/skill_prompt_compiler.h
    src/skill/skill_prompt_compiler.cpp
    src/skill/skill_executor.h
    src/skill/skill_executor.cpp
    src/service/persona/emotion_fusion_analyzer.cpp
    src/service/persona/emotion_fusion_analyzer.h
    src/service/persona/grpc_emotion_analyzer.cpp
    src/service/persona/grpc_emotion_analyzer.h
)

target_include_directories(agent_agent_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/persona
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/gateway
    ${CMAKE_CURRENT_SOURCE_DIR}/src/llm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/semantic_cache
    ${CMAKE_CURRENT_SOURCE_DIR}/src/memory
)

target_link_libraries(agent_agent_runtime PUBLIC
    multimodal_proto
    agent_core
    agent_llm
    agent_semantic_cache
    agent_memory
    nlohmann_json::nlohmann_json
    gRPC::grpc++
    spdlog::spdlog
)

if(AGENTLOOM_BUILD_REFERENCE_GATEWAY)
add_library(agent_gateway_auth STATIC
    src/service/gateway/gateway_auth.cpp
    src/service/gateway/gateway_auth.h
)

target_include_directories(agent_gateway_auth PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/gateway
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/src/semantic_cache
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage
)

target_link_libraries(agent_gateway_auth PUBLIC
    agent_core
    agent_net
    agent_semantic_cache
    agent_storage
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    spdlog::spdlog
)
endif()

add_library(agent_persona_interaction STATIC
    src/service/persona/persona_interaction.cpp
    src/service/persona/persona_interaction.h
)

target_include_directories(agent_persona_interaction PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/persona
)

target_link_libraries(agent_persona_interaction PUBLIC
    agent_agent_runtime
    agent_core
    spdlog::spdlog
)

add_library(agent_gateway_routing INTERFACE)

target_include_directories(agent_gateway_routing INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/gateway
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
)

target_link_libraries(agent_gateway_routing INTERFACE
    agent_core
    agent_net
)

add_library(agent_gateway_foundation STATIC
    src/service/gateway/gateway_lifecycle.cpp
    src/service/gateway/gateway_lifecycle.h
    src/service/gateway/gateway_maintenance.cpp
    src/service/gateway/gateway_maintenance.h
    src/service/gateway/runtime_maintenance_service.cpp
    src/service/gateway/runtime_maintenance_service.h
)

target_include_directories(agent_gateway_foundation PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/gateway
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/persona
)

target_link_libraries(agent_gateway_foundation PUBLIC
    agent_agent_runtime
    agent_gateway_routing
    agent_core
    agent_net
    agent_document
    agent_ipc
    spdlog::spdlog
)

if(TARGET agent_media)
    target_compile_definitions(agent_gateway_foundation PRIVATE AGENTLOOM_HAS_MEDIA=1)
    target_link_libraries(agent_gateway_foundation PRIVATE agent_media)
endif()

add_library(agent_gateway INTERFACE)

target_link_libraries(agent_gateway INTERFACE
    agent_gateway_foundation
    agent_persona_interaction
    agent_gateway_routing
)

if(AGENTLOOM_BUILD_REFERENCE_GATEWAY)
add_library(agent_gateway_server_lib STATIC
    src/service/gateway/auth_session_maintenance_task.cpp
    src/service/gateway/auth_session_maintenance_task.h
    src/service/gateway/classroom_scheduler.cpp
    src/service/gateway/classroom_scheduler.h
    src/service/gateway/gateway_models.h
    src/service/gateway/persona_gateway_http_adapter.cpp
    src/service/gateway/persona_gateway_http_adapter.h
    src/service/gateway/persona_gateway_route_core.h
    src/service/gateway/persona_gateway_route_helpers.h
    src/service/gateway/persona_gateway_service.cpp
    src/service/gateway/persona_gateway_service.h
    src/service/gateway/report_evaluator.h
    src/service/gateway/persona_gateway_server.cpp
    src/service/gateway/persona_gateway_server.h
)

target_include_directories(agent_gateway_server_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/document
    ${CMAKE_CURRENT_SOURCE_DIR}/src/document/ooxml
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/persona
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/gateway
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/src/llm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/semantic_cache
    ${CMAKE_CURRENT_SOURCE_DIR}/src/memory
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage
)

target_link_libraries(agent_gateway_server_lib PUBLIC
    agent_service_core
    agent_agent_runtime
    agent_persona_interaction
    agent_gateway_foundation
    agent_gateway_routing
    agent_gateway_auth
    agent_config
    agent_llm
    agent_semantic_cache
    agent_memory
    agent_storage
    agent_document
    agent_ipc
    agent_core
    agent_net
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    spdlog::spdlog
)

if(TARGET agent_media)
    target_compile_definitions(agent_gateway_server_lib PRIVATE AGENTLOOM_HAS_MEDIA=1)
    target_link_libraries(agent_gateway_server_lib PRIVATE agent_media)
endif()

add_library(agent_agent_gateway STATIC
    src/service/gateway/persona_gateway_agent_routes.cpp
    src/service/gateway/persona_gateway_route_core.h
    src/service/gateway/persona_gateway_route_helpers.h
)

target_include_directories(agent_agent_gateway PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/gateway
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/persona
    ${CMAKE_CURRENT_SOURCE_DIR}/src/semantic_cache
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage
)

target_link_libraries(agent_agent_gateway PUBLIC
    agent_gateway_server_lib
)

add_library(agent_classroom_gateway STATIC
    src/service/gateway/persona_gateway_classroom_routes.cpp
    src/service/gateway/persona_gateway_route_core.h
    src/service/gateway/persona_gateway_route_helpers.h
)

target_include_directories(agent_classroom_gateway PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/gateway
)

target_link_libraries(agent_classroom_gateway PUBLIC
    agent_agent_gateway
    agent_service_core
)

add_library(agent_training_report_gateway STATIC
    src/service/gateway/persona_gateway_teaching_routes.cpp
    src/service/gateway/persona_gateway_route_core.h
    src/service/gateway/persona_gateway_route_helpers.h
)

target_include_directories(agent_training_report_gateway PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/gateway
)

target_link_libraries(agent_training_report_gateway PUBLIC
    agent_agent_gateway
    agent_service_core
)

add_library(agent_document_gateway STATIC
    src/service/gateway/persona_gateway_document_routes.cpp
    src/service/gateway/persona_gateway_route_core.h
    src/service/gateway/persona_gateway_route_helpers.h
)

target_include_directories(agent_document_gateway PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/document
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/gateway
    ${CMAKE_CURRENT_SOURCE_DIR}/src/llm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/semantic_cache
)

target_link_libraries(agent_document_gateway PUBLIC
    agent_agent_gateway
    agent_document
    agent_llm
    agent_semantic_cache
    agent_service_core
)

endif()

add_library(agent_service INTERFACE)

target_link_libraries(agent_service INTERFACE
    agent_service_core
    agent_agent_runtime
    agent_gateway
)
if(AGENTLOOM_BUILD_REFERENCE_GATEWAY)
    target_link_libraries(agent_service INTERFACE
        agent_gateway_auth
        agent_gateway_server_lib
        agent_agent_gateway
        agent_classroom_gateway
        agent_training_report_gateway
        agent_document_gateway)
endif()

if(AGENTLOOM_BUILD_REFERENCE_GATEWAY)
foreach(_gateway_route_target IN ITEMS
        document_analysis_e2e_test
        persona_gateway_e2e_server
        agent_gateway_server
        webrtc_signaling_smoke_server)
    if(TARGET ${_gateway_route_target})
        link_whole_archive(${_gateway_route_target} agent_agent_gateway)
        link_whole_archive(${_gateway_route_target} agent_classroom_gateway)
        link_whole_archive(${_gateway_route_target} agent_training_report_gateway)
        link_whole_archive(${_gateway_route_target} agent_document_gateway)
    endif()
endforeach()
endif()

if(TARGET agent_media)
    add_library(agent_skill_media STATIC
        src/service/persona/media_inference_execution.cpp
        src/service/persona/media_inference_execution.h
        src/service/persona/skill_vision_event_sink.cpp
        src/service/persona/skill_vision_event_sink.h
    )

    target_include_directories(agent_skill_media PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/service/persona
    )

    target_link_libraries(agent_skill_media PUBLIC
        agent_agent_runtime
        agent_media
        agent_media_inference
    )

    if(BERT_BUILD_TESTS)
        add_executable(media_inference_execution_bench
            tests/service/media_inference_execution_bench.cpp
        )
        target_link_libraries(media_inference_execution_bench PRIVATE
            agent_skill_media
        )
    endif()
endif()

if(BERT_BUILD_TESTS)
    add_executable(gateway_session_affinity_bench
        tests/service/gateway_session_affinity_bench.cpp
    )
    target_link_libraries(gateway_session_affinity_bench PRIVATE
        agent_agent_runtime
    )

    add_executable(persona_llm_pool_bench
        tests/service/persona_llm_pool_bench.cpp
    )
    target_link_libraries(persona_llm_pool_bench PRIVATE
        agent_agent_runtime
    )

    function(agentloom_configure_service_test_target target labels)
        target_include_directories(${target} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tools)
        copy_runtime_files(${target} "${BERT_SQLITE_DLL}")
        copy_runtime_files(${target} ${BERT_FAISS_RUNTIME_FILES})
        copy_runtime_files(${target} ${VCPKG_RUNTIME_DLLS})
        gtest_discover_tests(${target}
            DISCOVERY_MODE PRE_TEST
            PROPERTIES LABELS "${labels}")
    endfunction()

    add_executable(persona_runtime_tests
        tests/service/persona_algorithm_test.cpp
        tests/service/session_persistence_contract_test.cpp
        tests/service/persona_runtime_test.cpp
        tests/service/semantic_memory_context_provider_test.cpp
        tests/service/emotion_fusion_analyzer_test.cpp
        tests/service/skill_registry_test.cpp
        tests/service/skill_prompt_compiler_test.cpp
    )

    target_link_libraries(persona_runtime_tests PRIVATE
        agent_agent_runtime
        agent_gateway_foundation
        GTest::gtest_main
    )
    agentloom_configure_service_test_target(
        persona_runtime_tests "service;persona;ci")

    if(AGENTLOOM_BUILD_REFERENCE_GATEWAY)
        add_executable(gateway_service_tests
            tests/service/session_manager_test.cpp
            tests/service/gateway_foundation_test.cpp
            tests/service/persona_gateway_service_test.cpp
        )
        target_link_libraries(gateway_service_tests PRIVATE
            agent_service
            GTest::gtest_main
        )
        link_whole_archive(gateway_service_tests agent_agent_gateway)
        link_whole_archive(gateway_service_tests agent_classroom_gateway)
        link_whole_archive(gateway_service_tests agent_training_report_gateway)
        link_whole_archive(gateway_service_tests agent_document_gateway)
        agentloom_configure_service_test_target(
            gateway_service_tests "service;gateway;ci")
    endif()

    if(TARGET agent_skill_media)
        add_executable(media_skill_tests
            tests/service/media_inference_execution_test.cpp
            tests/service/skill_vision_event_sink_test.cpp
        )
        target_link_libraries(media_skill_tests PRIVATE
            agent_skill_media
            GTest::gtest_main
        )
        # Skill 的静态注册对象可能没有其它符号引用，必须 whole-archive 保证注册发生。
        link_whole_archive(media_skill_tests agent_skill_media)
        agentloom_configure_service_test_target(
            media_skill_tests "service;media")
    endif()

    add_custom_target(agentloom_service_tests
        DEPENDS persona_runtime_tests)
    if(TARGET gateway_service_tests)
        add_dependencies(agentloom_service_tests gateway_service_tests)
    endif()
    if(TARGET media_skill_tests)
        add_dependencies(agentloom_service_tests media_skill_tests)
    endif()
endif()
