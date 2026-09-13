#include "CubeRenderer.h"
#include "HingeSensorReader.h"

#include <cstring>
#include <stdexcept>
#include <cmath>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

using namespace CubeRenderer;
using namespace Microsoft::WRL;

static Graphics g_graphics;
static bool g_graphicsReady = false;
static bool g_windowVisible = false;

static float g_tiltDeg = 0.0f;

static HingeSensorReader g_hingeReader;
static float g_hingeSmooth = 0.0f;
static float g_calibOffset = 90.0f;
static DWORD g_lastHingeRead = 0;

static ComPtr<IDXGIOutputDuplication> g_duplication;
static ComPtr<ID3D11Texture2D> g_desktopTexture;
static ComPtr<ID3D11ShaderResourceView> g_desktopSRV;

static ComPtr<ID3D11VertexShader> g_quadVS;
static ComPtr<ID3D11PixelShader> g_quadPS;
static ComPtr<ID3D11InputLayout> g_quadLayout;
static ComPtr<ID3D11Buffer> g_quadVB;
static ComPtr<ID3D11Buffer> g_quadIB;
static ComPtr<ID3D11Buffer> g_transformCB;
static ComPtr<ID3D11SamplerState> g_quadSampler;
static ComPtr<ID3D11RasterizerState> g_quadRaster;
static float g_quadHalfHeight = 1.0f;

static ComPtr<ID3D11PixelShader> g_blurPS;
static ComPtr<ID3D11Buffer> g_fadeCB;
static ComPtr<ID3D11Buffer> g_blurCB;
static ComPtr<ID3D11Buffer> g_blurVB;
static ComPtr<ID3D11Buffer> g_blurIB;

static Graphics::RenderTarget g_sceneTarget;
static Graphics::RenderTarget g_blurTarget;

static float g_fadeStart = 0.2f;
static float g_fadeEnd = 1.0f;
static float g_fadeStrength = 0.0f;
static float g_blurRadius = 20.0f;
static float g_blurRadiusMultiplier = 2.0f;
static float g_blurRadiusMin = 0.0f;
static DWORD g_hingeSampleIntervalMs = 16;

static bool calibrated = false;

static bool trueHide = true;

static void Calibrate(HWND hwnd) {
    if (!g_hingeReader.IsReady()) return;
    calibrated = true;
    float hinge = 0;
    if (FAILED(g_hingeReader.GetHingeAngleFloat(&hinge)))
        return;
    g_calibOffset = hinge - 87.0f;
}

