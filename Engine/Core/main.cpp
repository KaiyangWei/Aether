#include <iostream>
#include <Platform/Window/Window.h>
int main()
{
    Aether::Window window;
    if (!window.Initialize(1280, 720, "Aether"))
        return -1;

    while (window.IsOpen())
    {
        window.PollEvents();
    }
    window.Shutdown();
    return 0;
}