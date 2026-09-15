#include "D3D12Context.h"
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
#include <DirectXMath.h>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Path utilities

static std::string Dirname(const std::string& path)
{
    size_t p = path.find_last_of("/\\");
    return (p == std::string::npos) ? std::string() : path.substr(0, p + 1);
}

static std::string JoinPath(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    return (a.back() == '/' || a.back() == '\\') ? a + b : a + "/" + b;
}

static bool FileExistsA(const std::string& path)
{
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string ExeDirA()
{
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return (n && n < MAX_PATH) ? Dirname(std::string(buf)) : std::string();
}

static std::string ResolveAssetPath(const std::string& name)
{
    std::string exe = ExeDirA();
    std::vector<std::string> candidates = {
        name,
        JoinPath("assets", name),
        JoinPath("sponza", name),         // <-- добавить
        JoinPath("textures", name),       // <-- добавить
        JoinPath("sponza/textures", name),// <-- добавить (на всякий)
        JoinPath("shaders", name),        // <-- добавить (для шейдера)
        JoinPath(exe, name),
        JoinPath(JoinPath(exe, "assets"), name),
        JoinPath("..", name),
        JoinPath("../..", name),
    };
    for (auto& c : candidates)
        if (FileExistsA(c)) return c;
    return name;
}

static std::string Trim(std::string s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
}

static std::wstring ToWString(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
}

// ---------------------------------------------------------------------------
// Image loading

struct Image
{
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bgra;
};

static bool LoadImageTGA(const std::string& path, Image& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    uint8_t h[18]; f.read((char*)h, 18);
    if (!f) return false;

    uint8_t idLen = h[0], colorMapType = h[1], imageType = h[2];
    uint16_t w = h[12] | (h[13] << 8), hh = h[14] | (h[15] << 8);
    uint8_t bpp = h[16], desc = h[17];

    if (colorMapType != 0 || !w || !hh) return false;
    if (bpp != 24 && bpp != 32)         return false;
    if (imageType != 2 && imageType != 10) return false;

    if (idLen) f.seekg(idLen, std::ios::cur);

    const uint32_t Bpp = bpp / 8, n = (uint32_t)w * hh;
    out.width = w; out.height = hh;
    out.bgra.assign((size_t)n * 4, 255);

    auto put = [&](uint32_t i, const uint8_t* px) {
        size_t o = (size_t)i * 4;
        out.bgra[o]   = px[0]; out.bgra[o+1] = px[1];
        out.bgra[o+2] = px[2]; out.bgra[o+3] = (Bpp == 4) ? px[3] : 255;
    };

    std::vector<uint8_t> px(Bpp);
    if (imageType == 2)
    {
        std::vector<uint8_t> buf((size_t)n * Bpp);
        f.read((char*)buf.data(), buf.size());
        if (!f) return false;
        for (uint32_t i = 0; i < n; ++i) put(i, &buf[(size_t)i * Bpp]);
    }
    else
    {
        for (uint32_t i = 0; i < n; )
        {
            uint8_t rep; f.read((char*)&rep, 1); if (!f) return false;
            uint32_t cnt = (rep & 0x7F) + 1;
            if (rep & 0x80) {
                f.read((char*)px.data(), Bpp); if (!f) return false;
                for (uint32_t k = 0; k < cnt && i < n; ++k, ++i) put(i, px.data());
            } else {
                for (uint32_t k = 0; k < cnt && i < n; ++k, ++i) {
                    f.read((char*)px.data(), Bpp); if (!f) return false;
                    put(i, px.data());
                }
            }
        }
    }

    if (!(desc & 0x20))
    {
        uint32_t rowBytes = (uint32_t)w * 4;
        std::vector<uint8_t> tmp(rowBytes);
        for (uint32_t y = 0; y < hh / 2; ++y)
        {
            uint8_t* a = out.bgra.data() + (size_t)y * rowBytes;
            uint8_t* b = out.bgra.data() + (size_t)(hh - 1 - y) * rowBytes;
            std::memcpy(tmp.data(), a, rowBytes);
            std::memcpy(a, b, rowBytes);
            std::memcpy(b, tmp.data(), rowBytes);
        }
    }
    return true;
}

static bool LoadImageWIC(const std::string& path, Image& out)
{
    static bool comInited = false;
    if (!comInited) { CoInitializeEx(nullptr, COINIT_MULTITHREADED); comInited = true; }

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory,  nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
            return false;

    std::wstring wpath(path.begin(), path.end());
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand, &decoder))) return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;

    UINT w = 0, h = 0; frame->GetSize(&w, &h);
    if (!w || !h) return false;

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;

    out.width = w; out.height = h;
    out.bgra.resize((size_t)w * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)out.bgra.size(), out.bgra.data()));
}

