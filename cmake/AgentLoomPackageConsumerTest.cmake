cmake_minimum_required(VERSION 3.20)

foreach(_required_variable IN ITEMS
        AGENTLOOM_SOURCE_DIR
        AGENTLOOM_PACKAGE_BUILD_DIR
        AGENTLOOM_INSTALL_PREFIX
        AGENTLOOM_CONSUMER_BUILD_DIR)
    if(NOT DEFINED ${_required_variable} OR "${${_required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${_required_variable} must be provided")
    endif()
endforeach()

if(NOT DEFINED AGENTLOOM_PACKAGE_CONFIG OR AGENTLOOM_PACKAGE_CONFIG STREQUAL "")
    set(AGENTLOOM_PACKAGE_CONFIG Release)
endif()

function(_agentloom_run step)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE _result
        COMMAND_ECHO STDOUT)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "${step} failed with exit code ${_result}")
    endif()
endfunction()

# A package verification must not inherit files or CMake cache entries from a
# previous install/consumer run.
file(REMOVE_RECURSE
    "${AGENTLOOM_INSTALL_PREFIX}"
    "${AGENTLOOM_CONSUMER_BUILD_DIR}")

_agentloom_run("AgentLoom SDK build"
    "${CMAKE_COMMAND}" --build "${AGENTLOOM_PACKAGE_BUILD_DIR}"
    --config "${AGENTLOOM_PACKAGE_CONFIG}" --target agentloom_sdk --parallel)

_agentloom_run("AgentLoom SDK install"
    "${CMAKE_COMMAND}" --install "${AGENTLOOM_PACKAGE_BUILD_DIR}"
    --config "${AGENTLOOM_PACKAGE_CONFIG}"
    --prefix "${AGENTLOOM_INSTALL_PREFIX}")

file(GLOB_RECURSE _package_cmake_files
    "${AGENTLOOM_INSTALL_PREFIX}/*.cmake")
if(NOT _package_cmake_files)
    message(FATAL_ERROR "No installed CMake package files were found")
endif()

file(TO_CMAKE_PATH "${AGENTLOOM_SOURCE_DIR}" _source_path)
file(TO_CMAKE_PATH "${AGENTLOOM_PACKAGE_BUILD_DIR}" _build_path)
foreach(_package_file IN LISTS _package_cmake_files)
    file(READ "${_package_file}" _package_contents)
    string(REPLACE "\\" "/" _package_contents "${_package_contents}")
    foreach(_forbidden_path IN ITEMS "${_source_path}" "${_build_path}")
        string(FIND "${_package_contents}" "${_forbidden_path}" _path_offset)
        if(NOT _path_offset EQUAL -1)
            message(FATAL_ERROR
                "Installed package file ${_package_file} contains build-machine path ${_forbidden_path}")
        endif()
    endforeach()
endforeach()

set(_consumer_effective_triplet "${AGENTLOOM_CONSUMER_TRIPLET}")
if(DEFINED AGENTLOOM_CONSUMER_VCPKG_INSTALLED_DIR AND
        NOT AGENTLOOM_CONSUMER_VCPKG_INSTALLED_DIR STREQUAL "" AND
        DEFINED AGENTLOOM_CONSUMER_TRIPLET AND
        NOT AGENTLOOM_CONSUMER_TRIPLET STREQUAL "")
    set(_requested_triplet_dir
        "${AGENTLOOM_CONSUMER_VCPKG_INSTALLED_DIR}/${AGENTLOOM_CONSUMER_TRIPLET}")
    if(NOT IS_DIRECTORY "${_requested_triplet_dir}" AND
            AGENTLOOM_CONSUMER_TRIPLET MATCHES "-release$")
        string(REGEX REPLACE "-release$" "" _vcpkg_base_triplet
            "${AGENTLOOM_CONSUMER_TRIPLET}")
        if(IS_DIRECTORY
                "${AGENTLOOM_CONSUMER_VCPKG_INSTALLED_DIR}/${_vcpkg_base_triplet}")
            set(_consumer_effective_triplet "${_vcpkg_base_triplet}")
        endif()
    endif()
endif()

set(_consumer_configure_command
    "${CMAKE_COMMAND}"
    -S "${AGENTLOOM_SOURCE_DIR}/tests/package/consumer"
    -B "${AGENTLOOM_CONSUMER_BUILD_DIR}"
    "-DCMAKE_PREFIX_PATH=${AGENTLOOM_INSTALL_PREFIX}")
if(DEFINED AGENTLOOM_CONSUMER_GENERATOR AND NOT AGENTLOOM_CONSUMER_GENERATOR STREQUAL "")
    list(APPEND _consumer_configure_command -G "${AGENTLOOM_CONSUMER_GENERATOR}")
endif()
if(DEFINED AGENTLOOM_CONSUMER_ARCHITECTURE AND NOT AGENTLOOM_CONSUMER_ARCHITECTURE STREQUAL "")
    list(APPEND _consumer_configure_command -A "${AGENTLOOM_CONSUMER_ARCHITECTURE}")
endif()
if(DEFINED AGENTLOOM_CONSUMER_TOOLCHAIN_FILE AND NOT AGENTLOOM_CONSUMER_TOOLCHAIN_FILE STREQUAL "")
    list(APPEND _consumer_configure_command
        "-DCMAKE_TOOLCHAIN_FILE=${AGENTLOOM_CONSUMER_TOOLCHAIN_FILE}")
endif()
if(DEFINED _consumer_effective_triplet AND NOT _consumer_effective_triplet STREQUAL "")
    list(APPEND _consumer_configure_command
        "-DVCPKG_TARGET_TRIPLET=${_consumer_effective_triplet}")
endif()
if(DEFINED AGENTLOOM_CONSUMER_VCPKG_INSTALLED_DIR AND
        NOT AGENTLOOM_CONSUMER_VCPKG_INSTALLED_DIR STREQUAL "")
    list(APPEND _consumer_configure_command
        "-DVCPKG_INSTALLED_DIR=${AGENTLOOM_CONSUMER_VCPKG_INSTALLED_DIR}")
endif()
if(DEFINED AGENTLOOM_CONSUMER_BOOST_ROOT AND NOT AGENTLOOM_CONSUMER_BOOST_ROOT STREQUAL "")
    list(APPEND _consumer_configure_command
        "-DBOOST_ROOT=${AGENTLOOM_CONSUMER_BOOST_ROOT}")
endif()
if(DEFINED AGENTLOOM_CONSUMER_BUILD_TYPE AND NOT AGENTLOOM_CONSUMER_BUILD_TYPE STREQUAL "")
    list(APPEND _consumer_configure_command
        "-DCMAKE_BUILD_TYPE=${AGENTLOOM_CONSUMER_BUILD_TYPE}")
endif()

_agentloom_run("Package consumer configure" ${_consumer_configure_command})
_agentloom_run("Package consumer build"
    "${CMAKE_COMMAND}" --build "${AGENTLOOM_CONSUMER_BUILD_DIR}"
    --config "${AGENTLOOM_PACKAGE_CONFIG}" --parallel)
_agentloom_run("Package consumer test"
    "${CMAKE_CTEST_COMMAND}" --test-dir "${AGENTLOOM_CONSUMER_BUILD_DIR}"
    -C "${AGENTLOOM_PACKAGE_CONFIG}" --output-on-failure)

message(STATUS "AgentLoom installed package consumer E2E passed")
