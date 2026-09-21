cmake_minimum_required(VERSION 3.20)
project(dx12_textures_winapi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 1. Исходники (.cpp) — все из папки src
add_executable(dx12_textures
        src/main.cpp
        src/App.cpp
        src/Window.cpp
        src/Input.cpp
        src/D3D12Context.cpp
        src/GBuffer.cpp
        src/ShadowMap.cpp
        src/RenderingSystem.cpp
        src/Frustum.cpp
        src/Octree.cpp
)

# 2. Заголовки (.h) — ищем в папке include
target_include_directories(dx12_textures PRIVATE include)

# 3. Макросы Windows
target_compile_definitions(dx12_textures PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# 4. Это GUI приложение
set_target_properties(dx12_textures PROPERTIES WIN32_EXECUTABLE TRUE)

# 5. Библиотеки
target_link_libraries(dx12_textures PRIVATE
        user32
        gdi32
        dxgi
        d3d12
        d3dcompiler
)