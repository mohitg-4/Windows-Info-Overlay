cmake_minimum_required(VERSION 3.10)
project(WindowsInfoOverlay)

set(CMAKE_CXX_STANDARD 17)

# Collect ImGui source files
file(GLOB IMGUI_SOURCES "imgui/*.cpp")
file(GLOB IMGUI_HEADERS "imgui/*.h")

add_executable(WindowsInfoOverlay WIN32
    main.cpp
    Overlay.cpp
    Overlay.h
    ${IMGUI_SOURCES}
    ${IMGUI_HEADERS}
)

target_include_directories(WindowsInfoOverlay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui
)

target_link_libraries(WindowsInfoOverlay PRIVATE
    d3d11
    dwmapi
    d3dcompiler
    iphlpapi
    pdh
    wbemuuid
    oleaut32
    ole32
)