static std::string ExtLower(const std::string& path)
{
    size_t p = path.find_last_of('.');
    if (p == std::string::npos) return {};
    std::string ext = path.substr(p);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return ext;
}

static bool LoadImage(const std::string& path, Image& out)
{
    return (ExtLower(path) == ".tga") ? LoadImageTGA(path, out) : LoadImageWIC(path, out);
}

// ---------------------------------------------------------------------------
// OBJ / MTL loading


struct MtlData
{
    std::string diffusePath;
    XMFLOAT3    kd{ 1.f, 1.f, 1.f };
};

static std::unordered_map<std::string, MtlData> LoadMtlData(const std::string& mtlPath)
{
    std::unordered_map<std::string, MtlData> out;
    std::ifstream f(mtlPath);
    if (!f.is_open()) return out;

    std::string baseDir = Dirname(mtlPath);
    std::string line, cur;

    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string cmd; ss >> cmd;

        if (cmd == "newmtl")          { ss >> cur; }
        else if (cmd == "Kd" && !cur.empty())
        {
            float r = 1.f, g = 1.f, b = 1.f;
            ss >> r >> g >> b;
            out[cur].kd = { r, g, b };
        }
        else if (cmd == "map_Kd" && !cur.empty())
        {
            std::string tok, last;
            while (ss >> tok) last = tok;
            if (!last.empty()) out[cur].diffusePath = JoinPath(baseDir, last);
        }
    }
    return out;
}

struct ObjKey
{
    int p = -1, t = -1, n = -1;
    bool operator==(const ObjKey& o) const { return p==o.p && t==o.t && n==o.n; }
};
struct ObjKeyHash
{
    size_t operator()(const ObjKey& k) const noexcept
    { return (size_t)k.p*73856093u ^ (size_t)k.t*19349663u ^ (size_t)k.n*83492791u; }
};

static int FixIdx(int i, int sz) { return i > 0 ? i-1 : i < 0 ? sz+i : -1; }

static void ParseFaceTok(const std::string& tok, int& p, int& t, int& n)
{
    p = t = n = 0;
    size_t s1 = tok.find('/');
    if (s1 == std::string::npos) { p = std::stoi(tok); return; }
    if (s1) p = std::stoi(tok.substr(0, s1));
    size_t s2 = tok.find('/', s1+1);
    if (s2 == std::string::npos) { if (s1+1 < tok.size()) t = std::stoi(tok.substr(s1+1)); return; }
    if (s2 > s1+1) t = std::stoi(tok.substr(s1+1, s2-s1-1));
    if (s2+1 < tok.size()) n = std::stoi(tok.substr(s2+1));
}

struct ObjGroup { uint32_t start = 0, count = 0; std::string mtl; };

struct ObjMesh
{
    std::vector<D3D12Context::Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<ObjGroup> groups;
    std::unordered_map<std::string, MtlData> mtlData;
};

