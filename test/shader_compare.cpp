// Offline equivalence test for replacement pixel shaders.
// Renders the game's original ShadowBuffer pixel shader and our reconstruction on
// D3D12 WARP (software, deterministic) with identical synthetic inputs - linear depth,
// a shadow map, the $Globals constants - and compares every output pixel.
//
// Usage: shader_compare.exe [--hbao | --spot | --volumetric] <vs.dxil> <original_ps.dxil> <replacement_ps.dxil>
//        [seeds] [settings]
// --spot: DeferredLight spotlightPS; --volumetric: VolumetricLight #15 (VS: test/volumetric_vs.hlsl).
// --hbao: the inputs of HBAO.rfx coarseAOPS instead of ShadowBuffer (t0 RGBA normals,
// t1 quarter-res view depth, two ordinary samplers, HBAO+ constants).
// Exit code 0 = all seeds within tolerance.
// With [settings] (the AWTweaks settings code the enhanced shader decodes from
// g_fCascadePCFSize_d3t: 1 + sun * 100 + contact) the outputs are expected to differ: it checks the replacement for
// NaNs and out-of-range values instead, and writes both shadow masks of seed 1 as
// shadow_a.pgm / shadow_b.pgm.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#define CHECKHR(x)                                                                 \
    do {                                                                           \
        HRESULT hr_ = (x);                                                         \
        if (FAILED(hr_)) {                                                         \
            fprintf(stderr, "%s failed: 0x%08lx (line %d)\n", #x, hr_, __LINE__);  \
            exit(2);                                                               \
        }                                                                          \
    } while (0)

static std::vector<uint8_t> ReadFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "can't open %s\n", path);
        exit(2);
    }
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + n);
    fclose(f);
    return d;
}

constexpr UINT kW = 256, kH = 256, kShadowRes = 512;
static bool g_hbao = false;

struct Gpu {
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fenceValue = 0;
    HANDLE event = nullptr;

