// Offline equivalence test for replacement pixel shaders.
// Renders the game's original ShadowBuffer pixel shader and our reconstruction on
// D3D12 WARP (software, deterministic) with identical synthetic inputs - linear depth,
// a shadow map, the $Globals constants - and compares every output pixel.
//
// Usage: shader_compare.exe [--hbao] <vs.dxil> <original_ps.dxil> <replacement_ps.dxil> [seeds] [settings]
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

int main(int argc, char** argv) {
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