static bool LoadObj(const std::string& objPath, ObjMesh& out)
{
    std::ifstream file(objPath);
    if (!file.is_open()) return false;

    std::string baseDir = Dirname(objPath);

    std::vector<XMFLOAT3> pos, nrm;
    std::vector<XMFLOAT2> uv;
    pos.reserve(200000); nrm.reserve(200000); uv.reserve(200000);

    std::unordered_map<ObjKey, uint32_t, ObjKeyHash> vmap;
    std::vector<std::string> mtlLibs;
    std::string curMtl;

    auto switchMtl = [&](const std::string& newMtl)
    {
        if (!out.groups.empty() && curMtl != newMtl)
            out.groups.back().count = (uint32_t)out.indices.size() - out.groups.back().start;
        if (out.groups.empty() || curMtl != newMtl)
        {
            curMtl = newMtl;
            out.groups.push_back({ (uint32_t)out.indices.size(), 0, curMtl });
        }
    };

    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#') continue;

        if (line.rfind("mtllib ", 0) == 0)
        {
            std::istringstream ss(line); std::string cmd, rest;
            ss >> cmd; std::getline(ss, rest); rest = Trim(rest);
            std::istringstream ls(rest); std::string name;
            while (ls >> name) mtlLibs.push_back(JoinPath(baseDir, name));
            continue;
        }
        if (line.rfind("usemtl ", 0) == 0)
        {
            std::istringstream ss(line); std::string cmd, rest;
            ss >> cmd; std::getline(ss, rest); rest = Trim(rest);
            if (!rest.empty()) switchMtl(rest);
            continue;
        }

        std::istringstream ss(line);
        std::string tag; ss >> tag;

        if (tag == "v")  { XMFLOAT3 v; ss>>v.x>>v.y>>v.z; pos.push_back(v); }
        else if (tag == "vn") { XMFLOAT3 v; ss>>v.x>>v.y>>v.z; nrm.push_back(v); }
        else if (tag == "vt") { XMFLOAT2 v; ss>>v.x>>v.y; v.y = 1.f-v.y; uv.push_back(v); }
        else if (tag == "f")
        {
            if (out.groups.empty())
                out.groups.push_back({ (uint32_t)out.indices.size(), 0, curMtl });

            std::vector<uint32_t> face; face.reserve(8);
            std::string tok;
            while (ss >> tok)
            {
                int pi=0, ti=0, ni=0;
                ParseFaceTok(tok, pi, ti, ni);
                pi = FixIdx(pi, (int)pos.size());
                ti = FixIdx(ti, (int)uv.size());
                ni = FixIdx(ni, (int)nrm.size());
                if (pi < 0) continue;

                ObjKey key{pi, ti, ni};
                auto it = vmap.find(key);
                if (it == vmap.end())
                {
                    D3D12Context::Vertex v{};
                    v.Pos    = pos[pi];
                    v.Normal = (ni >= 0) ? nrm[ni] : XMFLOAT3(0,1,0);
                    v.TexC   = (ti >= 0) ? uv[ti]  : XMFLOAT2(0,0);
                    uint32_t idx = (uint32_t)out.vertices.size();
                    out.vertices.push_back(v);
                    vmap.emplace(key, idx);
                    face.push_back(idx);
                }
                else face.push_back(it->second);
            }
            for (size_t i = 1; i+1 < face.size(); ++i)
            {
                out.indices.push_back(face[0]);
                out.indices.push_back(face[i]);
                out.indices.push_back(face[i+1]);
            }
        }
    }

    if (!out.groups.empty())
        out.groups.back().count = (uint32_t)out.indices.size() - out.groups.back().start;

    for (const auto& lib : mtlLibs)
    {
        auto data = LoadMtlData(lib);
        out.mtlData.insert(data.begin(), data.end());
    }

    return !out.vertices.empty() && !out.indices.empty();
}

// ---------------------------------------------------------------------------
// D3D12 helpers

static void ThrowIfFailed(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s (hr=0x%08X)", what, (unsigned)hr);
        throw std::runtime_error(buf);
    }
}

static uint32_t AlignCB(uint32_t size) { return (size + 255u) & ~255u; }

static D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type;
    p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    p.CreationNodeMask = p.VisibleNodeMask = 1;
    return p;
}

static D3D12_RESOURCE_DESC BufferDesc(UINT64 size)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN; d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

static D3D12_RESOURCE_DESC Tex2DDesc(uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = fmt; d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return d;
}

// ---------------------------------------------------------------------------
// D3D12Context

