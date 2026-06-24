# ============================================================
# DirectX12 - d3d12.dll proxy and minimal DXGI/Present hook
# ============================================================
add_library(DirectX12 SHARED
    D3D12Wrapper.cpp
    DX12BindingTracker.cpp
    DX12CommandListHooks.cpp
    DX12DeviceHooks.cpp
    DX12FrameAnalysis.cpp
    DX12FrameAnalysisManifest.cpp
    DX12HookManager.cpp
    DX12Input.cpp
    DX12Json.cpp
    DX12ModRuntime.cpp
    DX12Overlay.cpp
    DX12ResourceDump.cpp
    DX12ResourceTracker.cpp
    DX12Runtime.cpp
    DX12ShaderDump.cpp
    DX12ShaderHunt.cpp
    DX12State.cpp
    DXGIHooks.cpp
)

target_include_directories(DirectX12 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/3rdparty
    ${CMAKE_SOURCE_DIR}/3rdparty/crc32c-hw-1.0.5/include
)

target_link_libraries(DirectX12 PRIVATE
    BunnyCore
    Nektra::NktHookLib
    crc32c
    dxgi.lib
    User32.lib
    Gdi32.lib
    Shlwapi.lib
)

target_sources(DirectX12 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/d3d12Wrapper.def
)

set_target_properties(DirectX12 PROPERTIES
    OUTPUT_NAME "d3d12"
)

target_compile_definitions(DirectX12 PRIVATE
    UNICODE
    _UNICODE
    _USRDLL
    MIGOTO_DX=12
)

file(GLOB _D3D12_HEADERS "/opt/msvc/kits/10/Include/*/um/d3d12.h")
if(_D3D12_HEADERS)
    list(SORT _D3D12_HEADERS)
    list(GET _D3D12_HEADERS -1 _D3D12_HEADER)
    file(READ "${_D3D12_HEADER}" _D3D12_HEADER_CONTENT)
    if(_D3D12_HEADER_CONTENT MATCHES "D3D12_DEPTH_STENCIL_DESC2" AND
       _D3D12_HEADER_CONTENT MATCHES "D3D12_RASTERIZER_DESC1" AND
       _D3D12_HEADER_CONTENT MATCHES "D3D12_RASTERIZER_DESC2")
        target_compile_definitions(DirectX12 PRIVATE
            PROJECT_HAS_D3D12_PIPELINE_STREAM_DESC2=1)
    endif()
endif()

set_target_msvc_options(DirectX12)
set_target_common_definitions(DirectX12)
