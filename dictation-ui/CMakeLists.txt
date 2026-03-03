cmake_minimum_required(VERSION 3.20)

project(Murmur VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Dependencies via vcpkg ---
find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)

# --- Executable ---
add_executable(${PROJECT_NAME}
    src/main.cpp
    src/app.cpp
    src/engine_client.cpp
    src/engine_process.cpp
    src/dx11_helpers.cpp
    src/murmur.rc
)

# --- Link libraries ---
target_link_libraries(${PROJECT_NAME} PRIVATE
    imgui::imgui
    nlohmann_json::nlohmann_json
    httplib::httplib
    d3d11.lib
    dxgi.lib
    d3dcompiler.lib
)

target_include_directories(${PROJECT_NAME} PRIVATE src)

# Hide console window, keep standard main()
target_link_options(${PROJECT_NAME} PRIVATE "/SUBSYSTEM:WINDOWS" "/ENTRY:mainCRTStartup")