void ToggleWindowVisible(HWND hwnd, bool visible) {
    if (g_windowVisible == visible)
        return;

    g_windowVisible = visible;
    if (trueHide) ShowWindow(hwnd, visible ? SW_SHOW : SW_HIDE);
    else SetLayeredWindowAttributes(hwnd, 0, visible ? 255 : 0, LWA_ALPHA);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SIZE:
        if (g_graphicsReady)
            g_graphics.Resize(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_SPACE)
            Calibrate(hwnd);
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

static ComPtr<ID3DBlob> CompileShader(const char* source, const char* target)
{
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    CheckHr(D3DCompile(source, strlen(source), nullptr, nullptr, nullptr,
                      "main", target, 0, 0, &blob, &error));
    return blob;
}

static void CreateQuadPipeline(ID3D11Device* device)
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

    const char* psSource =
        "Texture2D desktopTex : register(t0);\n"
        "SamplerState samp : register(s0);\n"
        "cbuffer SceneCB : register(b1) { float4 fadeParams; };\n"
        "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
        "float4 main(VSOut i) : SV_TARGET\n"
        "{\n"
        "    float4 c = desktopTex.Sample(samp, i.uv);\n"
        "    float fade = 1.0 - fadeParams.z * smoothstep(fadeParams.x, fadeParams.y, 1.0 - i.uv.y);\n"
        "    return float4(c.rgb * fade, c.a);\n"
        "}\n";

    const char* blurSrc =
        "Texture2D sceneTex : register(t0);\n"
        "SamplerState samp : register(s0);\n"
        "cbuffer BlurCB : register(b1)\n"
        "{ float4 texelDir; float4 ranges; };\n"
        "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
        "float4 main(VSOut i) : SV_TARGET\n"
        "{\n"
        "    float2 texel = float2(texelDir.x, texelDir.y);\n"
        "    float2 dir = lerp(float2(texel.x, 0.0), float2(0.0, texel.y), texelDir.z);\n"
        "    float sigma = max(0.001, lerp(ranges.x, ranges.y, 1.0 - i.uv.y));\n"
        "    int halfK = min(32, (int)(sigma * 2.5 + 0.5));\n"
        "    float wsum = 0.0;\n"
        "    float4 c = 0.0;\n"
        "    [loop]\n"
        "    for (int k = -halfK; k <= halfK; ++k)\n"
        "    {\n"
        "        float w = exp(-((float)k * (float)k) / (2.0 * sigma * sigma));\n"
        "        c += sceneTex.Sample(samp, i.uv + dir * (float)k) * w;\n"
        "        wsum += w;\n"
        "    }\n"
        "    return c / wsum;\n"
        "}\n";

    auto vsBlob = CompileShader(vsSrc, "vs_5_0");
    auto psBlob = CompileShader(psSource, "ps_5_0");
    auto blurBlob = CompileShader(blurSrc, "ps_5_0");

    CheckHr(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_quadVS));
    CheckHr(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_quadPS));
    CheckHr(device->CreatePixelShader(blurBlob->GetBufferPointer(), blurBlob->GetBufferSize(), nullptr, &g_blurPS));

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    CheckHr(device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_quadLayout));

    Plane plane = CreatePlane(2.0f, 2.0f);

    D3D11_BUFFER_DESC desc = {};
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.ByteWidth = plane.vertexCount * sizeof(Vertex);
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data = { plane.vertices, 0, 0 };
    CheckHr(device->CreateBuffer(&desc, &data, &g_quadVB));

    desc.ByteWidth = plane.indexCount * sizeof(USHORT);
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    data = { plane.indices, 0, 0 };
    CheckHr(device->CreateBuffer(&desc, &data, &g_quadIB));

    DeletePlane(plane);

    Plane blurPlane = CreatePlane(2.0f, 2.0f);

    desc.ByteWidth = blurPlane.vertexCount * sizeof(Vertex);
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    data = { blurPlane.vertices, 0, 0 };
    CheckHr(device->CreateBuffer(&desc, &data, &g_blurVB));

    desc.ByteWidth = blurPlane.indexCount * sizeof(USHORT);
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    data = { blurPlane.indices, 0, 0 };
    CheckHr(device->CreateBuffer(&desc, &data, &g_blurIB));

    DeletePlane(blurPlane);

    desc.ByteWidth = sizeof(XMMATRIX);
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    CheckHr(device->CreateBuffer(&desc, nullptr, &g_transformCB));

    desc.ByteWidth = 16;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    CheckHr(device->CreateBuffer(&desc, nullptr, &g_fadeCB));
    desc.ByteWidth = 32;
    CheckHr(device->CreateBuffer(&desc, nullptr, &g_blurCB));

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    CheckHr(device->CreateSamplerState(&sampDesc, &g_quadSampler));

    D3D11_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    CheckHr(device->CreateRasterizerState(&rasterDesc, &g_quadRaster));
}

static void UpdateTiltFromHinge()
{
    if (!g_hingeReader.IsReady())
        return;

    DWORD now = GetTickCount();
    if (now - g_lastHingeRead < g_hingeSampleIntervalMs)
        return;
    DWORD elapsed = g_lastHingeRead == 0 ? g_hingeSampleIntervalMs : now - g_lastHingeRead;
    g_lastHingeRead = now;

    float hinge = 0;
    if (FAILED(g_hingeReader.GetHingeAngleFloat(&hinge)))
        return;

    float smoothAlpha = 1.0f - expf(-(float)elapsed / 55.0f);
    g_hingeSmooth += smoothAlpha  * ((float)hinge - g_hingeSmooth);
    g_tiltDeg = g_hingeSmooth - 90.0f - g_calibOffset;

    g_tiltDeg = max(g_tiltDeg, 0);

    g_blurRadius = g_tiltDeg * g_blurRadiusMultiplier;

    g_fadeStrength = min(g_tiltDeg / 60, 1);
}

static void InitDesktopCapture(ID3D11Device* device)
{
    ComPtr<IDXGIDevice> dxgiDevice;
    CheckHr(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)));

    ComPtr<IDXGIAdapter> adapter;
    CheckHr(dxgiDevice->GetAdapter(&adapter));

    ComPtr<IDXGIOutput> output;
    CheckHr(adapter->EnumOutputs(0, &output));

    ComPtr<IDXGIOutput1> output1;
    CheckHr(output->QueryInterface(IID_PPV_ARGS(&output1)));

    CheckHr(output1->DuplicateOutput(device, &g_duplication));
}

