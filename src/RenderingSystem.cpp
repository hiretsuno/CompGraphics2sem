#include "RenderingSystem.h"
#include "GBuffer.h"
#include "ShadowMap.h"
#include "ParticleSystem.h"
RenderingSystem::RenderingSystem() = default;
RenderingSystem::~RenderingSystem() = default;

#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <wincodec.h>
#include <objbase.h>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
    void ThrowIfFailed(HRESULT hr, const char* what)
    {
        if (FAILED(hr))
        {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "%s (hr=0x%08X)", what, static_cast<unsigned>(hr));
            throw std::runtime_error(buffer);
        }
    }

    uint32_t AlignConstantBufferSize(uint32_t size)
    {
        return (size + 255u) & ~255u;
    }

    D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type)
    {
        D3D12_HEAP_PROPERTIES props{};
        props.Type = type;
        props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        props.CreationNodeMask = 1;
        props.VisibleNodeMask = 1;
        return props;
    }

    D3D12_RESOURCE_DESC BufferDesc(UINT64 size)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Alignment = 0;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        return desc;
    }

    D3D12_RESOURCE_DESC TextureDesc2D(uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t mipLevels = 1)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = static_cast<UINT16>(mipLevels);
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        return desc;
    }

    std::string DirectoryName(const std::string& path)
    {
        const size_t pos = path.find_last_of("\\/");
        return (pos == std::string::npos) ? std::string() : path.substr(0, pos + 1);
    }

    std::string JoinPath(const std::string& left, const std::string& right)
    {
        if (left.empty()) return right;
        if (right.empty()) return left;
        if (left.back() == '/' || left.back() == '\\') return left + right;
        return left + "/" + right;
    }

    bool FileExistsA(const std::string& path)
    {
        const DWORD attributes = GetFileAttributesA(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    std::string ExeDirectoryA()
    {
        char buffer[MAX_PATH]{};
        const DWORD count = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
        return (count > 0 && count < MAX_PATH) ? DirectoryName(std::string(buffer)) : std::string();
    }

    std::string ResolveAssetPath(const std::string& name)
    {
        const std::string exeDir = ExeDirectoryA();
        const std::vector<std::string> candidates = {
            name,
            JoinPath("assets", name),
            JoinPath(exeDir, name),
            JoinPath(JoinPath(exeDir, "assets"), name),
            JoinPath("..", name),
            JoinPath("../..", name),
            JoinPath("models", name),
            JoinPath("sponza", name),
            JoinPath("shaders", name),
        };

        for (const auto& candidate : candidates)
        {
            if (FileExistsA(candidate))
                return candidate;
        }

        return name;
    }

    std::wstring ToWide(const std::string& text)
    {
        return std::wstring(text.begin(), text.end());
    }

    std::string Trim(std::string value)
    {
        const size_t begin = value.find_first_not_of(" \t\r\n");
        const size_t end = value.find_last_not_of(" \t\r\n");
        return (begin == std::string::npos) ? std::string() : value.substr(begin, end - begin + 1);
    }

    struct Image
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> bgra;
    };

    bool LoadImageTga(const std::string& path, Image& out)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;

        uint8_t header[18]{};
        file.read(reinterpret_cast<char*>(header), sizeof(header));
        if (!file)
            return false;

        const uint8_t idLength = header[0];
        const uint8_t colorMapType = header[1];
        const uint8_t imageType = header[2];
        const uint16_t width = static_cast<uint16_t>(header[12] | (header[13] << 8));
        const uint16_t height = static_cast<uint16_t>(header[14] | (header[15] << 8));
        const uint8_t bitsPerPixel = header[16];
        const uint8_t descriptor = header[17];

        if (colorMapType != 0 || width == 0 || height == 0)
            return false;
        if (bitsPerPixel != 24 && bitsPerPixel != 32)
            return false;
        if (imageType != 2 && imageType != 10)
            return false;

        if (idLength > 0)
            file.seekg(idLength, std::ios::cur);

        const uint32_t bytesPerPixel = bitsPerPixel / 8;
        const uint32_t pixelCount = static_cast<uint32_t>(width) * static_cast<uint32_t>(height);

        out.width = width;
        out.height = height;
        out.bgra.assign(static_cast<size_t>(pixelCount) * 4u, 255u);

        auto writePixel = [&](uint32_t index, const uint8_t* pixel)
        {
            const size_t offset = static_cast<size_t>(index) * 4u;
            out.bgra[offset + 0] = pixel[0];
            out.bgra[offset + 1] = pixel[1];
            out.bgra[offset + 2] = pixel[2];
            out.bgra[offset + 3] = (bytesPerPixel == 4) ? pixel[3] : 255u;
        };

        std::vector<uint8_t> temp(bytesPerPixel);
        if (imageType == 2)
        {
            std::vector<uint8_t> pixels(static_cast<size_t>(pixelCount) * bytesPerPixel);
            file.read(reinterpret_cast<char*>(pixels.data()), pixels.size());
            if (!file)
                return false;

            for (uint32_t i = 0; i < pixelCount; ++i)
                writePixel(i, &pixels[static_cast<size_t>(i) * bytesPerPixel]);
        }
        else
        {
            for (uint32_t i = 0; i < pixelCount;)
            {
                uint8_t packet = 0;
                file.read(reinterpret_cast<char*>(&packet), 1);
                if (!file)
                    return false;

                const uint32_t count = (packet & 0x7Fu) + 1u;
                if (packet & 0x80u)
                {
                    file.read(reinterpret_cast<char*>(temp.data()), bytesPerPixel);
                    if (!file)
                        return false;

                    for (uint32_t k = 0; k < count && i < pixelCount; ++k, ++i)
                        writePixel(i, temp.data());
                }
                else
                {
                    for (uint32_t k = 0; k < count && i < pixelCount; ++k, ++i)
                    {
                        file.read(reinterpret_cast<char*>(temp.data()), bytesPerPixel);
                        if (!file)
                            return false;
                        writePixel(i, temp.data());
                    }
                }
            }
        }

        if ((descriptor & 0x20u) == 0)
        {
            const uint32_t rowBytes = static_cast<uint32_t>(width) * 4u;
            std::vector<uint8_t> row(rowBytes);
            for (uint32_t y = 0; y < height / 2; ++y)
            {
                uint8_t* top = out.bgra.data() + static_cast<size_t>(y) * rowBytes;
                uint8_t* bottom = out.bgra.data() + static_cast<size_t>(height - 1 - y) * rowBytes;
                std::memcpy(row.data(), top, rowBytes);
                std::memcpy(top, bottom, rowBytes);
                std::memcpy(bottom, row.data(), rowBytes);
            }
        }

        return true;
    }

    bool LoadImageWic(const std::string& path, Image& out)
    {
        static bool comInitialized = false;
        if (!comInitialized)
        {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            comInitialized = true;
        }

        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        {
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
                return false;
        }

        const std::wstring widePath(path.begin(), path.end());
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromFilename(widePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
            return false;

        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame)))
            return false;

        UINT width = 0;
        UINT height = 0;
        frame->GetSize(&width, &height);
        if (width == 0 || height == 0)
            return false;

        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(&converter)))
            return false;

        if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom)))
            return false;

        out.width = width;
        out.height = height;
        out.bgra.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
        return SUCCEEDED(converter->CopyPixels(nullptr, width * 4u, static_cast<UINT>(out.bgra.size()), out.bgra.data()));
    }

    std::string LowercaseExtension(const std::string& path)
    {
        const size_t dot = path.find_last_of('.');
        if (dot == std::string::npos)
            return {};

        std::string extension = path.substr(dot);
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return extension;
    }

    bool LoadImage(const std::string& path, Image& out)
    {
        return LowercaseExtension(path) == ".tga" ? LoadImageTga(path, out) : LoadImageWic(path, out);
    }

    struct MtlData
    {
        std::string diffusePath;
        std::string normalPath;     // map_Disp / map_Bump / map_bump / norm — тангенциальная normal map
        std::string metalRoughPath; // map_MR (проектный, не Wavefront-стандарт): G = roughness, B = metallic, как в glTF metallicRoughnessTexture
        XMFLOAT3 kd{ 1.f, 1.f, 1.f };
        XMFLOAT3 ks{ 0.18f, 0.18f, 0.18f };
        float ns = 32.f;
        float metallic = 0.f;     // Pm (PBR-расширение MTL). Если есть map_MR, это множитель на канал B, иначе константа.
        float roughness = -1.f;   // Pr; если < 0 и нет map_MR, выводим из Ns (см. BuildGeometry)
    };

    // Последний токен строки — путь к текстуре (опции вида -bm 1.0 идут раньше).
    std::string LastToken(std::istringstream& stream)
    {
        std::string token, last;
        while (stream >> token)
            last = token;
        return last;
    }

    std::unordered_map<std::string, MtlData> LoadMtlData(const std::string& mtlPath)
    {
        std::unordered_map<std::string, MtlData> materials;
        std::ifstream file(mtlPath);
        if (!file.is_open())
            return materials;

        const std::string baseDir = DirectoryName(mtlPath);
        std::string line;
        std::string currentMaterial;

        while (std::getline(file, line))
        {
            if (line.empty() || line[0] == '#')
                continue;

            std::istringstream stream(line);
            std::string command;
            stream >> command;

            if (command == "newmtl")
            {
                stream >> currentMaterial;
            }
            else if (command == "Kd" && !currentMaterial.empty())
            {
                stream >> materials[currentMaterial].kd.x >> materials[currentMaterial].kd.y >> materials[currentMaterial].kd.z;
            }
            else if (command == "Ks" && !currentMaterial.empty())
            {
                stream >> materials[currentMaterial].ks.x >> materials[currentMaterial].ks.y >> materials[currentMaterial].ks.z;
            }
            else if (command == "Ns" && !currentMaterial.empty())
            {
                stream >> materials[currentMaterial].ns;
            }
            else if (command == "Pm" && !currentMaterial.empty())
            {
                stream >> materials[currentMaterial].metallic;
            }
            else if (command == "Pr" && !currentMaterial.empty())
            {
                stream >> materials[currentMaterial].roughness;
            }
            else if ((command == "map_Disp" || command == "map_Bump" || command == "map_bump" || command == "bump" || command == "norm") && !currentMaterial.empty())
            {
                const std::string last = LastToken(stream);
                if (!last.empty())
                    materials[currentMaterial].normalPath = JoinPath(baseDir, last);
            }
            else if (command == "map_MR" && !currentMaterial.empty())
            {
                const std::string last = LastToken(stream);
                if (!last.empty())
                    materials[currentMaterial].metalRoughPath = JoinPath(baseDir, last);
            }
            else if (command == "map_Kd" && !currentMaterial.empty())
            {
                std::string token;
                std::string last;
                while (stream >> token)
                    last = token;
                if (!last.empty())
                    materials[currentMaterial].diffusePath = JoinPath(baseDir, last);
            }
        }

        return materials;
    }

    struct ObjKey
    {
        int p = -1;
        int t = -1;
        int n = -1;

        bool operator==(const ObjKey& other) const
        {
            return p == other.p && t == other.t && n == other.n;
        }
    };

    struct ObjKeyHash
    {
        size_t operator()(const ObjKey& key) const noexcept
        {
            return static_cast<size_t>(key.p) * 73856093u ^ static_cast<size_t>(key.t) * 19349663u ^ static_cast<size_t>(key.n) * 83492791u;
        }
    };

    int FixIndex(int value, int size)
    {
        if (value > 0) return value - 1;
        if (value < 0) return size + value;
        return -1;
    }

    void ParseFaceToken(const std::string& token, int& p, int& t, int& n)
    {
        p = t = n = 0;

        const size_t firstSlash = token.find('/');
        if (firstSlash == std::string::npos)
        {
            p = std::stoi(token);
            return;
        }

        if (firstSlash > 0)
            p = std::stoi(token.substr(0, firstSlash));

        const size_t secondSlash = token.find('/', firstSlash + 1);
        if (secondSlash == std::string::npos)
        {
            if (firstSlash + 1 < token.size())
                t = std::stoi(token.substr(firstSlash + 1));
            return;
        }

        if (secondSlash > firstSlash + 1)
            t = std::stoi(token.substr(firstSlash + 1, secondSlash - firstSlash - 1));
        if (secondSlash + 1 < token.size())
            n = std::stoi(token.substr(secondSlash + 1));
    }

    struct ObjGroup
    {
        uint32_t start = 0;
        uint32_t count = 0;
        std::string material;
    };

    struct ObjMesh
    {
        std::vector<RenderingSystem::Vertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<ObjGroup> groups;
        std::unordered_map<std::string, MtlData> materials;
    };

    bool LoadObj(const std::string& objPath, ObjMesh& out)
    {
        std::ifstream file(objPath);
        if (!file.is_open())
            return false;

        const std::string baseDir = DirectoryName(objPath);

        std::vector<XMFLOAT3> positions;
        std::vector<XMFLOAT3> normals;
        std::vector<XMFLOAT2> texcoords;
        positions.reserve(200000);
        normals.reserve(200000);
        texcoords.reserve(200000);

        std::unordered_map<ObjKey, uint32_t, ObjKeyHash> vertexMap;
        std::vector<std::string> materialLibraries;
        std::string currentMaterial;
        std::string line;

        auto switchMaterial = [&](const std::string& newMaterial)
        {
            if (!out.groups.empty() && currentMaterial != newMaterial)
                out.groups.back().count = static_cast<uint32_t>(out.indices.size()) - out.groups.back().start;

            if (out.groups.empty() || currentMaterial != newMaterial)
            {
                currentMaterial = newMaterial;
                out.groups.push_back({ static_cast<uint32_t>(out.indices.size()), 0u, currentMaterial });
            }
        };

        while (std::getline(file, line))
        {
            if (line.empty() || line[0] == '#')
                continue;

            if (line.rfind("mtllib ", 0) == 0)
            {
                std::istringstream stream(line);
                std::string command;
                std::string rest;
                stream >> command;
                std::getline(stream, rest);
                std::istringstream names(Trim(rest));
                std::string name;
                while (names >> name)
                    materialLibraries.push_back(JoinPath(baseDir, name));
                continue;
            }

            if (line.rfind("usemtl ", 0) == 0)
            {
                std::istringstream stream(line);
                std::string command;
                std::string rest;
                stream >> command;
                std::getline(stream, rest);
                rest = Trim(rest);
                if (!rest.empty())
                    switchMaterial(rest);
                continue;
            }

            std::istringstream stream(line);
            std::string tag;
            stream >> tag;

            if (tag == "v")
            {
                XMFLOAT3 position{};
                stream >> position.x >> position.y >> position.z;
                positions.push_back(position);
            }
            else if (tag == "vn")
            {
                XMFLOAT3 normal{};
                stream >> normal.x >> normal.y >> normal.z;
                normals.push_back(normal);
            }
            else if (tag == "vt")
            {
                XMFLOAT2 uv{};
                stream >> uv.x >> uv.y;
                uv.y = 1.f - uv.y;
                texcoords.push_back(uv);
            }
            else if (tag == "f")
            {
                if (out.groups.empty())
                    out.groups.push_back({ static_cast<uint32_t>(out.indices.size()), 0u, currentMaterial });

                std::vector<uint32_t> face;
                face.reserve(8);
                std::string token;
                while (stream >> token)
                {
                    int p = 0;
                    int t = 0;
                    int n = 0;
                    ParseFaceToken(token, p, t, n);
                    p = FixIndex(p, static_cast<int>(positions.size()));
                    t = FixIndex(t, static_cast<int>(texcoords.size()));
                    n = FixIndex(n, static_cast<int>(normals.size()));
                    if (p < 0)
                        continue;

                    const ObjKey key{ p, t, n };
                    const auto it = vertexMap.find(key);
                    if (it == vertexMap.end())
                    {
                        RenderingSystem::Vertex vertex{};
                        vertex.Pos = positions[p];
                        vertex.Normal = (n >= 0) ? normals[n] : XMFLOAT3(0.f, 1.f, 0.f);
                        vertex.TexC = (t >= 0) ? texcoords[t] : XMFLOAT2(0.f, 0.f);
                        const uint32_t newIndex = static_cast<uint32_t>(out.vertices.size());
                        out.vertices.push_back(vertex);
                        vertexMap.emplace(key, newIndex);
                        face.push_back(newIndex);
                    }
                    else
                    {
                        face.push_back(it->second);
                    }
                }

                for (size_t i = 1; i + 1 < face.size(); ++i)
                {
                    out.indices.push_back(face[0]);
                    out.indices.push_back(face[i]);
                    out.indices.push_back(face[i + 1]);
                }
            }
        }

        if (!out.groups.empty())
            out.groups.back().count = static_cast<uint32_t>(out.indices.size()) - out.groups.back().start;

        for (const auto& library : materialLibraries)
        {
            auto materialData = LoadMtlData(library);
            out.materials.insert(materialData.begin(), materialData.end());
        }

        return !out.vertices.empty() && !out.indices.empty();
    }

    // Дозагружает .obj и вливает его в dst с трансформом (своя позиция/масштаб/поворот в сцене),
    // не трогая уже накопленную геометрию dst. Индексы src сдвигаются на текущий размер dst.vertices,
    // группы src — на текущий размер dst.indices. Отсутствие файла не фатально: сцена просто рисуется без объекта.
    bool AppendObjMesh(ObjMesh& dst, const std::string& path, CXMMATRIX transform)
    {
        ObjMesh src{};
        if (!LoadObj(path, src))
        {
            OutputDebugStringA(("BuildGeometry: cannot load " + path + ", skipping\n").c_str());
            return false;
        }

        const uint32_t vertexOffset = static_cast<uint32_t>(dst.vertices.size());
        const uint32_t indexOffset = static_cast<uint32_t>(dst.indices.size());

        dst.vertices.reserve(dst.vertices.size() + src.vertices.size());
        for (const RenderingSystem::Vertex& v : src.vertices)
        {
            RenderingSystem::Vertex out = v;
            XMStoreFloat3(&out.Pos, XMVector3TransformCoord(XMLoadFloat3(&v.Pos), transform));
            XMStoreFloat3(&out.Normal, XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&v.Normal), transform)));
            dst.vertices.push_back(out);
        }

        dst.indices.reserve(dst.indices.size() + src.indices.size());
        for (uint32_t index : src.indices)
            dst.indices.push_back(index + vertexOffset);

        for (const ObjGroup& group : src.groups)
            dst.groups.push_back({ group.start + indexOffset, group.count, group.material });

        dst.materials.insert(src.materials.begin(), src.materials.end());
        return true;
    }

    // Количество mip-уровней для полной цепочки до 1x1.
    uint32_t MipCount(uint32_t width, uint32_t height)
    {
        uint32_t count = 1;
        while (width > 1 || height > 1)
        {
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
            ++count;
        }
        return count;
    }

    // Следующий mip: усредняем блок 2x2 (box-фильтр), края зажимаем — работает и для не-степеней двойки.
    Image Downsample(const Image& src)
    {
        Image dst;
        dst.width = std::max(1u, src.width / 2);
        dst.height = std::max(1u, src.height / 2);
        dst.bgra.resize(static_cast<size_t>(dst.width) * dst.height * 4u);

        for (uint32_t y = 0; y < dst.height; ++y)
        {
            for (uint32_t x = 0; x < dst.width; ++x)
            {
                const uint32_t x0 = std::min(x * 2, src.width - 1), x1 = std::min(x * 2 + 1, src.width - 1);
                const uint32_t y0 = std::min(y * 2, src.height - 1), y1 = std::min(y * 2 + 1, src.height - 1);
                for (uint32_t c = 0; c < 4; ++c)
                {
                    const uint32_t sum =
                        src.bgra[(static_cast<size_t>(y0) * src.width + x0) * 4u + c] +
                        src.bgra[(static_cast<size_t>(y0) * src.width + x1) * 4u + c] +
                        src.bgra[(static_cast<size_t>(y1) * src.width + x0) * 4u + c] +
                        src.bgra[(static_cast<size_t>(y1) * src.width + x1) * 4u + c];
                    dst.bgra[(static_cast<size_t>(y) * dst.width + x) * 4u + c] = static_cast<uint8_t>((sum + 2) / 4);
                }
            }
        }
        return dst;
    }

    void UploadTexture(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* texture,
        const Image& image,
        std::vector<ComPtr<ID3D12Resource>>& uploadResources)
    {
        auto uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        const uint32_t mipCount = MipCount(image.width, image.height);
        const D3D12_RESOURCE_DESC textureDesc = TextureDesc2D(image.width, image.height, DXGI_FORMAT_B8G8R8A8_UNORM, mipCount);

        // Цепочка mip-уровней на CPU: level 0 = исходник, дальше каждый вдвое меньше.
        std::vector<Image> mips(mipCount);
        mips[0] = image;
        for (uint32_t m = 1; m < mipCount; ++m)
            mips[m] = Downsample(mips[m - 1]);

        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mipCount);
        UINT64 totalBytes = 0;
        device->GetCopyableFootprints(&textureDesc, 0, mipCount, 0, footprints.data(), nullptr, nullptr, &totalBytes);

        ComPtr<ID3D12Resource> uploadBuffer;
        const D3D12_RESOURCE_DESC uploadDesc = BufferDesc(totalBytes);
        ThrowIfFailed(
            device->CreateCommittedResource(
                &uploadHeap,
                D3D12_HEAP_FLAG_NONE,
                &uploadDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&uploadBuffer)),
            "Create texture upload buffer");

        void* mapped = nullptr;
        D3D12_RANGE readRange{ 0, 0 };
        ThrowIfFailed(uploadBuffer->Map(0, &readRange, &mapped), "Map texture upload buffer");

        // Строки каждого mip кладём в upload-буфер по его footprint (с учётом row pitch).
        for (uint32_t m = 0; m < mipCount; ++m)
        {
            const Image& level = mips[m];
            const uint32_t srcRowPitch = level.width * 4u;
            uint8_t* dstBase = static_cast<uint8_t*>(mapped) + footprints[m].Offset;
            for (uint32_t y = 0; y < level.height; ++y)
            {
                std::memcpy(
                    dstBase + static_cast<size_t>(y) * footprints[m].Footprint.RowPitch,
                    level.bgra.data() + static_cast<size_t>(y) * srcRowPitch,
                    srcRowPitch);
            }
        }
        uploadBuffer->Unmap(0, nullptr);

        for (uint32_t m = 0; m < mipCount; ++m)
        {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = texture;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = m;

            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = uploadBuffer.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = footprints[m];

            commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &barrier);

        uploadResources.push_back(uploadBuffer);
    }

    // ---------------- IBL: минимальный загрузчик DDS ----------------
    // Только несжатые форматы и DX10-заголовок (texconv -dx10). Порядок данных в файле:
    // для каждой грани/слайса — все её mip-уровни подряд; это совпадает с индексом subresource в D3D12.
    struct DdsHeader
    {
        uint32_t magic, size, flags, height, width, pitchOrLinearSize, depth, mipCount, reserved1[11];
        uint32_t pfSize, pfFlags, pfFourCC, pfBitCount, pfRMask, pfGMask, pfBMask, pfAMask;
        uint32_t caps, caps2, caps3, caps4, reserved2;
    };

    struct DdsHeaderDx10
    {
        uint32_t format, dimension, miscFlag, arraySize, miscFlags2;
    };

    struct DdsImage
    {
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint32_t width = 0, height = 0, mipLevels = 1, arraySize = 1;
        bool isCube = false;
        std::vector<uint8_t> data;   // все subresource подряд, без выравнивания строк
    };

    uint32_t BytesPerPixel(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
        case DXGI_FORMAT_R32G32_FLOAT:       return 8;
        case DXGI_FORMAT_R16G16_FLOAT:       return 4;
        case DXGI_FORMAT_R8G8B8A8_UNORM:     return 4;
        default:                             return 0;
        }
    }

    bool LoadDds(const std::string& path, DdsImage& out)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
            return false;

        DdsHeader header{};
        DdsHeaderDx10 dx10{};
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!file || header.magic != 0x20534444u || header.pfFourCC != 0x30315844u)   // 'DDS ' и 'DX10'
            return false;
        file.read(reinterpret_cast<char*>(&dx10), sizeof(dx10));
        if (!file)
            return false;

        out.format = static_cast<DXGI_FORMAT>(dx10.format);
        out.width = header.width;
        out.height = header.height;
        out.mipLevels = std::max(1u, header.mipCount);
        out.isCube = (dx10.miscFlag & 0x4u) != 0;           // D3D11_RESOURCE_MISC_TEXTURECUBE
        out.arraySize = out.isCube ? 6u : std::max(1u, dx10.arraySize);

        const uint32_t bpp = BytesPerPixel(out.format);
        if (bpp == 0)
            return false;

        size_t total = 0;
        for (uint32_t w = out.width, h = out.height, m = 0; m < out.mipLevels; ++m, w = std::max(1u, w / 2), h = std::max(1u, h / 2))
            total += static_cast<size_t>(w) * h * bpp;
        total *= out.arraySize;

        out.data.resize(total);
        file.read(reinterpret_cast<char*>(out.data.data()), static_cast<std::streamsize>(total));
        return static_cast<bool>(file);
    }

    // Заглушка 1x1, если файла нет: кубмап одного цвета (равномерное "небо", радиансу = цвет во все стороны)
    // и LUT (scale=1, bias=0). Для равномерного окружения irradiance == prefiltered == этот цвет.
    DdsImage MakeFallbackCube()
    {
        const float sky[4] = { 0.12f, 0.14f, 0.18f, 1.f };
        DdsImage image;
        image.format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        image.width = image.height = 1;
        image.arraySize = 6;
        image.isCube = true;
        image.data.resize(6 * sizeof(sky));
        for (int face = 0; face < 6; ++face)
            std::memcpy(image.data.data() + face * sizeof(sky), sky, sizeof(sky));
        return image;
    }

    DdsImage MakeFallbackLut()
    {
        DdsImage image;
        image.format = DXGI_FORMAT_R16G16_FLOAT;
        image.width = image.height = 1;
        image.data = { 0x00, 0x3C, 0x00, 0x00 };   // half(1.0), half(0.0)
        return image;
    }

    ComPtr<ID3D12Resource> UploadDds(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* commandList,
        const DdsImage& image,
        std::vector<ComPtr<ID3D12Resource>>& uploadResources)
    {
        D3D12_RESOURCE_DESC desc = TextureDesc2D(image.width, image.height, image.format, image.mipLevels);
        desc.DepthOrArraySize = static_cast<UINT16>(image.arraySize);

        auto defaultHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
        ComPtr<ID3D12Resource> texture;
        ThrowIfFailed(
            device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)),
            "Create IBL texture");

        const uint32_t subresourceCount = image.mipLevels * image.arraySize;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresourceCount);
        std::vector<UINT> numRows(subresourceCount);
        std::vector<UINT64> rowSizes(subresourceCount);
        UINT64 totalBytes = 0;
        device->GetCopyableFootprints(&desc, 0, subresourceCount, 0, footprints.data(), numRows.data(), rowSizes.data(), &totalBytes);

        auto uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        const D3D12_RESOURCE_DESC uploadDesc = BufferDesc(totalBytes);
        ComPtr<ID3D12Resource> uploadBuffer;
        ThrowIfFailed(
            device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer)),
            "Create IBL upload buffer");

        uint8_t* mapped = nullptr;
        D3D12_RANGE readRange{ 0, 0 };
        ThrowIfFailed(uploadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mapped)), "Map IBL upload buffer");

        const uint8_t* src = image.data.data();
        for (uint32_t s = 0; s < subresourceCount; ++s)
        {
            for (UINT row = 0; row < numRows[s]; ++row)
            {
                std::memcpy(mapped + footprints[s].Offset + static_cast<size_t>(row) * footprints[s].Footprint.RowPitch, src, static_cast<size_t>(rowSizes[s]));
                src += rowSizes[s];
            }
        }
        uploadBuffer->Unmap(0, nullptr);

        for (uint32_t s = 0; s < subresourceCount; ++s)
        {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = texture.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = s;

            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.pResource = uploadBuffer.Get();
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLoc.PlacedFootprint = footprints[s];

            commandList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
        }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &barrier);

        uploadResources.push_back(uploadBuffer);
        return texture;
    }

    float Clamp01(float value)
    {
        return std::max(0.f, std::min(1.f, value));
    }
}

