#include "CubeRenderer.h"

#include <vector>
#include <stdexcept>
#include <string>
#include <cstring>
#include <cstdio>

using namespace CubeRenderer;
using namespace Microsoft::WRL;

static Graphics g_graphics;
static bool g_graphicsReady = false;

static ComPtr<IDXGIOutputDuplication> g_duplication;
static ComPtr<ID3D11Texture2D> g_desktopTexture;
static ComPtr<ID3D11ShaderResourceView> g_desktopSRV;

static ComPtr<ID3D11VertexShader> g_quadVS;
static ComPtr<ID3D11PixelShader> g_quadPS;
static ComPtr<ID3D11InputLayout> g_quadLayout;
static ComPtr<ID3D11Buffer> g_quadVB;
static ComPtr<ID3D11Buffer> g_quadIB;
static ComPtr<ID3D11Buffer> g_transformCB;
static ComPtr<ID3D11SamplerState> g_captureSampler;
static ComPtr<ID3D11RasterizerState> g_captureRaster;

static XMMATRIX g_quadTransform;

struct QuadVertex
{
    float x, y, z;
    float u, v;
};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SIZE:
        if (g_graphicsReady)
            g_graphics.Resize(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void LogState(const char* fmt, ...)
{
    FILE* f = nullptr;
    fopen_s(&f, "J:\\FlowDuo\\flowduo.log", "a");
    if (!f)
        return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

static void CheckHr(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        LogState("CheckHr FAILED hr=0x%08X %s", (unsigned)hr, what);
        throw std::runtime_error(what);
    }
}

Texture* CreateCheckerTexture(ID3D11Device* device)
{
    const UINT size = 64;
    const UINT cell = 8;

    std::vector<BYTE> pixels((size_t)size * size * 4);
    for (UINT y = 0; y < size; ++y)
        for (UINT x = 0; x < size; ++x)
        {
            bool dark = ((x / cell) + (y / cell)) % 2 == 0;
            BYTE v = dark ? 30 : 225;
            BYTE* p = &pixels[((size_t)y * size + x) * 4];
            p[0] = v;
            p[1] = v;
            p[2] = v;
            p[3] = 255;
        }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = size;
    desc.Height = size;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = pixels.data();
    data.SysMemPitch = size * 4;

    ID3D11Texture2D* texture = nullptr;
    device->CreateTexture2D(&desc, &data, &texture);
    return new Texture(texture);
}

static ComPtr<ID3DBlob> CompileShader(const char* source, const char* target)
{
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr,
                            "main", target, 0, 0, &blob, &error);
    if (FAILED(hr))
    {
        if (error)
        {
            OutputDebugStringA(static_cast<const char*>(error->GetBufferPointer()));
            LogState("shader compile FAILED target=%s:\n%s", target, (const char*)error->GetBufferPointer());
        }
        throw std::runtime_error("shader compile failed");
    }
    return blob;
}

