add_executable(static_site_test_server
    tests/net/static_site_server.cpp
)

target_link_libraries(static_site_test_server PRIVATE
    agent_net
)

add_executable(static_site_concurrency_bench
    tests/net/static_site_concurrency_bench.cpp
)

target_link_libraries(static_site_concurrency_bench PRIVATE
    agent_net
)

add_executable(net_stability_bench
    tests/net/net_stability_bench.cpp
)

target_link_libraries(net_stability_bench PRIVATE
    agent_net
)

add_executable(ws_download_server
    tests/net/ws_download_server.cpp
)

target_link_libraries(ws_download_server PRIVATE
    agent_net
)

add_executable(ws_download_client
    tests/net/ws_download_client.cpp
)

target_link_libraries(ws_download_client PRIVATE
    agent_net
)

if(TARGET agent_media)
    add_executable(webrtc_signaling_smoke_server
        tools/webrtc_signaling_smoke_server.cpp
    )

    target_link_libraries(webrtc_signaling_smoke_server PRIVATE
        agent_service
        agent_media
        agent_net
    )

    copy_runtime_files(webrtc_signaling_smoke_server ${BERT_OPENCV_RUNTIME_FILES})

    if(WIN32)
        set_target_properties(webrtc_signaling_smoke_server PROPERTIES
            VS_DEBUGGER_ENVIRONMENT "PATH=${GSTREAMER_ROOT}/bin;%PATH%;GST_PLUGIN_PATH=${GSTREAMER_ROOT}/lib/gstreamer-1.0"
        )
    endif()
endif()

add_custom_command(TARGET static_site_test_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/net/static_site"
        "$<TARGET_FILE_DIR:static_site_test_server>/static_site"
)