bool RenderingSystem::Initialize(HWND hwnd, uint32_t width, uint32_t height)
{
    m_hwnd = hwnd;
    m_width = width;
    m_height = height;

#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();
#endif

    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&m_factory)), "CreateDXGIFactory1");

    CreateDevice();
    CreateCommandObjects();

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "CreateFence");
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
        throw std::runtime_error("CreateEvent failed");

    CreateSwapChain();

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    CreateBackBufferHeap();
    CreateBackBufferRTVs();

    m_viewport = { 0.f, 0.f, static_cast<float>(m_width), static_cast<float>(m_height), 0.f, 1.f };
    m_scissorRect = { 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };

    // Sponza в сантиметрах -> метры.
    XMStoreFloat4x4(&m_world, XMMatrixScaling(0.01f, 0.01f, 0.01f));
    SetCamera(m_eyePos, 1.f, 0.f);

    const float aspect = (m_height > 0) ? static_cast<float>(m_width) / static_cast<float>(m_height) : 1.f;
    XMStoreFloat4x4(&m_proj, XMMatrixPerspectiveFovLH(kFovY, aspect, kNearZ, kFarZ));

    BuildShaders();
    BuildRootSignature();
    BuildPostRootSignature();
    BuildGeometry();
    BuildFrameResources();

    m_gBuffer = std::make_unique<GBuffer>();
    m_gBuffer->Initialize(m_device.Get(), m_width, m_height);
    CreateSceneColor();   // после GBuffer: SRV пишется в его кучу
    BuildIblResources();  // тоже после GBuffer: SRV t5..t7 лежат в его куче

    m_shadowMap = std::make_unique<ShadowMap>();
    m_shadowMap->Initialize(m_device.Get(), m_gBuffer->GetShadowSrvCpu());

    m_particles = std::make_unique<ParticleSystem>();
    m_particles->Initialize(m_device.Get(), m_commandQueue.Get(), ToWide(ResolveAssetPath("shaders/ParticleCS.hlsl")));

    CreateSceneLights();
    UpdatePassConstants();
    UpdateLightConstants(0.f);
    BuildPSOs();

    m_initialized = true;
    return true;
}