bool D3D12Context::Initialize(HWND hwnd, uint32_t width, uint32_t height)
{
    m_hwnd = hwnd; m_width = width; m_height = height;

#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
#endif

    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&m_factory)), "CreateDXGIFactory1");

    CreateDevice();
    CreateCommandObjects();

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "CreateFence");
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) throw std::runtime_error("CreateEvent failed");

    CreateSwapChain();

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_cbvSrvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    CreateDescriptorHeaps();
    CreateRtvForBackBuffers();
    CreateDepthStencil();

    m_viewport = { 0.f, 0.f, (float)m_width, (float)m_height, 0.f, 1.f };
    m_scissorRect = { 0, 0, (LONG)m_width, (LONG)m_height };

    XMStoreFloat4x4(&m_world, XMMatrixScaling(.01f, .01f, .01f));

    XMVECTOR eye = XMVectorSet(m_eyePos.x, m_eyePos.y, m_eyePos.z, 1.f);
    XMStoreFloat4x4(&m_view, XMMatrixLookAtLH(eye, XMVectorZero(), XMVectorSet(0,1,0,0)));

    float aspect = m_height > 0 ? (float)m_width / m_height : 1.f;
    XMStoreFloat4x4(&m_proj, XMMatrixPerspectiveFovLH(0.25f * XM_PI, aspect, 0.05f, 1000.f));

    BuildShaders();
    BuildGeometry();
    BuildConstantBuffer();
    BuildRootSignature();
    BuildPSO();

    m_initialized = true;
    return true;
}

void D3D12Context::Shutdown()
{
    if (m_cmdQueue) FlushCommandQueue();
    if (m_objectCB && m_mappedObjectCB) { m_objectCB->Unmap(0, nullptr); m_mappedObjectCB = nullptr; }
    if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
}

bool D3D12Context::CreateDevice()
{
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
    if (FAILED(hr))
    {
        ComPtr<IDXGIAdapter> warp;
        ThrowIfFailed(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "EnumWarpAdapter");
        ThrowIfFailed(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)), "D3D12CreateDevice (WARP)");
    }
    return true;
}

bool D3D12Context::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_cmdQueue)), "CreateCommandQueue");
    ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmdAlloc)), "CreateCommandAllocator");
    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&m_cmdList)), "CreateCommandList");
    ThrowIfFailed(m_cmdList->Close(), "CommandList initial Close");
    return true;
}

bool D3D12Context::CreateSwapChain()
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Width = m_width;
    sd.BufferDesc.Height = m_height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = { 60, 1 };
    sd.SampleDesc.Count  = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kSwapChainBufferCount;
    sd.OutputWindow = m_hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ThrowIfFailed(m_factory->CreateSwapChain(m_cmdQueue.Get(), &sd, m_swapChain.GetAddressOf()), "CreateSwapChain");
    return true;
}

bool D3D12Context::CreateDescriptorHeaps()
{
    auto mkHeap = [&](uint32_t n, D3D12_DESCRIPTOR_HEAP_TYPE type, D3D12_DESCRIPTOR_HEAP_FLAGS flags, ComPtr<ID3D12DescriptorHeap>& heap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC d{}; d.NumDescriptors = n; d.Type = type; d.Flags = flags;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap");
    };
    mkHeap(kSwapChainBufferCount, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, m_rtvHeap);
    mkHeap(1, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, m_dsvHeap);
    return true;
}

bool D3D12Context::CreateRtvForBackBuffers()
{
    auto h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < kSwapChainBufferCount; ++i)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_swapChainBuffers[i])), "GetBuffer");
        m_device->CreateRenderTargetView(m_swapChainBuffers[i].Get(), nullptr, h);
        h.ptr += m_rtvDescriptorSize;
    }
    return true;
}

bool D3D12Context::CreateDepthStencil()
{
    m_depthStencilBuffer.Reset();

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width; desc.Height = m_height;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE cv{};
    cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    cv.DepthStencil.Depth = 1.f;

    auto hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, &cv, IID_PPV_ARGS(&m_depthStencilBuffer)), "Create DepthStencil");

    ThrowIfFailed(m_cmdAlloc->Reset(), "CmdAlloc Reset (DS)");
    ThrowIfFailed(m_cmdList->Reset(m_cmdAlloc.Get(), nullptr), "CmdList Reset (DS)");

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_depthStencilBuffer.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &b);

    ThrowIfFailed(m_cmdList->Close(), "CmdList Close (DS)");
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);
    FlushCommandQueue();

    m_device->CreateDepthStencilView(m_depthStencilBuffer.Get(), nullptr,
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    return true;
}

