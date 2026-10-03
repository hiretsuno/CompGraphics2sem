#include "RenderingSystem.h"
#include "GBuffer.h"
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
#include <random>
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

    D3D12_RESOURCE_DESC TextureDesc2D(uint32_t width, uint32_t height, DXGI_FORMAT format)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
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
            JoinPath("../..", name)
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
        XMFLOAT3 kd{ 1.f, 1.f, 1.f };
        XMFLOAT3 ks{ 0.18f, 0.18f, 0.18f };
        float ns = 32.f;
    };

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

    void UploadTexture(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* texture,
        const Image& image,
        std::vector<ComPtr<ID3D12Resource>>& uploadResources)
    {
        auto uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        const D3D12_RESOURCE_DESC textureDesc = TextureDesc2D(image.width, image.height, DXGI_FORMAT_B8G8R8A8_UNORM);

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 totalBytes = 0;
        device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);

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

        const uint32_t srcRowPitch = image.width * 4u;
        const uint32_t dstRowPitch = footprint.Footprint.RowPitch;
        for (uint32_t y = 0; y < image.height; ++y)
        {
            std::memcpy(
                static_cast<uint8_t*>(mapped) + static_cast<size_t>(y) * dstRowPitch,
                image.bgra.data() + static_cast<size_t>(y) * srcRowPitch,
                srcRowPitch);
        }
        uploadBuffer->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = uploadBuffer.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprint;

        commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &barrier);

        uploadResources.push_back(uploadBuffer);
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

    XMStoreFloat4x4(&m_world, XMMatrixScaling(0.01f, 0.01f, 0.01f));
    SetCamera(m_eyePos, 1.f, 0.f);

    const float aspect = (m_height > 0) ? static_cast<float>(m_width) / static_cast<float>(m_height) : 1.f;
    XMStoreFloat4x4(&m_proj, XMMatrixPerspectiveFovLH(0.25f * XM_PI, aspect, 0.05f, 1000.f));

    BuildShaders();
    BuildRootSignature();
    BuildGeometry();
    BuildLightVolumes();
    BuildFrameResources();
    BuildLightGun();

    m_gBuffer = std::make_unique<GBuffer>();
    m_gBuffer->Initialize(m_device.Get(), m_width, m_height);

    CreateSceneLights();
    UpdatePassConstants();
    UpdateLights(0.f);
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

    if (m_lightBuffer && m_mappedLights)
    {
        m_lightBuffer->Unmap(0, nullptr);
        m_mappedLights = nullptr;
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
        m_gBuffer->Resize(m_device.Get(), m_width, m_height);

    m_viewport = { 0.f, 0.f, static_cast<float>(m_width), static_cast<float>(m_height), 0.f, 1.f };
    m_scissorRect = { 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };

    const float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    XMStoreFloat4x4(&m_proj, XMMatrixPerspectiveFovLH(0.25f * XM_PI, aspect, 0.05f, 1000.f));

    UpdatePassConstants();
}

