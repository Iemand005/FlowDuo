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
static UINT g_backBufferWidth = 0;
static UINT g_backBufferHeight = 0;

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

static float g_fadeStrength = 0.0f;
static float g_blurPixelsPerWorldUnit = 100.0f;
static DWORD g_hingeSampleIntervalMs = 1;
static UINT g_presentSyncInterval = 1;
static float g_headEyeHeight = 1.25f;
static float g_headDistance = 3.0f;

static bool calibrated = false;

struct DisplayGeometry {
    XMVECTOR virtualBottomLeft;
    XMVECTOR virtualBottomRight;
    XMVECTOR virtualTopLeft;
    XMVECTOR virtualTopRight;
    XMVECTOR lidTopLeft;
    XMVECTOR lidTopRight;
};

static bool BuildDisplayGeometry(float screenHeight, float hingeAngle, DisplayGeometry* geometry) {
    const float screenWidth = 2.0f;
    const XMVECTOR virtualBottomLeft = XMVectorSet(-screenWidth * 0.5f, 0.0f, 0.0f, 1.0f);
    const XMVECTOR virtualBottomRight = XMVectorSet(screenWidth * 0.5f, 0.0f, 0.0f, 1.0f);
    const XMVECTOR virtualTopLeft = XMVectorSet(-screenWidth * 0.5f, screenHeight, 0.0f, 1.0f);
    const XMVECTOR virtualTopRight = XMVectorSet(screenWidth * 0.5f, screenHeight, 0.0f, 1.0f);
    const XMVECTOR lidTopLeft = XMVectorSet(-screenWidth * 0.5f, screenHeight * cosf(hingeAngle), screenHeight * sinf(hingeAngle), 1.0f);
    const XMVECTOR lidTopRight = XMVectorSet(screenWidth * 0.5f, screenHeight * cosf(hingeAngle), screenHeight * sinf(hingeAngle), 1.0f);

    geometry->virtualBottomLeft = virtualBottomLeft;
    geometry->virtualBottomRight = virtualBottomRight;
    geometry->virtualTopLeft = virtualTopLeft;
    geometry->virtualTopRight = virtualTopRight;
    geometry->lidTopLeft = lidTopLeft;
    geometry->lidTopRight = lidTopRight;
    return true;
}

static void BuildFullscreenVertices(Vertex* vertices) {
    vertices[0].position = { -1.0f, 1.0f, 0.0f };
    vertices[1].position = { 1.0f, 1.0f, 0.0f };
    vertices[2].position = { -1.0f, -1.0f, 0.0f };
    vertices[3].position = { 1.0f, -1.0f, 0.0f };
    vertices[0].textureCoordinate = { 0.0f, 0.0f };
    vertices[1].textureCoordinate = { 1.0f, 0.0f };
    vertices[2].textureCoordinate = { 0.0f, 1.0f };
    vertices[3].textureCoordinate = { 1.0f, 1.0f };
}

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
    if (!visible) g_desktopCapture.Reset(g_graphics.GetContext());
    SetLayeredWindowAttributes(hwnd, 0, visible ? 255 : 0, LWA_ALPHA);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (g_graphicsReady)
        {
            g_graphics.Resize(hwnd);
            g_backBufferWidth = LOWORD(lParam);
            g_backBufferHeight = HIWORD(lParam);
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_SPACE) Calibrate();
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void CheckHr(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("HRESULT failed");
}

static void CreateQuadPipeline(ID3D11Device* device) {
    auto vsBlob = g_graphics.CompileShaderResource(IDR_QUAD_VERTEX, "vs_5_0");
    auto psBlob = g_graphics.CompileShaderResource(IDR_DESKTOP_PIXEL, "ps_5_0");

    CheckHr(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_quadVS));
    CheckHr(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_quadPS));

    g_graphics.CreateQuadResources(vsBlob.Get(), g_quadResources);
}

static void ConfigureQuadPipeline(ID3D11DeviceContext* context) {
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->IASetInputLayout(nullptr);
    context->VSSetConstantBuffers(0, 1, g_quadResources.transformBuffer.GetAddressOf());
    context->VSSetShader(g_quadVS.Get(), nullptr, 0);
    context->PSSetSamplers(0, 1, g_quadResources.sampler.GetAddressOf());
    context->RSSetState(g_quadResources.rasterizer.Get());
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

    g_fadeStrength = min(g_tiltDeg / 60, 1);
}

