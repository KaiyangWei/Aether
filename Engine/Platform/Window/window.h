#pragma once

#include <string>

struct GLFWwindow;

namespace Aether
{
	class Window
	{
	public:
		Window() = default;
		~Window();

		Window(const Window&) = delete;
		Window& operator = (const Window&) = delete;

		bool Initialize(uint32_t width , uint32_t height , const std::string& title);

		void PollEvents();

		bool IsOpen() const;

		void Shutdown();

		GLFWwindow* GetNativeHandle() const;

		uint32_t GetWidth() const;

		uint32_t GetHeight() const;

	private:
		GLFWwindow* m_window = nullptr;

		uint32_t m_width = 0;
		uint32_t m_height = 0;
		bool m_initialized = false;
	
		void ProcessInput();
	};
}