void RenderingSystem::Draw(float dt)
{
    if (!m_initialized)
        return;

    UpdatePassConstants();
    UpdateLightGun(dt);
    UpdateLights(dt);

    ThrowIfFailed(m_commandAllocator->Reset(), "Reset command allocator");
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr), "Reset command list");

    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissorRect);
    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());

    // ---------------------------------------------------------------------------------
    // 1. GEOMETRY PASS: сцена -> G-buffer (albedo/spec, normal/shininess, depth)
    // ---------------------------------------------------------------------------------
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

        m_commandList->SetGraphicsRootDescriptorTable(1, textureHandle);
        m_commandList->SetGraphicsRoot32BitConstants(2, 8, &drawItem.Material, 0);
        m_commandList->DrawIndexedInstanced(drawItem.IndexCount, 1, drawItem.StartIndexLocation, 0, 0);
    }

    // ---------------------------------------------------------------------------------
    // 2. LIGHTING PASS: G-buffer -> back buffer, по одному draw call на тип источника,
    //    результаты складываются аддитивно
    // ---------------------------------------------------------------------------------
    m_gBuffer->TransitionToRead(m_commandList.Get());

    D3D12_RESOURCE_BARRIER toRenderTarget{};
    toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRenderTarget.Transition.pResource = CurrentBackBuffer();
    toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &toRenderTarget);

    // Read-only DSV: глубина сцены используется для depth test световых объёмов,
    // и одновременно та же глубина читается шейдером через SRV
    const auto backBufferRtv = CurrentBackBufferRTV();
    const auto readOnlyDsv = m_gBuffer->GetReadOnlyDsv();
    const float clearColor[4] = { 0.f, 0.f, 0.f, 1.f };
    m_commandList->OMSetRenderTargets(1, &backBufferRtv, TRUE, &readOnlyDsv);
    m_commandList->ClearRenderTargetView(backBufferRtv, clearColor, 0, nullptr);

    ID3D12DescriptorHeap* lightingHeaps[] = { m_gBuffer->GetSrvHeap() };
    m_commandList->SetDescriptorHeaps(1, lightingHeaps);
    m_commandList->SetGraphicsRootConstantBufferView(0, m_passConstantBuffer->GetGPUVirtualAddress());
    m_commandList->SetGraphicsRootDescriptorTable(4, m_gBuffer->GetSrvTable());
    m_commandList->SetGraphicsRootShaderResourceView(5, m_lightBuffer->GetGPUVirtualAddress());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    auto setLightOffset = [&](uint32_t offset)
    {
        m_lightPass.LightOffset = offset;
        m_commandList->SetGraphicsRoot32BitConstants(3, sizeof(LightPassConstants) / 4, &m_lightPass, 0);
    };

    // 2a. Ambient — полноэкранный треугольник, перезаписывает цвет
    setLightOffset(0);
    m_commandList->SetPipelineState(m_ambientPSO.Get());
    m_commandList->DrawInstanced(3, 1, 0, 0);

    // 2b. Directional — полноэкранный треугольник на каждый источник, additive
    if (m_directionalCount > 0)
    {
        setLightOffset(0);
        m_commandList->SetPipelineState(m_directionalPSO.Get());
        m_commandList->DrawInstanced(3, m_directionalCount, 0, 0);
    }

    // 2c. Point / Spot — световые объёмы (сфера / конус), instancing: 1 инстанс = 1 источник
    if (m_pointCount + m_spotCount > 0)
    {
        m_commandList->SetPipelineState(m_lightVolumePSO.Get());
        m_commandList->IASetVertexBuffers(0, 1, &m_volumeVBView);
        m_commandList->IASetIndexBuffer(&m_volumeIBView);

        if (m_pointCount > 0)
        {
            setLightOffset(m_directionalCount);
            m_commandList->DrawIndexedInstanced(m_sphereMesh.IndexCount, m_pointCount, m_sphereMesh.StartIndex, m_sphereMesh.BaseVertex, 0);
        }
        if (m_spotCount > 0)
        {
            setLightOffset(m_directionalCount + m_pointCount);
            m_commandList->DrawIndexedInstanced(m_coneMesh.IndexCount, m_spotCount, m_coneMesh.StartIndex, m_coneMesh.BaseVertex, 0);
        }
    }

    // ---------------------------------------------------------------------------------
    // 3. HUD (прицел) и probe для пулемёта
    // ---------------------------------------------------------------------------------
    setLightOffset(0);
    m_commandList->SetPipelineState(m_hudPSO.Get());
    m_commandList->DrawInstanced(3, 1, 0, 0);

    if (m_probePending)
        RecordProbePass();

    D3D12_RESOURCE_BARRIER toPresent = toRenderTarget;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_commandList->ResourceBarrier(1, &toPresent);

    ThrowIfFailed(m_commandList->Close(), "Close command list");
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    ThrowIfFailed(m_swapChain->Present(1, 0), "Present");
    m_backBufferIndex = (m_backBufferIndex + 1) % SwapChainBufferCount;
    FlushCommandQueue();

    // GPU закончил кадр — результат probe уже лежит в readback-буфере
    if (m_probePending)
    {
        ResolveProbe();
        m_probePending = false;
    }
}

