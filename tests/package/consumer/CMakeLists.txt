cmake_minimum_required(VERSION 3.20)
project(AgentLoomPackageConsumer LANGUAGES CXX)

find_package(AgentLoom CONFIG REQUIRED)

add_library(agentloom_package_consumer_section OBJECT consumer_config_section.cpp)
target_compile_features(agentloom_package_consumer_section PRIVATE cxx_std_20)
target_link_libraries(agentloom_package_consumer_section PRIVATE AgentLoom::config)

add_executable(agentloom_package_consumer
    main.cpp
    $<TARGET_OBJECTS:agentloom_package_consumer_section>)
target_compile_features(agentloom_package_consumer PRIVATE cxx_std_20)
target_link_libraries(agentloom_package_consumer PRIVATE
    AgentLoom::core
    AgentLoom::bert_proto
    AgentLoom::grpc_runtime
    AgentLoom::gateway
    AgentLoom::gateway_foundation
    AgentLoom::gateway_routing
    AgentLoom::persona_interaction)

agentloom_link_whole_archive(agentloom_package_consumer AgentLoom::config)

enable_testing()
add_test(NAME agentloom_package_consumer COMMAND agentloom_package_consumer)
if(WIN32)
    get_filename_component(_agentloom_package_bin
        "${AgentLoom_DIR}/../../../bin" ABSOLUTE)
    string(REPLACE ";" "\\;" _agentloom_test_path "$ENV{PATH}")
    set_tests_properties(agentloom_package_consumer PROPERTIES
        ENVIRONMENT "PATH=${_agentloom_package_bin}\\;${_agentloom_test_path}")
endif()