void RenderingSystem::Shutdown()
{
    if (m_commandQueue)
        FlushCommandQueue();

    if (m_passConstantBuffer && m_mappedPassConstants)
    {
        m_passConstantBuffer->Unmap(0, nullptr);
        m_mappedPassConstants = nullptr;
    }

    if (m_lightConstantBuffer && m_mappedLightConstants)
    {
        m_lightConstantBuffer->Unmap(0, nullptr);
        m_mappedLightConstants = nullptr;
    }

    if (m_postConstantBuffer && m_mappedPostConstants)
    {
        m_postConstantBuffer->Unmap(0, nullptr);
        m_mappedPostConstants = nullptr;
    }

    if (m_particles)
    {
        m_particles->Shutdown();
        m_particles.reset();
    }

    if (m_shadowMap)
    {
        m_shadowMap->Shutdown();
        m_shadowMap.reset();
    }

    if (m_gBuffer)
    {
        m_gBuffer->Shutdown();
        m_gBuffer.reset();
    }

    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
}

void RenderingSystem::OnResize(uint32_t width, uint32_t height)
{
    if (!m_initialized || width == 0 || height == 0)
        return;

    m_width = width;
    m_height = height;

    FlushCommandQueue();

    for (auto& buffer : m_backBuffers)
        buffer.Reset();

    ThrowIfFailed(
        m_swapChain->ResizeBuffers(SwapChainBufferCount, m_width, m_height, DXGI_FORMAT_R8G8B8A8_UNORM, 0),
        "ResizeBuffers");

    m_backBufferIndex = 0;
    CreateBackBufferRTVs();

    if (m_gBuffer)
    {
        m_gBuffer->Resize(m_device.Get(), m_width, m_height);
        CreateSceneColor();
    }

    m_viewport = { 0.f, 0.f, static_cast<float>(m_width), static_cast<float>(m_height), 0.f, 1.f };
    m_scissorRect = { 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };

    const float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    XMStoreFloat4x4(&m_proj, XMMatrixPerspectiveFovLH(kFovY, aspect, kNearZ, kFarZ));

    UpdatePassConstants();
}