static void UpdateDesktopFrame(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (!g_duplication)
    {
        InitDesktopCapture(device);
        if (!g_duplication)
            return;
    }

    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = g_duplication->AcquireNextFrame(0, &frameInfo, &resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT || hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE || FAILED(hr))
    {
        if (hr == DXGI_ERROR_ACCESS_LOST)
            g_duplication.Reset();
        return;
    }

    if (resource)
    {
        ComPtr<ID3D11Texture2D> desktopImage;
        if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&desktopImage))))
        {
            D3D11_TEXTURE2D_DESC desc;
            desktopImage->GetDesc(&desc);

            if (!g_desktopTexture)
            {
                D3D11_TEXTURE2D_DESC copyDesc = desc;
                copyDesc.MiscFlags = 0;
                copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                copyDesc.Usage = D3D11_USAGE_DEFAULT;
                copyDesc.CPUAccessFlags = 0;
                CheckHr(device->CreateTexture2D(&copyDesc, nullptr, &g_desktopTexture));
                CheckHr(device->CreateShaderResourceView(g_desktopTexture.Get(), nullptr, &g_desktopSRV));

                Plane plane = CreatePlane(2.0f, 2.0f * (float)desc.Height / (float)desc.Width);
                g_quadHalfHeight = (float)desc.Height / (float)desc.Width;
                D3D11_BUFFER_DESC vbDesc = {};
                vbDesc.Usage = D3D11_USAGE_DEFAULT;
                vbDesc.ByteWidth = plane.vertexCount * sizeof(Vertex);
                vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                D3D11_SUBRESOURCE_DATA vbData = { plane.vertices, 0, 0 };
                CheckHr(device->CreateBuffer(&vbDesc, &vbData, &g_quadVB));
                DeletePlane(plane);
            }

            context->CopyResource(g_desktopTexture.Get(), desktopImage.Get());
        }
    }

    g_duplication->ReleaseFrame();
}

