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

static float g_fadeStart = 0.2f;
static float g_fadeEnd = 1.0f;
static float g_fadeStrength = 0.0f;
static float g_blurScale = 0.08f;
static bool g_flipV = true;
static DWORD g_hingeSampleIntervalMs = 1;
static UINT g_presentSyncInterval = 1;
static XMFLOAT3 g_headPosition = { 0.0f, 0.0f, -3.0f };

static bool calibrated = false;

struct DisplayGeometry {
    XMVECTOR virtualBottomLeft;
    XMVECTOR virtualBottomRight;
    XMVECTOR virtualTopLeft;
    XMVECTOR virtualTopRight;
    XMVECTOR lidTopLeft;
    XMVECTOR lidTopRight;
    XMVECTOR lidCenter;
    XMVECTOR projectedTopLeft;
    XMVECTOR projectedTopRight;
    XMFLOAT2 projectedTopLeftUv;
    XMFLOAT2 projectedTopRightUv;
    float lidTopBlur;
};

static XMVECTOR RotateAroundAxis(const XMVECTOR& vector, const XMVECTOR& axis, float angle) {
    const XMVECTOR unitAxis = XMVector3Normalize(axis);
    const float cosine = cosf(angle);
    const float sine = sinf(angle);
    const XMVECTOR parallel = XMVectorScale(
        unitAxis,
        XMVectorGetX(XMVector3Dot(unitAxis, vector)) * (1.0f - cosine));
    return XMVectorAdd(
        XMVectorAdd(XMVectorScale(vector, cosine), XMVectorScale(XMVector3Cross(unitAxis, vector), sine)),
        parallel);
}

static bool RayPlaneIntersection(
    const XMVECTOR& rayOrigin,
    const XMVECTOR& ray,
    const XMVECTOR& planePoint,
    const XMVECTOR& planeNormal,
    XMVECTOR* intersection) {
    constexpr float epsilon = 1.0e-5f;
    const float denominator = XMVectorGetX(XMVector3Dot(ray, planeNormal));
    if (fabsf(denominator) < epsilon)
        return false;

    const float distance = XMVectorGetX(XMVector3Dot(
        XMVectorSubtract(planePoint, rayOrigin), planeNormal)) / denominator;
    if (distance < epsilon)
        return false;

    *intersection = XMVectorAdd(rayOrigin, XMVectorScale(ray, distance));
    return true;
}

static bool BuildDisplayGeometry(float screenHeight, float hingeAngle, DisplayGeometry* geometry) {
    const float screenWidth = 2.0f;
    const float halfHeight = screenHeight * 0.5f;
    const XMVECTOR virtualBottomLeft = XMVectorSet(-screenWidth * 0.5f, -halfHeight, 0.0f, 1.0f);
    const XMVECTOR virtualBottomRight = XMVectorSet(screenWidth * 0.5f, -halfHeight, 0.0f, 1.0f);
    const XMVECTOR virtualTopLeft = XMVectorSet(-screenWidth * 0.5f, halfHeight, 0.0f, 1.0f);
    const XMVECTOR virtualTopRight = XMVectorSet(screenWidth * 0.5f, halfHeight, 0.0f, 1.0f);

    const XMVECTOR hingeAxis = XMVector3Normalize(XMVectorSubtract(virtualBottomRight, virtualBottomLeft));
    const XMVECTOR displayHeight = XMVector3Normalize(XMVectorSubtract(virtualTopLeft, virtualBottomLeft));
    const XMVECTOR lidHeight = XMVectorScale(
        RotateAroundAxis(displayHeight, hingeAxis, hingeAngle), screenHeight);
    const XMVECTOR lidTopLeft = XMVectorAdd(virtualBottomLeft, lidHeight);
    const XMVECTOR lidTopRight = XMVectorAdd(virtualBottomRight, lidHeight);

    const XMVECTOR head = XMLoadFloat3(&g_headPosition);
    const XMVECTOR virtualNormal = XMVector3Normalize(XMVector3Cross(
        XMVectorSubtract(virtualBottomRight, virtualBottomLeft),
        XMVectorSubtract(virtualTopLeft, virtualBottomLeft)));
    XMVECTOR projectedTopLeft;
    XMVECTOR projectedTopRight;
    if (!RayPlaneIntersection(head, XMVectorSubtract(lidTopLeft, head), virtualBottomLeft, virtualNormal, &projectedTopLeft) ||
        !RayPlaneIntersection(head, XMVectorSubtract(lidTopRight, head), virtualBottomLeft, virtualNormal, &projectedTopRight))
        return false;

    const XMVECTOR virtualX = XMVector3Normalize(XMVectorSubtract(virtualBottomRight, virtualBottomLeft));
    const XMVECTOR virtualY = XMVector3Normalize(XMVectorSubtract(virtualTopLeft, virtualBottomLeft));
    const float virtualWidth = XMVectorGetX(XMVector3Length(XMVectorSubtract(virtualBottomRight, virtualBottomLeft)));
    const float virtualHeight = XMVectorGetX(XMVector3Length(XMVectorSubtract(virtualTopLeft, virtualBottomLeft)));
    const XMVECTOR projectedLeftOffset = XMVectorSubtract(projectedTopLeft, virtualBottomLeft);
    const XMVECTOR projectedRightOffset = XMVectorSubtract(projectedTopRight, virtualBottomLeft);
    const XMFLOAT2 projectedTopLeftUv = {
        XMVectorGetX(XMVector3Dot(projectedLeftOffset, virtualX)) / virtualWidth,
        1.0f - XMVectorGetX(XMVector3Dot(projectedLeftOffset, virtualY)) / virtualHeight
    };
    const XMFLOAT2 projectedTopRightUv = {
        XMVectorGetX(XMVector3Dot(projectedRightOffset, virtualX)) / virtualWidth,
        1.0f - XMVectorGetX(XMVector3Dot(projectedRightOffset, virtualY)) / virtualHeight
    };
    if (!isfinite(projectedTopLeftUv.x) || !isfinite(projectedTopLeftUv.y) ||
        !isfinite(projectedTopRightUv.x) || !isfinite(projectedTopRightUv.y))
        return false;

    geometry->virtualBottomLeft = virtualBottomLeft;
    geometry->virtualBottomRight = virtualBottomRight;
    geometry->virtualTopLeft = virtualTopLeft;
    geometry->virtualTopRight = virtualTopRight;
    geometry->lidTopLeft = lidTopLeft;
    geometry->lidTopRight = lidTopRight;
    geometry->lidCenter = XMVectorScale(
        XMVectorAdd(XMVectorAdd(virtualBottomLeft, virtualBottomRight),
                    XMVectorAdd(lidTopLeft, lidTopRight)), 0.25f);
    geometry->lidTopBlur = fabsf(XMVectorGetZ(lidTopLeft));
    geometry->projectedTopLeft = projectedTopLeft;
    geometry->projectedTopRight = projectedTopRight;
    geometry->projectedTopLeftUv = projectedTopLeftUv;
    geometry->projectedTopRightUv = projectedTopRightUv;
    return true;
}