void RenderingSystem::Draw(float dt)
{
    if (!m_initialized)
        return;

    UpdatePassConstants();
    UpdateLightConstants(dt);

    const float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    m_shadowMap->UpdateCascades(m_view, kFovY, aspect, kNearZ, kShadowDistance, m_sunDir);


    ThrowIfFailed(m_commandAllocator->Reset(), "Reset command allocator");
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr), "Reset command list");

    // --- Particle compute pass: обновить частицы и родить новые (всё на GPU) ---
    // Барьеры UAV -> vertex buffer / indirect args делает PrepareDraw; сама отрисовка — после lighting pass.
    m_particles->Simulate(m_commandList.Get(), dt);
    m_particles->PrepareDraw(m_commandList.Get());

    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissorRect);
    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());

    // --- Shadow pass: рисуем сцену глубиной в каждый каскад ---
    m_shadowMap->TransitionToWrite(m_commandList.Get());

    const D3D12_VIEWPORT shadowViewport = { 0.f, 0.f, static_cast<float>(ShadowMap::Size), static_cast<float>(ShadowMap::Size), 0.f, 1.f };
    const D3D12_RECT shadowScissor = { 0, 0, static_cast<LONG>(ShadowMap::Size), static_cast<LONG>(ShadowMap::Size) };
    m_commandList->RSSetViewports(1, &shadowViewport);
    m_commandList->RSSetScissorRects(1, &shadowScissor);

    m_commandList->SetPipelineState(m_shadowPSO.Get());
    m_commandList->SetGraphicsRootConstantBufferView(0, m_passConstantBuffer->GetGPUVirtualAddress());   // World
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    m_commandList->IASetIndexBuffer(&m_indexBufferView);

    for (uint32_t c = 0; c < ShadowMap::CascadeCount; ++c)
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE cascadeDsv = m_shadowMap->GetDsv(c);
        m_commandList->OMSetRenderTargets(0, nullptr, FALSE, &cascadeDsv);
        m_commandList->ClearDepthStencilView(cascadeDsv, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);
        m_commandList->SetGraphicsRoot32BitConstants(6, 16, &m_shadowMap->GetConstants().LightViewProj[c], 0);

        for (const DrawItem& drawItem : m_drawItems)
            m_commandList->DrawIndexedInstanced(drawItem.IndexCount, 1, drawItem.StartIndexLocation, 0, 0);
    }

    m_shadowMap->TransitionToRead(m_commandList.Get());

    //Geometry pass: пишем в G-buffer
    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissorRect);
    m_gBuffer->TransitionToWrite(m_commandList.Get());
    m_gBuffer->BindForGeometryPass(m_commandList.Get());

    m_commandList->SetPipelineState(m_geometryPSO.Get());
    m_commandList->SetGraphicsRootConstantBufferView(0, m_passConstantBuffer->GetGPUVirtualAddress());

    ID3D12DescriptorHeap* geometryHeaps[] = { m_textureHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, geometryHeaps);
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    m_commandList->IASetIndexBuffer(&m_indexBufferView);

    const auto textureHeapStart = m_textureHeap->GetGPUDescriptorHandleForHeapStart();
    for (const DrawItem& drawItem : m_drawItems)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = textureHeapStart;
        textureHandle.ptr += static_cast<UINT64>(drawItem.TextureIndex) * m_srvDescriptorSize;

        D3D12_GPU_DESCRIPTOR_HANDLE normalHandle = textureHeapStart;
        normalHandle.ptr += static_cast<UINT64>(drawItem.NormalIndex) * m_srvDescriptorSize;

        D3D12_GPU_DESCRIPTOR_HANDLE metalRoughHandle = textureHeapStart;
        metalRoughHandle.ptr += static_cast<UINT64>(drawItem.MetalRoughIndex) * m_srvDescriptorSize;

        m_commandList->SetGraphicsRootDescriptorTable(1, textureHandle);      // t0: albedo
        m_commandList->SetGraphicsRootDescriptorTable(7, normalHandle);       // t9: normal map
        m_commandList->SetGraphicsRootDescriptorTable(8, metalRoughHandle);   // t10: map_MR
        m_commandList->SetGraphicsRoot32BitConstants(2, 8, &drawItem.Material, 0);
        m_commandList->DrawIndexedInstanced(
            drawItem.IndexCount, 1, drawItem.StartIndexLocation, 0, 0);
    }

    //Lighting pass: читаем G-buffer, пишем в SceneColor (не в back buffer: результат нужен постпроцессу как текстура)
    m_gBuffer->TransitionToRead(m_commandList.Get());

    D3D12_RESOURCE_BARRIER sceneToRt{};
    sceneToRt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    sceneToRt.Transition.pResource   = m_sceneColor.Get();
    sceneToRt.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    sceneToRt.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    sceneToRt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &sceneToRt);

    const D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv = m_sceneRtvHeap->GetCPUDescriptorHandleForHeapStart();
    const float clearColor[4] = { 0.f, 0.f, 0.f, 1.f };
    m_commandList->OMSetRenderTargets(1, &sceneRtv, TRUE, nullptr);
    m_commandList->ClearRenderTargetView(sceneRtv, clearColor, 0, nullptr);

    m_commandList->SetPipelineState(m_lightingPSO.Get());
    m_commandList->SetGraphicsRootConstantBufferView(0, m_passConstantBuffer->GetGPUVirtualAddress());
    m_commandList->SetGraphicsRootConstantBufferView(3, m_lightConstantBuffer->GetGPUVirtualAddress());
    m_commandList->SetGraphicsRootConstantBufferView(5, m_shadowMap->GetConstantsGpuAddress());

    ID3D12DescriptorHeap* lightingHeaps[] = { m_gBuffer->GetSrvHeap() };
    m_commandList->SetDescriptorHeaps(1, lightingHeaps);
    m_commandList->SetGraphicsRootDescriptorTable(4, m_gBuffer->GetSrvTable());
    // Таблица текстур (param 1) осталась от geometry pass и указывает в другую кучу. Lighting-шейдер её не читает,
    // но debug layer требует, чтобы она указывала в текущую кучу — даём любой валидный дескриптор из неё.
    m_commandList->SetGraphicsRootDescriptorTable(1, m_gBuffer->GetSrvTable());
    m_commandList->SetGraphicsRootDescriptorTable(7, m_gBuffer->GetSrvTable());   // то же самое для таблицы normal map
    m_commandList->SetGraphicsRootDescriptorTable(8, m_gBuffer->GetSrvTable());   // и для таблицы map_MR
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->DrawInstanced(3, 1, 0, 0);

    //Particle render pass: точки -> GS-билборды, depth-тест по глубине сцены
    const D3D12_CPU_DESCRIPTOR_HANDLE sceneDsv = m_gBuffer->GetDsv();
    m_commandList->OMSetRenderTargets(1, &sceneRtv, FALSE, &sceneDsv);
    m_commandList->SetPipelineState(m_particlePSO.Get());
    m_commandList->SetGraphicsRootConstantBufferView(0, m_passConstantBuffer->GetGPUVirtualAddress());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    const D3D12_VERTEX_BUFFER_VIEW particleVbv = m_particles->GetVertexBufferView();
    m_commandList->IASetVertexBuffers(0, 1, &particleVbv);
    // Сколько точек рисовать — знает только GPU (счётчик в буфере аргументов), поэтому ExecuteIndirect.
    m_commandList->ExecuteIndirect(m_particles->GetDrawSignature(), 1, m_particles->GetIndirectArgs(), 0, nullptr, 0);

    // Post-process pass: SceneColor RT -> SRV, back buffer PRESENT -> RT (одним вызовом)
    D3D12_RESOURCE_BARRIER postBarriers[2] = {};
    postBarriers[0] = sceneToRt;
    postBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    postBarriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    postBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postBarriers[1].Transition.pResource   = CurrentBackBuffer();
    postBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    postBarriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    postBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(2, postBarriers);

    const auto backBufferRtv = CurrentBackBufferRTV();
    m_commandList->OMSetRenderTargets(1, &backBufferRtv, TRUE, nullptr);   // без clear: треугольник перезаписывает все пиксели

    m_commandList->SetGraphicsRootSignature(m_postRootSignature.Get());   // смена RS сбрасывает прежние биндинги
    m_commandList->SetPipelineState(m_postPSO.Get());
    ID3D12DescriptorHeap* postHeaps[] = { m_gBuffer->GetSrvHeap() };
    m_commandList->SetDescriptorHeaps(1, postHeaps);
    m_commandList->SetGraphicsRootConstantBufferView(0, m_postConstantBuffer->GetGPUVirtualAddress());
    m_commandList->SetGraphicsRootDescriptorTable(1, m_gBuffer->GetSrvTable());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->DrawInstanced(3, 1, 0, 0);

    // Back buffer: RENDER_TARGET → PRESENT
    D3D12_RESOURCE_BARRIER toPresent = postBarriers[1];
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    m_commandList->ResourceBarrier(1, &toPresent);

    // Буфер частиц обратно в UAV, consume/append меняются ролями.
    m_particles->FinishFrame(m_commandList.Get());

    ThrowIfFailed(m_commandList->Close(), "Close command list");
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    ThrowIfFailed(m_swapChain->Present(1, 0), "Present");
    m_backBufferIndex = (m_backBufferIndex + 1) % SwapChainBufferCount;
    FlushCommandQueue();
}

