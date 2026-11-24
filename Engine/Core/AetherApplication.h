#pragma once

#include <cstdint>
#include <string_view>
#include <memory>
#include <vector>

#include <Platform/Window.h>
#include <Assets/GltfImporter.h>
#include <RHI/Vulkan/Assets/VulkanMaterialSet.h>

#include <RHI/Vulkan/VulkanContext.h>
#include <RHI/Vulkan/VulkanCommandManager.h>
#include <RHI/Vulkan/VulkanSyncManager.h>
#include <RHI/Vulkan/VulkanRenderer.h>
#include <RHI/Vulkan/VulkanMemoryManager.h>
#include <RHI/Vulkan/Resources/VulkanMesh.h>
#include <Scene/Camera.h>
#include <Scene/OrbitCameraController.h>
#include <Scene/Scene.h>

namespace Aether {

    class AetherApplication
    {
    public:
        AetherApplication() = default;
        ~AetherApplication();

        AetherApplication(const AetherApplication&) = delete;
        AetherApplication& operator=(const AetherApplication&) = delete;

        int Run(bool singleCube = false,bool materialStudy=false,const std::filesystem::path& sample={});
        int RunSwapchainTest(std::string_view scenario);

    private:
        static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

        int RunPbrVisualTest(bool helmet);
        int RunShadowGpuTest();
        int RunShadowVisualTest();
        int RunBloomGpuTest();
        int RunBloomVisualTest();
        int RunTaaGpuTest();
        int RunTaaVisualTest();
        int RunRayTracingFoundationTest();
        int RunRayTracingAsTest();
        int RunRayQueryTest();
        int RunRayQueryVisualTest();
        int RunRtShadowTest();
        int RunRtReflectionTest();
        int RunRtDenoiserTest();
        int RunHybridRtTest();
        bool Initialize(bool singleCube);
        bool CreateScene(bool singleCube);
        bool LoadDefaultCornellBoxScene();
        bool ToggleRenderModeInteractive();
        std::string AddImportedAsset(ImportedAsset&& asset, std::optional<size_t> failPrimitive = {});
        VulkanRenderer::FrameResult DrawScene();
        bool RecreateSwapchain();
        bool MainLoop();
        void UpdateCameraAspect();
        void UpdateCameraInput();
        void ProcessDroppedAssets();
        void FrameImportedAsset(const glm::vec3& minimum, const glm::vec3& maximum);
        void Shutdown();

    private:
        Window m_window;

        VulkanContext m_vulkanContext;
        VulkanMemoryManager m_memoryManager;
        VulkanCommandManager m_commandManager;
        VulkanSyncManager m_syncManager;
        VulkanRenderer m_renderer;
        Camera m_camera;
        OrbitCameraController m_cameraController;
        bool SelectStudioEnvironment(bool warm);
        bool SwitchEnvironment(const HdrImage& image,bool injectFailure=false);
        std::unique_ptr<VulkanEnvironment> m_environment;
        bool m_warmEnvironment=false;
        bool m_environmentKeyDown=false;
        float m_exposure=0;
        float m_lightAngle = 0.6f;
        ShadowConfig m_shadowConfig;
        bool m_shadowToggleKeyDown = false;
        bool m_shadowDebugKeyDown = false;
        BloomConfig m_bloomConfig;
        bool m_bloomToggleKeyDown = false;
        bool m_bloomDebugKeyDown = false;
        TaaConfig m_taaConfig;
        bool m_taaToggleKeyDown = false;
        bool m_taaResetKeyDown = false;
        uint32_t m_taaFrameIndex = 0;
        glm::mat4 m_prevViewMatrix{ 1.0f };
        glm::mat4 m_prevUnjitteredProj{ 1.0f };
        glm::vec2 m_prevJitterNdc{ 0.0f };
        glm::vec3 m_prevEye{ 0.0f };
        float m_prevFocusDistance = 0.0f;
        uint32_t m_prevJitterSequenceLength = 0;
        bool m_hasPrevCamera = false;
        void ResetTemporalHistory();
        std::optional<glm::vec3> m_sceneMin;
        std::optional<glm::vec3> m_sceneMax;
        double m_cursorX = 0.0;
        double m_cursorY = 0.0;
        bool m_orbitDragging = false;
        bool m_panDragging = false;

        std::vector<std::unique_ptr<VulkanMesh>> m_meshes;
        Scene m_scene;
        std::optional<ImportedAsset> m_previewAsset;
        std::unique_ptr<VulkanMaterialSet> m_previewMaterials;
        std::vector<VulkanRenderer::DrawItem> m_drawItems;
        bool m_updateRayTracingScene = false;
        RenderMode m_renderMode = RenderMode::Rasterization;
        RayQueryConfig m_rayQueryConfig{};
        HybridRtConfig m_hybridConfig{};
        bool m_isCornellBoxScene = false;
        std::vector<RtGpuLight> m_customSceneLights;
        bool m_renderModeKeyDown = false;
        bool m_rayQueryDebugKeyDown = false;

        bool m_windowInitialized = false;
        bool m_contextInitialized = false;
        bool m_memoryManagerInitialized = false;
        bool m_commandManagerInitialized = false;
        bool m_syncManagerInitialized = false;
        bool m_rendererInitialized = false;
    };

}
