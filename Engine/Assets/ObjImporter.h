#pragma once

#include <Renderer/MeshData.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Aether
{
    struct ObjImportResult
    {
        std::optional<MeshData> mesh;
        std::string error;
    };

    ObjImportResult ParseObj(std::string_view source);
    ObjImportResult ImportObj(const std::filesystem::path& path);
}
