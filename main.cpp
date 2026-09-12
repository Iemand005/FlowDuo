#include "CubeRenderer.h"

#include <vector>

using namespace CubeRenderer;

static Graphics g_graphics;
static bool g_graphicsReady = false;

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

        g_graphics.Render(angle, 0.0f, 0.0f, 0.0f);
        angle += 0.01f;
        Sleep(16);
    }
}