static bool PresentQuad(ID3D11DeviceContext* context) {
    ID3D11RenderTargetView* backRTV = g_graphics.GetRenderTargetView();
    if (!backRTV)
        return false;

    UINT width = g_backBufferWidth;
    UINT height = g_backBufferHeight;
    if (width == 0 || height == 0)
        return false;

    D3D11_VIEWPORT viewport = { 0, 0, (float)width, (float)height, 0.0f, 1.0f };
    context->RSSetViewports(1, &viewport);
    Vertex fullscreenVertices[4] = {};
    DisplayGeometry geometry = {};
    if (!BuildDisplayGeometry(2.0f * g_quadHalfHeight, XMConvertToRadians(g_tiltDeg), &geometry))
        return false;
    BuildFullscreenVertices(fullscreenVertices);
    context->UpdateSubresource(g_quadResources.vertexBuffer.Get(), 0, nullptr, fullscreenVertices, 0, 0);

    const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    context->OMSetRenderTargets(1, &backRTV, nullptr);
    context->ClearRenderTargetView(backRTV, clear);

    struct SceneConstants {
        XMFLOAT4 headPos;
        XMFLOAT4 dispOrigin;
        XMFLOAT4 dispRightAxis;
        XMFLOAT4 dispUpAxis;
        XMFLOAT4 dispNormal;
        XMFLOAT4 lidBL;
        XMFLOAT4 lidBR;
        XMFLOAT4 lidTL;
        XMFLOAT4 lidTR;
        XMFLOAT4 displayMetrics;
        XMFLOAT4 blurMetrics;
        XMFLOAT4 effectMetrics;
    } sceneConstants = {};
    sceneConstants.headPos = { 0.0f, g_headEyeHeight, g_headDistance, 0.0f };
    sceneConstants.dispOrigin = { -1.0f, 0.0f, 0.0f, 0.0f };
    sceneConstants.dispRightAxis = { 1.0f, 0.0f, 0.0f, 0.0f };
    sceneConstants.dispUpAxis = { 0.0f, 1.0f, 0.0f, 0.0f };
    sceneConstants.dispNormal = { 0.0f, 0.0f, 1.0f, 0.0f };
    XMStoreFloat4(&sceneConstants.lidBL, geometry.virtualBottomLeft);
    XMStoreFloat4(&sceneConstants.lidBR, geometry.virtualBottomRight);
    XMStoreFloat4(&sceneConstants.lidTL, geometry.lidTopLeft);
    XMStoreFloat4(&sceneConstants.lidTR, geometry.lidTopRight);
    sceneConstants.displayMetrics = { 2.0f, 2.0f * g_quadHalfHeight, 0.0f, 0.0f };
    sceneConstants.blurMetrics = { 0.0f, 0.0f, g_blurPixelsPerWorldUnit, 0.0f };
    sceneConstants.effectMetrics = { g_fadeStrength, 0.0f, 0.0f, 0.0f };
    context->UpdateSubresource(g_quadResources.transformBuffer.Get(), 0, nullptr, &sceneConstants, 0, 0);
    context->VSSetConstantBuffers(0, 1, g_quadResources.transformBuffer.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, g_quadResources.transformBuffer.GetAddressOf());
    context->PSSetShader(g_quadPS.Get(), nullptr, 0);

    if (ID3D11ShaderResourceView* desktopSRV = g_desktopCapture.GetShaderResourceView())
    {
        context->PSSetShaderResources(0, 1, &desktopSRV);
        context->Draw(4, 0);
    }
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
        WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT, wc.lpszClassName, L"FlowDuo",
        WS_POPUP,
        0, 0,
        GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN),
        nullptr, nullptr, hInstance, nullptr);

    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);

    g_graphics.InitForCustomRendering(hwnd);
    g_hingeReader.Init();
    g_hingeReader.useRawAccelerometer = true;

    ID3D11Device* device = g_graphics.GetDevice(); 
    ID3D11DeviceContext* context = g_graphics.GetContext();

    CreateQuadPipeline(device);
    ConfigureQuadPipeline(context);
    g_graphics.GetBackBufferSize(&g_backBufferWidth, &g_backBufferHeight);
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
        wantsVisible = true;
        if (!wantsVisible)
        {
            ToggleWindowVisible(hwnd, false);
            Sleep(50);
            continue;
        }

        bool hasFreshFrame = g_desktopCapture.Update(device, context);
        if (!g_windowVisible && !hasFreshFrame)
        {
            Sleep(1);
            continue;
        }

        if (hasFreshFrame)
        {
            float aspectRatio = g_desktopCapture.GetAspectRatio();
            if (aspectRatio > 0.0f && g_quadHalfHeight != 1.0f / aspectRatio)
            {
                g_quadHalfHeight = 1.0f / aspectRatio;
                g_graphics.CreateQuadVertexBuffer(2.0f, 2.0f * g_quadHalfHeight, g_quadResources.vertexBuffer);
            }
        }

        bool presented = PresentQuad(context);
        if (presented) ToggleWindowVisible(hwnd, true);
    }
}