if(BERT_BUILD_TESTS)
    set(BERT_GTEST_ROOT "" CACHE PATH "Path to a prebuilt GTest package")
    if(BERT_GTEST_ROOT)
        list(PREPEND CMAKE_PREFIX_PATH "${BERT_GTEST_ROOT}")
    endif()

    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)

    add_test(NAME llama_abi_guard_release_match
        COMMAND "${CMAKE_COMMAND}"
            -DAGENT_CONFIG=Release
            -DLLAMA_CONFIG=RelWithDebInfo
            -DLLAMA_BUILD=test-llama-build
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AgentLoomLlamaAbiGuard.cmake")
    add_test(NAME llama_abi_guard_debug_mismatch
        COMMAND "${CMAKE_COMMAND}"
            -DAGENT_CONFIG=Debug
            -DLLAMA_CONFIG=Release
            -DLLAMA_BUILD=test-llama-build
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AgentLoomLlamaAbiGuard.cmake")
    set_tests_properties(llama_abi_guard_debug_mismatch PROPERTIES WILL_FAIL TRUE)

    add_executable(core_tests
        tests/core/core_infra_test.cpp
        tests/core/keyed_serial_executor_test.cpp
        tests/core/optimizer_test.cpp
        tests/core/ordered_bitmap_window_test.cpp
        tests/core/coroutine_awaitable_test.cpp
    )

    target_link_libraries(core_tests PRIVATE
        agent_core
        GTest::gtest_main
    )

    gtest_discover_tests(core_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS "ci;unit;core")

    add_executable(media_inference_tests
        tests/media/grpc_vlm_vision_client_test.cpp
        tests/media/inference_frame_backlog_test.cpp
        tests/media/inference_frame_coordinator_test.cpp
        tests/media/inference_frame_ipc_test.cpp
        tests/media/inference_frame_spool_test.cpp
        tests/media/inference_frame_spool_e2e_test.cpp
        tests/media/ordered_inference_frame_admission_test.cpp
    )

    target_link_libraries(media_inference_tests PRIVATE
        agent_media_inference
        agent_media_vlm_grpc
        GTest::gtest_main
    )

    target_include_directories(media_inference_tests PRIVATE
        ${GENERATED_DIR}
    )

    copy_runtime_files(media_inference_tests ${VCPKG_RUNTIME_DLLS})

    gtest_discover_tests(media_inference_tests DISCOVERY_MODE PRE_TEST)

    add_executable(media_inference_bench
        tests/media/inference_frame_backlog_bench.cpp
    )

    target_link_libraries(media_inference_bench PRIVATE
        agent_media_inference
    )

    add_executable(media_inference_ipc_bench
        tests/media/inference_frame_ipc_bench.cpp
    )

    target_link_libraries(media_inference_ipc_bench PRIVATE
        agent_media_inference
    )

    add_executable(media_inference_spool_bench
        tests/media/inference_frame_spool_bench.cpp
    )

    target_link_libraries(media_inference_spool_bench PRIVATE
        agent_media_inference
    )

    add_executable(net_tests
        tests/net/net_protocol_test.cpp
    )

    target_link_libraries(net_tests PRIVATE
        agent_net
        GTest::gtest_main
    )

    gtest_discover_tests(net_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS "ci;unit;net")

    if(TARGET agent_media)
        add_executable(media_tests
            tests/media/frame_encoding_test.cpp
            tests/media/opencv_frame_sampler_test.cpp
            tests/media/ordered_encoded_frame_sink_test.cpp
            tests/media/webrtc_media_pipeline_test.cpp
            tests/media/webrtc_signaling_handler_test.cpp
            tests/media/webrtc_bin_test.cpp
        )

        target_link_libraries(media_tests PRIVATE
            agent_media
            GTest::gtest_main
        )

        copy_runtime_files(media_tests ${BERT_OPENCV_RUNTIME_FILES})

        if(WIN32)
            set_target_properties(media_tests PROPERTIES
                VS_DEBUGGER_ENVIRONMENT "PATH=${GSTREAMER_ROOT}/bin;%PATH%;GST_PLUGIN_PATH=${GSTREAMER_ROOT}/lib/gstreamer-1.0"
            )
        endif()

        gtest_discover_tests(media_tests DISCOVERY_MODE PRE_TEST)

        add_executable(media_frame_encoding_bench
            tests/media/frame_encoding_bench.cpp
        )

        target_link_libraries(media_frame_encoding_bench PRIVATE
            agent_media
        )

        add_executable(media_rtc_ipc_fake_vlm_stress
            tests/media/rtc_ipc_fake_vlm_stress.cpp
        )

        target_link_libraries(media_rtc_ipc_fake_vlm_stress PRIVATE
            agent_media
            agent_media_inference
        )

        copy_runtime_files(media_rtc_ipc_fake_vlm_stress ${BERT_OPENCV_RUNTIME_FILES})

        if(WIN32)
            set_target_properties(media_rtc_ipc_fake_vlm_stress PROPERTIES
                VS_DEBUGGER_ENVIRONMENT "PATH=${GSTREAMER_ROOT}/bin;%PATH%;GST_PLUGIN_PATH=${GSTREAMER_ROOT}/lib/gstreamer-1.0"
            )
        endif()

        add_executable(media_real_video_e2e_bench
            tests/media/real_video_e2e_bench.cpp
        )

        target_link_libraries(media_real_video_e2e_bench PRIVATE
            agent_skill_media
        )

        copy_runtime_files(media_real_video_e2e_bench ${BERT_OPENCV_RUNTIME_FILES})

        if(WIN32)
            set_target_properties(media_real_video_e2e_bench PROPERTIES
                VS_DEBUGGER_ENVIRONMENT "PATH=${GSTREAMER_ROOT}/bin;%PATH%;GST_PLUGIN_PATH=${GSTREAMER_ROOT}/lib/gstreamer-1.0"
            )
        endif()

        add_executable(media_real_video_shared_vlm_e2e
            tools/real_video_shared_vlm_e2e.cpp
        )

        target_link_libraries(media_real_video_shared_vlm_e2e PRIVATE
            agent_media
            agent_ipc_grpc
        )

        copy_runtime_files(media_real_video_shared_vlm_e2e ${BERT_OPENCV_RUNTIME_FILES})
        copy_runtime_files(media_real_video_shared_vlm_e2e ${VCPKG_RUNTIME_DLLS})

        if(WIN32)
            set_target_properties(media_real_video_shared_vlm_e2e PROPERTIES
                VS_DEBUGGER_ENVIRONMENT "PATH=${GSTREAMER_ROOT}/bin;%PATH%;GST_PLUGIN_PATH=${GSTREAMER_ROOT}/lib/gstreamer-1.0"
            )
        endif()

        add_executable(media_ipc_e2e_peer
            tests/media/inference_frame_ipc_e2e_peer.cpp
        )

        target_link_libraries(media_ipc_e2e_peer PRIVATE
            agent_media_inference
        )

        add_executable(media_ipc_process_e2e_tests
            tests/media/inference_frame_ipc_process_e2e_test.cpp
        )

        target_link_libraries(media_ipc_process_e2e_tests PRIVATE
            agent_media
            GTest::gtest_main
        )

        target_compile_definitions(media_ipc_process_e2e_tests PRIVATE
            BOOST_PROCESS_USE_STD_FS
            MEDIA_IPC_E2E_PEER_PATH="$<TARGET_FILE:media_ipc_e2e_peer>"
        )

        add_dependencies(media_ipc_process_e2e_tests media_ipc_e2e_peer)
        gtest_discover_tests(media_ipc_process_e2e_tests DISCOVERY_MODE PRE_TEST)
    endif()

    add_executable(storage_tests
        tests/storage/sqlite_storage_test.cpp
        tests/storage/sqlite_migration_test.cpp
    )

    target_link_libraries(storage_tests PRIVATE
        agent_storage
        GTest::gtest_main
    )

    copy_runtime_files(storage_tests "${BERT_SQLITE_DLL}")

    gtest_discover_tests(storage_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS "ci;unit;storage")

    add_executable(vector_storage_tests
        tests/storage/vector/vector_storage_test.cpp
    )

    target_link_libraries(vector_storage_tests PRIVATE
        agent_vector_storage
        GTest::gtest_main
    )

    copy_runtime_files(vector_storage_tests "${BERT_SQLITE_DLL}")

    gtest_discover_tests(vector_storage_tests DISCOVERY_MODE PRE_TEST)

endif()