void D3D12Context::OnResize(uint32_t width, uint32_t height)
{
    if (!m_initialized || !width || !height) return;
    m_width = width; m_height = height;

    FlushCommandQueue();
    for (auto& b : m_swapChainBuffers) b.Reset();

    ThrowIfFailed(m_swapChain->ResizeBuffers(kSwapChainBufferCount, m_width, m_height,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0), "ResizeBuffers");
    m_currBackBuffer = 0;

    CreateRtvForBackBuffers();
    CreateDepthStencil();

    m_viewport    = { 0.f, 0.f, (float)m_width, (float)m_height, 0.f, 1.f };
    m_scissorRect = { 0, 0, (LONG)m_width, (LONG)m_height };

    float aspect = (float)m_width / m_height;
    XMStoreFloat4x4(&m_proj, XMMatrixPerspectiveFovLH(0.25f * XM_PI, aspect, 0.05f, 1000.f));
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Context::CurrentBackBufferRTV() const
{
    auto h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (size_t)m_currBackBuffer * m_rtvDescriptorSize;
    return h;
}

ID3D12Resource* D3D12Context::CurrentBackBuffer() const
{
    return m_swapChainBuffers[m_currBackBuffer].Get();
}

void D3D12Context::Draw(float dt)
{
    if (!m_initialized) return;

    m_time += dt;
    // анимация текстуры
    m_uvOffset.x = 0.3f * sinf(m_time * 1.5f);
    m_uvOffset.y = 0.f;
    // m_uvTiling = { 1.f, 1.f };
    float tilingScale = 1.0f + 0.5f * (0.5f + 0.5f * sinf(m_time * 0.7f));
    m_uvTiling = { tilingScale, tilingScale };

    UpdateConstantBuffer();

    ThrowIfFailed(m_cmdAlloc->Reset(), "CmdAlloc Reset");
    ThrowIfFailed(m_cmdList->Reset(m_cmdAlloc.Get(), m_pso.Get()), "CmdList Reset");

    m_cmdList->RSSetViewports(1, &m_viewport);
    m_cmdList->RSSetScissorRects(1, &m_scissorRect);
    m_cmdList->SetGraphicsRootSignature(m_rootSignature.Get());

    D3D12_RESOURCE_BARRIER toRT{};
    toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRT.Transition.pResource = CurrentBackBuffer();
    toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &toRT);

    auto rtv = CurrentBackBufferRTV();
    auto dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_cmdList->OMSetRenderTargets(1, &rtv, TRUE, &dsv);

    const float clearColor[] = { 0.1f, 0.1f, 0.35f, 1.f };
    m_cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);

    ID3D12DescriptorHeap* heaps[] = { m_cbvHeap.Get() };
    m_cmdList->SetDescriptorHeaps(1, heaps);

    auto base = m_cbvHeap->GetGPUDescriptorHandleForHeapStart();
    m_cmdList->SetGraphicsRootDescriptorTable(0, base);
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmdList->IASetVertexBuffers(0, 1, &m_vbv);
    m_cmdList->IASetIndexBuffer(&m_ibv);

    for (const auto& di : m_drawItems)
    {
        auto srv = base;
        srv.ptr += (UINT64)di.TextureSrvIndex * m_cbvSrvUavDescriptorSize;
        m_cmdList->SetGraphicsRootDescriptorTable(1, srv);

        float kd[4] = { di.Kd.x, di.Kd.y, di.Kd.z, 1.f };
        m_cmdList->SetGraphicsRoot32BitConstants(2, 4, kd, 0);

        m_cmdList->DrawIndexedInstanced(di.IndexCount, 1, di.StartIndexLocation, 0, 0);
    }

    D3D12_RESOURCE_BARRIER toPresent = toRT;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_cmdList->ResourceBarrier(1, &toPresent);

    ThrowIfFailed(m_cmdList->Close(), "CmdList Close");
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);

    ThrowIfFailed(m_swapChain->Present(1, 0), "Present");
    m_currBackBuffer = (m_currBackBuffer + 1) % kSwapChainBufferCount;
    FlushCommandQueue();
}

