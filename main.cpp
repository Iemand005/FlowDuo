#include "CubeRenderer.h"

#include <cstring>
#include <stdexcept>

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
static ComPtr<ID3D11SamplerState> g_quadSampler;
static ComPtr<ID3D11RasterizerState> g_quadRaster;

static XMMATRIX g_world = XMMatrixIdentity();

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

    const char* psSrc =
        "Texture2D desktopTex : register(t0);\n"
        "SamplerState samp : register(s0);\n"
        "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
        "float4 main(VSOut i) : SV_TARGET\n"
        "{\n"
        "    float4 c = desktopTex.Sample(samp, i.uv);\n"
        "    return float4(c.rgb, 1.0);\n"
        "}\n";

    auto vsBlob = CompileShader(vsSrc, "vs_5_0");
    auto psBlob = CompileShader(psSrc, "ps_5_0");

    CheckHr(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_quadVS));
    CheckHr(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_quadPS));

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

    desc.ByteWidth = sizeof(XMMATRIX);
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    CheckHr(device->CreateBuffer(&desc, nullptr, &g_transformCB));

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
    HRESULT hr = g_duplication->AcquireNextFrame(16, &frameInfo, &resource);

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

    const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    context->ClearRenderTargetView(rtv.Get(), clear);

    D3D11_VIEWPORT viewport = { 0, 0, (float)bbDesc.Width, (float)bbDesc.Height, 0.0f, 1.0f };
    context->RSSetViewports(1, &viewport);
    context->RSSetState(g_quadRaster.Get());

    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(g_quadLayout.Get());
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ID3D11Buffer* vb = g_quadVB.Get();
    context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    context->IASetIndexBuffer(g_quadIB.Get(), DXGI_FORMAT_R16_UINT, 0);

    float aspect = (float)bbDesc.Width / (float)bbDesc.Height;
    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0.0f, 0.0f, -3.0f, 1.0f),
                                     XMVectorZero(), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), aspect, 0.1f, 100.0f);
    XMMATRIX transform = XMMatrixTranspose(g_world * view * proj);
    context->UpdateSubresource(g_transformCB.Get(), 0, nullptr, &transform, 0, 0);
    context->VSSetConstantBuffers(0, 1, g_transformCB.GetAddressOf());

    context->VSSetShader(g_quadVS.Get(), nullptr, 0);
    context->PSSetShader(g_quadPS.Get(), nullptr, 0);
    context->PSSetSamplers(0, 1, g_quadSampler.GetAddressOf());

    if (g_desktopSRV)
    {
        ID3D11ShaderResourceView* srv = g_desktopSRV.Get();
        context->PSSetShaderResources(0, 1, &srv);
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

    g_graphics.Init(hwnd);

    ID3D11Device* device = g_graphics.GetDevice();
    ID3D11DeviceContext* context = g_graphics.GetContext();
    IDXGISwapChain* swapChain = g_graphics.GetSwapChain();

    CreateQuadPipeline(device);
    InitDesktopCapture(device);

    g_world = XMMatrixRotationY(XMConvertToRadians(-25.0f)) *
              XMMatrixRotationX(XMConvertToRadians(-55.0f));

    g_graphicsReady = true;

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

        UpdateDesktopFrame(device, context);
        PresentQuad(device, context, swapChain);
        Sleep(16);
    }
}