void RenderingSystem::SetCamera(const XMFLOAT3& eyePos, float yaw, float pitch)
{
    m_eyePos = eyePos;
    const float sy = std::sinf(yaw);
    const float cy = std::cosf(yaw);
    const float sp = std::sinf(pitch);
    const float cp = std::cosf(pitch);

    const XMVECTOR forward = XMVector3Normalize(XMVectorSet(sy * cp, sp, cy * cp, 0.f));
    XMStoreFloat3(&m_camForward, forward);
    XMStoreFloat4x4(
        &m_view,
        XMMatrixLookToLH(XMVectorSet(eyePos.x, eyePos.y, eyePos.z, 1.f), forward, XMVectorSet(0.f, 1.f, 0.f, 0.f)));
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
    compile("FullscreenVS", "vs_5_0", m_fullscreenVS);
    compile("AmbientPS", "ps_5_0", m_ambientPS);
    compile("DirectionalPS", "ps_5_0", m_directionalPS);
    compile("LightVolumeVS", "vs_5_0", m_lightVolumeVS);
    compile("LightVolumePS", "ps_5_0", m_lightVolumePS);
    compile("ProbePS", "ps_5_0", m_probePS);
    compile("HudPS", "ps_5_0", m_hudPS);

    m_inputLayout[0] = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    m_inputLayout[1] = { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    m_inputLayout[2] = { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };

    m_volumeInputLayout[0] = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
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
    gbufferRange.NumDescriptors = 3;
    gbufferRange.BaseShaderRegister = 1;
    gbufferRange.RegisterSpace = 0;
    gbufferRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // 0: CBV  b0      PassCB                     (все проходы)
    // 1: table t0      diffuse texture            (geometry)
    // 2: const b2      MaterialCB                 (geometry)
    // 3: const b1      LightPassCB (ambient, offset)   (lighting)
    // 4: table t1..t3  G-buffer: albedo, normal, depth (lighting)
    // 5: SRV   t4      StructuredBuffer<GpuLight>      (lighting)
    D3D12_ROOT_PARAMETER params[6]{};

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

    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[3].Constants.ShaderRegister = 1;
    params[3].Constants.RegisterSpace = 0;
    params[3].Constants.Num32BitValues = sizeof(LightPassConstants) / 4;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 1;
    params[4].DescriptorTable.pDescriptorRanges = &gbufferRange;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[5].Descriptor.ShaderRegister = 4;
    params[5].Descriptor.RegisterSpace = 0;
    params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MipLODBias = 0.f;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    sampler.MinLOD = 0.f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(_countof(params));
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
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
    geometryPso.DSVFormat = m_gBuffer->GetDepthStencilFormat();
    geometryPso.SampleDesc.Count = 1;
    geometryPso.SampleDesc.Quality = 0;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&geometryPso, IID_PPV_ARGS(&m_geometryPSO)), "Create geometry PSO");

    // Additive blending: каждый источник добавляет свой вклад к уже накопленному цвету
    D3D12_BLEND_DESC additiveBlend = blend;
    additiveBlend.RenderTarget[0].BlendEnable = TRUE;
    additiveBlend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    additiveBlend.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    additiveBlend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;

    D3D12_DEPTH_STENCIL_DESC noDepth{};
    noDepth.DepthEnable = FALSE;
    noDepth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    noDepth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    noDepth.StencilEnable = FALSE;

    // Fullscreen-проходы (ambient, directional)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC fullscreenPso{};
    fullscreenPso.pRootSignature = m_rootSignature.Get();
    fullscreenPso.VS = { m_fullscreenVS->GetBufferPointer(), m_fullscreenVS->GetBufferSize() };
    fullscreenPso.PS = { m_ambientPS->GetBufferPointer(), m_ambientPS->GetBufferSize() };
    fullscreenPso.BlendState = blend;
    fullscreenPso.SampleMask = UINT_MAX;
    fullscreenPso.RasterizerState = rasterizer;
    fullscreenPso.DepthStencilState = noDepth;
    fullscreenPso.InputLayout = { nullptr, 0 };
    fullscreenPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    fullscreenPso.NumRenderTargets = 1;
    fullscreenPso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    fullscreenPso.DSVFormat = m_gBuffer->GetDepthStencilFormat();
    fullscreenPso.SampleDesc.Count = 1;
    fullscreenPso.SampleDesc.Quality = 0;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&fullscreenPso, IID_PPV_ARGS(&m_ambientPSO)), "Create ambient PSO");

    fullscreenPso.PS = { m_directionalPS->GetBufferPointer(), m_directionalPS->GetBufferSize() };
    fullscreenPso.BlendState = additiveBlend;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&fullscreenPso, IID_PPV_ARGS(&m_directionalPSO)), "Create directional PSO");

    // Световые объёмы (point / spot):
    //  - рисуем только ЗАДНИЕ грани (cull front) — работает и когда камера внутри объёма;
    //  - depth test GREATER_EQUAL без записи: пиксель проходит, если поверхность сцены
    //    находится перед задней стенкой объёма (т.е. потенциально внутри него);
    //  - depth clip выключен, чтобы дальняя стенка не обрезалась far-плоскостью.
    D3D12_RASTERIZER_DESC volumeRasterizer = rasterizer;
    volumeRasterizer.CullMode = D3D12_CULL_MODE_FRONT;
    volumeRasterizer.FrontCounterClockwise = FALSE; // меш объёмов строится с CW-гранями снаружи
    volumeRasterizer.DepthClipEnable = FALSE;

    D3D12_DEPTH_STENCIL_DESC volumeDepth{};
    volumeDepth.DepthEnable = TRUE;
    volumeDepth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    volumeDepth.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    volumeDepth.StencilEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC volumePso = fullscreenPso;
    volumePso.VS = { m_lightVolumeVS->GetBufferPointer(), m_lightVolumeVS->GetBufferSize() };
    volumePso.PS = { m_lightVolumePS->GetBufferPointer(), m_lightVolumePS->GetBufferSize() };
    volumePso.BlendState = additiveBlend;
    volumePso.RasterizerState = volumeRasterizer;
    volumePso.DepthStencilState = volumeDepth;
    volumePso.InputLayout = { m_volumeInputLayout, static_cast<UINT>(_countof(m_volumeInputLayout)) };
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&volumePso, IID_PPV_ARGS(&m_lightVolumePSO)), "Create light volume PSO");

    // HUD: полноэкранный треугольник поверх кадра, обычный alpha blending
    D3D12_BLEND_DESC alphaBlend = blend;
    alphaBlend.RenderTarget[0].BlendEnable = TRUE;
    alphaBlend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    alphaBlend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    alphaBlend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC hudPso = fullscreenPso;
    hudPso.PS = { m_hudPS->GetBufferPointer(), m_hudPS->GetBufferSize() };
    hudPso.BlendState = alphaBlend;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&hudPso, IID_PPV_ARGS(&m_hudPSO)), "Create HUD PSO");

    // Probe: 2 RT 1x1 формата RGBA32F (позиция попадания и нормаль), без depth
    D3D12_GRAPHICS_PIPELINE_STATE_DESC probePso = fullscreenPso;
    probePso.PS = { m_probePS->GetBufferPointer(), m_probePS->GetBufferSize() };
    probePso.BlendState = blend;
    probePso.NumRenderTargets = 2;
    probePso.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    probePso.RTVFormats[1] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    probePso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&probePso, IID_PPV_ARGS(&m_probePSO)), "Create probe PSO");

    return true;
}