static bool CreateQuadPipeline(ID3D11Device* device)
{
    try
    {
        const char* vsSrc =
            "cbuffer TransformCB : register(b0) { float4x4 transform; };\n"
            "struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; };\n"
            "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
            "VSOut main(VSIn i)\n"
            "{\n"
            "    VSOut o;\n"
            "    o.pos = mul(float4(i.pos, 1.0), transform);\n"
            "    o.uv = i.uv;\n"
            "    return o;\n"
            "}\n";

        const char* psSrc =
            "Texture2D desktopTex : register(t0);\n"
            "SamplerState samp : register(s0);\n"
            "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
            "float4 main(VSOut i) : SV_TARGET\n"
            "{\n"
            "    float4 c = desktopTex.Sample(samp, i.uv);\n"
            "    return float4(c.b, c.g, c.r, 1.0);\n"
            "}\n";

        auto vsBlob = CompileShader(vsSrc, "vs_5_0");
        auto psBlob = CompileShader(psSrc, "ps_5_0");

        CheckHr(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_quadVS), "CreateVertexShader");
        CheckHr(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_quadPS), "CreatePixelShader");

        D3D11_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        CheckHr(device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_quadLayout), "CreateInputLayout");

        QuadVertex verts[] = {
            { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
            {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
            {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f },
            { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        };
        const USHORT indices[] = { 0, 1, 2, 0, 2, 3 };

        D3D11_BUFFER_DESC vbDesc = {};
        vbDesc.ByteWidth = sizeof(verts);
        vbDesc.Usage = D3D11_USAGE_DEFAULT;
        vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vbData = { verts, 0, 0 };
        CheckHr(device->CreateBuffer(&vbDesc, &vbData, &g_quadVB), "CreateVB");

        D3D11_BUFFER_DESC ibDesc = {};
        ibDesc.ByteWidth = sizeof(indices);
        ibDesc.Usage = D3D11_USAGE_DEFAULT;
        ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA ibData = { indices, 0, 0 };
        CheckHr(device->CreateBuffer(&ibDesc, &ibData, &g_quadIB), "CreateIB");

        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = sizeof(XMMATRIX);
        cbDesc.Usage = D3D11_USAGE_DEFAULT;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        CheckHr(device->CreateBuffer(&cbDesc, nullptr, &g_transformCB), "CreateCB");

        D3D11_SAMPLER_DESC sampDesc = {};
        sampDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        CheckHr(device->CreateSamplerState(&sampDesc, &g_captureSampler), "CreateSampler");

        D3D11_RASTERIZER_DESC rasterDesc = {};
        rasterDesc.FillMode = D3D11_FILL_SOLID;
        rasterDesc.CullMode = D3D11_CULL_NONE;
        CheckHr(device->CreateRasterizerState(&rasterDesc, &g_captureRaster), "CreateRaster");

        return true;
    }
    catch (const std::exception& e)
    {
        LogState("CreateQuadPipeline EXCEPTION: %s", e.what());
        return false;
    }
    catch (...)
    {
        LogState("CreateQuadPipeline EXCEPTION (unknown)");
        return false;
    }
}

static bool InitDesktopCapture(ID3D11Device* device)
{
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))))
    {
        LogState("InitDesktopCapture: QI IDXGIDevice failed");
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter)))
    {
        LogState("InitDesktopCapture: GetAdapter failed");
        return false;
    }

    ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(0, &output)))
    {
        LogState("InitDesktopCapture: EnumOutputs(0) failed");
        return false;
    }

    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(&output1))))
    {
        LogState("InitDesktopCapture: QI IDXGIOutput1 failed");
        return false;
    }

    HRESULT hr = output1->DuplicateOutput(device, &g_duplication);
    if (FAILED(hr))
    {
        LogState("InitDesktopCapture: DuplicateOutput hr=0x%08X", (unsigned)hr);
        return false;
    }

    return g_duplication != nullptr;
}

static bool UpdateDesktopFrame(ID3D11Device* device, ID3D11DeviceContext* context, HWND hwnd)
{
    if (!g_duplication)
    {
        if (!InitDesktopCapture(device))
            return g_desktopSRV != nullptr;
    }

    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = g_duplication->AcquireNextFrame(16, &frameInfo, &resource);

    if (hr == DXGI_ERROR_ACCESS_LOST)
    {
        g_duplication.Reset();
        InitDesktopCapture(device);
        return g_desktopSRV != nullptr;
    }
    if (hr == DXGI_ERROR_WAIT_TIMEOUT || hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
        return g_desktopSRV != nullptr;
    if (FAILED(hr))
        return g_desktopSRV != nullptr;

    bool updated = g_desktopSRV != nullptr;

    if (resource)
    {
        ComPtr<ID3D11Texture2D> desktopImage;
        if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&desktopImage))))
        {
            D3D11_TEXTURE2D_DESC desc;
            desktopImage->GetDesc(&desc);

            D3D11_TEXTURE2D_DESC cur = {};
            if (g_desktopTexture)
                g_desktopTexture->GetDesc(&cur);

            if (!g_desktopTexture ||
                cur.Width != desc.Width || cur.Height != desc.Height || cur.Format != desc.Format)
            {
                g_desktopSRV.Reset();
                g_desktopTexture.Reset();

                D3D11_TEXTURE2D_DESC copyDesc = desc;
                copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                copyDesc.Usage = D3D11_USAGE_DEFAULT;
                copyDesc.CPUAccessFlags = 0;

                if (FAILED(device->CreateTexture2D(&copyDesc, nullptr, &g_desktopTexture)) ||
                    FAILED(device->CreateShaderResourceView(g_desktopTexture.Get(), nullptr, &g_desktopSRV)))
                {
                    g_desktopSRV.Reset();
                    g_desktopTexture.Reset();
                }
                else
                {
                    std::wstring title = L"FlowDuo - Desktop " +
                        std::to_wstring(desc.Width) + L"x" + std::to_wstring(desc.Height);
                    SetWindowTextW(hwnd, title.c_str());
                }
            }

            if (g_desktopTexture)
            {
                context->CopyResource(g_desktopTexture.Get(), desktopImage.Get());
                updated = true;
            }
        }
    }

    g_duplication->ReleaseFrame();
    return updated;
}

