#pragma once

#include <RHI/Vulkan/Resources/VulkanTexture.h>
#include <cstddef>
#include <functional>
#include <RHI/Vulkan/Assets/VulkanEnvironment.h>

#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>
#include <span>
#include <glm/mat4x4.hpp>

#include <RHI/Vulkan/Resources/VulkanImage.h>
#include <RHI/Vulkan/VulkanRayTracingCapabilities.h>
#include <RHI/Vulkan/RayTracing/VulkanRayTracingScene.h>
#include <RHI/Vulkan/Passes/VulkanHybridGBufferPass.h>
#include <RHI/Vulkan/Passes/VulkanRayQueryPass.h>
#include <RHI/Vulkan/Passes/VulkanRtDenoiserPass.h>
#include <RHI/Vulkan/Passes/VulkanRtReflectionPass.h>
#include <RHI/Vulkan/Passes/VulkanRtShadowPass.h>
#include <RHI/Vulkan/Pipeline/VulkanGraphicsPipeline.h>
#include <Renderer/DirectionalShadow.h>
#include <Renderer/RayTracingConfig.h>
#include <Renderer/RtLighting.h>
#include <Renderer/Passes/BloomPass.h>
#include <Renderer/Passes/ShadowPass.h>
#include <Renderer/Passes/TaaPass.h>
#include <RHI/Vulkan/Passes/VulkanBloomPass.h>
#include <RHI/Vulkan/Passes/VulkanTaaPass.h>

namespace Aether
{
    class VulkanContext;
    class VulkanMesh;
    class VulkanCommandManager;
    class VulkanSyncManager;

    class VulkanRenderer
    {
    public:
        VulkanRenderer() = default;
        ~VulkanRenderer();

        VulkanRenderer(const VulkanRenderer&) = delete;
        VulkanRenderer& operator=(const VulkanRenderer&) = delete;

        bool Initialize(
            VulkanContext& context,
            VulkanCommandManager& commandManager,
            VulkanSyncManager& syncManager,
            const VulkanMemoryManager& memoryManager
        );

        enum class FrameResult { Rendered, RecreateSwapchain, FatalError };

        struct PbrDrawConstants
        {
            glm::mat4 mvp;
            glm::vec4 modelRows[3];
            glm::vec4 eyeAndLightAngle;
        };
        static_assert(sizeof(PbrDrawConstants) == 128);

        struct DrawItem
        {
            const VulkanMesh* mesh;
            glm::mat4 mvp;
            bool mirrored = false;
            bool doubleSided = false;
            VkDescriptorSet material = VK_NULL_HANDLE;
            bool alphaBlend = false;
            PbrDrawConstants pbr{};
            glm::mat4 model{ 1.0f };
            bool alphaMask = false;
            glm::mat4 prevMvpJittered{ 0.0f };
            uint32_t sceneObjectIndex = 0;
            glm::mat4 prevModel{ 1.0f };
            bool hasPrevModel = false;
            RtMaterialSummary rtMaterial{};
            VkImageView alphaMaskView = VK_NULL_HANDLE;
            VkSampler alphaMaskSampler = VK_NULL_HANDLE;
            uint32_t rtInstanceIndex = UINT32_MAX;
        };

