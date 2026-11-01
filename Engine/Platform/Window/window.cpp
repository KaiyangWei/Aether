#include "Window.h"
#include <iostream>
#include <GLFW/glfw3.h>
bool Aether::Window::Initialize(uint32_t width, uint32_t height, const std::string& title)
{
	if (m_initialized)
		return false;
	if (!glfwInit())
	{
		std::cout << "Failed to init GLFW!" << "\n";
		return false;
	}
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
	if (m_window == nullptr)
	{
		std::cout << "Failed to create window!" << "\n";
		glfwTerminate();
		return false;
	}
	m_width = width; m_height = height;
	m_initialized = true;
	return true;
}
void Aether::Window::ProcessInput() {
	if (glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
		glfwSetWindowShouldClose(m_window, true);
}

void Aether::Window::PollEvents()
{
	if (m_window == nullptr)
	{
		std::cout << "No available window!" << "\n";
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

void Aether::Window::Shutdown()
{

		if (m_window != nullptr)
		{
			glfwDestroyWindow(m_window);
			m_window = nullptr;
		}

		if (m_initialized)
		{
			glfwTerminate();
			m_initialized = false;
		}

		m_width = 0;
		m_height = 0;
}

Aether::Window::~Window()
{
	std::cout << "Destroying the Window!" << "\n";
	Shutdown();
}