#include "Window.h"

#include <iostream>
#include <utility>
#include <GLFW/glfw3.h>

bool Aether::Window::Initialize(uint32_t width, uint32_t height, const std::string& title)
{
	if (m_initialized)
		return false;

	if (!glfwInit())
	{
		std::cerr << "Failed to init GLFW!\n";
		return false;
	}

	// 显式指定无 OpenGL 上下文，并允许窗口调整尺寸
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

	m_window = glfwCreateWindow(
		static_cast<int>(width),
		static_cast<int>(height),
		title.c_str(),
		nullptr,
		nullptr
	);

	if (m_window == nullptr)
	{
		std::cerr << "Failed to create window!\n";
		glfwTerminate();
		return false;
	}

	int fbWidth = 0;
	int fbHeight = 0;
	glfwGetFramebufferSize(m_window, &fbWidth, &fbHeight);
	m_width = static_cast<uint32_t>(fbWidth);
	m_height = static_cast<uint32_t>(fbHeight);

	glfwSetWindowUserPointer(m_window, this);
	glfwSetFramebufferSizeCallback(m_window, FramebufferResizeCallback);
	glfwSetScrollCallback(m_window, ScrollCallback);
	glfwSetDropCallback(m_window, DropCallback);

	m_initialized = true;
	return true;
}

void Aether::Window::FramebufferResizeCallback(GLFWwindow* window, int width, int height)
{
	auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
	if (self == nullptr)
		return;

	self->m_width = static_cast<uint32_t>(width);
	self->m_height = static_cast<uint32_t>(height);
	self->m_resized = true;

	if (self->m_resizeCallback)
	{
		self->m_resizeCallback(self->m_width, self->m_height);
	}
}

void Aether::Window::ScrollCallback(GLFWwindow* window, double, double y)
{
	auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
	if (self)
		self->m_scrollY += y;
}

double Aether::Window::ConsumeScrollY()
{
	return std::exchange(m_scrollY, 0.0);
}

void Aether::Window::DropCallback(GLFWwindow* window, int count, const char** paths)
{
	auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
	if (!self)
		return;

	try
	{
		for (int i = 0; i < count; ++i)
			if (paths[i])
				self->m_droppedPaths.emplace_back(paths[i]);
	}
	catch (const std::exception& error)
	{
		std::cerr << "Could not queue dropped file: " << error.what() << '\n';
	}
}

std::vector<std::string> Aether::Window::ConsumeDroppedPaths()
{
	return std::exchange(m_droppedPaths, {});
}

void Aether::Window::ProcessInput()
{
	if (glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
		glfwSetWindowShouldClose(m_window, GLFW_TRUE);
}

void Aether::Window::PollEvents()
{
	if (m_window == nullptr)
	{
		std::cerr << "No available window!\n";
		return;
	}

	glfwPollEvents();
	ProcessInput();
}

bool Aether::Window::IsOpen() const
{
	return m_window != nullptr &&
		m_initialized &&
		!glfwWindowShouldClose(m_window);
}

GLFWwindow* Aether::Window::GetNativeHandle() const
{
	return m_window;
}

uint32_t Aether::Window::GetWidth() const
{
	return m_width;
}

uint32_t Aether::Window::GetHeight() const
{
	return m_height;
}

void Aether::Window::Shutdown()
{
	if (m_window != nullptr)
	{
		glfwSetFramebufferSizeCallback(m_window, nullptr);
		glfwDestroyWindow(m_window);
		m_window = nullptr;
	}

	if (m_initialized)
	{
		glfwTerminate();
		m_initialized = false;
	}

	m_droppedPaths.clear();
	m_scrollY = 0.0;
	m_width = 0;
	m_height = 0;
	m_resized = false;
	m_resizeCallback = nullptr;
}

Aether::Window::~Window()
{
	Shutdown();
}