bool RenderingSystem::BuildGeometry()
{
    ObjMesh model{};
    if (!LoadObj(ResolveAssetPath("sponza/sponza.obj"), model))
        throw std::runtime_error("Failed to load sponza.obj");

    std::unordered_map<std::string, uint32_t> pathToIndex;
    std::vector<std::string> uniquePaths;

    auto getTextureIndex = [&](const std::string& path) -> uint32_t
    {
        if (path.empty())
            return 0;

        auto [it, inserted] = pathToIndex.emplace(path, static_cast<uint32_t>(uniquePaths.size()) + 1u);
        if (inserted)
            uniquePaths.push_back(path);
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
            drawItem.TextureIndex = getTextureIndex(material.diffusePath);
            drawItem.Material.BaseColor = XMFLOAT4(material.kd.x, material.kd.y, material.kd.z, 1.f);
            const float ksAverage = (material.ks.x + material.ks.y + material.ks.z) / 3.f;
            drawItem.Material.SurfaceParams.x = std::max(0.04f, ksAverage);
            drawItem.Material.SurfaceParams.y = std::max(8.f, std::min(material.ns, 128.f));
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

    createBuffer(vbSize, D3D12_RESOURCE_STATE_COPY_DEST, m_vertexBuffer);
    createBuffer(ibSize, D3D12_RESOURCE_STATE_COPY_DEST, m_indexBuffer);

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
        const D3D12_RESOURCE_DESC desc = TextureDesc2D(width, height, DXGI_FORMAT_B8G8R8A8_UNORM);
        ThrowIfFailed(
            m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)),
            "Create texture");
        return texture;
    };

    m_textures.clear();
    m_textures.resize(uniquePaths.size() + 1u);
    m_textures[0] = createTexture(1, 1);
    for (size_t i = 0; i < uniquePaths.size(); ++i)
    {
        if (loaded[i])
            m_textures[i + 1] = createTexture(images[i].width, images[i].height);
        else
            m_textures[i + 1] = m_textures[0];
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
    UploadTexture(m_device.Get(), m_commandList.Get(), m_textures[0].Get(), whiteTexture, textureUploads);

    for (size_t i = 0; i < uniquePaths.size(); ++i)
    {
        if (loaded[i])
            UploadTexture(m_device.Get(), m_commandList.Get(), m_textures[i + 1].Get(), images[i], textureUploads);
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
        srvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_textures[i].Get(), &srvDesc, srvHandle);
        srvHandle.ptr += m_srvDescriptorSize;
    }

    return true;
}

bool RenderingSystem::BuildFrameResources()
{
    auto uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);

    const uint32_t passCbSize = AlignConstantBufferSize(sizeof(PassConstants));
    const D3D12_RESOURCE_DESC passDesc = BufferDesc(passCbSize);
    ThrowIfFailed(
        m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &passDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_passConstantBuffer)),
        "Create pass constant buffer");

    // StructuredBuffer<GpuLight> — привязывается как root SRV (t4)
    const D3D12_RESOURCE_DESC lightDesc = BufferDesc(static_cast<UINT64>(MaxLights) * sizeof(GpuLight));
    ThrowIfFailed(
        m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &lightDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_lightBuffer)),
        "Create light buffer");

    D3D12_RANGE readRange{ 0, 0 };
    ThrowIfFailed(m_passConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_mappedPassConstants)), "Map pass constant buffer");
    ThrowIfFailed(m_lightBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_mappedLights)), "Map light buffer");
    return true;
}