void RenderingSystem::SetCamera(const XMFLOAT3& eyePos, float yaw, float pitch)
{
    m_eyePos = eyePos;
    const float sy = std::sinf(yaw);
    const float cy = std::cosf(yaw);
    const float sp = std::sinf(pitch);
    const float cp = std::cosf(pitch);

    const XMVECTOR forward = XMVector3Normalize(XMVectorSet(sy * cp, sp, cy * cp, 0.f));
    XMStoreFloat4x4(
        &m_view,
        XMMatrixLookToLH(XMVectorSet(eyePos.x, eyePos.y, eyePos.z, 1.f), forward, XMVectorSet(0.f, 1.f, 0.f, 0.f)));
}

void RenderingSystem::SetPostEffects(bool vignette, bool chroma, int debugView)
{
    m_post.Flags = XMFLOAT4(vignette ? 1.f : 0.f, chroma ? 1.f : 0.f, 0.f, static_cast<float>(debugView));
}

void RenderingSystem::SetPbrDebug(int materialOverride, bool iblOn, bool directOn)
{
    m_pbrOverride = materialOverride;
    m_iblOn = iblOn;
    m_directOn = directOn;
}

bool RenderingSystem::CreateDevice()
{
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
    if (FAILED(hr))
    {
        ComPtr<IDXGIAdapter> warpAdapter;
        ThrowIfFailed(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)), "EnumWarpAdapter");
        ThrowIfFailed(D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)), "Create WARP device");
    }

    return true;
}

bool RenderingSystem::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)), "Create command queue");
    ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator)), "Create command allocator");
    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList)), "Create command list");
    ThrowIfFailed(m_commandList->Close(), "Initial command list close");
    return true;
}

bool RenderingSystem::CreateSwapChain()
{
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = SwapChainBufferCount;
    desc.BufferDesc.Width = m_width;
    desc.BufferDesc.Height = m_height;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = m_hwnd;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ThrowIfFailed(m_factory->CreateSwapChain(m_commandQueue.Get(), &desc, m_swapChain.GetAddressOf()), "CreateSwapChain");
    return true;
}

bool RenderingSystem::CreateBackBufferHeap()
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = SwapChainBufferCount;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_backBufferRtvHeap)), "Create backbuffer RTV heap");
    return true;
}

bool RenderingSystem::CreateBackBufferRTVs()
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_backBufferRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < SwapChainBufferCount; ++i)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])), "Get backbuffer");
        m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, handle);
        handle.ptr += m_rtvDescriptorSize;
    }
    return true;
}

bool RenderingSystem::BuildShaders()
{
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errors;
    const std::wstring shaderPath = ToWide(ResolveAssetPath("shaders/Shaders.hlsl"));

    auto compile = [&](const char* entryPoint, const char* target, ComPtr<ID3DBlob>& bytecode)
    {
        errors.Reset();
        const HRESULT hr = D3DCompileFromFile(
            shaderPath.c_str(),
            nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entryPoint,
            target,
            compileFlags,
            0,
            &bytecode,
            &errors);

        if (FAILED(hr))
        {
            if (errors)
                throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
            ThrowIfFailed(hr, entryPoint);
        }
    };

    compile("GeometryVS", "vs_5_0", m_geometryVS);
    compile("GeometryPS", "ps_5_0", m_geometryPS);
    compile("LightingVS", "vs_5_0", m_lightingVS);
    compile("LightingPS", "ps_5_0", m_lightingPS);
    compile("ShadowVS", "vs_5_0", m_shadowVS);

    // Шейдеры частиц лежат в отдельном файле: VS + GS + PS.
    const std::wstring particlePath = ToWide(ResolveAssetPath("shaders/ParticleRender.hlsl"));
    auto compileParticle = [&](const char* entryPoint, const char* target, ComPtr<ID3DBlob>& bytecode)
    {
        errors.Reset();
        const HRESULT hr = D3DCompileFromFile(particlePath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entryPoint, target, compileFlags, 0, &bytecode, &errors);
        if (FAILED(hr))
        {
            if (errors)
                throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
            ThrowIfFailed(hr, entryPoint);
        }
    };
    compileParticle("ParticleVS", "vs_5_0", m_particleVS);
    compileParticle("ParticleGS", "gs_5_0", m_particleGS);
    compileParticle("ParticlePS", "ps_5_0", m_particlePS);

    // Post-process: VS и PS в разных файлах.
    auto compileFile = [&](const char* relPath, const char* entryPoint, const char* target, ComPtr<ID3DBlob>& bytecode)
    {
        errors.Reset();
        const std::wstring path = ToWide(ResolveAssetPath(relPath));
        const HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entryPoint, target, compileFlags, 0, &bytecode, &errors);
        if (FAILED(hr))
        {
            if (errors)
                throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
            ThrowIfFailed(hr, entryPoint);
        }
    };
    compileFile("shaders/FullscreenQuadVS.hlsl", "FullscreenVS", "vs_5_0", m_postVS);
    compileFile("shaders/PostProcessPS.hlsl", "PostProcessPS", "ps_5_0", m_postPS);

    m_inputLayout[0] = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    m_inputLayout[1] = { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    m_inputLayout[2] = { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    return true;
}

bool RenderingSystem::BuildRootSignature()
{
    D3D12_DESCRIPTOR_RANGE textureRange{};
    textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRange.NumDescriptors = 1;
    textureRange.BaseShaderRegister = 0;
    textureRange.RegisterSpace = 0;
    textureRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE gbufferRange{};
    gbufferRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    gbufferRange.NumDescriptors = GBuffer::TargetCount + 1;   // t1..t4 = G-buffer (albedo, normal, depth, material), t5 = shadow map array
    gbufferRange.BaseShaderRegister = 1;
    gbufferRange.RegisterSpace = 0;
    gbufferRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // t6 = irradiance (TextureCube), t7 = prefiltered env (TextureCube), t8 = BRDF LUT (Texture2D).
    // Слот TargetCount+1 кучи — SceneColor для post-прохода, lighting его не читает, поэтому offset задан явно.
    D3D12_DESCRIPTOR_RANGE iblRange{};
    iblRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    iblRange.NumDescriptors = GBuffer::IblCount;
    iblRange.BaseShaderRegister = 6;
    iblRange.RegisterSpace = 0;
    iblRange.OffsetInDescriptorsFromTableStart = GBuffer::IblFirstSlot;

    // Одна таблица (param 4): G-buffer + shadow map + IBL. Всё лежит в SRV-куче GBuffer.
    D3D12_DESCRIPTOR_RANGE lightingRanges[2] = { gbufferRange, iblRange };

    // t9 — normal map материала (geometry pass). Отдельная таблица: albedo (t0) и normal лежат в куче текстур не рядом.
    D3D12_DESCRIPTOR_RANGE normalMapRange{};
    normalMapRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    normalMapRange.NumDescriptors = 1;
    normalMapRange.BaseShaderRegister = 9;
    normalMapRange.RegisterSpace = 0;
    normalMapRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // t10 — map_MR материала (G=roughness, B=metallic), та же логика отдельной таблицы, что и normal map.
    D3D12_DESCRIPTOR_RANGE metalRoughRange{};
    metalRoughRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    metalRoughRange.NumDescriptors = 1;
    metalRoughRange.BaseShaderRegister = 10;
    metalRoughRange.RegisterSpace = 0;
    metalRoughRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[9]{};

    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &textureRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 2;
    params[2].Constants.RegisterSpace = 0;
    params[2].Constants.Num32BitValues = 8;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[3].Descriptor.ShaderRegister = 1;
    params[3].Descriptor.RegisterSpace = 0;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = _countof(lightingRanges);
    params[4].DescriptorTable.pDescriptorRanges = lightingRanges;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // b3 — ShadowCB (матрицы каскадов, splits) для lighting pass
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[5].Descriptor.ShaderRegister = 3;
    params[5].Descriptor.RegisterSpace = 0;
    params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // b4 — LightViewProj текущего каскада для shadow pass (16 float)
    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[6].Constants.ShaderRegister = 4;
    params[6].Constants.RegisterSpace = 0;
    params[6].Constants.Num32BitValues = 16;
    params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[7].DescriptorTable.NumDescriptorRanges = 1;
    params[7].DescriptorTable.pDescriptorRanges = &normalMapRange;
    params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[8].DescriptorTable.NumDescriptorRanges = 1;
    params[8].DescriptorTable.pDescriptorRanges = &metalRoughRange;
    params[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_ANISOTROPIC;   // + mip-цепочка = нет муара на дальних/скошенных поверхностях
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MipLODBias = 0.f;
    sampler.MaxAnisotropy = 16;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    sampler.MinLOD = 0.f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s1 — сравнивающий сэмплер для теней: SampleCmpLevelZero + аппаратный билинейный PCF.
    // За пределами карты (border = 1.0) считаем "свет".
    D3D12_STATIC_SAMPLER_DESC shadowSampler{};
    shadowSampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadowSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.MaxAnisotropy = 1;
    shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadowSampler.MinLOD = 0.f;
    shadowSampler.MaxLOD = D3D12_FLOAT32_MAX;
    shadowSampler.ShaderRegister = 1;
    shadowSampler.RegisterSpace = 0;
    shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s2 — кубмапы IBL (irradiance + prefiltered): трилинейная фильтрация по mip-цепочке, clamp.
    D3D12_STATIC_SAMPLER_DESC iblCubeSampler{};
    iblCubeSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    iblCubeSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    iblCubeSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    iblCubeSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    iblCubeSampler.MaxAnisotropy = 1;
    iblCubeSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    iblCubeSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    iblCubeSampler.MinLOD = 0.f;
    iblCubeSampler.MaxLOD = D3D12_FLOAT32_MAX;
    iblCubeSampler.ShaderRegister = 2;
    iblCubeSampler.RegisterSpace = 0;
    iblCubeSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s3 — BRDF LUT: билинейно, строго clamp (иначе на краях (NdotV=0/1, roughness=0/1) подмешается противоположный край).
    D3D12_STATIC_SAMPLER_DESC brdfLutSampler = iblCubeSampler;
    brdfLutSampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    brdfLutSampler.ShaderRegister = 3;

    D3D12_STATIC_SAMPLER_DESC samplers[4] = { sampler, shadowSampler, iblCubeSampler, brdfLutSampler };

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(_countof(params));
    desc.pParameters = params;
    desc.NumStaticSamplers = _countof(samplers);
    desc.pStaticSamplers = samplers;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(hr))
    {
        if (errors)
            throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
        ThrowIfFailed(hr, "SerializeRootSignature");
    }

    ThrowIfFailed(
        m_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)),
        "CreateRootSignature");
    return true;
}

