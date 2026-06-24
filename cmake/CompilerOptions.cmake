# ============================================================
# CompilerOptions.cmake — 统一编译器选项（MSVC）
# ============================================================

# The Linux-hosted MSVC wrapper does not expose INCLUDE/LIB to cl.exe reliably
# after some toolchain updates, so add the toolchain include paths explicitly.
file(GLOB _PROJECT_MSVC_STL_HEADERS "/opt/msvc/VC/Tools/MSVC/*/include/yvals_core.h")
if(_PROJECT_MSVC_STL_HEADERS)
    list(SORT _PROJECT_MSVC_STL_HEADERS)
    list(GET _PROJECT_MSVC_STL_HEADERS -1 _PROJECT_MSVC_STL_HEADER)
    get_filename_component(PROJECT_MSVC_INCLUDE_DIR
        "${_PROJECT_MSVC_STL_HEADER}" DIRECTORY)
    get_filename_component(_PROJECT_MSVC_VERSION_DIR
        "${PROJECT_MSVC_INCLUDE_DIR}" DIRECTORY)
    set(PROJECT_MSVC_ATLMFC_INCLUDE_DIR
        "${_PROJECT_MSVC_VERSION_DIR}/atlmfc/include")
endif()

file(GLOB _PROJECT_MSVC_SDK_HEADERS "/opt/msvc/kits/10/Include/*/um/windows.h")
if(_PROJECT_MSVC_SDK_HEADERS)
    list(SORT _PROJECT_MSVC_SDK_HEADERS)
    list(GET _PROJECT_MSVC_SDK_HEADERS -1 _PROJECT_MSVC_SDK_HEADER)
    get_filename_component(_PROJECT_MSVC_SDK_UM_INCLUDE_DIR
        "${_PROJECT_MSVC_SDK_HEADER}" DIRECTORY)
    get_filename_component(PROJECT_MSVC_SDK_INCLUDE_DIR
        "${_PROJECT_MSVC_SDK_UM_INCLUDE_DIR}" DIRECTORY)
endif()

# 运行时库 — 对应 /MT (Release) 和 /MTd (Debug)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 全局 MSVC 编译选项
function(set_target_msvc_options TARGET)
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE
            /W3                          # Warning level 3
            /EHsc                        # Async exception handling (/EHa)
            /utf-8                       # UTF-8 source charset
        )

        # Debug builds: disable optimization, enable debug info
        target_compile_options(${TARGET} PRIVATE
            $<$<CONFIG:Debug>:/Od>
            $<$<CONFIG:Debug>:/RTC1>
        )

        # Release builds: optimize for speed
        target_compile_options(${TARGET} PRIVATE
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/GL>     # Whole Program Optimization
            $<$<NOT:$<CONFIG:Debug>>:/Gy>  # Function Level Linking
        )

        # Buffer security check: ON for Debug, OFF for Release
        target_compile_options(${TARGET} PRIVATE
            $<$<CONFIG:Debug>:/GS>
            $<$<NOT:$<CONFIG:Debug>>:/GS->
        )

        # Multi-processor compilation
        target_compile_options(${TARGET} PRIVATE /MP)

        if(PROJECT_MSVC_INCLUDE_DIR)
            target_include_directories(${TARGET} PRIVATE
                "${PROJECT_MSVC_INCLUDE_DIR}")
        endif()

        if(EXISTS "${PROJECT_MSVC_ATLMFC_INCLUDE_DIR}")
            target_include_directories(${TARGET} PRIVATE
                "${PROJECT_MSVC_ATLMFC_INCLUDE_DIR}")
        endif()

        if(PROJECT_MSVC_SDK_INCLUDE_DIR)
            target_include_directories(${TARGET} PRIVATE
                "${PROJECT_MSVC_SDK_INCLUDE_DIR}/shared"
                "${PROJECT_MSVC_SDK_INCLUDE_DIR}/ucrt"
                "${PROJECT_MSVC_SDK_INCLUDE_DIR}/um"
                "${PROJECT_MSVC_SDK_INCLUDE_DIR}/winrt"
                "${PROJECT_MSVC_SDK_INCLUDE_DIR}/km")
        endif()

        # Linker options
        target_link_options(${TARGET} PRIVATE
            /MANIFEST:NO
            $<$<CONFIG:Debug>:/DEBUG>
            $<$<NOT:$<CONFIG:Debug>>:/DEBUG /OPT:REF /OPT:ICF>
            $<$<CONFIG:Release>:/LTCG>   # Link Time Code Generation
            $<$<CONFIG:Release>:/INCREMENTAL:NO>
        )

        # 如果使用 Ninja，添加必要的链接器标志
        if(CMAKE_GENERATOR MATCHES "Ninja")
            target_link_options(${TARGET} PRIVATE /NOLOGO)
        endif()
    endif()
endfunction()

# 设置预处理器定义
function(set_target_common_definitions TARGET)
    target_compile_definitions(${TARGET} PRIVATE
        $<$<CONFIG:Debug>:_DEBUG>
        $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
        UNICODE
        _UNICODE
        _CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES=1
        _CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES_COUNT=1
        _WINDOWS
    )
endfunction()