bool RenderingSystem::BuildLightVolumes()
{
    std::vector<XMFLOAT3> vertices;
    std::vector<uint32_t> indices;

    auto addTriangle = [&](uint32_t base, uint32_t i0, uint32_t i1, uint32_t i2, const XMFLOAT3& interior)
    {
        const XMVECTOR a = XMLoadFloat3(&vertices[base + i0]);
        const XMVECTOR b = XMLoadFloat3(&vertices[base + i1]);
        const XMVECTOR c = XMLoadFloat3(&vertices[base + i2]);
        const XMVECTOR n = XMVector3Cross(XMVectorSubtract(b, a), XMVectorSubtract(c, a));
        const XMVECTOR centroid = XMVectorScale(XMVectorAdd(XMVectorAdd(a, b), c), 1.f / 3.f);
        const XMVECTOR outward = XMVectorSubtract(centroid, XMLoadFloat3(&interior));

        indices.push_back(i0);
        if (XMVectorGetX(XMVector3Dot(n, outward)) >= 0.f)
        {
            indices.push_back(i1);
            indices.push_back(i2);
        }
        else
        {
            indices.push_back(i2);
            indices.push_back(i1);
        }
    };

    // Сфера (UV-sphere). Вершины лежат на сфере, а грани — внутри неё,
    //     поэтому радиус чуть увеличен, чтобы многогранник целиком охватывал range.
    {
        const uint32_t stacks = 12;
        const uint32_t slices = 16;
        const float radius = 1.f / std::cos(XM_PI / static_cast<float>(slices)) * 1.03f;
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        m_sphereMesh.StartIndex = static_cast<uint32_t>(indices.size());
        m_sphereMesh.BaseVertex = static_cast<int32_t>(base);

        for (uint32_t i = 0; i <= stacks; ++i)
        {
            const float phi = XM_PI * static_cast<float>(i) / static_cast<float>(stacks);
            for (uint32_t j = 0; j <= slices; ++j)
            {
                const float theta = XM_2PI * static_cast<float>(j) / static_cast<float>(slices);
                vertices.push_back(XMFLOAT3(
                    radius * std::sin(phi) * std::cos(theta),
                    radius * std::cos(phi),
                    radius * std::sin(phi) * std::sin(theta)));
            }
        }

        const XMFLOAT3 center(0.f, 0.f, 0.f);
        for (uint32_t i = 0; i < stacks; ++i)
        {
            for (uint32_t j = 0; j < slices; ++j)
            {
                const uint32_t v0 = i * (slices + 1) + j;
                const uint32_t v1 = v0 + 1;
                const uint32_t v2 = v0 + (slices + 1);
                const uint32_t v3 = v2 + 1;
                if (i != 0)          addTriangle(base, v0, v1, v2, center);
                if (i != stacks - 1) addTriangle(base, v1, v3, v2, center);
            }
        }
        m_sphereMesh.IndexCount = static_cast<uint32_t>(indices.size()) - m_sphereMesh.StartIndex;
    }

    // Конус: вершина в (0,0,0), ось +Z, длина 1, радиус основания 1 (с запасом на полигональность)
    {
        const uint32_t segments = 16;
        const float radius = 1.f / std::cos(XM_PI / static_cast<float>(segments)) * 1.03f;
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        m_coneMesh.StartIndex = static_cast<uint32_t>(indices.size());
        m_coneMesh.BaseVertex = static_cast<int32_t>(base);

        vertices.push_back(XMFLOAT3(0.f, 0.f, 0.f)); // 0 — вершина конуса (позиция источника)
        vertices.push_back(XMFLOAT3(0.f, 0.f, 1.f)); // 1 — центр основания
        for (uint32_t j = 0; j < segments; ++j)
        {
            const float theta = XM_2PI * static_cast<float>(j) / static_cast<float>(segments);
            vertices.push_back(XMFLOAT3(radius * std::cos(theta), radius * std::sin(theta), 1.f));
        }

        const XMFLOAT3 interior(0.f, 0.f, 0.75f);
        for (uint32_t j = 0; j < segments; ++j)
        {
            const uint32_t a = 2 + j;
            const uint32_t b = 2 + (j + 1) % segments;
            addTriangle(base, 0, a, b, interior); // боковая поверхность
            addTriangle(base, 1, b, a, interior); // основание
        }
        m_coneMesh.IndexCount = static_cast<uint32_t>(indices.size()) - m_coneMesh.StartIndex;
    }

    // Меши маленькие и статические upload heap достаточно
    auto uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto createUploadBuffer = [&](UINT64 size, const void* data, ComPtr<ID3D12Resource>& resource)
    {
        const D3D12_RESOURCE_DESC desc = BufferDesc(size);
        ThrowIfFailed(
            m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource)),
            "Create light volume buffer");

        void* mapped = nullptr;
        D3D12_RANGE readRange{ 0, 0 };
        ThrowIfFailed(resource->Map(0, &readRange, &mapped), "Map light volume buffer");
        std::memcpy(mapped, data, static_cast<size_t>(size));
        resource->Unmap(0, nullptr);
    };

    const UINT64 vbSize = static_cast<UINT64>(vertices.size()) * sizeof(XMFLOAT3);
    const UINT64 ibSize = static_cast<UINT64>(indices.size()) * sizeof(uint32_t);
    createUploadBuffer(vbSize, vertices.data(), m_volumeVertexBuffer);
    createUploadBuffer(ibSize, indices.data(), m_volumeIndexBuffer);

    m_volumeVBView.BufferLocation = m_volumeVertexBuffer->GetGPUVirtualAddress();
    m_volumeVBView.SizeInBytes = static_cast<UINT>(vbSize);
    m_volumeVBView.StrideInBytes = sizeof(XMFLOAT3);

    m_volumeIBView.BufferLocation = m_volumeIndexBuffer->GetGPUVirtualAddress();
    m_volumeIBView.SizeInBytes = static_cast<UINT>(ibSize);
    m_volumeIBView.Format = DXGI_FORMAT_R32_UINT;
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
}

