
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

set(GLFW_DIR "${THIRD_PARTY_DIR}/glfw")
set(GLM_DIR  "${THIRD_PARTY_DIR}/glm")
set(IMGUI_DIR "${THIRD_PARTY_DIR}/imgui")
set(VKB_DIR  "${THIRD_PARTY_DIR}/vk-bootstrap")
set(VMA_DIR  "${THIRD_PARTY_DIR}/vma")

# ============================================================
# Vulkan SDK
# ============================================================

find_package(Vulkan REQUIRED)

message(STATUS "Vulkan SDK: ${Vulkan_VERSION}")

# ============================================================
# DirectX Shader Compiler (DXC)
# ============================================================

find_program(DXC_EXECUTABLE
    NAMES dxc dxc.exe
    HINTS
        "C:/VulkanSDK/1.4.357.0/Bin"
    REQUIRED
)

message(STATUS "DXC Compiler: ${DXC_EXECUTABLE}")

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

add_library(imgui STATIC
    ${IMGUI_SOURCES}
)

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
# Shader Directories
# ============================================================

set(SHADER_SOURCE_DIR
    "${CMAKE_SOURCE_DIR}/Shaders"
)

set(SHADER_BINARY_DIR
    "${CMAKE_BINARY_DIR}/Shaders"
)

# ============================================================
# Aether Engine
# ============================================================

include(FetchContent)
set(FASTGLTF_ENABLE_INSTALL OFF)
set(SIMDJSON_DEVELOPER_MODE OFF)
set(SIMDUTF_TESTS OFF)
set(SIMDUTF_TOOLS OFF)
FetchContent_Declare(simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG ec73615105bd405fc36ddcd1b4a6afb045aeffc1 # v5.1.0
)
FetchContent_Declare(simdutf
    GIT_REPOSITORY https://github.com/simdutf/simdutf.git
    GIT_TAG 59589cd3df6823cbfdbd253cde3377e7c3e1fe9b # v9.3.3
)
FetchContent_MakeAvailable(simdjson simdutf)
FetchContent_Declare(fastgltf
    GIT_REPOSITORY https://github.com/spnda/fastgltf.git
    GIT_TAG 87cfbceeeda127ccc9092694279437b700046119
)
FetchContent_MakeAvailable(fastgltf)

