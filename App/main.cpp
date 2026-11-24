
#include <Core/AetherApplication.h>

int main(int argc, char** argv)
{
    Aether::AetherApplication application;
    if (argc == 3 && std::string_view(argv[1]) == "--swapchain-test")
        return application.RunSwapchainTest(argv[2]);
    if (argc >= 2 && argc <= 3 && std::string_view(argv[1]) == "--material-test")
        return application.Run(false,true,argc==3?argv[2]:"");
    return application.Run(argc == 2 && std::string_view(argv[1]) == "--single-cube");
}

