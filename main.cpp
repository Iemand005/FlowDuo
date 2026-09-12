#include "CubeRenderer.h"

#include <vector>
#include <stdexcept>
#include <string>
#include <cstring>

using namespace CubeRenderer;
using namespace Microsoft::WRL;

static Graphics g_graphics;
static bool g_graphicsReady = false;

static ComPtr<IDXGIOutputDuplication> g_duplication;
static ComPtr<ID3D11Texture2D> g_desktopTexture;
static ComPtr<ID3D11ShaderResourceView> g_desktopSRV;
static ComPtr<ID3D11VertexShader> g_fullscreenVS;
static ComPtr<ID3D11PixelShader> g_fullscreenPS;
static ComPtr<ID3D11SamplerState> g_captureSampler;
static ComPtr<ID3D11RasterizerState> g_captureRaster;

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

static void CheckHr(HRESULT hr)
{
    if (FAILED(hr))
        throw std::runtime_error("HRESULT failed");
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
            OutputDebugStringA(static_cast<const char*>(error->GetBufferPointer()));
        throw std::runtime_error("shader compile failed");
    }
    return blob;
}

static bool CreateCapturePipeline(ID3D11Device* device)
{
    try
    {
        const char* vsSrc =
            "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
            "VSOut main(uint vid : SV_VertexID)\n"
            "{\n"
            "    VSOut o;\n"
            "    float2 px = float2((vid << 1) & 2, vid & 2);\n"
            "    o.pos = float4(px * 2.0 - 1.0, 0.0, 1.0);\n"
            "    o.uv = float2(px.x * 0.5, 1.0 - px.y * 0.5);\n"
            "    return o;\n"
            "}\n";

        const char* psSrc =
            "Texture2D desktop : register(t0);\n"
            "SamplerState samp : register(s0);\n"
            "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
            "float4 main(VSOut i) : SV_TARGET\n"
            "{\n"
            "    float4 c = desktop.Sample(samp, i.uv);\n"
            "    return float4(c.b, c.g, c.r, 1.0);\n"
            "}\n";

        auto vsBlob = CompileShader(vsSrc, "vs_5_0");
        auto psBlob = CompileShader(psSrc, "ps_5_0");

        CheckHr(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_fullscreenVS));
        CheckHr(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_fullscreenPS));

        D3D11_SAMPLER_DESC sampDesc = {};
        sampDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        CheckHr(device->CreateSamplerState(&sampDesc, &g_captureSampler));

        D3D11_RASTERIZER_DESC rasterDesc = {};
        rasterDesc.FillMode = D3D11_FILL_SOLID;
        rasterDesc.CullMode = D3D11_CULL_NONE;
        CheckHr(device->CreateRasterizerState(&rasterDesc, &g_captureRaster));

        return true;
    }
    catch (...)
    {
        return false;
    }
}

static void InitDesktopCapture(ID3D11Device* device)
{
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))))
        return;

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter)))
        return;

    ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(0, &output)))
        return;

    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(&output1))))
        return;

    if (FAILED(output1->DuplicateOutput(device, &g_duplication)))
        g_duplication.Reset();
}

static void UpdateDesktopFrame(ID3D11Device* device, ID3D11DeviceContext* context)
{
    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = g_duplication->AcquireNextFrame(16, &frameInfo, &resource);

    if (hr == DXGI_ERROR_ACCESS_LOST)
    {
        g_duplication.Reset();
        InitDesktopCapture(device);
        return;
    }
    if (FAILED(hr))
        return;

    if (frameInfo.LastPresentTime.QuadPart != 0 && resource)
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
            }

            if (g_desktopTexture)
            {
                context->CopyResource(g_desktopTexture.Get(), desktopImage.Get());
                context->Flush();
            }
        }
    }

    g_duplication->ReleaseFrame();
}

static void PresentDesktop(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain)
{
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        return;

    D3D11_TEXTURE2D_DESC bbDesc;
    backBuffer->GetDesc(&bbDesc);

    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv)))
        return;

    D3D11_VIEWPORT viewport = { 0, 0, (float)bbDesc.Width, (float)bbDesc.Height, 0.0f, 1.0f };

    context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    context->RSSetViewports(1, &viewport);
    context->RSSetState(g_captureRaster.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr);
    UINT stride = 0, offset = 0;
    context->IASetVertexBuffers(0, 0, nullptr, &stride, &offset);
    context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);
    context->VSSetShader(g_fullscreenVS.Get(), nullptr, 0);
    context->PSSetShader(g_fullscreenPS.Get(), nullptr, 0);
    context->PSSetSamplers(0, 1, g_captureSampler.GetAddressOf());

    if (g_desktopSRV)
    {
        ID3D11ShaderResourceView* srv = g_desktopSRV.Get();
        context->PSSetShaderResources(0, 1, &srv);
        context->Draw(3, 0);
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

    bool mirror = CreateCapturePipeline(g_graphics.GetDevice());
    InitDesktopCapture(g_graphics.GetDevice());
    mirror = mirror && g_duplication != nullptr;

    ID3D11Device* device = g_graphics.GetDevice();
    ID3D11DeviceContext* context = g_graphics.GetContext();
    IDXGISwapChain* swapChain = g_graphics.GetSwapChain();

    float angle = 0.0f;
    for (;;)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                return (int)msg.wParam;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (mirror)
        {
            UpdateDesktopFrame(device, context);
            PresentDesktop(device, context, swapChain);
        }
        else
        {
            g_graphics.Render(angle, 0.0f, 0.0f, 0.0f);
            angle += 0.01f;
        }
        Sleep(16);
    }
}