void D3D12Context::SetCamera(const XMFLOAT3& eyePos, float yaw, float pitch)
{
    m_eyePos = eyePos;
    float sy = sinf(yaw), cy = cosf(yaw), sp = sinf(pitch), cp = cosf(pitch);
    XMVECTOR fwd = XMVector3Normalize(XMVectorSet(sy*cp, sp, cy*cp, 0.f));
    XMStoreFloat4x4(&m_view, XMMatrixLookToLH(
        XMVectorSet(eyePos.x, eyePos.y, eyePos.z, 1.f), fwd, XMVectorSet(0,1,0,0)));
}

void D3D12Context::FlushCommandQueue()
{
    const uint64_t val = ++m_fenceValue;
    ThrowIfFailed(m_cmdQueue->Signal(m_fence.Get(), val), "Fence Signal");
    if (m_fence->GetCompletedValue() < val)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(val, m_fenceEvent), "SetEventOnCompletion");
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

bool D3D12Context::BuildShaders()
{
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> errors;
    auto shaderPath = ToWString(ResolveAssetPath("shaders/Shaders.hlsl"));

    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out)
    {
        errors.Reset();
        HRESULT hr = D3DCompileFromFile(shaderPath.c_str(), nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target, flags, 0, &out, &errors);
        if (FAILED(hr))
        {
            if (errors) throw std::runtime_error((const char*)errors->GetBufferPointer());
            ThrowIfFailed(hr, entry);
        }
    };
    compile("VSMain", "vs_5_0", m_vsBytecode);
    compile("PSMain", "ps_5_0", m_psBytecode);

    m_inputLayout[0] = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    m_inputLayout[1] = { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    m_inputLayout[2] = { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    return true;
}

// upload image data to GPU texture & -> to SRV state
static void UploadTexture(
    ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* tex, const Image& img,
    std::vector<ComPtr<ID3D12Resource>>& uploads)
{
    auto up = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC td = Tex2DDesc(img.width, img.height, DXGI_FORMAT_B8G8R8A8_UNORM);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &totalBytes);

    auto upDesc = BufferDesc(totalBytes);
    ComPtr<ID3D12Resource> upload;
    ThrowIfFailed(device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &upDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "Create tex upload");

    void* mapped; D3D12_RANGE rr{0,0};
    ThrowIfFailed(upload->Map(0, &rr, &mapped), "Map tex upload");

    uint32_t srcPitch = img.width * 4;
    uint32_t dstPitch = fp.Footprint.RowPitch;
    for (uint32_t y = 0; y < img.height; ++y)
        std::memcpy((uint8_t*)mapped + (size_t)y*dstPitch, img.bgra.data() + (size_t)y*srcPitch, srcPitch);
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    src.PlacedFootprint = fp;
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER tb{};
    tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    tb.Transition.pResource = tex;
    tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    tb.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &tb);

    uploads.push_back(upload);
}

