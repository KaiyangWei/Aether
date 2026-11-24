#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <Core/AetherApplication.h>
#include <Renderer/MeshData.h>
#include <Assets/CornellBoxScene.h>
#include <Assets/MaterialStudy.h>
#include <RHI/Vulkan/Assets/VulkanAssetUploader.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <cstdint>

namespace Aether {

	AetherApplication::~AetherApplication()
	{
		Shutdown();
	}

	int AetherApplication::Run(bool singleCube,bool materialStudy,const std::filesystem::path& sample)
	{
		if (!Initialize(singleCube))
		{
			Shutdown();
			return -1;
		}

        if(materialStudy)
        {
            ImportedAsset model;
            if(!sample.empty())
            {
                auto import=ImportGltf(sample);
                if(!import.asset) {std::cerr<<import.error<<'\n';return -1;}
                model=std::move(*import.asset);
            }
            m_camera.LookAt({0,0,10},{0,0,0});
            const auto error=AddImportedAsset(MakeMaterialStudy(std::move(model)));
            if(!error.empty()) {std::cerr<<error<<'\n';return -1;}
        }
        else if (!singleCube)
        {
            if (!LoadDefaultCornellBoxScene())
            {
                Shutdown();
                return -1;
            }
        }
		const bool completed = MainLoop();

		Shutdown();

		return completed ? 0 : -1;
	}

	bool AetherApplication::Initialize(bool singleCube)
	{
		if (!m_window.Initialize(1280, 720, "Aether"))
		{
			std::cerr << "Failed to initialize Window!\n";
			return false;
		}

		m_windowInitialized = true;

		if (!m_vulkanContext.Initialize(m_window))
		{
			std::cerr << "Failed to initialize Vulkan Context!\n";
			return false;
		}

		m_contextInitialized = true;

		if (!m_vulkanContext.CreateSurface(m_window))
			return false;

		if (!m_vulkanContext.SelectPhysicalDevice())
			return false;

		if (!m_vulkanContext.CreateLogicalDevice())
			return false;

		if (!m_vulkanContext.CreateSwapchain(m_window))
			return false;
		UpdateCameraAspect();
		m_camera.SetFov(45.0f);
		m_cameraController.SetDistanceRange(0.25f, 50.0f);
		m_camera.SetClipPlanes(0.1f, 100.0f);
		m_camera.LookAt(
			glm::vec3(1.8f, 1.4f, 2.2f) * (singleCube ? 1.0f : 2.0f),
			glm::vec3(0.0f, 0.0f, 0.0f),
			glm::vec3(0.0f, 1.0f, 0.0f)
		);

		if (!m_memoryManager.Initialize(m_vulkanContext))
		{
			std::cerr << "Failed to initialize Memory Manager!\n";
			return false;
		}

		m_memoryManagerInitialized = true;

		if (!CreateScene(singleCube))
			return false;

		if (!m_commandManager.Initialize(
			m_vulkanContext.GetDevice(),
			m_vulkanContext.GetGraphicsQueueFamily(),
			MAX_FRAMES_IN_FLIGHT))
		{
			std::cerr << "Failed to initialize Command Manager!\n";
			return false;
		}

		m_commandManagerInitialized = true;

		if (!m_syncManager.Initialize(
			m_vulkanContext.GetDevice(),
			MAX_FRAMES_IN_FLIGHT,
			static_cast<uint32_t>(
				m_vulkanContext.GetSwapchainImages().size())))
		{
			std::cerr << "Failed to initialize Sync Manager!\n";
			return false;
		}

		m_syncManagerInitialized = true;

		if (!m_renderer.Initialize(
			m_vulkanContext,
			m_commandManager,
			m_syncManager,
			m_memoryManager))
		{
			std::cerr << "Failed to initialize Renderer!\n";
			return false;
		}

		m_rendererInitialized = true;
        if (!SelectStudioEnvironment(false)) return false;

		return true;
	}