file(GLOB_RECURSE AETHER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Engine/*.cpp"
)

add_executable(Aether
    App/main.cpp
    ${AETHER_SOURCES}
)

target_include_directories(Aether PRIVATE
    "${CMAKE_SOURCE_DIR}/Engine"
    "${VKB_DIR}/src"
)

target_link_libraries(Aether PRIVATE
    fastgltf::fastgltf
    windowscodecs
    ole32
    Vulkan::Vulkan
    glfw
    glm::glm
    vk-bootstrap::vk-bootstrap
    imgui
    VMA::VMA
)

# ============================================================
# Shader Compilation
# ============================================================

function(add_aether_shader SOURCE_FILE PROFILE OUTPUT_FILE)
    set(SPIRV_TARGET vulkan1.3)
    if(ARGC GREATER 3)
        set(SPIRV_TARGET "${ARGV3}")
    endif()

    set(SOURCE_PATH
        "${SHADER_SOURCE_DIR}/${SOURCE_FILE}"
    )

    set(OUTPUT_PATH
        "${SHADER_BINARY_DIR}/${OUTPUT_FILE}"
    )

    add_custom_command(
        OUTPUT "${OUTPUT_PATH}"

        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${SHADER_BINARY_DIR}"

        COMMAND "${DXC_EXECUTABLE}"
            -spirv
            -fspv-target-env=${SPIRV_TARGET}
            -T "${PROFILE}"
            -E main
            "${SOURCE_PATH}"
            -Fo "${OUTPUT_PATH}"

        DEPENDS "${SOURCE_PATH}"

        COMMENT "Compiling Shader: ${SOURCE_FILE}"

        VERBATIM
    )

    set(AETHER_SHADER_OUTPUTS
        ${AETHER_SHADER_OUTPUTS}
        "${OUTPUT_PATH}"
        PARENT_SCOPE
    )

endfunction()

# Vertex Shader
add_aether_shader(
    "triangle.vert.hlsl"
    "vs_6_0"
    "triangle.vert.spv"
)

# Fragment Shader
add_aether_shader(
    "triangle.frag.hlsl"
    "ps_6_0"
    "triangle.frag.spv"
)

add_aether_shader("pbr.vert.hlsl" "vs_6_0" "pbr.vert.spv")
# Use OpKill for alpha masking without requiring shaderDemoteToHelperInvocation.
add_aether_shader("pbr.frag.hlsl" "ps_6_0" "pbr.frag.spv" vulkan1.1)

add_aether_shader("ibl.comp.hlsl" "cs_6_0" "ibl.comp.spv")

add_aether_shader("tonemap.vert.hlsl" "vs_6_0" "tonemap.vert.spv")
add_aether_shader("tonemap.frag.hlsl" "ps_6_0" "tonemap.frag.spv")
add_aether_shader("skybox.vert.hlsl" "vs_6_0" "skybox.vert.spv")
add_aether_shader("skybox.frag.hlsl" "ps_6_0" "skybox.frag.spv")
add_aether_shader("shadow.vert.hlsl" "vs_6_0" "shadow.vert.spv")
add_aether_shader("shadow_mask.vert.hlsl" "vs_6_0" "shadow_mask.vert.spv")
# Use OpKill for alpha masking without requiring shaderDemoteToHelperInvocation.
add_aether_shader("shadow_mask.frag.hlsl" "ps_6_0" "shadow_mask.frag.spv" vulkan1.1)
add_aether_shader("shadow_debug.vert.hlsl" "vs_6_0" "shadow_debug.vert.spv")
add_aether_shader("shadow_debug.frag.hlsl" "ps_6_0" "shadow_debug.frag.spv")
add_aether_shader("bloom_fullscreen.vert.hlsl" "vs_6_0" "bloom_fullscreen.vert.spv")
add_aether_shader("bloom_bright.frag.hlsl" "ps_6_0" "bloom_bright.frag.spv")
add_aether_shader("bloom_downsample.frag.hlsl" "ps_6_0" "bloom_downsample.frag.spv")
add_aether_shader("bloom_blur.frag.hlsl" "ps_6_0" "bloom_blur.frag.spv")
add_aether_shader("bloom_upsample.frag.hlsl" "ps_6_0" "bloom_upsample.frag.spv")
add_aether_shader("bloom_composite.frag.hlsl" "ps_6_0" "bloom_composite.frag.spv")
add_aether_shader("taa_velocity_sky.frag.hlsl" "ps_6_0" "taa_velocity_sky.frag.spv")
add_aether_shader("taa_velocity.vert.hlsl" "vs_6_0" "taa_velocity.vert.spv")
add_aether_shader("taa_velocity.frag.hlsl" "ps_6_0" "taa_velocity.frag.spv")
add_aether_shader("taa_velocity_mask.vert.hlsl" "vs_6_0" "taa_velocity_mask.vert.spv")
# Use OpKill for alpha masking without requiring shaderDemoteToHelperInvocation.
add_aether_shader("taa_velocity_mask.frag.hlsl" "ps_6_0" "taa_velocity_mask.frag.spv" vulkan1.1)
add_aether_shader("taa_resolve.frag.hlsl" "ps_6_0" "taa_resolve.frag.spv")
add_aether_shader("rayquery_debug.comp.hlsl" "cs_6_5" "rayquery_debug.comp.spv")
add_aether_shader("hybrid_gbuffer.vert.hlsl" "vs_6_0" "hybrid_gbuffer.vert.spv")
# Use OpKill for alpha masking without requiring shaderDemoteToHelperInvocation.
add_aether_shader("hybrid_gbuffer.frag.hlsl" "ps_6_0" "hybrid_gbuffer.frag.spv" vulkan1.1)
add_aether_shader("hybrid_gbuffer_color.frag.hlsl" "ps_6_0" "hybrid_gbuffer_color.frag.spv")
add_aether_shader("rt_shadow.comp.hlsl" "cs_6_5" "rt_shadow.comp.spv")
add_aether_shader("rt_reflection.comp.hlsl" "cs_6_5" "rt_reflection.comp.spv")
add_aether_shader("rt_denoise_temporal.comp.hlsl" "cs_6_0" "rt_denoise_temporal.comp.spv")
add_aether_shader("rt_denoise_atrous.comp.hlsl" "cs_6_0" "rt_denoise_atrous.comp.spv")
add_aether_shader("hybrid_composite.comp.hlsl" "cs_6_0" "hybrid_composite.comp.spv")

add_custom_target(AetherShaders ALL
    DEPENDS ${AETHER_SHADER_OUTPUTS}
)

add_dependencies(Aether AetherShaders)

# ============================================================
# Compile Definitions
# ============================================================

target_compile_definitions(Aether PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE

    AETHER_SHADER_DIR="${SHADER_BINARY_DIR}"
)

# ============================================================
# Compiler Options
# ============================================================

if(MSVC)
    target_compile_options(Aether PRIVATE
        /W4
        /utf-8
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

# ============================================================
# Configuration Summary
# ============================================================

message(STATUS "====================================")
message(STATUS "Aether Engine Configuration Complete")
message(STATUS "C++ Standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "Shader Source: ${SHADER_SOURCE_DIR}")
message(STATUS "Shader Output: ${SHADER_BINARY_DIR}")
message(STATUS "====================================")

enable_testing()
target_sources(Aether PRIVATE Tests/SwapchainRecovery.cpp Tests/PbrVisual.cpp Tests/ShadowVisual.cpp Tests/BloomVisual.cpp Tests/TaaVisual.cpp Tests/RayTracingAsTests.cpp Tests/RayTracingVisualTests.cpp Tests/HybridRayTracingTests.cpp)
foreach(scenario image-count color-format create-failure view-failure resize multi-mesh mesh-upload-failure asset-upload texture-upload environment-compute)
    add_test(NAME swapchain.${scenario}
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP=$<TARGET_FILE:Aether>"
            "-DSCENARIO=${scenario}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunSwapchainTest.cmake"
    )
    set_tests_properties(swapchain.${scenario} PROPERTIES
        RUN_SERIAL TRUE
        SKIP_REGULAR_EXPRESSION "Unsupported surface configuration"
        ENVIRONMENT "VK_LAYER_VALIDATE_SYNC=1"
    )
endforeach()

add_executable(AetherCameraTests Tests/CameraTests.cpp Engine/Scene/Camera.cpp
    Engine/Scene/OrbitCameraController.cpp)
target_include_directories(AetherCameraTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Engine")
target_include_directories(AetherCameraTests SYSTEM PRIVATE "${THIRD_PARTY_DIR}")
if(MSVC)
    target_compile_options(AetherCameraTests PRIVATE /utf-8)
endif()
add_test(NAME camera.matrices COMMAND AetherCameraTests)

add_executable(AetherMeshTests Tests/MeshTests.cpp Engine/Renderer/MeshData.cpp)
target_include_directories(AetherMeshTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Engine")
target_include_directories(AetherMeshTests SYSTEM PRIVATE "${THIRD_PARTY_DIR}")
add_test(NAME mesh.data COMMAND AetherMeshTests)

add_executable(AetherObjTests Tests/ObjImporterTests.cpp Engine/Assets/ObjImporter.cpp Engine/Renderer/MeshData.cpp)
target_include_directories(AetherObjTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Engine")
target_include_directories(AetherObjTests SYSTEM PRIVATE "${THIRD_PARTY_DIR}")
if(MSVC)
    target_compile_options(AetherObjTests PRIVATE /W4 /utf-8 /permissive- /Zc:preprocessor)
endif()
add_test(NAME asset.obj COMMAND AetherObjTests)

add_executable(AetherSceneTests Tests/SceneTests.cpp Engine/Scene/Transform.cpp)
target_include_directories(AetherSceneTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Engine")
target_include_directories(AetherSceneTests SYSTEM PRIVATE "${THIRD_PARTY_DIR}")
add_test(NAME scene.transforms COMMAND AetherSceneTests)

add_executable(AetherGltfTests Tests/GltfImporterTests.cpp Engine/Assets/GltfImporter.cpp Engine/Renderer/MeshData.cpp)
target_include_directories(AetherGltfTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Engine")
target_include_directories(AetherGltfTests SYSTEM PRIVATE "${THIRD_PARTY_DIR}")
target_link_libraries(AetherGltfTests PRIVATE fastgltf::fastgltf)
if(MSVC)
    target_compile_options(AetherGltfTests PRIVATE /utf-8)
endif()
add_test(NAME asset.gltf COMMAND AetherGltfTests)
set_tests_properties(asset.gltf PROPERTIES FIXTURES_SETUP gltf-assets)
set_tests_properties(swapchain.asset-upload swapchain.texture-upload PROPERTIES FIXTURES_REQUIRED gltf-assets)
if(WIN32)
    add_test(NAME asset.windows-drop
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP=$<TARGET_FILE:Aether>"
            "-DSCENARIO=asset-drop"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunSwapchainTest.cmake"
    )
        add_test(NAME asset.pbr-visual
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP=$<TARGET_FILE:Aether>"
            "-DSCENARIO=pbr-visual"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunSwapchainTest.cmake"
    )
    set_tests_properties(asset.windows-drop asset.pbr-visual PROPERTIES
        RUN_SERIAL TRUE
        FIXTURES_REQUIRED gltf-assets
        ENVIRONMENT "VK_LAYER_VALIDATE_SYNC=1"
    )
endif()

add_executable(AetherShadowTests Tests/ShadowTests.cpp Engine/Renderer/DirectionalShadow.cpp Engine/Scene/Camera.cpp)
target_include_directories(AetherShadowTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Engine")
target_include_directories(AetherShadowTests SYSTEM PRIVATE "${THIRD_PARTY_DIR}")
target_compile_definitions(AetherShadowTests PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE)
if(MSVC)
    target_compile_options(AetherShadowTests PRIVATE /W4 /utf-8 /permissive- /Zc:preprocessor)
endif()
add_test(NAME shadow.math COMMAND AetherShadowTests)

if(WIN32)
    add_test(NAME shadow.gpu
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP=$<TARGET_FILE:Aether>"
            "-DSCENARIO=shadow-gpu"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunSwapchainTest.cmake"
    )
    add_test(NAME shadow.visual
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP=$<TARGET_FILE:Aether>"
            "-DSCENARIO=shadow-visual"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunSwapchainTest.cmake"
    )
    set_tests_properties(shadow.gpu shadow.visual PROPERTIES
        RUN_SERIAL TRUE
        ENVIRONMENT "VK_LAYER_VALIDATE_SYNC=1"
    )
endif()

add_executable(AetherBloomTests Tests/BloomTests.cpp Engine/Renderer/Passes/BloomPass.cpp)
target_include_directories(AetherBloomTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Engine")
if(MSVC)
    target_compile_options(AetherBloomTests PRIVATE /W4 /utf-8 /permissive- /Zc:preprocessor)
endif()
add_test(NAME bloom.config COMMAND AetherBloomTests)

if(WIN32)
    add_test(NAME bloom.gpu
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP=$<TARGET_FILE:Aether>"
            "-DSCENARIO=bloom-gpu"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunSwapchainTest.cmake"
    )
    add_test(NAME bloom.visual
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP=$<TARGET_FILE:Aether>"
            "-DSCENARIO=bloom-visual"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunSwapchainTest.cmake"
    )
    set_tests_properties(bloom.gpu bloom.visual PROPERTIES
        RUN_SERIAL TRUE
        ENVIRONMENT "VK_LAYER_VALIDATE_SYNC=1"
    )
endif()

add_executable(AetherTaaTests Tests/TaaTests.cpp Engine/Renderer/Passes/TaaPass.cpp Engine/Scene/Camera.cpp)
target_include_directories(AetherTaaTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Engine")
target_include_directories(AetherTaaTests SYSTEM PRIVATE "${THIRD_PARTY_DIR}")
target_compile_definitions(AetherTaaTests PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE)
if(MSVC)
    target_compile_options(AetherTaaTests PRIVATE /W4 /utf-8 /permissive- /Zc:preprocessor)
endif()
add_test(NAME taa.math COMMAND AetherTaaTests)

if(WIN32)
    add_test(NAME taa.gpu
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP=$<TARGET_FILE:Aether>"
            "-DSCENARIO=taa-gpu"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunSwapchainTest.cmake"
    )
    add_test(NAME taa.visual
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP=$<TARGET_FILE:Aether>"
            "-DSCENARIO=taa-visual"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunSwapchainTest.cmake"
    )
    set_tests_properties(taa.gpu taa.visual PROPERTIES
        RUN_SERIAL TRUE
        ENVIRONMENT "VK_LAYER_VALIDATE_SYNC=1"
    )
endif()

add_executable(AetherRayTracingCapabilitiesTests Tests/RayTracingCapabilitiesTests.cpp)
target_include_directories(AetherRayTracingCapabilitiesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Engine")
target_link_libraries(AetherRayTracingCapabilitiesTests PRIVATE Vulkan::Vulkan)
if(MSVC)
    target_compile_options(AetherRayTracingCapabilitiesTests PRIVATE /W4 /utf-8 /permissive- /Zc:preprocessor)
endif()
add_test(NAME rt.capabilities COMMAND AetherRayTracingCapabilitiesTests)

add_executable(AetherRayTracingMathTests
    Tests/RayTracingMathTests.cpp
    Engine/Scene/Transform.cpp
    Engine/Scene/Camera.cpp
    Engine/Assets/CornellBoxScene.cpp
    Engine/Renderer/MeshData.cpp)
target_include_directories(AetherRayTracingMathTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Engine")
target_include_directories(AetherRayTracingMathTests SYSTEM PRIVATE "${THIRD_PARTY_DIR}")
target_compile_definitions(AetherRayTracingMathTests PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE)
target_link_libraries(AetherRayTracingMathTests PRIVATE Vulkan::Vulkan)
if(MSVC)
    target_compile_options(AetherRayTracingMathTests PRIVATE /W4 /utf-8 /permissive- /Zc:preprocessor)
endif()
add_test(NAME rt.math COMMAND AetherRayTracingMathTests)

if(WIN32)
    add_test(NAME rt.foundation
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP=$<TARGET_FILE:Aether>"
            "-DSCENARIO=rt-foundation"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunSwapchainTest.cmake"
    )
    set_tests_properties(rt.foundation PROPERTIES
        RUN_SERIAL TRUE
        ENVIRONMENT "VK_LAYER_VALIDATE_SYNC=1"
    )
    add_test(NAME rt.as
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP=$<TARGET_FILE:Aether>"
            "-DSCENARIO=rt-as"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunSwapchainTest.cmake"
    )
    set_tests_properties(rt.as PROPERTIES
        RUN_SERIAL TRUE
        FIXTURES_REQUIRED gltf-assets
        SKIP_REGULAR_EXPRESSION "Hardware Ray Tracing is not ready"
        ENVIRONMENT "VK_LAYER_VALIDATE_SYNC=1"
    )
    add_test(NAME rt.rayquery
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP=$<TARGET_FILE:Aether>"
            "-DSCENARIO=rt-rayquery"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunSwapchainTest.cmake"
    )
    add_test(NAME rt.visual
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP=$<TARGET_FILE:Aether>"
            "-DSCENARIO=rt-visual"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunSwapchainTest.cmake"
    )
    set_tests_properties(rt.rayquery rt.visual PROPERTIES
        RUN_SERIAL TRUE
        SKIP_REGULAR_EXPRESSION "Hardware Ray Tracing / Ray Query is not ready"
        ENVIRONMENT "VK_LAYER_VALIDATE_SYNC=1"
    )
    add_test(NAME rt.shadow
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP=$<TARGET_FILE:Aether>"
            "-DSCENARIO=rt-shadow"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunSwapchainTest.cmake"
    )
    add_test(NAME rt.reflection
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP=$<TARGET_FILE:Aether>"
            "-DSCENARIO=rt-reflection"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunSwapchainTest.cmake"
    )
    add_test(NAME rt.denoise
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP=$<TARGET_FILE:Aether>"
            "-DSCENARIO=rt-denoise"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunSwapchainTest.cmake"
    )
    add_test(NAME rt.hybrid
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP=$<TARGET_FILE:Aether>"
            "-DSCENARIO=rt-hybrid"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunSwapchainTest.cmake"
    )
    set_tests_properties(rt.shadow rt.reflection rt.denoise rt.hybrid PROPERTIES
        RUN_SERIAL TRUE
        SKIP_REGULAR_EXPRESSION "Hardware Ray Tracing / Hybrid RT is not ready"
        ENVIRONMENT "VK_LAYER_VALIDATE_SYNC=1"
    )
endif()

add_executable(AetherHdrTests Tests/HdrTests.cpp Engine/Assets/HdrImage.cpp)
target_include_directories(AetherHdrTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Engine" "${THIRD_PARTY_DIR}")
foreach(preset studio sunset)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Assets/Environments/${preset}.hdr"
        "${CMAKE_CURRENT_BINARY_DIR}/Environments/${preset}.hdr" COPYONLY)
endforeach()
foreach(target Aether AetherHdrTests)
    target_compile_definitions(${target} PRIVATE AETHER_ENVIRONMENT_DIR="${CMAKE_CURRENT_BINARY_DIR}/Environments")
endforeach()
add_test(NAME environment.radiance COMMAND AetherHdrTests)

# Localized MSVC /showIncludes output may use a different encoding from
# Ninja's dependency prefix. Track project headers explicitly for that build.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    file(GLOB_RECURSE AETHER_PROJECT_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Engine/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Engine/*.hpp"
    )
    foreach(target Aether AetherHdrTests AetherCameraTests AetherMeshTests AetherObjTests AetherShadowTests AetherBloomTests AetherTaaTests AetherRayTracingCapabilitiesTests AetherRayTracingMathTests AetherSceneTests AetherGltfTests)
        get_target_property(sources ${target} SOURCES)
        set_property(SOURCE ${sources} APPEND PROPERTY OBJECT_DEPENDS "${AETHER_PROJECT_HEADERS}")
    endforeach()
endif()