    void Init() {
        IDXGIFactory4* factory = nullptr;
        CHECKHR(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory));
        IDXGIAdapter* warp = nullptr;
        CHECKHR(factory->EnumWarpAdapter(__uuidof(IDXGIAdapter), (void**)&warp));
        CHECKHR(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dev));
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        CHECKHR(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&queue));
        CHECKHR(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                            (void**)&alloc));
        CHECKHR(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                       __uuidof(ID3D12GraphicsCommandList), (void**)&list));
        CHECKHR(list->Close());
        CHECKHR(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence));
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    void Begin() {
        CHECKHR(alloc->Reset());
        CHECKHR(list->Reset(alloc, nullptr));
    }
    void SubmitAndWait() {
        CHECKHR(list->Close());
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        CHECKHR(queue->Signal(fence, ++fenceValue));
        CHECKHR(fence->SetEventOnCompletion(fenceValue, event));
        WaitForSingleObject(event, INFINITE);
    }
    ID3D12Resource* Buffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp = {heap};
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = size;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* r = nullptr;
        CHECKHR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                             __uuidof(ID3D12Resource), (void**)&r));
        return r;
    }
    ID3D12Resource* Texture(UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags,
                            D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.Flags = flags;
        ID3D12Resource* r = nullptr;
        CHECKHR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                             __uuidof(ID3D12Resource), (void**)&r));
        return r;
    }
    // Uploads float data into a R32_FLOAT (or, channels 4, R32G32B32A32_FLOAT) texture
    // (COPY_DEST -> PIXEL_SHADER_RESOURCE).
    void UploadR32(ID3D12Resource* tex, UINT w, UINT h, const std::vector<float>& data, UINT channels = 1) {
        const UINT rowBytes = w * 4 * channels;
        UINT64 pitch = (rowBytes + 255) & ~255ull;
        ID3D12Resource* up = Buffer(pitch * h, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        uint8_t* p = nullptr;
        CHECKHR(up->Map(0, nullptr, (void**)&p));
        for (UINT y = 0; y < h; ++y) memcpy(p + y * pitch, &data[y * w * channels], rowBytes);
        up->Unmap(0, nullptr);
        Begin();
        D3D12_TEXTURE_COPY_LOCATION dst = {tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION src = {up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint.Footprint = {channels == 4 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32_FLOAT,
                                         w, h, 1, (UINT)pitch};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {tex, 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
        list->ResourceBarrier(1, &b);
        SubmitAndWait();
        up->Release();
    }
};

// Root signature matching the game's: b0 root CBV, SRV table t0-t1, s0 linear clamp, s1 PCF compare.
static ID3D12RootSignature* MakeRootSignature(ID3D12Device* dev) {
    D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = {0, 0};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &range};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC s[2] = {};
    s[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    s[0].AddressU = s[0].AddressV = s[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    s[0].MaxLOD = D3D12_FLOAT32_MAX;
    s[0].ShaderRegister = 0;
    s[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    s[1] = s[0];
    if (!g_hbao) {
        s[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        s[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    } else {
        s[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;  // quarter-res depth is point-sampled
    }
    s[1].ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 2, s, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        fprintf(stderr, "root signature: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        exit(2);
    }
    ID3D12RootSignature* root = nullptr;
    CHECKHR(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                     __uuidof(ID3D12RootSignature), (void**)&root));
    return root;
}

static ID3D12PipelineState* MakePso(ID3D12Device* dev, ID3D12RootSignature* root, const std::vector<uint8_t>& vs,
                                    const std::vector<uint8_t>& ps) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = root;
    d.VS = {vs.data(), vs.size()};
    d.PS = {ps.data(), ps.size()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    d.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = nullptr;
    CHECKHR(dev->CreateGraphicsPipelineState(&d, __uuidof(ID3D12PipelineState), (void**)&pso));
    return pso;
}

// Constants with realistic ranges: view depth 0..~240 m, one cascade of a 512^2 test atlas.
static std::vector<uint8_t> MakeConstants(std::mt19937& rng) {
    std::vector<uint8_t> cb(1024, 0);  // 592 used; CBVs are 256-byte aligned
    auto f = [&](size_t off, float v) { memcpy(&cb[off], &v, 4); };
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    f(400, 1.0f / 0.2f);                // g_fInvNear (near = 0.2)
    f(404, 2000.0f);                    // g_fFar
    f(424, (float)kW);                  // g_vRenderRes
    f(428, (float)kH);
    f(472, (float)kShadowRes);          // g_vShadowMapRes
    f(476, (float)kShadowRes);
    f(488, 0.0005f + 0.002f * u(rng));  // g_fPCFBias_d3t
    f(496, std::round((5.0f + 8.0f * u(rng)) * 10.0f) / 10.0f);  // g_fCascadePCFSize_d3t: AWTweaks keeps it on a 0.1 grid
    f(512, 10.0f + 30.0f * u(rng));     // g_fRegionStart
    f(516, 60.0f + 150.0f * u(rng));    // g_fRegionEnd
    f(588, 0.0004f + 0.0008f * u(rng)); // g_fPCFSize
    // g_mShadowProjection (3x4, row-major at 528): xy -> atlas [0.05, 0.95], z -> [0, 1].
    float rows[3][4] = {{0.004f + 0.002f * u(rng), 0.001f * u(rng), 0.0015f, 0.5f},
                        {0.0005f * u(rng), 0.004f + 0.002f * u(rng), 0.001f, 0.5f},
                        {0.0001f, 0.0002f, 0.004f + 0.001f * u(rng), 0.02f}};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) f(528 + r * 16 + c * 4, rows[r][c]);
    return cb;
}

// HBAO+ constants for a 1024x1024 full-res view (256x256 per deinterleaved layer),
// 90 degree FOV, AO radius 0.5..2.5 m; the optional clamps and depth fade on or off.
static std::vector<uint8_t> MakeConstantsHBAO(std::mt19937& rng) {
    std::vector<uint8_t> cb(1024, 0);  // 656 used
    auto f = [&](size_t off, float v) { memcpy(&cb[off], &v, 4); };
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float tanHalf = 0.9f + 0.2f * u(rng);
    f(424, 1024.0f);  // g_vRenderRes
    f(428, 1024.0f);
    f(512, 2.0f * tanHalf);  // UVToViewAB
    f(516, -2.0f * tanHalf);
    f(520, -tanHalf);
    f(524, tanHalf);
    f(544, 1.0f / kW);  // quarter-res texel size
    f(548, 1.0f / kH);
    const bool fade = u(rng) < 0.5f;
    f(552, fade ? -1.0f / (20.0f + 40.0f * u(rng)) : 0.0f);
    f(556, fade ? 1.0f + 3.0f * u(rng) : -1.0f);
    const float radius = 0.5f + 2.0f * u(rng);
    f(560, -1.0f / (radius * radius));
    f(564, radius * 0.5f * kH / tanHalf);  // radius to quarter-res pixels at depth 1
    f(568, u(rng) < 0.5f ? -1.0f : 4.0f + 8.0f * u(rng));     // background clamp
    f(572, u(rng) < 0.5f ? -1.0f : 60.0f + 100.0f * u(rng));  // foreground clamp
    f(576, std::round(100.0f * u(rng)) / 1000.0f);  // N.V bias, on AWTweaks' 0.001 grid
    f(580, 0.5f + 1.5f * u(rng)); // small scale
    f(584, 0.5f + 2.0f * u(rng)); // large scale
    f(624, (float)(rng() % 4));   // PerPassOffset
    f(628, (float)(rng() % 4));
    const float a = 6.2831853f * u(rng);
    f(640, std::cos(a));  // PerPassJitter
    f(644, std::sin(a));
    f(648, u(rng));
    return cb;
}

// ---------------------------------------------------------------- --spot
// DeferredLight.rfx "spotlightPS" (shadow map + projection map): t0 linear depth,
// t1 normals, t2 tangent + gloss, t3 exposure, t4 shadow map, t5 projection map;
// s0-s3 and s5 linear clamp, s4 PCF compare.
static ID3D12RootSignature* MakeSpotRootSignature(ID3D12Device* dev) {
    D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = {0, 0};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &range};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC s[6] = {};
    for (int i = 0; i < 6; ++i) {
        s[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        s[i].AddressU = s[i].AddressV = s[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        s[i].MaxLOD = D3D12_FLOAT32_MAX;
        s[i].ShaderRegister = i;
        s[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    s[0].Filter = s[1].Filter = s[2].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;  // G-buffer reads
    s[4].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    s[4].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 6, s, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        fprintf(stderr, "root signature: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        exit(2);
    }
    ID3D12RootSignature* root = nullptr;
    CHECKHR(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                     __uuidof(ID3D12RootSignature), (void**)&root));
    return root;
}

// A flashlight 0.3 m right of and below the camera, pointing down the view (+z), with a
// perspective projection (texture) and shadow matrix: 40 degree cone, near 0.1, far 60.
static std::vector<uint8_t> MakeConstantsSpot(std::mt19937& rng, float* zNear, float* zFar) {
    std::vector<uint8_t> cb(2048, 0);  // 1976 used
    auto f = [&](size_t off, float v) { memcpy(&cb[off], &v, 4); };
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    f(424, (float)kW);  // g_vRenderRes
    f(428, (float)kH);
    f(472, (float)kShadowRes);  // g_vShadowMapRes
    f(476, (float)kShadowRes);
    f(500, std::round((2.0f + 8.0f * u(rng)) * 10.0f) / 10.0f);  // g_fPCFSize_d3t ("Filter Size")
    const float lx = 0.3f, ly = -0.3f, lz = 0.0f;
    f(1824, lx);  // g_vLightPosition
    f(1828, ly);
    f(1832, lz);
    for (int c = 0; c < 3; ++c) f(1856 + 4 * c, 0.5f + 4.0f * u(rng));  // g_vLightColor
    f(1872, 0.02f + 0.2f * u(rng));                                      // g_vLightFalloff
    f(1876, 0.05f * u(rng));
    const float n = 0.1f, fa = 60.0f, s = 1.0f / std::tan(20.0f * 3.14159265f / 180.0f);
    const float a = fa / (fa - n), b = -n * fa / (fa - n);
    *zNear = n;
    *zFar = fa;
    // Row-major: x' = s (x - lx) + small skew, y' = s (y - ly), z' = a (z - lz) + b, w' = z - lz.
    auto proj = [&](size_t off, float skew) {
        float m[4][4] = {{s, skew, 0.0f, -s * lx}, {0.0f, s, skew, -s * ly}, {0.0f, 0.0f, a, b - a * lz},
                         {0.0f, 0.0f, 1.0f, -lz}};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) f(off + r * 16 + c * 4, m[r][c]);
    };
    proj(1440, 0.05f * u(rng));  // g_mLightProjectionMatrix
    proj(1904, 0.0f);            // g_mShadowProjectionMatrix (shadow uv = x' * 0.5 + 0.5 by the caller's matrix)
    // The shadow matrix maps to atlas uv directly: fold the * 0.5 + 0.5 (and y flip) in.
    float sm[4][4];
    memcpy(sm, &cb[1904], 64);
    for (int c = 0; c < 4; ++c) {
        float w = sm[3][c];
        sm[0][c] = sm[0][c] * 0.5f + w * 0.5f;
        sm[1][c] = -sm[1][c] * 0.5f + w * 0.5f;
    }
    memcpy(&cb[1904], sm, 64);
    f(1968, u(rng) < 0.3f ? 1.0f : u(rng));  // g_fShadowLOD
    return cb;
}

static int RunSpot(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: --spot vs.dxil original_ps.dxil replacement_ps.dxil [seeds] [settings]\n");
        return 2;
    }
    auto vs = ReadFile(argv[1]), psA = ReadFile(argv[2]), psB = ReadFile(argv[3]);
    const int seeds = argc > 4 ? atoi(argv[4]) : 8;
    // Settings code (1 + lens * 100 + contact), carried in Filter Size's low digits: the
    // outputs then differ, so check the replacement for NaNs and range instead.
    const bool enhanced = argc > 5;
    const float settings = enhanced ? (float)atof(argv[5]) : 0.0f;
    Gpu g;
    g.Init();
    ID3D12RootSignature* root = MakeSpotRootSignature(g.dev);
    ID3D12PipelineState* psoA = MakePso(g.dev, root, vs, psA);
    ID3D12PipelineState* psoB = MakePso(g.dev, root, vs, psB);

    const UINT res[6][2] = {{kW, kH}, {kW, kH}, {kW, kH}, {4, 4}, {kShadowRes, kShadowRes}, {64, 64}};
    const bool rgba[6] = {false, true, true, false, false, true};
    ID3D12Resource* tex[6];
    for (int i = 0; i < 6; ++i)
        tex[i] = g.Texture(res[i][0], res[i][1], rgba[i] ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32_FLOAT,
                           D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* rt = g.Texture(kW, kH, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12Resource* cbuf = g.Buffer(2048, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    const UINT64 rbPitch = (kW * 16 + 255) & ~255ull;
    ID3D12Resource* readback = g.Buffer(rbPitch * kH, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 6,
                                              D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ID3D12DescriptorHeap* srvHeap = nullptr;
    CHECKHR(g.dev->CreateDescriptorHeap(&srvHeapDesc, __uuidof(ID3D12DescriptorHeap), (void**)&srvHeap));
    UINT inc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < 6; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = rgba[i] ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32_FLOAT;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        g.dev->CreateShaderResourceView(tex[i], &sv, cpu);
        cpu.ptr += inc;
    }
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    CHECKHR(g.dev->CreateDescriptorHeap(&rtvHeapDesc, __uuidof(ID3D12DescriptorHeap), (void**)&rtvHeap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    g.dev->CreateRenderTargetView(rt, nullptr, rtv);

    auto render = [&](ID3D12PipelineState* pso, std::vector<float>& out) {
        g.Begin();
        g.list->SetGraphicsRootSignature(root);
        g.list->SetDescriptorHeaps(1, &srvHeap);
        g.list->SetGraphicsRootConstantBufferView(0, cbuf->GetGPUVirtualAddress());
        g.list->SetGraphicsRootDescriptorTable(1, srvHeap->GetGPUDescriptorHandleForHeapStart());
        g.list->SetPipelineState(pso);
        D3D12_VIEWPORT vp = {0, 0, (float)kW, (float)kH, 0, 1};
        D3D12_RECT sc = {0, 0, (LONG)kW, (LONG)kH};
        g.list->RSSetViewports(1, &vp);
        g.list->RSSetScissorRects(1, &sc);
        g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.list->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {rt, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        g.list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION src = {rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION dst = {readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, kW, kH, 1, (UINT)rbPitch};
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b.Transition = {rt, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET};
        g.list->ResourceBarrier(1, &b);
        g.SubmitAndWait();
        out.resize(kW * kH * 4);
        uint8_t* p = nullptr;
        CHECKHR(readback->Map(0, nullptr, (void**)&p));
        for (UINT y = 0; y < kH; ++y) memcpy(&out[y * kW * 4], p + y * rbPitch, kW * 16);
        readback->Unmap(0, nullptr);
    };

    int failures = 0;
    for (int seed = 1; seed <= seeds; ++seed) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        float zn, zf;
        auto cb = MakeConstantsSpot(rng, &zn, &zf);
        if (enhanced) {
            float size;
            memcpy(&size, &cb[500], 4);
            size = (float)(size + settings * 0.00001);
            memcpy(&cb[500], &size, 4);
        }
        // View depth (the test VS's ray has z = 400, so depth is view z): 1.5..20 m with blocks.
        std::vector<float> depth(kW * kH), normals(kW * kH * 4), tangent(kW * kH * 4);
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x) {
                size_t i = y * kW + x;
                float z = 6.0f + 4.0f * std::sin(x * 0.04f + seed) * std::cos(y * 0.03f);
                if (((x / 20) + (y / 20) + seed) % 4 == 0) z -= 1.5f;
                depth[i] = std::max(1.5f, z + 0.01f * u(rng));
                float nx = u(rng) * 2 - 1, ny = u(rng) * 2 - 1, nz = -(0.2f + u(rng));
                float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                normals[i * 4 + 0] = nx / len * 0.5f + 0.5f;
                normals[i * 4 + 1] = ny / len * 0.5f + 0.5f;
                normals[i * 4 + 2] = nz / len * 0.5f + 0.5f;
                normals[i * 4 + 3] = 1.0f;
                bool aniso = ((x / 32) + (y / 32)) % 2 == 0;  // half Ward, half Blinn-Phong
                for (int c = 0; c < 3; ++c) tangent[i * 4 + c] = aniso ? u(rng) * 2 - 1 : 0.0f;
                tangent[i * 4 + 3] = u(rng) < 0.1f ? 0.0f : u(rng);  // gloss, sometimes 0
            }
        std::vector<float> exposure(16, 0.5f + 1.5f * u(rng)), shadow(kShadowRes * kShadowRes), projMap(64 * 64 * 4);
        // Shadow map: perspective depth of occluders 1..20 m from the light, smooth with hard steps.
        const float a = zf / (zf - zn), b = -zn * zf / (zf - zn);
        for (UINT y = 0; y < kShadowRes; ++y)
            for (UINT x = 0; x < kShadowRes; ++x) {
                float d = 5.0f + 3.0f * std::sin(x * 0.05f + seed) * std::cos(y * 0.04f);
                if (((x / 40) + (y / 40)) % 3 == 0) d = 2.0f + 2.0f * u(rng);
                shadow[y * kShadowRes + x] = a + b / d;
            }
        for (auto& v : projMap) v = 0.3f + 0.7f * u(rng);
        g.UploadR32(tex[0], kW, kH, depth);
        g.UploadR32(tex[1], kW, kH, normals, 4);
        g.UploadR32(tex[2], kW, kH, tangent, 4);
        g.UploadR32(tex[3], 4, 4, exposure);
        g.UploadR32(tex[4], kShadowRes, kShadowRes, shadow);
        g.UploadR32(tex[5], 64, 64, projMap, 4);
        uint8_t* p = nullptr;
        CHECKHR(cbuf->Map(0, nullptr, (void**)&p));
        memcpy(p, cb.data(), cb.size());
        cbuf->Unmap(0, nullptr);

        std::vector<float> outA, outB;
        render(psoA, outA);
        render(psoB, outB);
        size_t diff = 0, exact = 0, lit = 0;
        float maxDiff[4] = {};
        for (size_t i = 0; i < outA.size(); i += 4) {
            bool same = true, any = false;
            for (int c = 0; c < 4; ++c) {
                float d = std::fabs(outA[i + c] - outB[i + c]);
                maxDiff[c] = std::max(maxDiff[c], d);
                same &= outA[i + c] == outB[i + c];
                any |= d > 1e-4f * std::max(1.0f, std::fabs(outA[i + c]));
            }
            diff += any;
            exact += same;
            lit += outA[i] > 0.0f;
        }
        if (enhanced) {
            size_t bad = 0, darker = 0;
            double sumA = 0, sumB = 0;
            for (size_t i = 0; i < outB.size(); i += 4) {
                for (int c = 0; c < 4; ++c) bad += !(outB[i + c] >= 0.0f && outB[i + c] <= 32.0f);
                sumA += outA[i];
                sumB += outB[i];
                darker += outB[i] < outA[i] - 1e-5f;
            }
            failures += bad != 0;
            printf("seed %d: %s  red mean original %.4f, enhanced %.4f, darker %zu px, changed %zu px, invalid %zu\n",
                   seed, bad ? "BAD " : "OK  ", sumA / (kW * kH), sumB / (kW * kH), darker, diff, bad);
            if (seed == 1)
                for (int k = 0; k < 2; ++k) {
                    const std::vector<float>& img = k ? outB : outA;
                    float peak = 1e-6f;
                    for (size_t i = 0; i < img.size(); i += 4) peak = std::max(peak, img[i]);
                    FILE* f = fopen(k ? "spot_b.pgm" : "spot_a.pgm", "wb");
                    fprintf(f, "P5 %u %u 255\n", kW, kH);
                    for (size_t i = 0; i < img.size(); i += 4) fputc((int)(std::min(img[i] / peak, 1.0f) * 255.0f), f);
                    fclose(f);
                }
        } else {
        bool ok = diff == 0;
        failures += !ok;
        printf("seed %d: %s  bit-exact %.2f%%  >1e-4: %zu px  max diff r=%.2g g=%.2g b=%.2g a=%.2g  (lit %zu px)\n",
               seed, ok ? "OK  " : "DIFF", 100.0 * exact / (outA.size() / 4), diff, maxDiff[0], maxDiff[1],
               maxDiff[2], maxDiff[3], lit);
        }
        // Back to COPY_DEST for the next seed's uploads.
        D3D12_RESOURCE_BARRIER back[6] = {};
        for (int i = 0; i < 6; ++i) {
            back[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            back[i].Transition = {tex[i], 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST};
        }
        g.Begin();
        g.list->ResourceBarrier(6, back);
        g.SubmitAndWait();
    }
    printf(failures ? "FAILED (%d seed(s))\n" : enhanced ? "ALL SEEDS VALID\n" : "ALL SEEDS MATCH\n", failures);
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- --volumetric
// VolumetricLight.rfx "volumetric_light" #15 (the shadowed beam ray march): t0 half-res
// linear depth, t1 exposure, t2 the light's shadow map, t3 a 32x32 RGBA jitter map;
// s0-s2 point clamp, s3 point wrap. Needs test/volumetric_vs.hlsl (uv + view ray).
static ID3D12RootSignature* MakeVolumetricRootSignature(ID3D12Device* dev) {
    D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = {0, 0};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &range};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC s[4] = {};
    for (int i = 0; i < 4; ++i) {
        s[i].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        s[i].AddressU = s[i].AddressV = s[i].AddressW =
            i == 3 ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        s[i].MaxLOD = D3D12_FLOAT32_MAX;
        s[i].ShaderRegister = i;
        s[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 4, s, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        fprintf(stderr, "root signature: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        exit(2);
    }
    ID3D12RootSignature* root = nullptr;
    CHECKHR(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                     __uuidof(ID3D12RootSignature), (void**)&root));
    return root;
}

// A spotlight with a cone of half-angle 15..35 degrees and range 10..40 m, either at the
// camera (the flashlight: rays start inside the cone) or off to the side so the beam
// crosses the view. Cone space: apex at the light, x^2 + y^2 = z^2, far cap at z = 1.
// Light space: metres along the light's axes. Shadow: perspective, depth a + b / z.
static std::vector<uint8_t> MakeConstantsVolumetric(std::mt19937& rng, float* zNear, float* zFar) {
    std::vector<uint8_t> cb(1280, 0);  // 1028 used
    auto f = [&](size_t off, float v) { memcpy(&cb[off], &v, 4); };
    auto mat = [&](size_t off, const double m[4][4]) {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) f(off + r * 16 + c * 4, (float)m[r][c]);
    };
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const bool flashlight = u(rng) < 0.5;
    const double px = flashlight ? 0.25 : -6.0 + 12.0 * u(rng), py = flashlight ? -0.3 : 1.0 + 3.0 * u(rng),
                 pz = flashlight ? 0.0 : 4.0 + 10.0 * u(rng);
    // Axis: roughly down the view for the flashlight, else aimed somewhere into the scene.
    double ax, ay, az;
    if (flashlight) {
        ax = 0.15 * (u(rng) - 0.5), ay = 0.15 * (u(rng) - 0.5), az = 1.0;
    } else {
        ax = (-3.0 + 6.0 * u(rng)) - px, ay = -2.0 - py, az = (6.0 + 10.0 * u(rng)) - pz;
    }
    double len = std::sqrt(ax * ax + ay * ay + az * az);
    double Z[3] = {ax / len, ay / len, az / len};
    double X[3] = {Z[2], 0.0, -Z[0]};  // up = y
    len = std::sqrt(X[0] * X[0] + X[2] * X[2]);
    X[0] /= len, X[2] /= len;
    double Y[3] = {Z[1] * X[2] - Z[2] * X[1], Z[2] * X[0] - Z[0] * X[2], Z[0] * X[1] - Z[1] * X[0]};
    const double tanA = std::tan((15.0 + 20.0 * u(rng)) * 3.14159265358979 / 180.0), range = 10.0 + 30.0 * u(rng);
    const double sx = 1.0 / (range * tanA), sz = 1.0 / range;
    const double P[3] = {px, py, pz};
    // View -> cone: scale(sx, sx, sz) * rotation^T * translate(-P).
    double v2c[4][4] = {};
    const double* axes[3] = {X, Y, Z};
    const double scale[3] = {sx, sx, sz};
    for (int r = 0; r < 3; ++r) {
        double t = 0.0;
        for (int c = 0; c < 3; ++c) {
            v2c[r][c] = scale[r] * axes[r][c];
            t -= v2c[r][c] * P[c];
        }
        v2c[r][3] = t;
    }
    v2c[3][3] = 1.0;
    mat(656, v2c);  // g_mViewToCone
    // Cone -> light (metres): x * range * tan, y * range * tan, z * range.
    double c2l[4][4] = {{range * tanA, 0, 0, 0}, {0, range * tanA, 0, 0}, {0, 0, range, 0}, {0, 0, 0, 1}};
    mat(784, c2l);  // g_mConeToLight
    // Cone -> shadow: u = 0.5 + 0.5 x / (z tan), v = 0.5 - 0.5 y / (z tan), depth a + b / z.
    const double n = 0.1, fa = range, a = fa / (fa - n), b = -n * fa / (fa - n);
    *zNear = (float)n;
    *zFar = (float)fa;
    double c2s[4][4] = {{0.5 * range, 0, 0.5 * range, 0},
                        {0, -0.5 * range, 0.5 * range, 0},
                        {0, 0, a * range, b},
                        {0, 0, range, 0}};
    mat(848, c2s);  // g_mConeToShadow
    f(912, (float)(0.1 / range));  // g_fNearPlaneCap: the shadow near plane in cone space
    for (int c = 0; c < 3; ++c) f(916 + 4 * c, (float)(1.0 + 20.0 * u(rng)));  // g_vLightIntensity
    f(928, (float)(0.02 + 0.2 * u(rng)));                                        // g_fLightDecay
    for (int c = 0; c < 3; ++c) f(932 + 4 * c, (float)Z[c]);                     // g_vLightDirVS
    f(944, (float)kW);  // g_vOutputRes
    f(948, (float)kH);
    const double g = -0.3 + 1.1 * u(rng);  // g_vHGConstants: Henyey-Greenstein
    f(960, (float)((1.0 - g * g) / (4.0 * 3.14159265358979)));
    f(964, (float)(1.0 + g * g));
    f(968, (float)(-2.0 * g));
    f(976, (float)(1.5 / kShadowRes * u(rng)));  // g_vJitter: shadow uv, shadow uv, step
    f(980, (float)(1.5 / kShadowRes * u(rng)));
    f(984, (float)(0.5 * u(rng)));
    f(992, (float)(rng() % 32));  // g_vVolumetricLightJitterOffset
    f(996, (float)(rng() % 32));
    f(1024, (float)u(rng));  // g_fTemporalBlur
    return cb;
}

static int RunVolumetric(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: --volumetric volumetric_vs.dxil original_ps.dxil replacement_ps.dxil [seeds] [code]\n");
        return 2;
    }
    auto vs = ReadFile(argv[1]), psA = ReadFile(argv[2]), psB = ReadFile(argv[3]);
    const int seeds = argc > 4 ? atoi(argv[4]) : 8;
    // Settings code (beam quality * 10), carried in Jitter Z's low digits: the outputs then
    // differ, so check the replacement for NaNs and range and compare noise and time instead.
    const bool enhanced = argc > 5;
    const int code = enhanced ? atoi(argv[5]) : 0;
    Gpu g;
    g.Init();
    ID3D12RootSignature* root = MakeVolumetricRootSignature(g.dev);
    ID3D12PipelineState* psoA = MakePso(g.dev, root, vs, psA);
    ID3D12PipelineState* psoB = MakePso(g.dev, root, vs, psB);

    const UINT res[4][2] = {{kW, kH}, {4, 4}, {kShadowRes, kShadowRes}, {32, 32}};
    const bool rgba[4] = {false, false, false, true};
    ID3D12Resource* tex[4];
    for (int i = 0; i < 4; ++i)
        tex[i] = g.Texture(res[i][0], res[i][1], rgba[i] ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32_FLOAT,
                           D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* rt = g.Texture(kW, kH, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12Resource* cbuf = g.Buffer(1280, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    const UINT64 rbPitch = (kW * 16 + 255) & ~255ull;
    ID3D12Resource* readback = g.Buffer(rbPitch * kH, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                              D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ID3D12DescriptorHeap* srvHeap = nullptr;
    CHECKHR(g.dev->CreateDescriptorHeap(&srvHeapDesc, __uuidof(ID3D12DescriptorHeap), (void**)&srvHeap));
    UINT inc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < 4; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = rgba[i] ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32_FLOAT;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        g.dev->CreateShaderResourceView(tex[i], &sv, cpu);
        cpu.ptr += inc;
    }
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    CHECKHR(g.dev->CreateDescriptorHeap(&rtvHeapDesc, __uuidof(ID3D12DescriptorHeap), (void**)&rtvHeap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    g.dev->CreateRenderTargetView(rt, nullptr, rtv);

    auto render = [&](ID3D12PipelineState* pso, std::vector<float>& out) {
        g.Begin();
        g.list->SetGraphicsRootSignature(root);
        g.list->SetDescriptorHeaps(1, &srvHeap);
        g.list->SetGraphicsRootConstantBufferView(0, cbuf->GetGPUVirtualAddress());
        g.list->SetGraphicsRootDescriptorTable(1, srvHeap->GetGPUDescriptorHandleForHeapStart());
        g.list->SetPipelineState(pso);
        D3D12_VIEWPORT vp = {0, 0, (float)kW, (float)kH, 0, 1};
        D3D12_RECT sc = {0, 0, (LONG)kW, (LONG)kH};
        g.list->RSSetViewports(1, &vp);
        g.list->RSSetScissorRects(1, &sc);
        g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.list->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {rt, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        g.list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION src = {rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION dst = {readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, kW, kH, 1, (UINT)rbPitch};
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b.Transition = {rt, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET};
        g.list->ResourceBarrier(1, &b);
        g.SubmitAndWait();
        out.resize(kW * kH * 4);
        uint8_t* p = nullptr;
        CHECKHR(readback->Map(0, nullptr, (void**)&p));
        for (UINT y = 0; y < kH; ++y) memcpy(&out[y * kW * 4], p + y * rbPitch, kW * 16);
        readback->Unmap(0, nullptr);
    };

    int failures = 0;
    for (int seed = 1; seed <= seeds; ++seed) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        float zn, zf;
        auto cb = MakeConstantsVolumetric(rng, &zn, &zf);
        if (enhanced) {  // Jitter Z: the stock 0.4 on the 0.01 grid, plus code * 0.000001
            float jz = (float)(0.4 + code * 0.000001);
            memcpy(&cb[984], &jz, 4);
        }
        // View depth (z; the test VS's ray has z = 1): rolling 2..30 m with nearer blocks.
        std::vector<float> depth(kW * kH);
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x) {
                float z = 16.0f + 12.0f * std::sin(x * 0.03f + seed) * std::cos(y * 0.02f);
                if (((x / 24) + (y / 24) + seed) % 5 == 0) z = 2.0f + 4.0f * u(rng);
                depth[y * kW + x] = z;
            }
        std::vector<float> exposure(16, 0.3f + 1.5f * u(rng)), shadow(kShadowRes * kShadowRes), jitter(32 * 32 * 4);
        // Shadow map: occluders 1..30 m from the light, smooth with hard steps; blocks at
        // 1..6 m, thin bars (2-4 texels: blinds, branches) at 2..4 m; some open (1.0).
        const float a = zf / (zf - zn), b = -zn * zf / (zf - zn);
        std::vector<float> blockDepth(64), barDepth(64);
        for (auto& v : blockDepth) v = 1.0f + 5.0f * u(rng);
        for (auto& v : barDepth) v = 2.0f + 2.0f * u(rng);
        for (UINT y = 0; y < kShadowRes; ++y)
            for (UINT x = 0; x < kShadowRes; ++x) {
                float d = 15.0f + 10.0f * std::sin(x * 0.03f + seed) * std::cos(y * 0.025f);
                if (((x / 48) + (y / 48)) % 3 == 0) d = blockDepth[((x / 48) * 11 + y / 48) % 64];
                UINT bar = (x + y / 3) % 23;  // slanted bars, 2..4 texels wide
                if (y > kShadowRes / 3 && bar < 2u + (x / 23) % 3) d = std::min(d, barDepth[(x + y / 3) / 23 % 64]);
                shadow[y * kShadowRes + x] = ((x / 64) + (y / 64)) % 7 == 3 ? 1.0f : a + b / d;
            }
        for (auto& v : jitter) v = u(rng);
        g.UploadR32(tex[0], kW, kH, depth);
        g.UploadR32(tex[1], 4, 4, exposure);
        g.UploadR32(tex[2], kShadowRes, kShadowRes, shadow);
        g.UploadR32(tex[3], 32, 32, jitter, 4);
        uint8_t* p = nullptr;
        CHECKHR(cbuf->Map(0, nullptr, (void**)&p));
        memcpy(p, cb.data(), cb.size());
        cbuf->Unmap(0, nullptr);

        std::vector<float> outA, outB;
        LARGE_INTEGER t0, t1, t2, freq;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        render(psoA, outA);
        QueryPerformanceCounter(&t1);
        render(psoB, outB);
        QueryPerformanceCounter(&t2);
        if (enhanced) {
            // 16 frames, each with the jitter texture at another offset (as the game moves
            // it every frame). Reference: the original's 16-frame average, roughly what its
            // blur and temporal passes converge to. Per shader: flicker = mean |frame 1 -
            // frame 0|, error = mean |one frame - reference|, both relative to the
            // reference's brightness; bias = the 16-frame average's error.
            const int kFrames = 16;
            std::vector<float> frames[2][kFrames], ref(outA.size(), 0.0f), avgB(outA.size(), 0.0f);
            double ms[2] = {};
            for (int fr = 0; fr < kFrames; ++fr) {
                float off[2] = {(float)((fr * 7 + seed) % 32), (float)((fr * 13 + 3 * seed) % 32)};
                memcpy(&cb[992], off, 8);
                CHECKHR(cbuf->Map(0, nullptr, (void**)&p));
                memcpy(p, cb.data(), cb.size());
                cbuf->Unmap(0, nullptr);
                for (int k = 0; k < 2; ++k) {
                    LARGE_INTEGER a0, a1;
                    QueryPerformanceCounter(&a0);
                    render(k ? psoB : psoA, frames[k][fr]);
                    QueryPerformanceCounter(&a1);
                    if (fr) ms[k] += 1000.0 * (a1.QuadPart - a0.QuadPart) / freq.QuadPart;  // frame 0 warms up
                }
                for (size_t i = 0; i < ref.size(); ++i) {
                    ref[i] += frames[0][fr][i] / kFrames;
                    avgB[i] += frames[1][fr][i] / kFrames;
                }
            }
            size_t bad = 0;
            double refSum = 0, flicker[2] = {}, err[2] = {}, bias = 0, sumB = 0;
            for (int fr = 0; fr < kFrames; ++fr)
                for (float v : frames[1][fr]) bad += !(v >= 0.0f && v <= 32.0f);
            for (size_t i = 0; i < ref.size(); i += 4) {
                if (!(ref[i] > 0.0f)) continue;
                refSum += ref[i];
                sumB += avgB[i];
                bias += std::fabs(avgB[i] - ref[i]);
                for (int k = 0; k < 2; ++k) {
                    flicker[k] += std::fabs(frames[k][1][i] - frames[k][0][i]);
                    for (int fr = 0; fr < kFrames; ++fr) err[k] += std::fabs(frames[k][fr][i] - ref[i]) / kFrames;
                }
            }
            refSum = std::max(refSum, 1e-12);
            failures += bad != 0;
            printf("seed %d: %s  flicker %.3f -> %.3f, frame error %.3f -> %.3f, bias %.3f, brightness %.3f,"
                   " WARP %.0f -> %.0f ms, invalid %zu\n",
                   seed, bad ? "BAD " : "OK  ", flicker[0] / refSum, flicker[1] / refSum, err[0] / refSum,
                   err[1] / refSum, bias / refSum, sumB / refSum, ms[0], ms[1], bad);
            outA = frames[0][0];
            outB = frames[1][0];
        }
        size_t diff = 0, exact = 0, lit = 0, shown = 0;
        float maxDiff[4] = {};
        for (size_t i = 0; i < outA.size(); i += 4) {
            bool same = true, any = false;
            for (int c = 0; c < 4; ++c) {
                float d = std::fabs(outA[i + c] - outB[i + c]);
                maxDiff[c] = std::max(maxDiff[c], d);
                same &= memcmp(&outA[i + c], &outB[i + c], 4) == 0;
                any |= !(d <= 1e-4f * std::max(1.0f, std::fabs(outA[i + c])));
            }
            if (!same && !enhanced && shown++ < 4)  // the first few mismatches
                printf("  pixel (%zu, %zu): original %.9g %.9g %.9g %.9g, rebuild %.9g %.9g %.9g %.9g\n", i / 4 % kW,
                       i / 4 / kW, outA[i], outA[i + 1], outA[i + 2], outA[i + 3], outB[i], outB[i + 1], outB[i + 2],
                       outB[i + 3]);
            diff += any;
            exact += same;
            lit += outA[i] > 0.0f;
        }
        bool ok = exact == outA.size() / 4;
        failures += !ok && !enhanced;
        if (!enhanced)
            printf("seed %d: %s  bit-exact %.2f%%  >1e-4: %zu px  max diff r=%.2g g=%.2g b=%.2g a=%.2g  (in beam %zu px)\n",
               seed, ok ? "OK  " : "DIFF", 100.0 * exact / (outA.size() / 4), diff, maxDiff[0], maxDiff[1],
               maxDiff[2], maxDiff[3], lit);
        if (getenv("AWT_DUMP_FLOATS")) {  // the replacement's raw red channel, for scripts
            char name[32];
            snprintf(name, sizeof(name), "beam_b_%d.f32", seed);
            FILE* fp = fopen(name, "wb");
            for (size_t i = 0; i < outB.size(); i += 4) fwrite(&outB[i], 4, 1, fp);
            fclose(fp);
        }
        if (seed == 1) {
            float peak = 1e-6f;  // one scale for both images
            for (size_t i = 0; i < outA.size(); i += 4) peak = std::max({peak, outA[i], outB[i]});
            for (int k = 0; k < 2; ++k) {
                const std::vector<float>& img = k ? outB : outA;
                FILE* fp = fopen(k ? "beam_b.pgm" : "beam_a.pgm", "wb");
                fprintf(fp, "P5 %u %u 255\n", kW, kH);
                for (size_t i = 0; i < img.size(); i += 4) fputc((int)(std::min(img[i] / peak, 1.0f) * 255.0f), fp);
                fclose(fp);
            }
        }
        D3D12_RESOURCE_BARRIER back[4] = {};
        for (int i = 0; i < 4; ++i) {
            back[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            back[i].Transition = {tex[i], 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST};
        }
        g.Begin();
        g.list->ResourceBarrier(4, back);
        g.SubmitAndWait();
    }
    printf(failures ? "FAILED (%d seed(s))\n" : enhanced ? "ALL SEEDS VALID\n" : "ALL SEEDS MATCH\n", failures);
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--spot")) return RunSpot(argc - 1, argv + 1);
    if (argc > 1 && !strcmp(argv[1], "--volumetric")) return RunVolumetric(argc - 1, argv + 1);
    bool flat = false;  // --hbao --flat: a flat wall facing the camera (AO must be ~1)
    bool blocks = false;
    bool corner = false;  // --hbao --corner: a back wall at 6 m meeting a side wall at x = -1.5 m
    bool halo = false;    // --hbao --halo: a square 0.6 m in front of a wall at 6 m
    if (argc > 1 && !strcmp(argv[1], "--hbao")) {
        g_hbao = true;
        --argc;
        ++argv;
        if (argc > 1 && !strcmp(argv[1], "--flat")) {
            flat = true;
            --argc;
            ++argv;
        } else if (argc > 1 && !strcmp(argv[1], "--blocks")) {  // the wall with 1 m blocks on it
            flat = blocks = true;
            --argc;
            ++argv;
        } else if (argc > 1 && !strcmp(argv[1], "--corner")) {
            flat = corner = true;
            --argc;
            ++argv;
        } else if (argc > 1 && !strcmp(argv[1], "--halo")) {
            flat = corner = halo = true;
            --argc;
            ++argv;
        }
    }
    if (argc < 4) {
        fprintf(stderr, "usage: %s vs.dxil original_ps.dxil replacement_ps.dxil [seeds]\n", argv[0]);
        return 2;
    }
    auto vs = ReadFile(argv[1]), psA = ReadFile(argv[2]), psB = ReadFile(argv[3]);
    const int seeds = argc > 4 ? atoi(argv[4]) : 8;
    const bool enhanced = argc > 5;
    const float settings = enhanced ? (float)atof(argv[5]) : 0.0f;

    Gpu g;
    g.Init();
    ID3D12RootSignature* root = MakeRootSignature(g.dev);
    ID3D12PipelineState* psoA = MakePso(g.dev, root, vs, psA);
    ID3D12PipelineState* psoB = MakePso(g.dev, root, vs, psB);

    // t0, t1: linear depth + shadow map, or (--hbao) normals + quarter-res view depth.
    const DXGI_FORMAT tex0Format = g_hbao ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32_FLOAT;
    const UINT tex1Res = g_hbao ? kW : kShadowRes;
    ID3D12Resource* depthTex = g.Texture(kW, kH, tex0Format, D3D12_RESOURCE_FLAG_NONE,
                                         D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* shadowTex = g.Texture(tex1Res, tex1Res, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                                          D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* rt = g.Texture(kW, kH, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12Resource* cbuf = g.Buffer(1024, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    const UINT64 rbPitch = (kW * 16 + 255) & ~255ull;
    ID3D12Resource* readback = g.Buffer(rbPitch * kH, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                              D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ID3D12DescriptorHeap* srvHeap = nullptr;
    CHECKHR(g.dev->CreateDescriptorHeap(&srvHeapDesc, __uuidof(ID3D12DescriptorHeap), (void**)&srvHeap));
    UINT inc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
    sv.Format = tex0Format;
    g.dev->CreateShaderResourceView(depthTex, &sv, cpu);
    cpu.ptr += inc;
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    g.dev->CreateShaderResourceView(shadowTex, &sv, cpu);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    CHECKHR(g.dev->CreateDescriptorHeap(&rtvHeapDesc, __uuidof(ID3D12DescriptorHeap), (void**)&rtvHeap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    g.dev->CreateRenderTargetView(rt, nullptr, rtv);

    auto render = [&](ID3D12PipelineState* pso, std::vector<float>& out) {
        g.Begin();
        g.list->SetGraphicsRootSignature(root);
        g.list->SetDescriptorHeaps(1, &srvHeap);
        g.list->SetGraphicsRootConstantBufferView(0, cbuf->GetGPUVirtualAddress());
        g.list->SetGraphicsRootDescriptorTable(1, srvHeap->GetGPUDescriptorHandleForHeapStart());
        g.list->SetPipelineState(pso);
        D3D12_VIEWPORT vp = {0, 0, (float)kW, (float)kH, 0, 1};
        D3D12_RECT sc = {0, 0, (LONG)kW, (LONG)kH};
        g.list->RSSetViewports(1, &vp);
        g.list->RSSetScissorRects(1, &sc);
        g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.list->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {rt, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        g.list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION src = {rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION dst = {readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, kW, kH, 1, (UINT)rbPitch};
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b.Transition = {rt, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET};
        g.list->ResourceBarrier(1, &b);
        g.SubmitAndWait();
        out.resize(kW * kH * 4);
        uint8_t* p = nullptr;
        CHECKHR(readback->Map(0, nullptr, (void**)&p));
        for (UINT y = 0; y < kH; ++y) memcpy(&out[y * kW * 4], p + y * rbPitch, kW * 16);
        readback->Unmap(0, nullptr);
    };

    int failures = 0;
    for (int seed = 1; seed <= seeds; ++seed) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        // Linear depth (fraction of far plane via the view ray); some sky (1.0).
        std::vector<float> depth(kW * kH), shadow(kShadowRes * kShadowRes);
        for (auto& d : depth) d = u(rng) < 0.05f ? 1.0f : 0.002f + 0.12f * u(rng);
        // Shadow map: smooth-ish occluder depths with hard edges, so PCF has gradients.
        for (UINT y = 0; y < kShadowRes; ++y)
            for (UINT x = 0; x < kShadowRes; ++x)
                shadow[y * kShadowRes + x] =
                    0.3f + 0.4f * std::sin(x * 0.07f + seed) * std::cos(y * 0.05f) + 0.1f * u(rng);
        std::mt19937 cbRng(seed * 7919);
        auto cornerCb = MakeConstantsHBAO(cbRng);
        if (g_hbao) {
            // View depth: rolling ground 3..9 m with raised blocks and noise; normals
            // facing the camera, encoded * 0.5 + 0.5.
            std::vector<float> normals(kW * kH * 4), viewDepth(kW * kH);
            for (UINT y = 0; y < kH; ++y)
                for (UINT x = 0; x < kW; ++x) {
                    float z = 6.0f + 3.0f * std::sin(x * 0.05f + seed) * std::cos(y * 0.04f);
                    if (((x / 24) + (y / 24) + seed) % 3 == 0) z -= 1.0f + 0.5f * u(rng);
                    viewDepth[y * kW + x] = blocks ? ((x / 32 + y / 32) % 2 ? 5.7f : 6.0f) : flat ? 6.0f
                                                                                            : z + 0.02f * u(rng);
                    float nx = u(rng) * 2 - 1, ny = u(rng) * 2 - 1, nz = -(0.3f + u(rng));
                    if (flat) nx = ny = 0.0f, nz = -1.0f;
                    if (halo) {
                        bool inSquare = x >= kW * 3 / 8 && x < kW * 5 / 8 && y >= kH * 3 / 8 && y < kH * 5 / 8;
                        viewDepth[y * kW + x] = inSquare ? 5.4f : 6.0f;
                    } else if (corner) {  // inside corner: whichever wall is nearer along the view ray
                        float tanHalf;
                        memcpy(&tanHalf, &cornerCb[524], 4);
                        float dx = (2.0f * (x + 0.5f) / kW - 1.0f) * tanHalf;
                        float zSide = dx < 0.0f ? -1.5f / dx : 1e9f;
                        viewDepth[y * kW + x] = std::min(6.0f, zSide);
                        if (zSide < 6.0f) nx = 1.0f, nz = 0.0f;
                    }
                    float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                    float* n = &normals[(y * kW + x) * 4];
                    n[0] = nx / len * 0.5f + 0.5f;
                    n[1] = ny / len * 0.5f + 0.5f;
                    n[2] = nz / len * 0.5f + 0.5f;
                    n[3] = 1.0f;
                }
            g.UploadR32(depthTex, kW, kH, normals, 4);
            g.UploadR32(shadowTex, kW, kH, viewDepth);
        } else {
            g.UploadR32(depthTex, kW, kH, depth);
            g.UploadR32(shadowTex, kShadowRes, kShadowRes, shadow);
        }
        D3D12_RESOURCE_BARRIER back[2] = {};
        for (int i = 0; i < 2; ++i) {
            back[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            back[i].Transition = {i ? shadowTex : depthTex, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_COPY_DEST};
        }
        auto cb = corner ? cornerCb : g_hbao ? MakeConstantsHBAO(rng) : MakeConstants(rng);
        if (halo) {  // the game's radius is huge (50 "m", held back only by the screen clamps)
            float tanHalf, radius = 3.0f + seed, v;
            memcpy(&tanHalf, &cb[524], 4);
            v = -1.0f / (radius * radius);
            memcpy(&cb[560], &v, 4);
            v = radius * 0.5f * kH / tanHalf;
            memcpy(&cb[564], &v, 4);
            v = -1.0f;
            memcpy(&cb[568], &v, 4);
            memcpy(&cb[572], &v, 4);
        }
        if (enhanced && g_hbao) {  // HBAO bias (on the 0.001 grid) plus code * 0.000001
            float bias;
            memcpy(&bias, &cb[576], 4);
            bias = (float)(bias + settings * 0.000001);
            memcpy(&cb[576], &bias, 4);
        } else if (enhanced) {  // g_fCascadePCFSize_d3t rounded to 0.1, plus code * 0.00001
            float size;
            memcpy(&size, &cb[496], 4);
            size = (float)(std::floor(size * 10.0 + 0.5) / 10.0 + settings * 0.00001);
            memcpy(&cb[496], &size, 4);
        }
        uint8_t* p = nullptr;
        CHECKHR(cbuf->Map(0, nullptr, (void**)&p));
        memcpy(p, cb.data(), cb.size());
        cbuf->Unmap(0, nullptr);

        std::vector<float> a, b;
        render(psoA, a);
        render(psoB, b);
        float maxDiff[4] = {};
        size_t diffPixels = 0, exact = 0;
        double mean[4] = {};
        for (size_t i = 0; i < a.size(); i += 4) {
            bool any = false, same = true;
            for (int c = 0; c < 4; ++c) {
                float d = std::fabs(a[i + c] - b[i + c]);
                maxDiff[c] = std::max(maxDiff[c], d);
                mean[c] += a[i + c];
                any |= d > 1e-4f;
                same &= a[i + c] == b[i + c];
            }
            diffPixels += any;
            exact += same;
        }
        const size_t n = a.size() / 4;
        if (enhanced) {
            size_t bad = 0;
            double meanB = 0;
            for (size_t i = 0; i < b.size(); i += 4) {
                float x = b[i];
                bad += !(x >= 0.0f && x <= 1.0f);  // also catches NaN
                meanB += x;
            }
            failures += bad != 0;
            if (corner) {  // occlusion (1 - AO) where the original darkens, original vs enhanced
                double occA = 0, occB = 0;
                size_t m = 0;
                for (size_t i = 0; i < a.size(); i += 4) {
                    size_t px = i / 4, x = px % kW, y = px / kW;
                    bool inSquare = x >= kW * 3 / 8 && x < kW * 5 / 8 && y >= kH * 3 / 8 && y < kH * 5 / 8;
                    if (halo ? !inSquare : a[i] < 0.98f) occA += 1 - a[i], occB += 1 - b[i], ++m;
                }
                float r, sm, lg;
                memcpy(&r, &cb[560], 4);
                memcpy(&sm, &cb[580], 4);
                memcpy(&lg, &cb[584], 4);
                printf("seed %d: radius %.2f m, scales %.2f/%.2f: creased %zu px, occlusion original %.3f,"
                       " enhanced %.3f (ratio %.2f)\n",
                       seed, std::sqrt(-1.0f / r), sm, lg, m, occA / m, occB / m, occB / occA);
                if (seed == 1) {
                    const size_t row = kH / 2 * kW;
                    for (UINT x = 0; x < kW; x += 8)
                        if (a[(row + x) * 4] < 0.99f || b[(row + x) * 4] < 0.99f)
                            printf("  x %4u: original %.3f enhanced %.3f\n", x, a[(row + x) * 4],
                                   b[(row + x) * 4]);
                }
            }
            printf("seed %d: %s  shadow mean original %.3f, enhanced %.3f, changed %zu px, invalid %zu px\n",
                   seed, bad ? "BAD " : "OK  ", mean[0] / n, meanB / n, diffPixels, bad);
            if (seed == 1) {
                for (int k = 0; k < 2; ++k) {
                    const std::vector<float>& img = k ? b : a;
                    FILE* f = fopen(k ? "shadow_b.pgm" : "shadow_a.pgm", "wb");
                    fprintf(f, "P5 %u %u 255\n", kW, kH);
                    for (size_t i = 0; i < img.size(); i += 4)
                        fputc((int)(std::min(std::max(img[i], 0.0f), 1.0f) * 255.0f + 0.5f), f);
                    fclose(f);
                }
            }
            g.Begin();
            g.list->ResourceBarrier(2, back);
            g.SubmitAndWait();
            continue;
        }
        bool ok = diffPixels == 0;
        failures += !ok;
        printf("seed %d: %s  bit-exact %.2f%%  >1e-4: %zu px  max diff x=%.2g y=%.2g z=%.2g w=%.2g"
               "  (mean shadow %.3f, blend %.3f)\n",
               seed, ok ? "OK  " : "DIFF", 100.0 * exact / n, diffPixels, maxDiff[0], maxDiff[1], maxDiff[2],
               maxDiff[3], mean[0] / n, mean[3] / n);
        // Back to COPY_DEST for the next seed's uploads.
        g.Begin();
        g.list->ResourceBarrier(2, back);
        g.SubmitAndWait();
    }
    if (enhanced)
        printf(failures ? "FAILED (%d seed(s))\n" : "ALL SEEDS VALID\n", failures);
    else
        printf(failures ? "FAILED (%d seed(s))\n" : "ALL SEEDS MATCH\n", failures);
    return failures ? 1 : 0;
}
