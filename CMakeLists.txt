# ============================================================
# 3DMigotoDX12 — CMake 构建系统
# ============================================================
cmake_minimum_required(VERSION 3.20)

# The Wine/MSVC wrapper sends /showIncludes output to stderr. CMake's Makefile
# dependency wrapper only filters stdout, so GNU make later sees lines like
# "Note: including file:" as malformed dependency targets.
if(CMAKE_GENERATOR MATCHES "Makefiles" AND
   CMAKE_TOOLCHAIN_FILE MATCHES "/opt/msvc/cmake/toolchain")
    set(CMAKE_DEPENDS_USE_COMPILER FALSE CACHE BOOL
        "Disable compiler dependency scanning for Wine/MSVC Makefiles" FORCE)
endif()

if(CMAKE_TOOLCHAIN_FILE MATCHES "/opt/msvc/cmake/toolchain")
    file(GLOB _PROJECT_MSVC_CRT_LIBS "/opt/msvc/VC/Tools/MSVC/*/lib/x64/LIBCMT.lib")
    file(GLOB _PROJECT_MSVC_UCRT_LIBS "/opt/msvc/kits/10/Lib/*/ucrt/x64/libucrt.lib")
    file(GLOB _PROJECT_MSVC_UM_LIBS "/opt/msvc/kits/10/Lib/*/um/x64/kernel32.lib")
    foreach(_PROJECT_MSVC_LIBS
            _PROJECT_MSVC_CRT_LIBS
            _PROJECT_MSVC_UCRT_LIBS
            _PROJECT_MSVC_UM_LIBS)
        if(${_PROJECT_MSVC_LIBS})
            list(SORT ${_PROJECT_MSVC_LIBS})
            list(GET ${_PROJECT_MSVC_LIBS} -1 _PROJECT_MSVC_LIB)
            get_filename_component(_PROJECT_MSVC_LIB_DIR "${_PROJECT_MSVC_LIB}" DIRECTORY)
            list(APPEND _PROJECT_MSVC_LINK_DIRS "${_PROJECT_MSVC_LIB_DIR}")
        endif()
    endforeach()
    if(_PROJECT_MSVC_LINK_DIRS)
        foreach(_PROJECT_MSVC_LINK_DIR IN LISTS _PROJECT_MSVC_LINK_DIRS)
            set(_PROJECT_MSVC_LINK_FLAG " /LIBPATH:${_PROJECT_MSVC_LINK_DIR}")
            foreach(_PROJECT_MSVC_LINKER_FLAGS
                    CMAKE_EXE_LINKER_FLAGS
                    CMAKE_SHARED_LINKER_FLAGS
                    CMAKE_MODULE_LINKER_FLAGS)
                string(FIND "${${_PROJECT_MSVC_LINKER_FLAGS}}"
                    "${_PROJECT_MSVC_LINK_FLAG}" _PROJECT_MSVC_LINK_FLAG_INDEX)
                if(_PROJECT_MSVC_LINK_FLAG_INDEX EQUAL -1)
                    set(${_PROJECT_MSVC_LINKER_FLAGS}
                        "${${_PROJECT_MSVC_LINKER_FLAGS}}${_PROJECT_MSVC_LINK_FLAG}"
                        CACHE STRING "${_PROJECT_MSVC_LINKER_FLAGS}" FORCE)
                endif()
            endforeach()
        endforeach()
    endif()
endif()

project(3DMigotoDX12
    VERSION 1.3.16
    DESCRIPTION "3Dmigoto - DirectX API Hooking and Shader Manipulation Tool"
    LANGUAGES C CXX
)

# ---- C++ / C 标准 ----
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---- 输出目录 ----
# 保持与旧 MSBuild 一致的输出路径
foreach(CONF Debug Release)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONF}
        ${PROJECT_SOURCE_DIR}/x64/${CONF})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONF}
        ${PROJECT_SOURCE_DIR}/x64/${CONF})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONF}
        ${PROJECT_SOURCE_DIR}/x64/${CONF})
endforeach()

# ---- 包含 cmake 模块路径 ----
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ---- 加载编译选项模块 ----
include(CompilerOptions)

# ---- 查找 Nektra 预编译库 ----
find_package(Nektra)

# ---- 第三方依赖 ----
add_subdirectory(3rdparty/crc32c-hw-1.0.5)
add_subdirectory(3rdparty/DirectXTK)

# ---- 子项目 ----
add_subdirectory(src/Core)
add_subdirectory(src/BinaryDecompiler)
add_subdirectory(src/D3D_Shaders)
add_subdirectory(src/D3DCompiler_46)
add_subdirectory(src/DirectXGI)
add_subdirectory(src/DirectX11)
add_subdirectory(src/DirectX12)
add_subdirectory(src/Injector)
add_subdirectory(src/InjectorLib)
