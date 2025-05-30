cmake_minimum_required(VERSION 3.10)
project(WindowsInfoOverlay)

set(CMAKE_CXX_STANDARD 17)

# Add source files
set(SOURCES 
    main.cpp 
    Overlay.cpp
    AudioManager.cpp  # Add these new files
    NetworkManager.cpp
    imgui/imgui.cpp 
    imgui/imgui_demo.cpp 
    imgui/imgui_draw.cpp 
    imgui/imgui_impl_dx11.cpp 
    imgui/imgui_impl_win32.cpp 
    imgui/imgui_tables.cpp 
    imgui/imgui_widgets.cpp
)

# Add include directories
include_directories(imgui)

# Add executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Add Windows libraries
target_link_libraries(${PROJECT_NAME} 
    d3d11 
    dwmapi 
    d3dcompiler 
    iphlpapi 
    pdh 
    wbemuuid 
    oleaut32 
    ole32
    wlanapi
    winmm
)

# Set Windows subsystem
set_target_properties(${PROJECT_NAME} PROPERTIES 
    WIN32_EXECUTABLE TRUE
)