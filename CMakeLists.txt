cmake_minimum_required(VERSION 3.24)

# ============================================================
# Aether Engine
# ============================================================

project(Aether
    VERSION 0.1.0
    DESCRIPTION "A Vulkan-based Real-Time Rendering Engine"
    LANGUAGES CXX
)

# ============================================================
# C++ Standard
# ============================================================

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================
# Third Party Paths
# ============================================================

set(THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party")

set(GLFW_DIR       "${THIRD_PARTY_DIR}/glfw")
set(GLM_DIR        "${THIRD_PARTY_DIR}/glm")
set(IMGUI_DIR      "${THIRD_PARTY_DIR}/imgui")
set(VKB_DIR        "${THIRD_PARTY_DIR}/vk-bootstrap")
set(VMA_DIR        "${THIRD_PARTY_DIR}/vma")

# ============================================================
# Vulkan SDK
# ============================================================

find_package(Vulkan REQUIRED)

message(STATUS "Vulkan SDK: ${Vulkan_VERSION}")

# ============================================================
# GLFW
# ============================================================

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

add_subdirectory(
    "${GLFW_DIR}"
    "${CMAKE_BINARY_DIR}/third_party/glfw"
    EXCLUDE_FROM_ALL
)

# ============================================================
# GLM
# ============================================================

set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)

add_subdirectory(
    "${GLM_DIR}"
    "${CMAKE_BINARY_DIR}/third_party/glm"
    EXCLUDE_FROM_ALL
)

# ============================================================
# vk-bootstrap
# ============================================================

set(VK_BOOTSTRAP_TEST OFF CACHE BOOL "" FORCE)

add_subdirectory(
    "${VKB_DIR}"
    "${CMAKE_BINARY_DIR}/third_party/vk-bootstrap"
    EXCLUDE_FROM_ALL
)

# ============================================================
# Dear ImGui
# ============================================================

set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"

    # GLFW Platform Backend
    "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"

    # Vulkan Renderer Backend
    "${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp"
)

add_library(imgui STATIC ${IMGUI_SOURCES})

target_include_directories(imgui PUBLIC
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)

target_link_libraries(imgui PUBLIC
    glfw
    Vulkan::Vulkan
)

# ============================================================
# Vulkan Memory Allocator (VMA)
# ============================================================

add_library(VMA INTERFACE)

add_library(VMA::VMA ALIAS VMA)

target_include_directories(VMA INTERFACE
    "${VMA_DIR}/include"
)

# ============================================================
# Aether Engine
# ============================================================

file(GLOB_RECURSE AETHER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Engine/*.cpp"
)

add_executable(Aether
    ${AETHER_SOURCES}
)

target_include_directories(Aether PRIVATE
    "${CMAKE_SOURCE_DIR}/Engine"
)

target_link_libraries(Aether PRIVATE
    Vulkan::Vulkan
    glfw
    glm::glm
    vk-bootstrap::vk-bootstrap
    imgui
    VMA::VMA
)

# ============================================================
# Compile Definitions
# ============================================================

target_compile_definitions(Aether PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)

# ============================================================
# Compiler Options
# ============================================================

if(MSVC)
    target_compile_options(Aether PRIVATE
        /W4
        /permissive-
        /Zc:preprocessor
    )
endif()

# ============================================================
# Output
# ============================================================

set_target_properties(Aether PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

message(STATUS "====================================")
message(STATUS "Aether Engine Configuration Complete")
message(STATUS "C++ Standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "====================================")