static void PresentQuad(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain)
{
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        return;

    D3D11_TEXTURE2D_DESC bbDesc;
    backBuffer->GetDesc(&bbDesc);

    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv)))
        return;

    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    context->ClearRenderTargetView(rtv.Get(), clearColor);

    D3D11_VIEWPORT viewport = { 0, 0, (float)bbDesc.Width, (float)bbDesc.Height, 0.0f, 1.0f };
    context->RSSetViewports(1, &viewport);
    context->RSSetState(g_captureRaster.Get());

    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(g_quadLayout.Get());
    UINT stride = sizeof(QuadVertex);
    UINT offset = 0;
    ID3D11Buffer* vb = g_quadVB.Get();
    context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    context->IASetIndexBuffer(g_quadIB.Get(), DXGI_FORMAT_R16_UINT, 0);

    XMMATRIX transform = XMMatrixTranspose(g_quadTransform);
    context->UpdateSubresource(g_transformCB.Get(), 0, nullptr, &transform, 0, 0);
    context->VSSetConstantBuffers(0, 1, g_transformCB.GetAddressOf());

    context->VSSetShader(g_quadVS.Get(), nullptr, 0);
    context->PSSetShader(g_quadPS.Get(), nullptr, 0);
    context->PSSetSamplers(0, 1, g_captureSampler.GetAddressOf());

    if (g_desktopSRV)
    {
        context->PSSetShaderResources(0, 1, g_desktopSRV.GetAddressOf());
        context->DrawIndexed(6, 0, 0);
    }

    swapChain->Present(1, 0);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nShowCmd)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FlowDuoWindow";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"FlowDuo",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        nullptr, nullptr, hInstance, nullptr);

    ShowWindow(hwnd, nShowCmd);
    UpdateWindow(hwnd);

    Scene* scene = g_graphics.Init(hwnd);

    Texture* texture = CreateCheckerTexture(g_graphics.GetDevice());
    scene->SetTexture(texture);
    scene->AddCube(16.0f, 16.0f, 16.0f, 0, 0, 0, 0, 0, texture);
    g_graphics.UpdateScene();

    g_graphicsReady = true;

    ID3D11Device* device = g_graphics.GetDevice();
    ID3D11DeviceContext* context = g_graphics.GetContext();
    IDXGISwapChain* swapChain = g_graphics.GetSwapChain();

    bool mirror = CreateQuadPipeline(device);
    LogState("CreateQuadPipeline=%d", (int)mirror);
    if (mirror)
        mirror = InitDesktopCapture(device);
    LogState("InitDesktopCapture=%d mirror=%d", (int)(mirror ? 1 : 0), (int)mirror);

    if (mirror)
        g_quadTransform = XMMatrixIdentity();

    float angle = 0.0f;
    int framesWithoutFrame = 0;
    for (;;)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                LogState("WM_QUIT exit=%lu", (unsigned long)msg.wParam);
                return (int)msg.wParam;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (mirror)
        {
            if (UpdateDesktopFrame(device, context, hwnd))
                framesWithoutFrame = 0;
            else
                ++framesWithoutFrame;

            PresentQuad(device, context, swapChain);

            if (framesWithoutFrame > 120)
            {
                mirror = false;
                SetWindowTextW(hwnd, L"FlowDuo - Cube fallback");
            }
        }
        else
        {
            g_graphics.Render(angle, 0.0f, 0.0f, 0.0f);
            angle += 0.01f;
        }
        Sleep(16);
    }
}