        // Meshes and the environment must outlive the submitted GPU work.
        FrameResult DrawFrame(std::span<const DrawItem> items, VkDescriptorSet environment,
                              const glm::mat4& clipToWorldDirection, float exposure,
                              const ShadowLightSpace& lightSpace = {},
                              const ShadowConfig& shadowConfig = {},
                              const BloomConfig& bloomConfig = {},
                              const TaaConfig& taaConfig = {},
                              const glm::mat4& currClipToPrevClipRot = glm::mat4(1.0f),
                              const glm::vec2& jitterDeltaUv = glm::vec2(0.0f),
                              bool updateRayTracingScene = false,
                              RenderMode renderMode = RenderMode::Rasterization,
                              const glm::vec3& cameraPosition = glm::vec3(0.0f),
                              const RayQueryConfig& rayQueryConfig = {},
                              const glm::mat4& unjitteredClipToWorldDirection = glm::mat4(1.0f),
                              const HybridRtConfig& hybridConfig = {},
                              std::span<const RtGpuLight> sceneLights = {},
                              const glm::mat4& prevUnjitteredViewProj = glm::mat4(1.0f),
                              float lightAngle = 0.6f);
        bool SetShadowResolution(uint32_t resolution);
        double GetLastShadowGpuTimeMs() const { return m_shadowPass.GetLastGpuTimeMs(); }
        uint32_t GetShadowResolution() const { return m_shadowPass.GetResolution(); }
        VkFormat GetShadowDepthFormat() const { return m_shadowPass.GetDepthFormat(); }
        double GetLastBloomGpuTimeMs() const { return m_bloomPass.GetLastGpuTimeMs(); }
        uint32_t GetBloomLevelCount() const { return m_bloomPass.GetLevelCount(); }
        void ResetTaaHistory() { m_taaPass.ResetHistory(); }
        bool IsTaaHistoryValid() const { return m_taaPass.IsHistoryValid(); }
        VkFormat GetVelocityFormat() const { return m_taaPass.GetVelocityFormat(); }
        double GetLastTaaVelocityGpuTimeMs() const { return m_taaPass.GetLastVelocityGpuTimeMs(); }
        double GetLastTaaResolveGpuTimeMs() const { return m_taaPass.GetLastResolveGpuTimeMs(); }
        double GetLastTaaTotalGpuTimeMs() const { return m_taaPass.GetLastTotalGpuTimeMs(); }
        const VulkanRayTracingCapabilities& GetRayTracingCapabilities() const;
        bool IsRayTracingReady() const;
        bool IsRayTracingSceneInitialized() const { return m_rtScene.IsInitialized(); }
        bool IsRayQueryPassInitialized() const { return m_rayQueryPass.IsInitialized(); }
        bool CanUseRayQueryMode() const
        {
            return IsRayTracingReady() && m_rtScene.IsInitialized() && m_rayQueryPass.IsInitialized();
        }
        bool CanUseHybridRtMode() const
        {
            return IsRayTracingReady() && m_rtScene.IsInitialized() &&
                   m_hybridGBufferPass.IsInitialized() && m_rtShadowPass.IsInitialized() &&
                   m_rtReflectionPass.IsInitialized() && m_rtDenoiserPass.IsInitialized();
        }
        void ResetRtDenoiserHistory() { m_rtDenoiserPass.ResetHistory(); }
        bool IsRtDenoiserHistoryValid() const { return m_rtDenoiserPass.IsHistoryValid(); }
        double GetLastRayQueryGpuTimeMs() const { return m_rayQueryPass.GetLastGpuTimeMs(); }
        double GetLastHybridGBufferGpuTimeMs() const { return m_hybridGBufferPass.GetLastGpuTimeMs(); }
        double GetLastRtShadowGpuTimeMs() const { return m_rtShadowPass.GetLastGpuTimeMs(); }
        double GetLastRtReflectionGpuTimeMs() const { return m_rtReflectionPass.GetLastGpuTimeMs(); }
        double GetLastRtTemporalDenoiseGpuTimeMs() const { return m_rtDenoiserPass.GetLastTemporalGpuTimeMs(); }
        double GetLastRtAtrousDenoiseGpuTimeMs() const { return m_rtDenoiserPass.GetLastAtrousGpuTimeMs(); }
        double GetLastRtCompositeGpuTimeMs() const { return m_rtDenoiserPass.GetLastCompositeGpuTimeMs(); }
        double GetLastRtDenoiserTotalGpuTimeMs() const { return m_rtDenoiserPass.GetLastTotalGpuTimeMs(); }
        double GetLastFrameGpuTimeMs() const { return m_lastFrameGpuTimeMs; }
        const VulkanRayTracingScene& GetRayTracingScene() const { return m_rtScene; }
        VkResult UploadTexture(VulkanTexture& texture, VkExtent2D extent, bool srgb,
                               std::span<const std::byte> rgba, const TextureSampling& sampling);
        VkResult UploadEnvironment(VulkanEnvironment& env,const HdrImage& src);
        VkResult PrecomputeEnvironment(VulkanEnvironment& env,std::vector<glm::vec4>* verification=nullptr);
        bool OnSwapchainRecreated();
        void Shutdown();