	bool AetherApplication::CreateScene(bool singleCube)
	{
		const bool enableRayTracingUsage = m_vulkanContext.GetRayTracingCapabilities().IsReady();
		const auto uploadMesh = [this, enableRayTracingUsage](const MeshData& data) {
			auto mesh = std::make_unique<VulkanMesh>();
			const VkResult result = mesh->Initialize(
				m_vulkanContext.GetDevice(),
				m_memoryManager,
				data,
				VulkanMeshCreateOptions{ enableRayTracingUsage });
			if (result != VK_SUCCESS)
			{
				std::cerr << "Failed to upload scene mesh: " << result << '\n';
				return false;
			}
			m_meshes.push_back(std::move(mesh));
			return true;
		};

		const MeshId cubeId = m_meshes.size();
		if (!uploadMesh(CreateCubeMesh()))
			return false;
		m_scene.AddObject({ {}, { cubeId } });
		if (!singleCube)
		{
			Transform left;
			left.position = { -1.55f, 0.15f, -0.15f };
			left.rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(0, 1, 0));
			left.scale = { 0.8f, 1.3f, 0.8f };
			m_scene.AddObject({ left, { cubeId } });

			const MeshId tetrahedronId = m_meshes.size();
			if (!uploadMesh(CreateTetrahedronMesh()))
				return false;

			Transform right;
			right.position = { 1.2f, -0.25f, 0.35f };
			right.rotation = glm::angleAxis(glm::radians(-22.0f), glm::vec3(0, 0, 1));
			right.scale = glm::vec3(0.75f);
			m_scene.AddObject({ right, { tetrahedronId } });
		}
		return true;
	}

	bool AetherApplication::RecreateSwapchain()
	{
		if (!m_vulkanContext.RecreateSwapchain(m_window))
			return false;

		m_syncManager.Shutdown();
		if (!m_syncManager.Initialize(
			m_vulkanContext.GetDevice(),
			MAX_FRAMES_IN_FLIGHT,
			static_cast<uint32_t>(m_vulkanContext.GetSwapchainImages().size())))
			return false;

		UpdateCameraAspect();
		ResetTemporalHistory();
		return m_renderer.OnSwapchainRecreated();
	}

	void AetherApplication::ResetTemporalHistory()
	{
		m_hasPrevCamera = false;
		m_taaFrameIndex = 0;
		m_renderer.ResetTaaHistory();
		m_renderer.ResetRtDenoiserHistory();
		m_hybridConfig.resetHistory = true;
	}

	bool AetherApplication::LoadDefaultCornellBoxScene()
	{
		const std::string error = AddImportedAsset(MakeCornellBoxScene());
		if (!error.empty())
		{
			std::cerr << "Failed to load default Cornell Box scene: " << error << '\n';
			return false;
		}
		m_isCornellBoxScene = true;
		const CornellBoxCameraPreset cam = GetDefaultCornellBoxCamera();
		m_camera.SetFov(cam.fovDegrees);
		m_camera.SetClipPlanes(cam.nearZ, cam.farZ);
		m_camera.LookAt(cam.eye, cam.target, cam.up);
		m_cameraController.SetDistanceRange(0.25f, 40.0f);
		m_lightAngle = 0.65f;
		ResetTemporalHistory();
		std::cout << "Default Cornell Box validation scene loaded (meshes: "
			<< m_meshes.size() << ", objects: " << m_scene.GetObjects().size() << ")\n";
		return true;
	}

	bool AetherApplication::ToggleRenderModeInteractive()
	{
		const bool canUseRt = m_renderer.CanUseHybridRtMode() || m_renderer.CanUseRayQueryMode();
		const RenderModeSwitchResult result =
			ToggleRenderMode(m_renderMode, canUseRt);
		std::cout << result.message << '\n';
		if (result.switched)
		{
			m_renderMode = result.nextMode;
			if (result.invalidateTemporalHistory)
				ResetTemporalHistory();
			if (m_renderMode == RenderMode::HybridRayTracing)
			{
				std::cout << "   Active Hybrid RT Debug View: "
					<< HybridDebugViewToString(m_hybridConfig.debugView)
					<< " (press G to cycle)\n";
			}
			else if (m_renderMode == RenderMode::RayQuery)
			{
				std::cout << "   Active Ray Query Diagnostic Mode: "
					<< RayQueryDebugModeToString(m_rayQueryConfig.debugMode)
					<< " (press G to cycle)\n";
			}
		}
		return result.switched;
	}

	std::string AetherApplication::AddImportedAsset(ImportedAsset&& asset, std::optional<size_t> failPrimitive)
	{
		const bool enableRayTracingUsage = m_vulkanContext.GetRayTracingCapabilities().IsReady();
		auto result = failPrimitive ?
			VulkanAssetUploader::Upload(
				m_vulkanContext.GetDevice(), m_memoryManager, asset, 0, failPrimitive, enableRayTracingUsage) :
			VulkanAssetUploader::Upload(
				m_vulkanContext.GetDevice(), m_memoryManager, asset, 0, enableRayTracingUsage);
		if (!result.asset)
			return result.error;

		auto materials = std::make_unique<VulkanMaterialSet>();
		try
		{
			materials->Initialize(m_vulkanContext.GetDevice(), m_memoryManager, m_renderer, asset);
		}
		catch (const std::exception& error)
		{
			return error.what();
		}
		auto& batch = *result.asset;
		Scene nextScene;
		nextScene.AddObjects(batch.objects);
		const auto idle = vkDeviceWaitIdle(m_vulkanContext.GetDevice());
		if (idle != VK_SUCCESS)
			return "Could not retire previous scene (" + std::to_string(idle) + ")";
		m_renderer.m_rtScene.OnDeviceIdle();

		m_drawItems.clear();
		m_scene = std::move(nextScene);
		m_meshes.swap(batch.meshes);
		m_previewMaterials.swap(materials);
		m_previewAsset.emplace(std::move(asset));
		m_isCornellBoxScene = false;
		m_customSceneLights.clear();
		m_sceneMin = batch.minimum;
		m_sceneMax = batch.maximum;
		FrameImportedAsset(batch.minimum, batch.maximum);
		ResetTemporalHistory();
		return {};
	}

	VulkanRenderer::FrameResult AetherApplication::DrawScene()
	{
		auto objects = m_scene.GetObjects();
		const auto extent = m_vulkanContext.GetSwapchainExtent();
		const glm::mat4 currView = m_camera.GetViewMatrix();
		const glm::mat4 unjitteredProj = m_camera.GetProjectionMatrix();

		const bool taaActive =
			m_taaConfig.enabled &&
			m_renderMode == RenderMode::Rasterization &&
			!(m_shadowConfig.debugView && m_shadowConfig.enabled);
		const bool hybridTemporalActive =
			m_renderMode == RenderMode::HybridRayTracing &&
			m_renderer.CanUseHybridRtMode() &&
			m_hybridConfig.debugView != HybridDebugView::RayQueryDiagnostic;
		const bool temporalTrackingActive = taaActive || hybridTemporalActive;

		const auto eyePosition = m_camera.GetPosition();
		const float focusDistance = glm::distance(eyePosition, m_camera.GetTarget());
		const auto previousForward = -glm::vec3(glm::transpose(m_prevViewMatrix)[2]);
		if (m_hasPrevCamera &&
			(!temporalTrackingActive ||
			 (taaActive && m_taaConfig.resetHistory) ||
			 (taaActive && m_taaConfig.jitterSequenceLength != m_prevJitterSequenceLength) ||
			 (hybridTemporalActive && m_hybridConfig.resetHistory) ||
			 unjitteredProj != m_prevUnjitteredProj ||
			 glm::dot(m_camera.GetForward(), previousForward) < 0.5f ||
			 glm::distance(eyePosition, m_prevEye) > std::max(1e-4f, m_prevFocusDistance)))
			ResetTemporalHistory();
		const TaaJitterSample jitter = taaActive
			? ComputeTaaJitter(m_taaFrameIndex, extent.width, extent.height, m_taaConfig.jitterSequenceLength)
			: TaaJitterSample{};
		const glm::mat4 activeProj = taaActive
			? ApplyProjectionJitter(unjitteredProj, jitter.ndcOffset)
			: unjitteredProj;
		const glm::mat4 viewProjection = activeProj * currView;
		const glm::mat4 previousProjection = m_hasPrevCamera
			? ApplyProjectionJitter(m_prevUnjitteredProj, m_prevJitterNdc)
			: activeProj;
		const glm::mat4 previousViewProjection = previousProjection * m_prevViewMatrix;
		const glm::mat4 prevUnjitteredViewProj = m_hasPrevCamera
			? (m_prevUnjitteredProj * m_prevViewMatrix)
			: (unjitteredProj * currView);
		const glm::vec2 jitterDeltaUv = m_hasPrevCamera
			? 0.5f * (m_prevJitterNdc - jitter.ndcOffset) : glm::vec2(0.0f);

		m_drawItems.clear();
		m_drawItems.reserve(objects.size());
		for (size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
		{
			auto& object = objects[objectIndex];
			if (object.mesh.meshId >= m_meshes.size())
				return VulkanRenderer::FrameResult::FatalError;

			const auto model = object.GetModelMatrix();
			const auto rows = glm::transpose(model);
			const glm::mat4 previousMvp = m_hasPrevCamera && object.previousModelMatrix
				? previousViewProjection * *object.previousModelMatrix : glm::mat4(0.0f);

			RtMaterialSummary rtMat{};
			rtMat.baseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
			rtMat.metallic = 0.0f;
			rtMat.roughness = 0.55f;
			rtMat.ao = 1.0f;
			rtMat.doubleSided = object.doubleSided;

			m_drawItems.push_back({
				m_meshes[object.mesh.meshId].get(),
				viewProjection * model,
				glm::determinant(glm::mat3(model)) < 0.0f,
				object.doubleSided,
				VK_NULL_HANDLE,
				false,
				{ viewProjection * model, { rows[0], rows[1], rows[2] }, glm::vec4(m_camera.GetPosition(), m_lightAngle) },
				model,
				false,
				previousMvp,
				static_cast<uint32_t>(objectIndex),
				object.previousModelMatrix.value_or(model),
				m_hasPrevCamera && object.previousModelMatrix.has_value(),
				rtMat,
				VK_NULL_HANDLE,
				VK_NULL_HANDLE,
				UINT32_MAX
			});
			if (m_previewMaterials)
			{
				auto& draw = m_drawItems.back();
				draw.material = m_previewMaterials->GetSet(object.material);
				if (object.material && m_previewAsset && *object.material < m_previewAsset->materials.size())
				{
					const auto& srcMat = m_previewAsset->materials.at(*object.material);
					draw.alphaBlend = srcMat.alphaMode == ImportedMaterial::AlphaMode::Blend;
					draw.alphaMask = srcMat.alphaMode == ImportedMaterial::AlphaMode::Mask;
					draw.rtMaterial.baseColor = srcMat.baseColor;
					draw.rtMaterial.emissive = srcMat.emissive;
					draw.rtMaterial.metallic = srcMat.metallic;
					draw.rtMaterial.roughness = srcMat.roughness;
					draw.rtMaterial.alphaCutoff = srcMat.alphaCutoff;
					draw.rtMaterial.alphaMask = draw.alphaMask;
					draw.rtMaterial.doubleSided = object.doubleSided || srcMat.doubleSided;
					if (draw.alphaMask)
					{
						if (const VulkanTexture* baseTex = m_previewMaterials->GetBaseColorTexture(object.material))
						{
							draw.alphaMaskView = baseTex->GetImage().GetView();
							draw.alphaMaskSampler = baseTex->GetSampler();
						}
					}
				}
			}
		}
		if (m_previewMaterials)
		{
			const auto& eye = m_camera.GetPosition();
			const auto distanceSquared = [&eye](const auto& draw) {
				const auto& rows = draw.pbr.modelRows;
				const auto offset = glm::vec3(rows[0].w, rows[1].w, rows[2].w) - eye;
				return glm::dot(offset, offset);
			};
			std::stable_sort(m_drawItems.begin(), m_drawItems.end(), [&](const auto& a, const auto& b) {
				if (a.alphaBlend != b.alphaBlend)
					return !a.alphaBlend;
				return a.alphaBlend && distanceSquared(a) > distanceSquared(b);
			});
		}
		// Remove view translation before unprojecting rays in the Vulkan clip space used by meshes.
		const glm::mat4 currViewRotation{ glm::mat3(currView) };
		const glm::mat4 prevViewRotation{ glm::mat3(m_hasPrevCamera ? m_prevViewMatrix : currView) };
		const auto clipToWorldDirection = glm::inverse(activeProj * currViewRotation);
		const auto unjitteredClipToWorldDirection = glm::inverse(unjitteredProj * currViewRotation);
		const glm::mat4 currClipToPrevClipRot = m_hasPrevCamera
			? (previousProjection * prevViewRotation * clipToWorldDirection)
			: glm::mat4(1.0f);

		const auto lightDir = ComputeDirectionalLightDirection(m_lightAngle);
		const auto lightSpace = ComputeDirectionalLightSpace(
			lightDir,
			currView,
			unjitteredProj,
			m_renderer.GetShadowResolution(),
			m_shadowConfig.maxShadowDistance,
			m_shadowConfig.casterMargin,
			m_shadowConfig.stabilizeTexels,
			m_sceneMin ? &*m_sceneMin : nullptr,
			m_sceneMax ? &*m_sceneMax : nullptr);

		std::vector<RtGpuLight> activeSceneLights;
		if (!m_customSceneLights.empty())
			activeSceneLights = m_customSceneLights;
		else if (m_isCornellBoxScene)
			activeSceneLights = GetCornellBoxLights();

		const TaaConfig frameTaa = m_taaConfig;
		const auto result = m_renderer.DrawFrame(
			m_drawItems, m_environment->Descriptor(), clipToWorldDirection, m_exposure,
			lightSpace, m_shadowConfig, m_bloomConfig, frameTaa,
			currClipToPrevClipRot, jitterDeltaUv, m_updateRayTracingScene,
			m_renderMode, m_camera.GetPosition(), m_rayQueryConfig,
			unjitteredClipToWorldDirection, m_hybridConfig, activeSceneLights,
			prevUnjitteredViewProj, m_lightAngle);

		if (result == VulkanRenderer::FrameResult::Rendered)
		{
			if (temporalTrackingActive)
			{
				for (auto& object : objects)
					object.previousModelMatrix = object.GetModelMatrix();
				m_prevViewMatrix = currView;
				m_prevUnjitteredProj = unjitteredProj;
				m_prevJitterNdc = jitter.ndcOffset;
				m_prevEye = eyePosition;
				m_prevFocusDistance = focusDistance;
				m_prevJitterSequenceLength = m_taaConfig.jitterSequenceLength;
				m_hasPrevCamera = true;
				if (taaActive)
				{
					++m_taaFrameIndex;
					m_taaConfig.resetHistory = false;
				}
			}
			m_hybridConfig.resetHistory = false;
		}
		return result;
	}

	void AetherApplication::UpdateCameraAspect()
	{
		const auto extent = m_vulkanContext.GetSwapchainExtent();
		if (extent.width && extent.height)
			m_camera.SetAspectRatio(static_cast<float>(extent.width) / extent.height);
	}

	void AetherApplication::UpdateCameraInput()
	{
		auto* window = m_window.GetNativeHandle();
		double x, y;
		glfwGetCursorPos(window, &x, &y);
		const float dx = static_cast<float>(x - m_cursorX);
		const float dy = static_cast<float>(y - m_cursorY);
		const bool focused = glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE;
		const bool orbit = focused && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
		const bool pan = focused && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
		const float scroll = static_cast<float>(m_window.ConsumeScrollY());

		if (orbit && m_orbitDragging)
			m_cameraController.Orbit(m_camera, -0.005f * dx, 0.005f * dy);
		if (pan && m_panDragging)
		{
			int width, height;
			glfwGetWindowSize(window, &width, &height);
			if (height > 0)
			{
				const float distance = glm::distance(m_camera.GetPosition(), m_camera.GetTarget());
				const float unitsPerPixel = 2.0f * distance /
					(-m_camera.GetProjectionMatrix()[1][1] * static_cast<float>(height));
				m_cameraController.Pan(m_camera, -dx * unitsPerPixel, dy * unitsPerPixel);
			}
		}
		if (focused && scroll != 0.0f)
			m_cameraController.Zoom(m_camera, scroll);

		m_cursorX = x;
		m_cursorY = y;
		m_orbitDragging = orbit;
		m_panDragging = pan;
	}

	void AetherApplication::FrameImportedAsset(const glm::vec3& minimum, const glm::vec3& maximum)
	{
		const glm::vec3 center = minimum * 0.5f + maximum * 0.5f;
		const float radius = std::max(glm::length(maximum * 0.5f - minimum * 0.5f), 0.0001f);
		const auto& projection = m_camera.GetProjectionMatrix();
		const float halfAngle = std::atan(1.0f / std::max(projection[0][0], -projection[1][1]));
		const float distance = 1.15f * radius / std::sin(halfAngle);
		const float farPlane = 20.0f * distance;
		if (!std::isfinite(farPlane))
			return;

		m_camera.LookAt(center - m_camera.GetForward() * distance, center, m_camera.GetUp());
		if (m_camera.GetTarget() != center)
			return;
		m_camera.SetClipPlanes(distance * 0.001f, farPlane);
		m_cameraController.SetDistanceRange(std::max(radius * 0.02f, 0.00001f), farPlane * 0.5f);
	}

    bool AetherApplication::SelectStudioEnvironment(bool warm)
    {
        try
        {
            return SwitchEnvironment(LoadStudioEnvironment(warm));
        }
        catch (const std::exception& e)
        {
            std::cerr<<"Could not load studio environment: "<<e.what()<<'\n';
            return false;
        }
    }

    bool AetherApplication::SwitchEnvironment(const HdrImage& image,bool injectFailure)
    {
        auto candidate=std::make_unique<VulkanEnvironment>();
        auto status=m_renderer.UploadEnvironment(*candidate,image);
        if(status==VK_SUCCESS && injectFailure) status=VK_ERROR_UNKNOWN;
        if(status==VK_SUCCESS) status=m_renderer.PrecomputeEnvironment(*candidate);
        if(status==VK_SUCCESS) status=candidate->BindMaps();
        if(status!=VK_SUCCESS)
        {
            std::cerr<<"Environment upload failed ("<<status<<")\n";
            return false;
        }
        m_environment.swap(candidate);
        ResetTemporalHistory();
        return true;
    }

	void AetherApplication::ProcessDroppedAssets()
	{
		for (const auto& utf8 : m_window.ConsumeDroppedPaths())
		{
			try
			{
				const std::filesystem::path path(std::u8string(utf8.begin(), utf8.end()));
				auto extension = path.extension().u8string();
				for (auto& letter : extension)
					if (letter >= u8'A' && letter <= u8'Z')
						letter += u8'a' - u8'A';
				if (extension == u8".hdr")
                {
                    if (SwitchEnvironment(LoadRadiance(path))) std::cout<<"Environment loaded: "<<utf8<<'\n';
                    continue;
                }
                if (extension != u8".gltf" && extension != u8".glb")
				{
					std::cerr << utf8 << ": expected .hdr, .gltf or .glb\n";
					continue;
				}
				auto imported = ImportGltf(path);
				if (!imported.asset)
				{
					std::cerr << imported.error << '\n';
					continue;
				}
				const auto error = AddImportedAsset(std::move(*imported.asset));
				if (!error.empty())
					std::cerr << utf8 << ": " << error << '\n';
				else
					std::cout << "Imported " << utf8 << '\n';
			}
			catch (const std::exception& error)
			{
				std::cerr << utf8 << ": " << error.what() << '\n';
			}
		}
	}

	bool AetherApplication::MainLoop()
	{
		bool recreateRequested = false;
		const auto startTime = std::chrono::high_resolution_clock::now();
		auto previousTime = startTime;

		while (m_window.IsOpen())
		{
			m_window.PollEvents();

			if (m_window.GetWidth() == 0 || m_window.GetHeight() == 0 ||
				glfwGetWindowAttrib(m_window.GetNativeHandle(), GLFW_ICONIFIED))
			{
				if (m_hasPrevCamera)
					ResetTemporalHistory();
				glfwWaitEventsTimeout(0.1);
				continue;
			}

			if (recreateRequested || m_window.WasResized())
			{
				m_window.ResetResizedFlag();
				if (!RecreateSwapchain())
					return false;
				recreateRequested = false;
			}

			ProcessDroppedAssets();
			const auto currentTime = std::chrono::high_resolution_clock::now();
			const float elapsedSeconds =
				std::chrono::duration<float>(currentTime - startTime).count();

			UpdateCameraInput();
			const auto window = m_window.GetNativeHandle();
			const bool environmentKey=glfwGetKey(window,GLFW_KEY_E)==GLFW_PRESS;
            if(environmentKey && !m_environmentKeyDown && SelectStudioEnvironment(!m_warmEnvironment))
                m_warmEnvironment=!m_warmEnvironment;
            m_environmentKeyDown=environmentKey;

            const bool shadowKey = glfwGetKey(window, GLFW_KEY_H) == GLFW_PRESS;
            if (shadowKey && !m_shadowToggleKeyDown)
            {
                m_shadowConfig.enabled = !m_shadowConfig.enabled;
                ResetTemporalHistory();
                std::cout << "Directional shadows: " << (m_shadowConfig.enabled ? "ON" : "OFF") << '\n';
            }
            m_shadowToggleKeyDown = shadowKey;

            const bool shadowDebugKey = glfwGetKey(window, GLFW_KEY_M) == GLFW_PRESS;
            if (shadowDebugKey && !m_shadowDebugKeyDown)
            {
                m_shadowConfig.debugView = !m_shadowConfig.debugView;
                std::cout << "Shadow map debug view: " << (m_shadowConfig.debugView ? "ON" : "OFF") << '\n';
            }
            m_shadowDebugKeyDown = shadowDebugKey;

            const bool bloomKey = glfwGetKey(window, GLFW_KEY_B) == GLFW_PRESS;
            if (bloomKey && !m_bloomToggleKeyDown)
            {
                m_bloomConfig.enabled = !m_bloomConfig.enabled;
                std::cout << "HDR Bloom: " << (m_bloomConfig.enabled ? "ON" : "OFF") << '\n';
            }
            m_bloomToggleKeyDown = bloomKey;

            const bool bloomDebugKey = glfwGetKey(window, GLFW_KEY_V) == GLFW_PRESS;
            if (bloomDebugKey && !m_bloomDebugKeyDown)
            {
                const uint32_t next = (static_cast<uint32_t>(m_bloomConfig.debugMode) + 1u) % 4u;
                m_bloomConfig.debugMode = static_cast<BloomDebugMode>(next);
                std::cout << "Bloom debug mode: " << next << '\n';
            }
            m_bloomDebugKeyDown = bloomDebugKey;

            const bool taaKey = glfwGetKey(window, GLFW_KEY_T) == GLFW_PRESS;
            if (taaKey && !m_taaToggleKeyDown)
            {
                m_taaConfig.enabled = !m_taaConfig.enabled;
                if (m_taaConfig.enabled)
                    ResetTemporalHistory();
                std::cout << "Temporal Anti-Aliasing (TAA): " << (m_taaConfig.enabled ? "ON" : "OFF") << '\n';
            }
            m_taaToggleKeyDown = taaKey;

            const bool taaResetKey = glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS;
            if (taaResetKey && !m_taaResetKeyDown)
            {
                ResetTemporalHistory();
                std::cout << "TAA temporal history reset\n";
            }
            m_taaResetKeyDown = taaResetKey;

            const bool renderModeKey = glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS;
            if (renderModeKey && !m_renderModeKeyDown)
            {
                ToggleRenderModeInteractive();
            }
            m_renderModeKeyDown = renderModeKey;

            const bool rayQueryDebugKey = glfwGetKey(window, GLFW_KEY_G) == GLFW_PRESS;
            if (rayQueryDebugKey && !m_rayQueryDebugKeyDown)
            {
                if (m_renderMode == RenderMode::HybridRayTracing)
                {
                    m_hybridConfig.debugView = CycleHybridDebugView(m_hybridConfig.debugView);
                    std::cout << "Hybrid RT debug view: "
                        << HybridDebugViewToString(m_hybridConfig.debugView) << '\n';
                }
                else
                {
                    m_rayQueryConfig.debugMode = CycleInteractiveRayQueryDebugMode(m_rayQueryConfig.debugMode);
                    std::cout << "Ray Query diagnostic mode: "
                        << RayQueryDebugModeToString(m_rayQueryConfig.debugMode) << '\n';
                }
            }
            m_rayQueryDebugKeyDown = rayQueryDebugKey;
            const int lightMotion = (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS) -
				(glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS);
			const float frameSeconds = std::chrono::duration<float>(currentTime - previousTime).count();
            const int exposureDirection=(glfwGetKey(window,GLFW_KEY_UP)==GLFW_PRESS)-(glfwGetKey(window,GLFW_KEY_DOWN)==GLFW_PRESS);
            m_exposure=std::clamp(m_exposure+exposureDirection*std::min(frameSeconds,.05f)*2,-8.f,8.f);
			previousTime = currentTime;
			m_lightAngle += static_cast<float>(lightMotion) * std::min(frameSeconds, 0.05f) * 1.5f;

			if (auto objects = m_scene.GetObjects(); !m_previewAsset && !objects.empty())
			{
				objects.front().transform.rotation = glm::angleAxis(
					elapsedSeconds * glm::radians(60.0f),
					glm::normalize(glm::vec3(0.5f, 1.0f, 0.0f)));
			}
			const auto frameResult = DrawScene();
			if (frameResult == VulkanRenderer::FrameResult::FatalError)
				return false;

			recreateRequested = frameResult == VulkanRenderer::FrameResult::RecreateSwapchain;
		}
		return true;
	}

	void AetherApplication::Shutdown()
	{
		if (m_contextInitialized)
		{
			m_vulkanContext.WaitIdle();
		}

		if (m_rendererInitialized)
		{
			m_renderer.Shutdown();
			m_rendererInitialized = false;
		}

		m_drawItems.clear();
		m_scene.Clear();
		m_meshes.clear();
		m_previewAsset.reset();
		m_previewMaterials.reset();
        m_environment.reset();

		if (m_syncManagerInitialized)
		{
			m_syncManager.Shutdown();
			m_syncManagerInitialized = false;
		}

		if (m_commandManagerInitialized)
		{
			m_commandManager.Shutdown();
			m_commandManagerInitialized = false;
		}

		if (m_memoryManagerInitialized)
		{
			m_memoryManager.Shutdown();
			m_memoryManagerInitialized = false;
		}

		if (m_contextInitialized)
		{
			m_vulkanContext.Shutdown();
			m_contextInitialized = false;
		}

		if (m_windowInitialized)
		{
			m_windowInitialized = false;
		}
	}

}