bool RenderingSystem::BuildPSOs()
{
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.FrontCounterClockwise = TRUE;
    rasterizer.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rasterizer.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rasterizer.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.MultisampleEnable = FALSE;
    rasterizer.AntialiasedLineEnable = FALSE;
    rasterizer.ForcedSampleCount = 0;
    rasterizer.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_BLEND_DESC blend{};
    blend.AlphaToCoverageEnable = FALSE;
    blend.IndependentBlendEnable = FALSE;
    const D3D12_RENDER_TARGET_BLEND_DESC defaultRenderTargetBlend = {
        FALSE, FALSE,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP,
        D3D12_COLOR_WRITE_ENABLE_ALL
    };
    for (auto& rt : blend.RenderTarget)
        rt = defaultRenderTargetBlend;

    D3D12_DEPTH_STENCIL_DESC geometryDepth{};
    geometryDepth.DepthEnable = TRUE;
    geometryDepth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    geometryDepth.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    geometryDepth.StencilEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC geometryPso{};
    geometryPso.pRootSignature = m_rootSignature.Get();
    geometryPso.VS = { m_geometryVS->GetBufferPointer(), m_geometryVS->GetBufferSize() };
    geometryPso.PS = { m_geometryPS->GetBufferPointer(), m_geometryPS->GetBufferSize() };
    geometryPso.BlendState = blend;
    geometryPso.SampleMask = UINT_MAX;
    geometryPso.RasterizerState = rasterizer;
    geometryPso.DepthStencilState = geometryDepth;
    geometryPso.InputLayout = { m_inputLayout, static_cast<UINT>(_countof(m_inputLayout)) };
    geometryPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    geometryPso.NumRenderTargets = GBuffer::TargetCount;
    geometryPso.RTVFormats[0] = m_gBuffer->GetAlbedoSpecFormat();
    geometryPso.RTVFormats[1] = m_gBuffer->GetNormalFormat();
    geometryPso.RTVFormats[2] = m_gBuffer->GetDepthValueFormat();
    geometryPso.RTVFormats[3] = m_gBuffer->GetMaterialFormat();
    geometryPso.DSVFormat = m_gBuffer->GetDepthStencilFormat();
    geometryPso.SampleDesc.Count = 1;
    geometryPso.SampleDesc.Quality = 0;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&geometryPso, IID_PPV_ARGS(&m_geometryPSO)), "Create geometry PSO");

    D3D12_DEPTH_STENCIL_DESC lightingDepth{};
    lightingDepth.DepthEnable = FALSE;
    lightingDepth.StencilEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC lightingPso{};
    lightingPso.pRootSignature = m_rootSignature.Get();
    lightingPso.VS = { m_lightingVS->GetBufferPointer(), m_lightingVS->GetBufferSize() };
    lightingPso.PS = { m_lightingPS->GetBufferPointer(), m_lightingPS->GetBufferSize() };
    lightingPso.BlendState = blend;
    lightingPso.SampleMask = UINT_MAX;
    lightingPso.RasterizerState = rasterizer;
    lightingPso.DepthStencilState = lightingDepth;
    lightingPso.InputLayout = { nullptr, 0 };
    lightingPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    lightingPso.NumRenderTargets = 1;
    lightingPso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    lightingPso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    lightingPso.SampleDesc.Count = 1;
    lightingPso.SampleDesc.Quality = 0;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&lightingPso, IID_PPV_ARGS(&m_lightingPSO)), "Create lighting PSO");

    // Shadow PSO: только глубина, без цветовых целей и без пиксельного шейдера.
    // Cull NONE: стены Sponza не замкнутые. DepthClip выключен, чтобы объекты, стоящие
    // ближе к свету, чем near-плоскость каскада, не отсекались (а "прижимались" к ней).
    D3D12_RASTERIZER_DESC shadowRaster = rasterizer;
    shadowRaster.DepthClipEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC shadowPso{};
    shadowPso.pRootSignature = m_rootSignature.Get();
    shadowPso.VS = { m_shadowVS->GetBufferPointer(), m_shadowVS->GetBufferSize() };
    shadowPso.BlendState = blend;
    shadowPso.SampleMask = UINT_MAX;
    shadowPso.RasterizerState = shadowRaster;
    shadowPso.DepthStencilState = geometryDepth;
    shadowPso.InputLayout = { m_inputLayout, static_cast<UINT>(_countof(m_inputLayout)) };
    shadowPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    shadowPso.NumRenderTargets = 0;
    shadowPso.DSVFormat = m_shadowMap->GetDsvFormat();
    shadowPso.SampleDesc.Count = 1;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&shadowPso, IID_PPV_ARGS(&m_shadowPSO)), "Create shadow PSO");

    // Частицы: рисуются ПОСЛЕ освещения прямо в back buffer, но с depth-тестом по глубине Sponza
    // (общий depth buffer G-buffer'а). Непрозрачные: без блендинга, с обычной записью в Z.
    // На вход идут точки (POINTLIST), геометрический шейдер разворачивает их в квадраты.
    const D3D12_INPUT_ELEMENT_DESC particleLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "SIZE",     0, DXGI_FORMAT_R32_FLOAT,       0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "VELOCITY", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "LIFETIME", 0, DXGI_FORMAT_R32_FLOAT,       0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_RASTERIZER_DESC particleRaster = rasterizer;    // CULL_NONE: квадрат виден с любой стороны

    D3D12_GRAPHICS_PIPELINE_STATE_DESC particlePso{};
    particlePso.pRootSignature = m_rootSignature.Get();   // нужен только PassCB (b0)
    particlePso.VS = { m_particleVS->GetBufferPointer(), m_particleVS->GetBufferSize() };
    particlePso.GS = { m_particleGS->GetBufferPointer(), m_particleGS->GetBufferSize() };
    particlePso.PS = { m_particlePS->GetBufferPointer(), m_particlePS->GetBufferSize() };
    particlePso.BlendState = blend;
    particlePso.SampleMask = UINT_MAX;
    particlePso.RasterizerState = particleRaster;
    particlePso.DepthStencilState = geometryDepth;        // depth test LESS + запись в Z
    particlePso.InputLayout = { particleLayout, static_cast<UINT>(_countof(particleLayout)) };
    particlePso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    particlePso.NumRenderTargets = 1;
    particlePso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    particlePso.DSVFormat = m_gBuffer->GetDepthStencilFormat();
    particlePso.SampleDesc.Count = 1;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&particlePso, IID_PPV_ARGS(&m_particlePSO)), "Create particle PSO");

    // Post-process PSO: полноэкранный треугольник без vertex buffer, без depth, вывод в back buffer.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC postPso{};
    postPso.pRootSignature = m_postRootSignature.Get();
    postPso.VS = { m_postVS->GetBufferPointer(), m_postVS->GetBufferSize() };
    postPso.PS = { m_postPS->GetBufferPointer(), m_postPS->GetBufferSize() };
    postPso.BlendState = blend;
    postPso.SampleMask = UINT_MAX;
    postPso.RasterizerState = rasterizer;                 // CULL_NONE: порядок обхода вершин неважен
    postPso.DepthStencilState = lightingDepth;            // DepthEnable = FALSE
    postPso.InputLayout = { nullptr, 0 };
    postPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;   // в списке команд: TRIANGLELIST
    postPso.NumRenderTargets = 1;
    postPso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;   // формат back buffer
    postPso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    postPso.SampleDesc.Count = 1;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&postPso, IID_PPV_ARGS(&m_postPSO)), "Create post-process PSO");

    return true;
}

bool RenderingSystem::BuildPostRootSignature()
{
    // Таблица указывает в кучу GBuffer: слоты 0..3 = G-Buffer (t0..t3), слот 4 = shadow map (пропускаем), слот 5 = SceneColor (t4).
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = GBuffer::TargetCount;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;

    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = GBuffer::TargetCount;   // t4
    ranges[1].OffsetInDescriptorsFromTableStart = GBuffer::TargetCount + 1;   // перепрыгиваем слот shadow map

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   // b0 = PostCB
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};   // s0: linear clamp (для сдвинутых выборок хроматической аберрации у краёв)
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = _countof(params);
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;   // без ALLOW_INPUT_ASSEMBLER: vertex buffer не используется

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(hr))
    {
        if (errors)
            throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
        ThrowIfFailed(hr, "SerializeRootSignature (post)");
    }
    ThrowIfFailed(
        m_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_postRootSignature)),
        "CreateRootSignature (post)");
    return true;
}