void RenderingSystem::UpdateLights(float dt)
{
    if (!m_mappedLights)
        return;

    m_time += dt;

    uint32_t written = 0;
    uint32_t counts[3] = { 0, 0, 0 };

    for (uint32_t type = LightDirectional; type <= LightSpot; ++type)
    {
        for (size_t i = 0; i < m_sceneLights.size() && written < MaxLights; ++i)
        {
            GpuLight light = m_sceneLights[i];
            if (static_cast<uint32_t>(light.Params.x) != type)
                continue;

            if (type != LightDirectional)
            {
                const float pulse = 0.9f + 0.1f * std::sinf(m_time * 1.35f + static_cast<float>(i) * 0.75f);
                light.ColorIntensity.w *= pulse;
            }

            m_mappedLights[written++] = light;
            ++counts[type];
        }

        // Пули и следы пулемёта - point light
        if (type == LightPoint)
        {
            auto pushPoint = [&](const XMFLOAT3& pos, const XMFLOAT3& color, float intensity, float range)
            {
                if (written >= MaxLights)
                    return;
                GpuLight light{};
                light.PositionRange = XMFLOAT4(pos.x, pos.y, pos.z, range);
                light.ColorIntensity = XMFLOAT4(color.x, color.y, color.z, intensity);
                light.Params = XMFLOAT4(static_cast<float>(LightPoint), 0.f, 0.f, 0.f);
                m_mappedLights[written++] = light;
                ++counts[LightPoint];
            };

            for (const Projectile& p : m_projectiles)
                pushPoint(p.Pos, p.Color, 4.f, 3.f);
            for (const LightMark& m : m_lightMarks)
                pushPoint(m.Pos, m.Color, 1.6f, 1.3f);
        }
    }

    m_directionalCount = counts[LightDirectional];
    m_pointCount = counts[LightPoint];
    m_spotCount = counts[LightSpot];
}

