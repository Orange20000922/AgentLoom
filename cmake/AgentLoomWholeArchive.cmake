include_guard(GLOBAL)

function(agentloom_link_whole_archive target library)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "agentloom_link_whole_archive target does not exist: ${target}")
    endif()
    if(NOT TARGET "${library}")
        message(FATAL_ERROR "agentloom_link_whole_archive library does not exist: ${library}")
    endif()

    get_target_property(canonical_library "${library}" ALIASED_TARGET)
    if(NOT canonical_library)
        set(canonical_library "${library}")
    endif()
    get_property(whole_libraries TARGET "${target}" PROPERTY AGENTLOOM_WHOLE_ARCHIVE_LIBRARIES)
    if("${canonical_library}" IN_LIST whole_libraries)
        return()
    endif()
    set_property(TARGET "${target}" APPEND PROPERTY AGENTLOOM_WHOLE_ARCHIVE_LIBRARIES
        "${canonical_library}")

    # 普通依赖保留目标构建顺序与传递链接接口；整库选项只加一次，位于常规
    # archive 扫描之前。随后出现的普通依赖不会再次提取已定义对象。
    # CMake 3.28 的原生 WHOLE_ARCHIVE feature 仍可重复展开传递依赖，
    # 因此这里统一使用 link_options，兼容项目声明的 CMake 3.20 下限。
    target_link_libraries("${target}" PRIVATE "${canonical_library}")
    if(MSVC)
        target_link_options("${target}" PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:${canonical_library}>")
    elseif(APPLE)
        target_link_options("${target}" PRIVATE "LINKER:-force_load,$<TARGET_FILE:${canonical_library}>")
    else()
        target_link_options("${target}" PRIVATE
            "LINKER:--whole-archive,$<TARGET_FILE:${canonical_library}>,--no-whole-archive")
    endif()
endfunction()