void RenderingSystem::CreateSceneColor()
{
    // Вызывается и при Resize: старый ресурс уже никем не используется (OnResize делает FlushCommandQueue).
    m_sceneColor.Reset();

    if (!m_sceneRtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.NumDescriptors = 1;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_sceneRtvHeap)), "Create SceneColor RTV heap");
    }

    D3D12_RESOURCE_DESC desc = TextureDesc2D(m_width, m_height, DXGI_FORMAT_R8G8B8A8_UNORM);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = desc.Format;   // цвет очистки = (0,0,0,0)

    const auto defaultHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(
        m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&m_sceneColor)),   // как у G-Buffer: старт в SRV-состоянии
        "Create SceneColor");

    m_device->CreateRenderTargetView(m_sceneColor.Get(), nullptr, m_sceneRtvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = desc.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_sceneColor.Get(), &srv, m_gBuffer->GetSceneColorSrvCpu());
}

bool RenderingSystem::BuildGeometry()
{
    ObjMesh model{};
    if (!LoadObj(ResolveAssetPath("sponza/sponza.obj"), model))
        throw std::runtime_error("Failed to load sponza.obj");

    // Cerberus авторен в метрах (glTF), сцена Sponza — в сантиметрах (gWorld ниже делает *0.01 на весь draw).
    // Компенсируем масштабом *100 и ставим на постамент слева-впереди от стартовой камеры (-4, 2, 0 м).
    const XMMATRIX cerberusTransform =
        XMMatrixRotationY(XM_PIDIV2) *
        XMMatrixScaling(100.f, 100.f, 100.f) *
        XMMatrixTranslation(-100.f, 130.f, 150.f);
    AppendObjMesh(model, ResolveAssetPath("models/cerberus/cerberus.obj"), cerberusTransform);

    std::unordered_map<std::string, uint32_t> pathToIndex;
    std::vector<std::string> uniquePaths;

    enum class TexKind { Albedo, Normal, MetalRough };
    std::vector<TexKind> uniqueKind;

    // Слоты кучи текстур: 0 = белая заглушка (albedo), 1 = плоская нормаль (0,0,1),
    // 2 = нейтральная map_MR (G=1, B=1 — множитель-константы Pm/Pr проходят как есть), дальше — файлы.
    constexpr uint32_t kWhiteSlot = 0;
    constexpr uint32_t kFlatNormalSlot = 1;
    constexpr uint32_t kNeutralMrSlot = 2;
    constexpr uint32_t kFirstFileSlot = 3;

    auto getTextureIndex = [&](const std::string& path, TexKind kind) -> uint32_t
    {
        if (path.empty())
        {
            switch (kind)
            {
            case TexKind::Normal:     return kFlatNormalSlot;
            case TexKind::MetalRough: return kNeutralMrSlot;
            default:                  return kWhiteSlot;
            }
        }

        auto [it, inserted] = pathToIndex.emplace(path, static_cast<uint32_t>(uniquePaths.size()) + kFirstFileSlot);
        if (inserted)
        {
            uniquePaths.push_back(path);
            uniqueKind.push_back(kind);
        }
        return it->second;
    };

    m_drawItems.clear();
    m_drawItems.reserve(model.groups.size());
    for (const ObjGroup& group : model.groups)
    {
        DrawItem drawItem{};
        drawItem.StartIndexLocation = group.start;
        drawItem.IndexCount = group.count;

        const auto materialIt = model.materials.find(group.material);
        if (materialIt != model.materials.end())
        {
            const MtlData& material = materialIt->second;
            drawItem.TextureIndex = getTextureIndex(material.diffusePath, TexKind::Albedo);
            drawItem.NormalIndex = getTextureIndex(material.normalPath, TexKind::Normal);
            drawItem.MetalRoughIndex = getTextureIndex(material.metalRoughPath, TexKind::MetalRough);
            drawItem.Material.BaseColor = XMFLOAT4(material.kd.x, material.kd.y, material.kd.z, 1.f);

            // Metallic / roughness — константы, либо (если есть map_MR) множители на G/B канал текстуры.
            // Нет Pr в MTL и нет map_MR -> roughness из показателя Фонга: r = sqrt(2 / (Ns + 2)).
            const float roughness = (material.roughness >= 0.f) ? material.roughness : std::sqrt(2.f / (material.ns + 2.f));
            drawItem.Material.SurfaceParams = XMFLOAT4(Clamp01(material.metallic), std::max(0.05f, Clamp01(roughness)), 1.f, 0.f);
        }

        m_drawItems.push_back(drawItem);
    }

    const UINT64 vbSize = static_cast<UINT64>(model.vertices.size()) * sizeof(Vertex);
    const UINT64 ibSize = static_cast<UINT64>(model.indices.size()) * sizeof(uint32_t);

    auto defaultHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);

    auto createBuffer = [&](UINT64 size, D3D12_RESOURCE_STATES initialState, ComPtr<ID3D12Resource>& resource)
    {
        const D3D12_RESOURCE_DESC desc = BufferDesc(size);
        ThrowIfFailed(
            m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&resource)),
            "Create default buffer");
    };

    createBuffer(vbSize, D3D12_RESOURCE_STATE_COMMON, m_vertexBuffer);   // буферы всегда стартуют в COMMON
    createBuffer(ibSize, D3D12_RESOURCE_STATE_COMMON, m_indexBuffer);

    auto createUploadBuffer = [&](UINT64 size, const void* data) -> ComPtr<ID3D12Resource>
    {
        ComPtr<ID3D12Resource> resource;
        const D3D12_RESOURCE_DESC desc = BufferDesc(size);
        ThrowIfFailed(
            m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource)),
            "Create upload buffer");

        void* mapped = nullptr;
        D3D12_RANGE readRange{ 0, 0 };
        ThrowIfFailed(resource->Map(0, &readRange, &mapped), "Map upload buffer");
        std::memcpy(mapped, data, static_cast<size_t>(size));
        resource->Unmap(0, nullptr);
        return resource;
    };

    ComPtr<ID3D12Resource> vbUpload = createUploadBuffer(vbSize, model.vertices.data());
    ComPtr<ID3D12Resource> ibUpload = createUploadBuffer(ibSize, model.indices.data());

    std::vector<Image> images(uniquePaths.size());
    std::vector<bool> loaded(uniquePaths.size(), false);
    for (size_t i = 0; i < uniquePaths.size(); ++i)
        loaded[i] = LoadImage(uniquePaths[i], images[i]);

    auto createTexture = [&](uint32_t width, uint32_t height) -> ComPtr<ID3D12Resource>
    {
        ComPtr<ID3D12Resource> texture;
        const D3D12_RESOURCE_DESC desc = TextureDesc2D(width, height, DXGI_FORMAT_B8G8R8A8_UNORM, MipCount(width, height));
        ThrowIfFailed(
            m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)),
            "Create texture");
        return texture;
    };

    m_textures.clear();
    m_textures.resize(uniquePaths.size() + kFirstFileSlot);
    m_textures[kWhiteSlot] = createTexture(1, 1);
    m_textures[kFlatNormalSlot] = createTexture(1, 1);
    m_textures[kNeutralMrSlot] = createTexture(1, 1);

    auto fallbackSlotFor = [&](TexKind kind) -> uint32_t
    {
        switch (kind)
        {
        case TexKind::Normal:     return kFlatNormalSlot;
        case TexKind::MetalRough: return kNeutralMrSlot;
        default:                  return kWhiteSlot;
        }
    };

    for (size_t i = 0; i < uniquePaths.size(); ++i)
    {
        if (loaded[i])
            m_textures[i + kFirstFileSlot] = createTexture(images[i].width, images[i].height);
        else   // файл не загрузился -> заглушка нужного типа
            m_textures[i + kFirstFileSlot] = m_textures[fallbackSlotFor(uniqueKind[i])];
    }

    ThrowIfFailed(m_commandAllocator->Reset(), "Reset command allocator for geometry upload");
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr), "Reset command list for geometry upload");

    m_commandList->CopyBufferRegion(m_vertexBuffer.Get(), 0, vbUpload.Get(), 0, vbSize);
    m_commandList->CopyBufferRegion(m_indexBuffer.Get(), 0, ibUpload.Get(), 0, ibSize);

    D3D12_RESOURCE_BARRIER bufferBarriers[2]{};
    bufferBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bufferBarriers[0].Transition.pResource = m_vertexBuffer.Get();
    bufferBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bufferBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    bufferBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    bufferBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bufferBarriers[1].Transition.pResource = m_indexBuffer.Get();
    bufferBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bufferBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_INDEX_BUFFER;
    bufferBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    m_commandList->ResourceBarrier(2, bufferBarriers);

    std::vector<ComPtr<ID3D12Resource>> textureUploads;
    Image whiteTexture;
    whiteTexture.width = 1;
    whiteTexture.height = 1;
    whiteTexture.bgra = { 255, 255, 255, 255 };
    UploadTexture(m_device.Get(), m_commandList.Get(), m_textures[kWhiteSlot].Get(), whiteTexture, textureUploads);

    Image flatNormalTexture;   // BGRA: (0.5, 0.5, 1.0) -> тангенциальная нормаль (0, 0, 1)
    flatNormalTexture.width = 1;
    flatNormalTexture.height = 1;
    flatNormalTexture.bgra = { 255, 128, 128, 255 };
    UploadTexture(m_device.Get(), m_commandList.Get(), m_textures[kFlatNormalSlot].Get(), flatNormalTexture, textureUploads);

    Image neutralMrTexture;   // BGRA: R=1(metallic passthrough), G=1(roughness passthrough), B не используется
    neutralMrTexture.width = 1;
    neutralMrTexture.height = 1;
    neutralMrTexture.bgra = { 0, 255, 255, 255 };
    UploadTexture(m_device.Get(), m_commandList.Get(), m_textures[kNeutralMrSlot].Get(), neutralMrTexture, textureUploads);

    for (size_t i = 0; i < uniquePaths.size(); ++i)
    {
        if (loaded[i])
            UploadTexture(m_device.Get(), m_commandList.Get(), m_textures[i + kFirstFileSlot].Get(), images[i], textureUploads);
    }

    ThrowIfFailed(m_commandList->Close(), "Close geometry upload command list");
    ID3D12CommandList* uploadLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, uploadLists);
    FlushCommandQueue();

    m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vertexBufferView.SizeInBytes = static_cast<UINT>(vbSize);
    m_vertexBufferView.StrideInBytes = sizeof(Vertex);

    m_indexBufferView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
    m_indexBufferView.SizeInBytes = static_cast<UINT>(ibSize);
    m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;

    D3D12_DESCRIPTOR_HEAP_DESC textureHeapDesc{};
    textureHeapDesc.NumDescriptors = static_cast<UINT>(m_textures.size());
    textureHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    textureHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&textureHeapDesc, IID_PPV_ARGS(&m_textureHeap)), "Create texture heap");

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_textureHeap->GetCPUDescriptorHandleForHeapStart();
    for (size_t i = 0; i < m_textures.size(); ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = m_textures[i]->GetDesc().MipLevels;   // все уровни цепочки
        m_device->CreateShaderResourceView(m_textures[i].Get(), &srvDesc, srvHandle);
        srvHandle.ptr += m_srvDescriptorSize;
    }

    return true;
}