void RenderingSystem::CreateSceneLights()
{
    auto normalize = [](const XMFLOAT3& v) -> XMFLOAT3
    {
        XMFLOAT3 out{};
        XMStoreFloat3(&out, XMVector3Normalize(XMLoadFloat3(&v)));
        return out;
    };

    auto makeDirectional = [&](const XMFLOAT3& direction, const XMFLOAT3& color, float intensity) -> GpuLight
    {
        GpuLight light{};
        const XMFLOAT3 dir = normalize(direction);
        light.DirectionSpot = XMFLOAT4(dir.x, dir.y, dir.z, 0.f);
        light.ColorIntensity = XMFLOAT4(color.x, color.y, color.z, intensity);
        light.Params = XMFLOAT4(static_cast<float>(LightDirectional), 0.f, 0.f, 0.f);
        return light;
    };

    auto makePoint = [&](const XMFLOAT3& position, const XMFLOAT3& color, float intensity, float range) -> GpuLight
    {
        GpuLight light{};
        light.PositionRange = XMFLOAT4(position.x, position.y, position.z, range);
        light.ColorIntensity = XMFLOAT4(color.x, color.y, color.z, intensity);
        light.Params = XMFLOAT4(static_cast<float>(LightPoint), 0.f, 0.f, 0.f);
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
        light.Params = XMFLOAT4(static_cast<float>(LightSpot), std::cos(innerAngle), 0.f, 0.f);
        return light;
    };

    m_sceneLights.clear();

    // Directional слабый холодный свет сверху
    m_sceneLights.push_back(makeDirectional(XMFLOAT3(0.4f, -1.f, 0.3f), XMFLOAT3(0.6f, 0.6f, 1.0f), 0.8f));

    // Point разбросаны по первому этажу и галереям
    m_sceneLights.push_back(makePoint(XMFLOAT3(11.0f, 2.0f, -0.3f), XMFLOAT3(1.0f, 0.1f, 0.1f), 3.5f, 4.5f));
    m_sceneLights.push_back(makePoint(XMFLOAT3(-12.f, 1.5f, -4.f),  XMFLOAT3(1.0f, 0.55f, 0.15f), 3.f, 5.f));
    m_sceneLights.push_back(makePoint(XMFLOAT3(-12.f, 1.5f, 3.5f),  XMFLOAT3(0.2f, 0.9f, 1.0f), 3.f, 5.f));
    m_sceneLights.push_back(makePoint(XMFLOAT3(-4.f, 1.5f, -4.f),   XMFLOAT3(1.0f, 0.9f, 0.3f), 3.f, 5.f));
    m_sceneLights.push_back(makePoint(XMFLOAT3(-4.f, 1.5f, 3.5f),   XMFLOAT3(1.0f, 0.3f, 0.9f), 3.f, 5.f));
    m_sceneLights.push_back(makePoint(XMFLOAT3(4.f, 1.5f, -4.f),    XMFLOAT3(1.0f, 0.8f, 0.6f), 3.f, 5.f));
    m_sceneLights.push_back(makePoint(XMFLOAT3(4.f, 1.5f, 3.5f),    XMFLOAT3(0.3f, 0.4f, 1.0f), 3.f, 5.f));
    m_sceneLights.push_back(makePoint(XMFLOAT3(8.f, 6.5f, 5.f),     XMFLOAT3(0.4f, 1.0f, 0.4f), 3.f, 5.f));
    m_sceneLights.push_back(makePoint(XMFLOAT3(-8.f, 6.5f, -5.f),   XMFLOAT3(1.0f, 0.5f, 0.2f), 3.f, 5.f));
    // Spot
    m_sceneLights.push_back(makeSpot(XMFLOAT3(-10.f, 16.3f, -0.4f), XMFLOAT3(0.6f, -1.0f, 0.f),
        XMFLOAT3(0.2f, 1.0f, 0.2f), 7.0f, 19.0f, 7.0f, 19.0f));
    m_sceneLights.push_back(makeSpot(XMFLOAT3(0.f, 12.f, 0.f), XMFLOAT3(0.f, -1.f, 0.f),
        XMFLOAT3(1.0f, 0.95f, 0.8f), 5.0f, 16.0f, 12.0f, 22.0f));
    m_sceneLights.push_back(makeSpot(XMFLOAT3(-16.f, 3.f, 0.f), XMFLOAT3(1.f, -0.3f, 0.f),
        XMFLOAT3(1.0f, 0.4f, 0.1f), 6.0f, 14.0f, 10.0f, 18.0f));
}

//  ДОП. ЗАДАНИЕ: пулемёт лампочками
//
//  Пока зажат пробел, 12 раз в секунду:
//   1) в кадре дописывается probe pass — пиксельный шейдер в RT 1x1 читает центральный
//      пиксель G-buffer-а и восстанавливает точку, куда смотрит камера (GPU-«луч»);
//   2) после кадра CPU читает этот пиксель из readback-буфера;
//   3) из камеры вылетает «пуля» — движущийся point light, который по дороге освещает сцену;
//   4) долетев до точки попадания, пуля превращается в неподвижный point light — след.
//  Все пули и следы — обычные point light-ы deferred-рендера (сфера-объём на каждый).

bool RenderingSystem::BuildLightGun()
{
    auto defaultHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto readbackHeap = HeapProps(D3D12_HEAP_TYPE_READBACK);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = 2;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_probeRtvHeap)), "Create probe RTV heap");

    D3D12_RESOURCE_DESC desc = TextureDesc2D(1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_probeRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (auto& target : m_probeTargets)
    {
        ThrowIfFailed(
            m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&target)),
            "Create probe target");
        m_device->CreateRenderTargetView(target.Get(), nullptr, rtv);
        rtv.ptr += m_rtvDescriptorSize;
    }

    // Два пикселя по 16 байт, каждый со смещением, кратным D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT (512)
    const D3D12_RESOURCE_DESC readbackDesc = BufferDesc(2 * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    ThrowIfFailed(
        m_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_probeReadback)),
        "Create probe readback buffer");
    return true;
}

void RenderingSystem::UpdateLightGun(float dt)
{
    // Темп стрельбы
    m_fireCooldown = std::max(m_fireCooldown - dt, 0.f);
    if (m_firing && m_fireCooldown <= 0.f)
    {
        m_probePending = true;
        m_fireCooldown = FireInterval;
    }

    // Полёт пуль
    for (size_t i = 0; i < m_projectiles.size();)
    {
        Projectile& p = m_projectiles[i];
        const XMVECTOR pos = XMLoadFloat3(&p.Pos);
        const XMVECTOR toTarget = XMVectorSubtract(XMLoadFloat3(&p.Target), pos);
        const float dist = XMVectorGetX(XMVector3Length(toTarget));
        const float step = ProjectileSpeed * dt;

        if (step < dist)
        {
            XMStoreFloat3(&p.Pos, XMVectorAdd(pos, XMVectorScale(toTarget, step / dist)));
            ++i;
            continue;
        }

        // Долетела
        if (p.HasHit)
        {
            m_lightMarks.push_back({ p.Target, p.Color });
            while (m_lightMarks.size() > MaxMarks)
                m_lightMarks.pop_front();
        }
        m_projectiles[i] = m_projectiles.back();
        m_projectiles.pop_back();
    }
}