static void PresentQuad(ID3D11DeviceContext* context)
{
    ID3D11RenderTargetView* backRTV = g_graphics.GetRenderTargetView();
    if (!backRTV)
        return;

    UINT width = 0;
    UINT height = 0;
    g_graphics.GetBackBufferSize(&width, &height);
    if (width == 0 || height == 0)
        return;

    g_graphics.EnsureRenderTarget(g_sceneTarget, width, height);
    g_graphics.EnsureRenderTarget(g_blurTarget, width, height);

    D3D11_VIEWPORT viewport = { 0, 0, (float)width, (float)height, 0.0f, 1.0f };
    context->RSSetViewports(1, &viewport);
    context->RSSetState(g_quadRaster.Get());
    
    float aspect = (float)width / (float)height;
    float fov = 40.0f;
    float visibleH = 2.0f * 3.0f * tanf(XMConvertToRadians(fov / 2.0f));
    float visibleW = visibleH * aspect;
    float scale = max(visibleW / 2.0f, visibleH / (2.0f * g_quadHalfHeight));

    float scaleMulti = g_tiltDeg / 270;
    float scaleY = scale * (1 + scaleMulti) * 1;

    float topY = g_quadHalfHeight * (2.0f * scaleY - 1.0f);

    XMMATRIX world =
        XMMatrixTranslation(0.0f, g_quadHalfHeight, 0.0f) *
        XMMatrixScaling(scale, scaleY, scale) *
        XMMatrixRotationX(XMConvertToRadians(g_tiltDeg)) *
        XMMatrixTranslation(0.0f, -g_quadHalfHeight, 0.0f) *
        XMMatrixTranslation(0.0f, -(scale - 1.0f) * g_quadHalfHeight, 0.0f);
    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0.0f, 0.0f, -3.0f, 1.0f),
                                     XMVectorZero(), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(fov), aspect, 0.1f, 100.0f);
    XMMATRIX transform = XMMatrixTranspose(world * view * proj);

    const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    UINT stride = sizeof(Vertex);
    UINT offset = 0;

    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(g_quadLayout.Get());
    context->VSSetConstantBuffers(0, 1, g_transformCB.GetAddressOf());
    context->VSSetShader(g_quadVS.Get(), nullptr, 0);
    context->PSSetSamplers(0, 1, g_quadSampler.GetAddressOf());

    context->OMSetRenderTargets(1, g_sceneTarget.renderTargetView.GetAddressOf(), nullptr);
    context->ClearRenderTargetView(g_sceneTarget.renderTargetView.Get(), clear);

    context->UpdateSubresource(g_transformCB.Get(), 0, nullptr, &transform, 0, 0);
    ID3D11Buffer* vb = g_quadVB.Get();
    context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    context->IASetIndexBuffer(g_quadIB.Get(), DXGI_FORMAT_R16_UINT, 0);
    context->PSSetShader(g_quadPS.Get(), nullptr, 0);

    if (g_desktopSRV)
    {
        const XMFLOAT4 fadeParams(g_fadeStart, g_fadeEnd, g_fadeStrength, 0.0f);
        context->UpdateSubresource(g_fadeCB.Get(), 0, nullptr, &fadeParams, 0, 0);
        context->PSSetConstantBuffers(1, 1, g_fadeCB.GetAddressOf());

        ID3D11ShaderResourceView* srv = g_desktopSRV.Get();
        context->PSSetShaderResources(0, 1, &srv);
        context->DrawIndexed(6, 0, 0);
    }

    XMMATRIX identity = XMMatrixIdentity();
    context->UpdateSubresource(g_transformCB.Get(), 0, nullptr, &identity, 0, 0);

    vb = g_blurVB.Get();
    context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    context->IASetIndexBuffer(g_blurIB.Get(), DXGI_FORMAT_R16_UINT, 0);

    context->OMSetRenderTargets(1, g_blurTarget.renderTargetView.GetAddressOf(), nullptr);
    context->PSSetShader(g_blurPS.Get(), nullptr, 0);

    const float texelX = 1.0f / (float)width;
    const float texelY = 1.0f / (float)height;

    const XMFLOAT4 blurRanges(g_blurRadiusMin, g_blurRadius, 0.0f, 0.0f);

    const XMFLOAT4 blurH(texelX, texelY, 0.0f, 0.0f);
    const XMFLOAT4 blurH2 = blurRanges;
    struct { XMFLOAT4 a; XMFLOAT4 b; } blurHB = { blurH, blurH2 };
    context->UpdateSubresource(g_blurCB.Get(), 0, nullptr, &blurHB, 0, 0);
    context->PSSetConstantBuffers(1, 1, g_blurCB.GetAddressOf());
    ID3D11ShaderResourceView* sceneSRV = g_sceneTarget.shaderResourceView.Get();
    context->PSSetShaderResources(0, 1, &sceneSRV);
    context->DrawIndexed(6, 0, 0);
    context->PSSetShaderResources(0, 0, nullptr);

    context->OMSetRenderTargets(1, &backRTV, nullptr);
    context->ClearRenderTargetView(backRTV, clear);

    const XMFLOAT4 blurV(texelX, texelY, 1.0f, 0.0f);
    struct { XMFLOAT4 a; XMFLOAT4 b; } blurVB = { blurV, blurRanges };
    context->UpdateSubresource(g_blurCB.Get(), 0, nullptr, &blurVB, 0, 0);
    ID3D11ShaderResourceView* blurSRV = g_blurTarget.shaderResourceView.Get();
    context->PSSetShaderResources(0, 1, &blurSRV);
    context->DrawIndexed(6, 0, 0);
    context->PSSetShaderResources(0, 0, nullptr);

    g_graphics.Present();
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

    BOOL borderless = true;
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT, wc.lpszClassName, L"FlowDuo",
        borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW,
        0, 0,
        GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN),
        nullptr, nullptr, hInstance, nullptr);

    ShowWindow(hwnd, nShowCmd);
    bool excludeFromCapture = true;
    if (excludeFromCapture)
        SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    g_graphics.InitForCustomRendering(hwnd);
    g_hingeReader.Init();
    g_hingeReader.useRawAccelerometer = true;

    ID3D11Device* device = g_graphics.GetDevice(); 
    ID3D11DeviceContext* context = g_graphics.GetContext();

    CreateQuadPipeline(device);
    g_graphicsReady = true;

    while (true) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                return (int)msg.wParam;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!calibrated) Calibrate(hwnd);

        UpdateTiltFromHinge();

        ToggleWindowVisible(hwnd, g_tiltDeg > 0.0f);
        if (!g_windowVisible)
        {
            Sleep(50);
            continue;
        }

        UpdateDesktopFrame(device, context);
        PresentQuad(context);
    }
}