bool RenderingSystem::BuildIblResources()
{
    // Файлы: ibl/irradiance.dds (cube), ibl/prefilter.dds (cube + полная mip-цепочка), ibl/brdf_lut.dds (2D, RG).
    // Нет файла -> 1x1 заглушка, приложение не падает.
    struct IblFile { const char* path; bool isCube; };
    const IblFile files[GBuffer::IblCount] = {
        { "ibl/irradiance.dds", true },
        { "ibl/prefilter.dds",  true },
        { "ibl/brdf_lut.dds",   false },
    };

    ThrowIfFailed(m_commandAllocator->Reset(), "Reset command allocator for IBL upload");
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr), "Reset command list for IBL upload");

    std::vector<ComPtr<ID3D12Resource>> uploads;
    ComPtr<ID3D12Resource>* targets[GBuffer::IblCount] = { &m_irradianceMap, &m_prefilterMap, &m_brdfLut };

    for (uint32_t i = 0; i < GBuffer::IblCount; ++i)
    {
        DdsImage image;
        const bool ok = LoadDds(ResolveAssetPath(files[i].path), image) && image.isCube == files[i].isCube;
        if (!ok)
        {
            OutputDebugStringA((std::string("IBL: cannot load ") + files[i].path + ", using 1x1 fallback\n").c_str());
            image = files[i].isCube ? MakeFallbackCube() : MakeFallbackLut();
        }

        *targets[i] = UploadDds(m_device.Get(), m_commandList.Get(), image, uploads);

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = image.format;
        if (image.isCube)
        {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            srv.TextureCube.MostDetailedMip = 0;
            srv.TextureCube.MipLevels = image.mipLevels;
        }
        else
        {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = image.mipLevels;
        }
        m_device->CreateShaderResourceView(targets[i]->Get(), &srv, m_gBuffer->GetIblSrvCpu(i));
    }

    ThrowIfFailed(m_commandList->Close(), "Close IBL upload command list");
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
    FlushCommandQueue();   // upload-буферы (uploads) живы до этого места
    return true;
}

bool RenderingSystem::BuildFrameResources()
{
    auto uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);

    const uint32_t passCbSize = AlignConstantBufferSize(sizeof(PassConstants));
    const uint32_t lightCbSize = AlignConstantBufferSize(sizeof(LightConstants));

    const D3D12_RESOURCE_DESC passDesc = BufferDesc(passCbSize);
    ThrowIfFailed(
        m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &passDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_passConstantBuffer)),
        "Create pass constant buffer");

    const D3D12_RESOURCE_DESC lightDesc = BufferDesc(lightCbSize);
    ThrowIfFailed(
        m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &lightDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_lightConstantBuffer)),
        "Create light constant buffer");

    const D3D12_RESOURCE_DESC postDesc = BufferDesc(AlignConstantBufferSize(sizeof(PostConstants)));
    ThrowIfFailed(
        m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &postDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_postConstantBuffer)),
        "Create post constant buffer");

    D3D12_RANGE readRange{ 0, 0 };
    ThrowIfFailed(m_postConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_mappedPostConstants)), "Map post constant buffer");
    ThrowIfFailed(m_passConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_mappedPassConstants)), "Map pass constant buffer");
    ThrowIfFailed(m_lightConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_mappedLightConstants)), "Map light constant buffer");
    return true;
}

void RenderingSystem::UpdatePassConstants()
{
    if (!m_mappedPassConstants)
        return;

    PassConstants constants{};

    const XMMATRIX world = XMLoadFloat4x4(&m_world);
    const XMMATRIX view = XMLoadFloat4x4(&m_view);
    const XMMATRIX proj = XMLoadFloat4x4(&m_proj);
    const XMMATRIX viewProj = view * proj;
    const XMMATRIX invViewProj = XMMatrixInverse(nullptr, viewProj);

    XMStoreFloat4x4(&constants.World, XMMatrixTranspose(world));
    XMStoreFloat4x4(&constants.ViewProj, XMMatrixTranspose(viewProj));
    XMStoreFloat4x4(&constants.InvViewProj, XMMatrixTranspose(invViewProj));

    constants.EyePosW = XMFLOAT4(m_eyePos.x, m_eyePos.y, m_eyePos.z, 1.f);
    constants.RenderTargetSize = XMFLOAT4(
        static_cast<float>(m_width),
        static_cast<float>(m_height),
        1.f / static_cast<float>(m_width),
        1.f / static_cast<float>(m_height));

    std::memcpy(m_mappedPassConstants, &constants, sizeof(constants));

    // Post-process CB: размер RT берём из PassConstants, параметры эффектов лежат в m_post.
    if (m_mappedPostConstants)
    {
        m_post.RenderTargetSize = constants.RenderTargetSize;
        std::memcpy(m_mappedPostConstants, &m_post, sizeof(m_post));
    }
}

void RenderingSystem::UpdateLightConstants(float dt)
{
    if (!m_mappedLightConstants)
        return;

    m_time += dt;

    LightConstants constants{};
    constants.PbrDebug = XMFLOAT4(static_cast<float>(m_pbrOverride), m_iblOn ? 1.f : 0.f, m_directOn ? 1.f : 0.f, 0.f);

    const uint32_t lightCount = static_cast<uint32_t>(std::min<size_t>(m_sceneLights.size(), MaxLights));
    constants.LightCount = XMFLOAT4(static_cast<float>(lightCount), 0.f, 0.f, 0.f);

    for (uint32_t i = 0; i < lightCount; ++i)
    {
        GpuLight light = m_sceneLights[i];
        const float type = light.Params.x;
        if (type > 0.5f)
        {
            const float pulse = 0.9f + 0.1f * std::sinf(m_time * 1.35f + static_cast<float>(i) * 0.75f);
            light.ColorIntensity.w *= pulse;
        }
        constants.Lights[i] = light;
    }

    std::memcpy(m_mappedLightConstants, &constants, sizeof(constants));
}

void RenderingSystem::CreateSceneLights()
{
    auto normalize = [](const XMFLOAT3& v) -> XMFLOAT3
    {
        XMFLOAT3 out{};
        XMStoreFloat3(&out, XMVector3Normalize(XMLoadFloat3(&v)));
        return out;
    };

    auto makeDirectional = [&](const XMFLOAT3& direction, const XMFLOAT3& color, float intensity, bool castsShadow) -> GpuLight
    {
        GpuLight light{};
        const XMFLOAT3 dir = normalize(direction);
        light.DirectionSpot = XMFLOAT4(dir.x, dir.y, dir.z, 0.f);
        light.ColorIntensity = XMFLOAT4(color.x, color.y, color.z, intensity);
        light.Params = XMFLOAT4(0.f, 0.f, castsShadow ? 1.f : 0.f, 0.f);
        return light;
    };

    auto makePoint = [&](const XMFLOAT3& position, const XMFLOAT3& color, float intensity, float range) -> GpuLight
    {
        GpuLight light{};
        light.PositionRange = XMFLOAT4(position.x, position.y, position.z, range);
        light.ColorIntensity = XMFLOAT4(color.x, color.y, color.z, intensity);
        light.Params = XMFLOAT4(1.f, 0.f, 0.f, 0.f);
        return light;
    };

    auto makeSpot = [&](const XMFLOAT3& position, const XMFLOAT3& direction, const XMFLOAT3& color, float intensity, float range, float innerAngleDeg, float outerAngleDeg) -> GpuLight
    {
        GpuLight light{};
        const XMFLOAT3 dir = normalize(direction);
        const float innerAngle = XMConvertToRadians(innerAngleDeg);
        const float outerAngle = XMConvertToRadians(outerAngleDeg);

        light.PositionRange = XMFLOAT4(position.x, position.y, position.z, range);
        light.DirectionSpot = XMFLOAT4(dir.x, dir.y, dir.z, std::cos(outerAngle));
        light.ColorIntensity = XMFLOAT4(color.x, color.y, color.z, intensity);
        light.Params = XMFLOAT4(2.f, std::cos(innerAngle), 0.f, 0.f);
        return light;
    };

    m_sceneLights.clear();

    // [0] Солнце — тёплый свет с каскадными тенями. Направление должно совпадать с m_sunDir.
    // Интенсивности умножены на PI: в PBR диффузная часть = albedo / PI, и без множителя сцена стала бы в 3 раза темнее.
    m_sceneLights.push_back(makeDirectional(m_sunDir, XMFLOAT3(1.0f, 0.93f, 0.80f), 1.7f * XM_PI, true));

    // [1] Заполняющий свет — холодный, с противоположной стороны, без теней.
    //     Подсвечивает стороны, куда не попадает солнце, чтобы тени не были чёрными.
    m_sceneLights.push_back(makeDirectional(XMFLOAT3(0.30f, -0.45f, -0.85f), XMFLOAT3(0.55f, 0.65f, 0.95f), 0.45f * XM_PI, false));

}

void RenderingSystem::FlushCommandQueue()
{
    const uint64_t value = ++m_fenceValue;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), value), "Signal fence");
    if (m_fence->GetCompletedValue() < value)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(value, m_fenceEvent), "Set fence event");
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderingSystem::CurrentBackBufferRTV() const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_backBufferRtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(m_backBufferIndex) * m_rtvDescriptorSize;
    return handle;
}

ID3D12Resource* RenderingSystem::CurrentBackBuffer() const
{
    return m_backBuffers[m_backBufferIndex].Get();
}