void RenderingSystem::RecordProbePass()
{
    D3D12_RESOURCE_BARRIER barriers[2]{};
    for (int i = 0; i < 2; ++i)
    {
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition.pResource = m_probeTargets[i].Get();
        barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    m_commandList->ResourceBarrier(2, barriers);

    // Разброс: луч идёт не ровно в центр, а в случайную точку круга вокруг прицела
    static std::mt19937 rng{ 1337u };
    std::uniform_real_distribution<float> uniform(0.f, 1.f);
    const float radius = SpreadFraction * static_cast<float>(m_height) * std::sqrt(uniform(rng));
    const float angle = XM_2PI * uniform(rng);
    m_lightPass.ProbeJitter = XMFLOAT2(radius * std::cos(angle), radius * std::sin(angle));
    m_commandList->SetGraphicsRoot32BitConstants(3, sizeof(LightPassConstants) / 4, &m_lightPass, 0);

    // Рисуем в 1x1: шейдер сам берёт пиксель G-buffer-а у прицела (G-buffer и PassCB уже привязаны)
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_probeRtvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_VIEWPORT viewport{ 0.f, 0.f, 1.f, 1.f, 0.f, 1.f };
    const D3D12_RECT scissor{ 0, 0, 1, 1 };
    m_commandList->OMSetRenderTargets(2, &rtv, TRUE, nullptr);
    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissor);
    m_commandList->SetPipelineState(m_probePSO.Get());
    m_commandList->DrawInstanced(3, 1, 0, 0);

    for (auto& barrier : barriers)
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    m_commandList->ResourceBarrier(2, barriers);

    // Копируем оба пикселя в readback-буфер, чтобы прочитать их на CPU
    for (UINT i = 0; i < 2; ++i)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = m_probeReadback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = i * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT };

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = m_probeTargets[i].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissorRect);
}

void RenderingSystem::ResolveProbe()
{
    XMFLOAT4 hit{};
    XMFLOAT4 normal{};

    uint8_t* mapped = nullptr;
    const D3D12_RANGE readRange{ 0, 2 * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT };
    ThrowIfFailed(m_probeReadback->Map(0, &readRange, reinterpret_cast<void**>(&mapped)), "Map probe readback");
    std::memcpy(&hit, mapped, sizeof(hit));
    std::memcpy(&normal, mapped + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, sizeof(normal));
    const D3D12_RANGE writeRange{ 0, 0 };
    m_probeReadback->Unmap(0, &writeRange);

    ++m_shotCount;

    // Цвет пули
    const float hue = std::fmod(static_cast<float>(m_shotCount) * 0.618034f, 1.f) * 6.f;
    const float x = 1.f - std::fabs(std::fmod(hue, 2.f) - 1.f);
    XMFLOAT3 color{};
    switch (static_cast<int>(hue))
    {
    case 0: color = { 1.f, x, 0.f }; break;
    case 1: color = { x, 1.f, 0.f }; break;
    case 2: color = { 0.f, 1.f, x }; break;
    case 3: color = { 0.f, x, 1.f }; break;
    case 4: color = { x, 0.f, 1.f }; break;
    default: color = { 1.f, 0.f, x }; break;
    }

    const XMVECTOR eye = XMLoadFloat3(&m_eyePos);
    const XMVECTOR forward = XMLoadFloat3(&m_camForward);

    Projectile p{};
    p.Color = color;
    // Стартуем чуть впереди и ниже камеры — как из ствола
    XMStoreFloat3(&p.Pos, XMVectorAdd(eye, XMVectorAdd(XMVectorScale(forward, 0.6f), XMVectorSet(0.f, -0.25f, 0.f, 0.f))));

    if (hit.w > 0.5f)
    {
        // Нормаль разворачиваем к камере и отодвигаем след от поверхности:
        // свет, лежащий прямо на стене, почти не освещает её (N·L ≈ 0)
        XMVECTOR n = XMVector3Normalize(XMLoadFloat4(&normal));
        const XMVECTOR hitPos = XMVectorSetW(XMLoadFloat4(&hit), 0.f);
        if (XMVectorGetX(XMVector3Dot(n, XMVectorSubtract(eye, hitPos))) < 0.f)
            n = XMVectorNegate(n);

        XMStoreFloat3(&p.Target, XMVectorAdd(hitPos, XMVectorScale(n, 0.2f)));
        p.HasHit = true;
    }
    else
    {
        // Луч ушёл в небо: пуля улетает и исчезает, следа нет
        XMStoreFloat3(&p.Target, XMVectorAdd(eye, XMVectorScale(forward, 80.f)));
        p.HasHit = false;
    }

    m_projectiles.push_back(p);
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