bool D3D12Context::BuildGeometry()
{
    ObjMesh model{};
    if (!LoadObj(ResolveAssetPath("sponza/sponza.obj"), model))
        throw std::runtime_error("Failed to load sponza.obj");

    std::unordered_map<std::string, uint32_t> pathToIdx;
    std::vector<std::string> uniquePaths;

    auto getTexIdx = [&](const std::string& path) -> uint32_t
    {
        if (path.empty()) return 1; // slot 1 => white fallback SRV
        auto [it, inserted] = pathToIdx.emplace(path, 2u + (uint32_t)uniquePaths.size());
        if (inserted) uniquePaths.push_back(path);
        return it->second;
    };

    for (const auto& g : model.groups)
    {
        DrawItem di{};
        di.StartIndexLocation = g.start;
        di.IndexCount = g.count;

        auto it = model.mtlData.find(g.mtl);
        if (it != model.mtlData.end())
        {
            di.Kd = it->second.kd;
            di.TextureSrvIndex = getTexIdx(it->second.diffusePath);
        }
        m_drawItems.push_back(di);
    }

    // loading all images
    std::vector<Image> images(uniquePaths.size());
    std::vector<bool>  loaded(uniquePaths.size(), false);
    for (size_t i = 0; i < uniquePaths.size(); ++i)
        loaded[i] = LoadImage(uniquePaths[i], images[i]);

    // upload VB + IB
    UINT64 vbSize = model.vertices.size() * sizeof(Vertex);
    UINT64 ibSize = model.indices.size() * sizeof(uint32_t);

    auto def = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto up  = HeapProps(D3D12_HEAP_TYPE_UPLOAD);

    auto mkBuf = [&](UINT64 sz, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& res)
    {
        auto desc = BufferDesc(sz);
        ThrowIfFailed(m_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &desc,
        state, nullptr, IID_PPV_ARGS(&res)), "CreateBuffer");
    };
    mkBuf(vbSize, D3D12_RESOURCE_STATE_COPY_DEST, m_vertexBufferGPU);
    mkBuf(ibSize, D3D12_RESOURCE_STATE_COPY_DEST, m_indexBufferGPU);

    auto upload = [&](UINT64 sz, const void* data) -> ComPtr<ID3D12Resource>
    {
        auto desc = BufferDesc(sz);
        ComPtr<ID3D12Resource> res;
        ThrowIfFailed(m_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res)), "CreateUploadBuf");
        void* mapped; D3D12_RANGE rr{0,0};
        ThrowIfFailed(res->Map(0, &rr, &mapped), "Map");
        std::memcpy(mapped, data, (size_t)sz);
        res->Unmap(0, nullptr);
        return res;
    };
    auto vbUp = upload(vbSize, model.vertices.data());
    auto ibUp = upload(ibSize, model.indices.data());

    // create GPU textures
    auto mkTex = [&](uint32_t w, uint32_t h) -> ComPtr<ID3D12Resource>
    {
        auto td = Tex2DDesc(w, h, DXGI_FORMAT_B8G8R8A8_UNORM);
        ComPtr<ID3D12Resource> tex;
        ThrowIfFailed(m_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)), "CreateTex");
        return tex;
    };

    m_textures.clear();
    m_textures.push_back(mkTex(1, 1));  // index 0 = white fallback
    for (size_t i = 0; i < uniquePaths.size(); ++i)
        m_textures.push_back(loaded[i] ? mkTex(images[i].width, images[i].height) : m_textures[0]);

    // recording uploads
    ThrowIfFailed(m_cmdAlloc->Reset(), "CmdAlloc Reset (Geo)");
    ThrowIfFailed(m_cmdList->Reset(m_cmdAlloc.Get(), nullptr), "CmdList Reset (Geo)");

    m_cmdList->CopyBufferRegion(m_vertexBufferGPU.Get(), 0, vbUp.Get(), 0, vbSize);
    m_cmdList->CopyBufferRegion(m_indexBufferGPU.Get(),  0, ibUp.Get(), 0, ibSize);

    D3D12_RESOURCE_BARRIER bufBarriers[2]{};
    for (auto& b : bufBarriers) b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bufBarriers[0].Transition = { m_vertexBufferGPU.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER };
    bufBarriers[1].Transition = { m_indexBufferGPU.Get(),  D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER };
    m_cmdList->ResourceBarrier(2, bufBarriers);

    std::vector<ComPtr<ID3D12Resource>> texUploads;

    // White 1x1 texture
    Image white1x1; white1x1.width = white1x1.height = 1; white1x1.bgra = {255,255,255,255};
    UploadTexture(m_device.Get(), m_cmdList.Get(), m_textures[0].Get(), white1x1, texUploads);

    for (size_t i = 0; i < uniquePaths.size(); ++i)
        if (loaded[i])
            UploadTexture(m_device.Get(), m_cmdList.Get(), m_textures[i+1].Get(), images[i], texUploads);

    ThrowIfFailed(m_cmdList->Close(), "CmdList Close (Geo)");
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);
    FlushCommandQueue();

    m_vbv = { m_vertexBufferGPU->GetGPUVirtualAddress(), (UINT)vbSize, sizeof(Vertex) };
    m_ibv = { m_indexBufferGPU->GetGPUVirtualAddress(),  (UINT)ibSize, DXGI_FORMAT_R32_UINT };
    return true;
}

