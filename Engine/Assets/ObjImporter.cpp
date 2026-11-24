#include "ObjImporter.h"

#include <glm/geometric.hpp>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace Aether
{
    namespace
    {
        struct VertexKey
        {
            uint32_t position = 0;
            uint32_t uv = 0;
            uint32_t normal = 0;
            bool hasUv = false;
            bool hasNormal = false;

            bool operator==(const VertexKey&) const = default;
        };

        struct VertexKeyHash
        {
            size_t operator()(const VertexKey& key) const noexcept
            {
                size_t hash = std::hash<uint32_t>{}(key.position);
                hash ^= std::hash<uint32_t>{}(key.uv) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= std::hash<uint32_t>{}(key.normal) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= (static_cast<size_t>(key.hasUv) << 1) | static_cast<size_t>(key.hasNormal);
                return hash;
            }
        };

        std::string_view Trim(std::string_view text)
        {
            const auto first = text.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
                return {};
            const auto last = text.find_last_not_of(" \t\r\n");
            return text.substr(first, last - first + 1);
        }

        std::string_view NextToken(std::string_view& line)
        {
            const auto start = line.find_first_not_of(" \t");
            if (start == std::string_view::npos)
            {
                line = {};
                return {};
            }
            line.remove_prefix(start);
            const auto end = line.find_first_of(" \t");
            if (end == std::string_view::npos)
            {
                const auto token = line;
                line = {};
                return token;
            }
            const auto token = line.substr(0, end);
            line.remove_prefix(end);
            return token;
        }

        bool ParseFloat(std::string_view token, float& out)
        {
            if (token.empty())
                return false;
            float value = 0.0f;
            const auto* begin = token.data();
            const auto* end = begin + token.size();
            const auto result = std::from_chars(begin, end, value);
            if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(value))
                return false;
            out = value;
            return true;
        }

        bool ParseIndex(std::string_view token, size_t count, uint32_t& out)
        {
            if (token.empty())
                return false;
            int64_t raw = 0;
            const auto* begin = token.data();
            const auto* end = begin + token.size();
            const auto result = std::from_chars(begin, end, raw);
            if (result.ec != std::errc{} || result.ptr != end || raw == 0)
                return false;

            int64_t resolved = 0;
            if (raw > 0)
                resolved = raw - 1;
            else
                resolved = static_cast<int64_t>(count) + raw;

            if (resolved < 0 || static_cast<uint64_t>(resolved) >= count ||
                resolved > std::numeric_limits<uint32_t>::max())
                return false;

            out = static_cast<uint32_t>(resolved);
            return true;
        }

        bool ParseFaceCorner(
            std::string_view token,
            size_t posCount,
            size_t uvCount,
            size_t normCount,
            VertexKey& out)
        {
            out = {};
            const auto firstSlash = token.find('/');
            if (firstSlash == std::string_view::npos)
                return ParseIndex(token, posCount, out.position);

            const auto posToken = token.substr(0, firstSlash);
            if (!ParseIndex(posToken, posCount, out.position))
                return false;

            const auto rest = token.substr(firstSlash + 1);
            const auto secondSlash = rest.find('/');
            if (secondSlash == std::string_view::npos)
            {
                if (rest.empty())
                    return false;
                out.hasUv = true;
                return ParseIndex(rest, uvCount, out.uv);
            }

            const auto uvToken = rest.substr(0, secondSlash);
            const auto normToken = rest.substr(secondSlash + 1);
            if (normToken.find('/') != std::string_view::npos)
                return false;

            if (!uvToken.empty())
            {
                out.hasUv = true;
                if (!ParseIndex(uvToken, uvCount, out.uv))
                    return false;
            }
            if (!normToken.empty())
            {
                out.hasNormal = true;
                if (!ParseIndex(normToken, normCount, out.normal))
                    return false;
            }
            return out.hasUv || out.hasNormal;
        }

        glm::vec3 SafeNormalize(const glm::vec3& v, const glm::vec3& fallback)
        {
            const float lenSq = glm::dot(v, v);
            if (lenSq <= 1e-12f || !std::isfinite(lenSq))
                return fallback;
            return v * (1.0f / std::sqrt(lenSq));
        }

        glm::vec3 FallbackTangent(const glm::vec3& normal)
        {
            const glm::vec3 axis = std::abs(normal.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            return SafeNormalize(glm::cross(axis, normal), glm::vec3(1.0f, 0.0f, 0.0f));
        }

        void ComputeTangents(MeshData& mesh)
        {
            std::vector<glm::vec3> tan1(mesh.vertices.size(), glm::vec3(0.0f));
            std::vector<glm::vec3> tan2(mesh.vertices.size(), glm::vec3(0.0f));

            for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
            {
                const uint32_t i0 = mesh.indices[i];
                const uint32_t i1 = mesh.indices[i + 1];
                const uint32_t i2 = mesh.indices[i + 2];

                const auto& v0 = mesh.vertices[i0];
                const auto& v1 = mesh.vertices[i1];
                const auto& v2 = mesh.vertices[i2];

                const glm::vec3 e1 = v1.position - v0.position;
                const glm::vec3 e2 = v2.position - v0.position;
                const glm::vec2 d1 = v1.uv - v0.uv;
                const glm::vec2 d2 = v2.uv - v0.uv;

                const float det = d1.x * d2.y - d1.y * d2.x;
                if (std::abs(det) <= 1e-10f || !std::isfinite(det))
                    continue;

                const float invDet = 1.0f / det;
                const glm::vec3 sdir = (e1 * d2.y - e2 * d1.y) * invDet;
                const glm::vec3 tdir = (e2 * d1.x - e1 * d2.x) * invDet;

                tan1[i0] += sdir;
                tan1[i1] += sdir;
                tan1[i2] += sdir;

                tan2[i0] += tdir;
                tan2[i1] += tdir;
                tan2[i2] += tdir;
            }

            for (size_t i = 0; i < mesh.vertices.size(); ++i)
            {
                auto& vertex = mesh.vertices[i];
                const glm::vec3 n = SafeNormalize(vertex.normal, glm::vec3(0.0f, 1.0f, 0.0f));
                vertex.normal = n;

                const glm::vec3 t = tan1[i] - n * glm::dot(n, tan1[i]);
                if (glm::dot(t, t) > 1e-12f)
                {
                    const glm::vec3 ortho = glm::normalize(t);
                    const float handedness = glm::dot(glm::cross(n, ortho), tan2[i]) < 0.0f ? -1.0f : 1.0f;
                    vertex.tangent = glm::vec4(ortho, handedness);
                }
                else
                {
                    vertex.tangent = glm::vec4(FallbackTangent(n), 1.0f);
                }
            }
        }
    }

    ObjImportResult ParseObj(std::string_view source)
    {
        std::vector<glm::vec3> positions;
        std::vector<glm::vec3> colors;
        std::vector<glm::vec2> uvs;
        std::vector<glm::vec3> normals;

        MeshData mesh;
        std::unordered_map<VertexKey, uint32_t, VertexKeyHash> vertexCache;
        std::vector<VertexKey> faceCorners;

        size_t lineNumber = 0;
        size_t cursor = 0;
        while (cursor <= source.size())
        {
            const auto lineEnd = source.find('\n', cursor);
            const auto rawLine = lineEnd == std::string_view::npos
                ? source.substr(cursor)
                : source.substr(cursor, lineEnd - cursor);
            ++lineNumber;

            const auto commentPos = rawLine.find('#');
            auto line = Trim(commentPos == std::string_view::npos ? rawLine : rawLine.substr(0, commentPos));

            if (!line.empty())
            {
                const auto keyword = NextToken(line);
                if (keyword == "v")
                {
                    float x = 0, y = 0, z = 0;
                    if (!ParseFloat(NextToken(line), x) ||
                        !ParseFloat(NextToken(line), y) ||
                        !ParseFloat(NextToken(line), z))
                        return {{}, "OBJ line " + std::to_string(lineNumber) + ": invalid vertex position"};

                    glm::vec3 color(1.0f);
                    const auto rToken = NextToken(line);
                    if (!rToken.empty())
                    {
                        float r = 1, g = 1, b = 1;
                        if (!ParseFloat(rToken, r) ||
                            !ParseFloat(NextToken(line), g) ||
                            !ParseFloat(NextToken(line), b))
                            return {{}, "OBJ line " + std::to_string(lineNumber) + ": invalid vertex color"};
                        color = {r, g, b};
                    }
                    positions.emplace_back(x, y, z);
                    colors.push_back(color);
                }
                else if (keyword == "vt")
                {
                    float u = 0, v = 0;
                    if (!ParseFloat(NextToken(line), u) || !ParseFloat(NextToken(line), v))
                        return {{}, "OBJ line " + std::to_string(lineNumber) + ": invalid texture coordinate"};
                    uvs.emplace_back(u, v);
                }
                else if (keyword == "vn")
                {
                    float nx = 0, ny = 0, nz = 0;
                    if (!ParseFloat(NextToken(line), nx) ||
                        !ParseFloat(NextToken(line), ny) ||
                        !ParseFloat(NextToken(line), nz))
                        return {{}, "OBJ line " + std::to_string(lineNumber) + ": invalid vertex normal"};
                    normals.emplace_back(nx, ny, nz);
                }
                else if (keyword == "f")
                {
                    faceCorners.clear();
                    while (true)
                    {
                        const auto token = NextToken(line);
                        if (token.empty())
                            break;
                        VertexKey corner{};
                        if (!ParseFaceCorner(token, positions.size(), uvs.size(), normals.size(), corner))
                            return {{}, "OBJ line " + std::to_string(lineNumber) + ": invalid face index '" + std::string(token) + "'"};
                        faceCorners.push_back(corner);
                    }
                    if (faceCorners.size() < 3)
                        return {{}, "OBJ line " + std::to_string(lineNumber) + ": face must have at least 3 vertices"};

                    const auto emitVertex = [&](const VertexKey& corner, const glm::vec3& faceNormal) -> uint32_t {
                        if (corner.hasNormal)
                        {
                            if (const auto it = vertexCache.find(corner); it != vertexCache.end())
                                return it->second;
                        }

                        Vertex vertex{};
                        vertex.position = positions[corner.position];
                        vertex.color = colors[corner.position];
                        vertex.uv = corner.hasUv ? uvs[corner.uv] : glm::vec2(0.0f);
                        for (auto& channelUv : vertex.materialUv)
                            channelUv = vertex.uv;
                        vertex.normal = corner.hasNormal
                            ? SafeNormalize(normals[corner.normal], faceNormal)
                            : faceNormal;

                        const auto index = static_cast<uint32_t>(mesh.vertices.size());
                        mesh.vertices.push_back(vertex);
                        if (corner.hasNormal)
                            vertexCache.emplace(corner, index);
                        return index;
                    };

                    for (size_t i = 1; i + 1 < faceCorners.size(); ++i)
                    {
                        const auto& c0 = faceCorners[0];
                        const auto& c1 = faceCorners[i];
                        const auto& c2 = faceCorners[i + 1];

                        const glm::vec3 p0 = positions[c0.position];
                        const glm::vec3 p1 = positions[c1.position];
                        const glm::vec3 p2 = positions[c2.position];
                        const glm::vec3 faceNormal = SafeNormalize(glm::cross(p1 - p0, p2 - p0), glm::vec3(0.0f, 1.0f, 0.0f));

                        mesh.indices.push_back(emitVertex(c0, faceNormal));
                        mesh.indices.push_back(emitVertex(c1, faceNormal));
                        mesh.indices.push_back(emitVertex(c2, faceNormal));
                    }
                }
            }

            if (lineEnd == std::string_view::npos)
                break;
            cursor = lineEnd + 1;
        }

        if (mesh.vertices.empty() || mesh.indices.empty())
            return {{}, "OBJ contains no renderable faces"};

        ComputeTangents(mesh);
        if (!mesh.IsValid())
            return {{}, "OBJ produced invalid mesh geometry"};

        return {std::move(mesh), {}};
    }

    ObjImportResult ImportObj(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            const auto utf8 = path.u8string();
            return {{}, std::string(utf8.begin(), utf8.end()) + ": failed to open OBJ file"};
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        auto result = ParseObj(buffer.str());
        if (!result.mesh)
        {
            const auto utf8 = path.u8string();
            result.error = std::string(utf8.begin(), utf8.end()) + ": " + result.error;
        }
        return result;
    }
}
