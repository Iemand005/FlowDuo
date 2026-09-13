#include "CubeRenderer.h"
#include "DesktopCapture.h"
#include "HingeSensorReader.h"
#include "Resource.h"

#include <stdexcept>
#include <cmath>

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

static DesktopCapture g_desktopCapture;

static ComPtr<ID3D11VertexShader> g_quadVS;
static ComPtr<ID3D11PixelShader> g_quadPS;
static Graphics::QuadResources g_quadResources;
static float g_quadHalfHeight = 1.0f;

static ComPtr<ID3D11PixelShader> g_blurQualityPS;
static ComPtr<ID3D11PixelShader> g_blurPerformancePS;

static Graphics::RenderTarget g_sceneTarget;
static Graphics::RenderTarget g_blurTarget;

static float g_fadeStart = 0.2f;
static float g_fadeEnd = 1.0f;
static float g_fadeStrength = 0.0f;
static float g_blurRadius = 20.0f;
static float g_blurRadiusMultiplier = 2.0f;
static DWORD g_hingeSampleIntervalMs = 1;
static float g_effectResolutionScale = 0.7f;
static UINT g_presentSyncInterval = 1;
static bool g_highQualityBlur = true;

static bool calibrated = false;

static void Calibrate() {
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
    if (!visible)
        g_desktopCapture.Reset(g_graphics.GetContext());
    SetLayeredWindowAttributes(hwnd, 0, visible ? 255 : 0, LWA_ALPHA);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (g_graphicsReady)
            g_graphics.Resize(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_SPACE) Calibrate();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void CheckHr(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("HRESULT failed");
}

static ComPtr<ID3DBlob> CompileShaderResource(int resourceId, const char* target) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    CheckHr(resource ? S_OK : HRESULT_FROM_WIN32(GetLastError()));

    HGLOBAL data = LoadResource(module, resource);
    CheckHr(data ? S_OK : HRESULT_FROM_WIN32(GetLastError()));

    DWORD size = SizeofResource(module, resource);
    CheckHr(size ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
    return g_graphics.CompileShader(LockResource(data), size, target);
}

static void CreateQuadPipeline(ID3D11Device* device) {
    auto vsBlob = CompileShaderResource(IDR_QUAD_VERTEX, "vs_5_0");
    auto psBlob = CompileShaderResource(IDR_DESKTOP_PIXEL, "ps_5_0");
    auto blurQualityBlob = CompileShaderResource(IDR_BLUR_QUALITY, "ps_5_0");
    auto blurPerformanceBlob = CompileShaderResource(IDR_BLUR_PERFORMANCE, "ps_5_0");

    CheckHr(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_quadVS));
    CheckHr(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_quadPS));
    CheckHr(device->CreatePixelShader(blurQualityBlob->GetBufferPointer(), blurQualityBlob->GetBufferSize(), nullptr, &g_blurQualityPS));
    CheckHr(device->CreatePixelShader(blurPerformanceBlob->GetBufferPointer(), blurPerformanceBlob->GetBufferSize(), nullptr, &g_blurPerformancePS));

    g_graphics.CreateQuadResources(vsBlob.Get(), g_quadResources);
}