bool D3D12Context::BuildConstantBuffer()
{
    if (m_textures.empty()) throw std::runtime_error("No textures");

    m_objectCBByteSize = AlignCB(sizeof(ObjectConstants));
    auto up = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto desc = BufferDesc(m_objectCBByteSize);
    ThrowIfFailed(m_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_objectCB)), "Create CB");

    D3D12_RANGE rr{0,0};
    ThrowIfFailed(m_objectCB->Map(0, &rr, reinterpret_cast<void**>(&m_mappedObjectCB)), "Map CB");

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 1 + (UINT)m_textures.size();
    hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_cbvHeap)), "Create CBV heap");

    // CBV at slot 0
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvd{};
    cbvd.BufferLocation = m_objectCB->GetGPUVirtualAddress();
    cbvd.SizeInBytes    = m_objectCBByteSize;
    m_device->CreateConstantBufferView(&cbvd, m_cbvHeap->GetCPUDescriptorHandleForHeapStart());

    // SRVs at slots 1..N
    auto h = m_cbvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += m_cbvSrvUavDescriptorSize;
    for (auto& tex : m_textures)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(tex.Get(), &srv, h);
        h.ptr += m_cbvSrvUavDescriptorSize;
    }

    UpdateConstantBuffer();
    return true;
}

bool D3D12Context::BuildRootSignature()
{
    D3D12_DESCRIPTOR_RANGE cbvRange{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    D3D12_DESCRIPTOR_RANGE srvRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};

    D3D12_ROOT_PARAMETER params[3]{};
    params[0] = { D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, {}, D3D12_SHADER_VISIBILITY_ALL };
    params[0].DescriptorTable = { 1, &cbvRange };
    params[1] = { D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, {}, D3D12_SHADER_VISIBILITY_PIXEL };
    params[1].DescriptorTable = { 1, &srvRange };
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants = { 1, 0, 4 };
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 3;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &samp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized, errors;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(hr))
    {
        if (errors) throw std::runtime_error((const char*)errors->GetBufferPointer());
        ThrowIfFailed(hr, "SerializeRootSignature");
    }
    ThrowIfFailed(m_device->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)), "CreateRootSignature");
    return true;
}

bool D3D12Context::BuildPSO()
{
    D3D12_RASTERIZER_DESC rast{};
    rast.FillMode = D3D12_FILL_MODE_SOLID;
    rast.CullMode = D3D12_CULL_MODE_NONE;
    rast.FrontCounterClockwise = TRUE;
    rast.DepthClipEnable = TRUE;
    rast.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rast.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rast.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;

    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSignature.Get();
    pso.VS = { m_vsBytecode->GetBufferPointer(), m_vsBytecode->GetBufferSize() };
    pso.PS = { m_psBytecode->GetBufferPointer(), m_psBytecode->GetBufferSize() };
    pso.BlendState = blend;
    pso.RasterizerState = rast;
    pso.DepthStencilState = ds;
    pso.SampleMask = UINT_MAX;
    pso.InputLayout = { m_inputLayout, 3 };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pso.SampleDesc.Count = 1;

    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)), "CreatePSO");
    return true;
}

void D3D12Context::UpdateConstantBuffer()
{
    if (!m_mappedObjectCB) return;

    ObjectConstants cb{};
    XMMATRIX world = XMLoadFloat4x4(&m_world);
    XMMATRIX view = XMLoadFloat4x4(&m_view);
    XMMATRIX proj = XMLoadFloat4x4(&m_proj);

    XMStoreFloat4x4(&cb.World, XMMatrixTranspose(world));
    XMStoreFloat4x4(&cb.WorldViewProj, XMMatrixTranspose(world * view * proj));

    cb.EyePosW  = m_eyePos;
    XMStoreFloat3(&cb.LightDirW, XMVector3Normalize(XMLoadFloat3(&m_lightDir)));

    cb.UvOffsetTiling = { m_uvOffset.x, m_uvOffset.y, m_uvTiling.x, m_uvTiling.y };
    cb.SpecPower = 64.f;

    std::memcpy(m_mappedObjectCB, &cb, sizeof(cb));
}