    private:
        friend class AetherApplication;
        bool CreateDepthAttachment(const VulkanContext& context, const VulkanMemoryManager& memory, VkExtent2D extent = {});
        bool CreateHdrTarget(const VulkanContext& context, const VulkanMemoryManager& memory, VkExtent2D extent = {});
        bool ResizeRenderTargetsForBenchmark(VkExtent2D extent);
        bool CreatePipeline(const VulkanContext& context);
        VkFormat m_pipelineColorFormat = VK_FORMAT_UNDEFINED;
        static constexpr uint32_t MVP_SIZE = 64;

        VkResult UploadTexture(VulkanTexture& texture, VkExtent2D extent, bool srgb,
                               std::span<const std::byte> rgba, const TextureSampling& sampling, bool failAfterCreate);
        VkResult EnvironmentCommands(const std::function<void(VkCommandBuffer)>& record);
        static void CopyEnvironmentMap(VkCommandBuffer cmd,const VulkanImage& image,VkBuffer buffer,uint32_t layers,VkDeviceSize offset);
        VkBuffer m_captureTarget = VK_NULL_HANDLE;
        VkBuffer m_rayQueryReadbackTarget = VK_NULL_HANDLE;
        VkBuffer m_rtShadowReadbackTarget = VK_NULL_HANDLE;
        VkBuffer m_rtReflectionReadbackTarget = VK_NULL_HANDLE;
        VkBuffer m_rtReflectionAuxReadbackTarget = VK_NULL_HANDLE;
        VkBuffer m_rtVarConfReadbackTarget = VK_NULL_HANDLE;
        VkBuffer m_rtCompositeReadbackTarget = VK_NULL_HANDLE;
        VulkanContext* m_context = nullptr;
        VulkanCommandManager* m_commandManager = nullptr;
        VulkanSyncManager* m_syncManager = nullptr;

        uint32_t m_currentFrame = 0;

        std::vector<VkImageLayout> m_imageLayouts;

        const VulkanMemoryManager* m_memoryManager = nullptr;
        VulkanImage m_hdrColor;
        VkDescriptorPool m_hdrPool{};
        VkDescriptorSetLayout m_hdrLayout{};
        VkDescriptorSet m_hdrSet{};
        VulkanGraphicsPipeline m_tonePipeline;
        VulkanGraphicsPipeline m_skyboxPipeline;
        void ReleaseHdr(VkDevice device);
        bool CreateTonePipeline(const VulkanContext& context);
        bool CreateSkyboxPipeline(const VulkanContext& context);
        void DrawSkybox(VkCommandBuffer cmd, VkDescriptorSet environment, const glm::mat4& clipToWorldDirection);
        void ToneMap(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent, float exposure, VkDescriptorSet hdrInputSet);
        VulkanImage m_depthAttachment;
        VkImageLayout m_depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool CreatePbrPipelines(const VulkanContext& context);
        VulkanGraphicsPipeline m_graphicsPipeline;
        VulkanGraphicsPipeline m_pbrOpaque;
        VulkanGraphicsPipeline m_pbrBlend;
        ShadowPass m_shadowPass;
        std::vector<ShadowCasterItem> m_shadowCasters;
        VulkanTaaPass m_taaPass;
        std::vector<VelocityDrawItem> m_velocityItems;
        VulkanBloomPass m_bloomPass;
        VulkanRayTracingScene m_rtScene;
        VulkanRayQueryPass m_rayQueryPass;
        VulkanHybridGBufferPass m_hybridGBufferPass;
        VulkanRtShadowPass m_rtShadowPass;
        VulkanRtReflectionPass m_rtReflectionPass;
        VulkanRtDenoiserPass m_rtDenoiserPass;
        std::vector<RtSceneItem> m_rtItems;
        std::vector<HybridGBufferDrawItem> m_gbufferItems;
        std::vector<uint32_t> m_itemRtInstanceIndices;
        uint32_t m_hybridFrameCounter = 0;

        VkQueryPool m_frameTimestampPool = VK_NULL_HANDLE;
        double m_frameTimestampPeriodNs = 0.0;
        std::vector<bool> m_frameHasPendingTimestamp;
        double m_lastFrameGpuTimeMs = 0.0;
    };
}