static void UpdateTiltFromHinge() {
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

static bool PresentQuad(ID3D11DeviceContext* context) {
    ID3D11RenderTargetView* backRTV = g_graphics.GetRenderTargetView();
    if (!backRTV)
        return false;

    UINT width = 0;
    UINT height = 0;
    g_graphics.GetBackBufferSize(&width, &height);
    if (width == 0 || height == 0)
        return false;

    UINT effectWidth = max(1u, (UINT)(width * g_effectResolutionScale));
    UINT effectHeight = max(1u, (UINT)(height * g_effectResolutionScale));
    g_graphics.EnsureRenderTarget(g_sceneTarget, effectWidth, effectHeight);
    g_graphics.EnsureRenderTarget(g_blurTarget, effectWidth, effectHeight);

    D3D11_VIEWPORT viewport = { 0, 0, (float)effectWidth, (float)effectHeight, 0.0f, 1.0f };
    context->RSSetViewports(1, &viewport);
    context->RSSetState(g_quadResources.rasterizer.Get());
    
    float aspect = (float)width / (float)height;
    float fov = 40.0f;
    float visibleH = 2.0f * 3.0f * tanf(XMConvertToRadians(fov / 2.0f));
    float visibleW = visibleH * aspect;
    float scale = max(visibleW / 2.0f, visibleH / (2.0f * g_quadHalfHeight));

    float scaleMulti = g_tiltDeg / 100;
    float scaleY = scale * (1 + scaleMulti) * 1;

    XMMATRIX world =
        XMMatrixTranslation(0.0f, g_quadHalfHeight, 0.0f) *
        XMMatrixScaling(scale, scaleY, scale) *
        XMMatrixRotationX(XMConvertToRadians(g_tiltDeg)) *
        XMMatrixTranslation(0.0f, -g_quadHalfHeight, 0.0f) *
        XMMatrixTranslation(0.0f, -(scale - 1.0f) * g_quadHalfHeight, 0.0f);
    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0.0f, 0.0f, -3.0f, 1.0f), XMVectorZero(), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(fov), aspect, 0.1f, 100.0f);
    XMMATRIX transform = XMMatrixTranspose(world * view * proj);

    const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    UINT stride = sizeof(Vertex);
    UINT offset = 0;

    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(g_quadResources.inputLayout.Get());
    context->VSSetConstantBuffers(0, 1, g_quadResources.transformBuffer.GetAddressOf());
    context->VSSetShader(g_quadVS.Get(), nullptr, 0);
    context->PSSetSamplers(0, 1, g_quadResources.sampler.GetAddressOf());

    context->OMSetRenderTargets(1, g_sceneTarget.renderTargetView.GetAddressOf(), nullptr);
    context->ClearRenderTargetView(g_sceneTarget.renderTargetView.Get(), clear);

    context->UpdateSubresource(g_quadResources.transformBuffer.Get(), 0, nullptr, &transform, 0, 0);
    ID3D11Buffer* vb = g_quadResources.vertexBuffer.Get();
    context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    context->IASetIndexBuffer(g_quadResources.indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    context->PSSetShader(g_quadPS.Get(), nullptr, 0);

    if (ID3D11ShaderResourceView* desktopSRV = g_desktopCapture.GetShaderResourceView())
    {
        const XMFLOAT4 fadeParams(g_fadeStart, g_fadeEnd, g_fadeStrength, 0.0f);
        context->UpdateSubresource(g_quadResources.fadeBuffer.Get(), 0, nullptr, &fadeParams, 0, 0);
        context->PSSetConstantBuffers(1, 1, g_quadResources.fadeBuffer.GetAddressOf());

        context->PSSetShaderResources(0, 1, &desktopSRV);
        context->DrawIndexed(6, 0, 0);
    }

    XMMATRIX identity = XMMatrixIdentity();
    context->UpdateSubresource(g_quadResources.transformBuffer.Get(), 0, nullptr, &identity, 0, 0);

    vb = g_quadResources.blurVertexBuffer.Get();
    context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    context->IASetIndexBuffer(g_quadResources.blurIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);

    context->OMSetRenderTargets(1, g_blurTarget.renderTargetView.GetAddressOf(), nullptr);
    context->PSSetShader((g_highQualityBlur ? g_blurQualityPS : g_blurPerformancePS).Get(), nullptr, 0);

    const float texelX = 1.0f / (float)effectWidth;
    const float texelY = 1.0f / (float)effectHeight;

    const XMFLOAT4 blurRanges(0.0f, g_blurRadius * g_effectResolutionScale, 0.0f, 0.0f);

    const XMFLOAT4 blurH(texelX, texelY, 0.0f, 0.0f);
    const XMFLOAT4 blurH2 = blurRanges;
    struct { XMFLOAT4 a; XMFLOAT4 b; } blurHB = { blurH, blurH2 };
    context->UpdateSubresource(g_quadResources.blurBuffer.Get(), 0, nullptr, &blurHB, 0, 0);
    context->PSSetConstantBuffers(1, 1, g_quadResources.blurBuffer.GetAddressOf());
    ID3D11ShaderResourceView* sceneSRV = g_sceneTarget.shaderResourceView.Get();
    context->PSSetShaderResources(0, 1, &sceneSRV);
    context->DrawIndexed(6, 0, 0);
    context->PSSetShaderResources(0, 0, nullptr);

    viewport = { 0, 0, (float)width, (float)height, 0.0f, 1.0f };
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &backRTV, nullptr);
    context->ClearRenderTargetView(backRTV, clear);

    const XMFLOAT4 blurV(texelX, texelY, 1.0f, 0.0f);
    struct { XMFLOAT4 a; XMFLOAT4 b; } blurVB = { blurV, blurRanges };
    context->UpdateSubresource(g_quadResources.blurBuffer.Get(), 0, nullptr, &blurVB, 0, 0);
    ID3D11ShaderResourceView* blurSRV = g_blurTarget.shaderResourceView.Get();
    context->PSSetShaderResources(0, 1, &blurSRV);
    context->DrawIndexed(6, 0, 0);
    context->PSSetShaderResources(0, 0, nullptr);

    g_graphics.Present(g_presentSyncInterval);
    return true;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FlowDuoWindow";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT, wc.lpszClassName, L"FlowDuo",
        WS_POPUP,
        0, 0,
        GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN),
        nullptr, nullptr, hInstance, nullptr);

    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOW);
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);

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

        if (!calibrated) Calibrate();

        UpdateTiltFromHinge();

        bool wantsVisible = g_tiltDeg > 0.0f;
        if (!wantsVisible)
        {
            ToggleWindowVisible(hwnd, false);
            Sleep(50);
            continue;
        }

        bool hasFreshFrame = g_desktopCapture.Update(device, context);
        if (hasFreshFrame)
        {
            float aspectRatio = g_desktopCapture.GetAspectRatio();
            if (aspectRatio > 0.0f && g_quadHalfHeight != 1.0f / aspectRatio)
            {
                g_quadHalfHeight = 1.0f / aspectRatio;
                g_graphics.CreateQuadVertexBuffer(2.0f, 2.0f * g_quadHalfHeight, g_quadResources.vertexBuffer);
            }
        }
        if (!g_windowVisible && !hasFreshFrame)
        {
            Sleep(1);
            continue;
        }

        bool presented = PresentQuad(context);
        if (presented) ToggleWindowVisible(hwnd, true);
    }
}