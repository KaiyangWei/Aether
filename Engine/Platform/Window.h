#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include <vector>

struct GLFWwindow;

namespace Aether
{
	class Window
	{
	public:
		using ResizeCallback = std::function<void(uint32_t width, uint32_t height)>;

		Window() = default;
		~Window();

		Window(const Window&) = delete;
		Window& operator=(const Window&) = delete;

		bool Initialize(uint32_t width, uint32_t height, const std::string& title);

		void PollEvents();
		double ConsumeScrollY();
		std::vector<std::string> ConsumeDroppedPaths();

		bool IsOpen() const;

		void Shutdown();

		GLFWwindow* GetNativeHandle() const;

		uint32_t GetWidth() const;

		uint32_t GetHeight() const;

		// 检查本帧是否触发过窗口缩放
		bool WasResized() const { return m_resized; }

		// 重置缩放标志位
		void ResetResizedFlag() { m_resized = false; }

		// 注册尺寸变动事件监听回调
		void SetResizeCallback(const ResizeCallback& callback) { m_resizeCallback = callback; }

	private:
		void ProcessInput();
		static void ScrollCallback(GLFWwindow* window, double x, double y);
		static void DropCallback(GLFWwindow* window, int count, const char** paths);
		double m_scrollY = 0.0;
		std::vector<std::string> m_droppedPaths;

		// GLFW 静态回调
		static void FramebufferResizeCallback(GLFWwindow* window, int width, int height);

	private:
		GLFWwindow* m_window = nullptr;

		uint32_t m_width = 0;
		uint32_t m_height = 0;
		bool m_initialized = false;
		bool m_resized = false;

		ResizeCallback m_resizeCallback = nullptr;
	};
}