static void BuildLidVertices(const DisplayGeometry& geometry, Vertex* vertices) {
    const XMVECTOR positions[] = {
        geometry.virtualBottomLeft,
        geometry.virtualBottomRight,
        geometry.lidTopRight,
        geometry.lidTopLeft
    };
    for (int index = 0; index < 4; ++index)
        XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(&vertices[index].position), positions[index]);

    vertices[0].textureCoordinate = { 0.0f, 1.0f };
    vertices[1].textureCoordinate = { 1.0f, 1.0f };
    vertices[2].textureCoordinate = { geometry.projectedTopRightUv.x, geometry.projectedTopRightUv.y };
    vertices[3].textureCoordinate = { geometry.projectedTopLeftUv.x, geometry.projectedTopLeftUv.y };
    vertices[0].blur = 0.0f;
    vertices[1].blur = 0.0f;
    vertices[2].blur = geometry.lidTopBlur;
    vertices[3].blur = geometry.lidTopBlur;
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
        if (wParam == 'F') g_flipV = !g_flipV;
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
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(g_quadResources.inputLayout.Get());
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
    float aspect = (float)width / (float)height;
    float fov = 40.0f;
    const XMVECTOR head = XMLoadFloat3(&g_headPosition);
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(fov), aspect, 0.1f, 100.0f);
    Vertex lidVertices[4] = {};
    DisplayGeometry geometry = {};
    if (!BuildDisplayGeometry(2.0f * g_quadHalfHeight, XMConvertToRadians(g_tiltDeg), &geometry))
        return false;
    BuildLidVertices(geometry, lidVertices);
    context->UpdateSubresource(g_quadResources.vertexBuffer.Get(), 0, nullptr, lidVertices, 0, 0);

    XMMATRIX view = XMMatrixLookAtLH(head, geometry.lidCenter, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    XMMATRIX transform = XMMatrixTranspose(view * proj);

    const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    UINT stride = sizeof(Vertex);
    UINT offset = 0;

    context->OMSetRenderTargets(1, &backRTV, nullptr);
    context->ClearRenderTargetView(backRTV, clear);

    context->UpdateSubresource(g_quadResources.transformBuffer.Get(), 0, nullptr, &transform, 0, 0);
    ID3D11Buffer* vb = g_quadResources.vertexBuffer.Get();
    context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    context->IASetIndexBuffer(g_quadResources.indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    context->PSSetShader(g_quadPS.Get(), nullptr, 0);

    if (ID3D11ShaderResourceView* desktopSRV = g_desktopCapture.GetShaderResourceView())
    {
        struct SceneConstants {
            XMFLOAT4 fadeParams;
            XMFLOAT4 blurParams;
            XMFLOAT4 pad[4];
        } sceneConstants = {};
        sceneConstants.fadeParams = { g_fadeStart, g_fadeEnd, g_fadeStrength, 0.0f };
        sceneConstants.blurParams = { g_blurScale, g_flipV ? 1.0f : 0.0f, 0.0f, 0.0f };
        context->UpdateSubresource(g_quadResources.fadeBuffer.Get(), 0, nullptr, &sceneConstants, 0, 0);
        context->PSSetConstantBuffers(1, 1, g_quadResources.fadeBuffer.GetAddressOf());

        context->PSSetShaderResources(0, 1, &desktopSRV);
        context->DrawIndexed(6, 0, 0);
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