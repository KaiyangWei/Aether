#include <Assets/HdrImage.h>
#include <fstream>
#include <cmath>
int main()
{
    const auto path = std::filesystem::current_path() / "radiance-test.hdr";
    std::ofstream out(path, std::ios::binary);
    out << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 8\n";
    const unsigned char scan[] = {2, 2, 0, 8, 136, 128, 136, 64, 136, 32, 136, 131};
    for (int i = 0; i < 4; ++i)
        out.write(reinterpret_cast<const char *>(scan), sizeof(scan));
    out.close();
    auto image = Aether::LoadRadiance(path);
    for (unsigned face = 0; face < 6; ++face)
        for (auto p : Aether::PanoramaFace(image, face, 8).pixels)
            if (p.x != 4 || p.y != 2 || p.z != 1)
                return 1;
    auto sky = Aether::LoadStudioEnvironment(false);
    if (Aether::PanoramaFace(sky, 2, 1).pixels.front().z <= Aether::PanoramaFace(sky, 3, 1).pixels.front().z)
        return 2;
    std::filesystem::resize_file(path, 25);
    try
    {
        Aether::LoadRadiance(path);
    }
    catch (const std::exception &)
    {
        return 0;
    }
    return 3;
}
