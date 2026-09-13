#include "DesktopCapture.h"

#include <stdexcept>

using namespace Microsoft::WRL;

static void CheckCaptureHr(HRESULT hr)
{
    if (FAILED(hr))
        throw std::runtime_error("Desktop capture HRESULT failed");
}

bool DesktopCapture::Initialize(ID3D11Device* device)
{
    ComPtr<IDXGIDevice> dxgiDevice;
    CheckCaptureHr(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)));

    ComPtr<IDXGIAdapter> adapter;
    CheckCaptureHr(dxgiDevice->GetAdapter(&adapter));

    ComPtr<IDXGIOutput> output;
    CheckCaptureHr(adapter->EnumOutputs(0, &output));

    ComPtr<IDXGIOutput1> output1;
    CheckCaptureHr(output->QueryInterface(IID_PPV_ARGS(&output1)));
    CheckCaptureHr(output1->DuplicateOutput(device, &duplication));
    return true;
}

bool DesktopCapture::Update(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (!duplication && !Initialize(device))
        return false;

    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication->AcquireNextFrame(0, &frameInfo, &resource);
    if (FAILED(hr))
    {
        if (hr == DXGI_ERROR_ACCESS_LOST)
            duplication.Reset();
        return false;
    }

    bool copied = false;
    ComPtr<ID3D11Texture2D> desktopImage;
    if (resource && SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&desktopImage))))
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desktopImage->GetDesc(&desc);

        if (!texture)
        {
            D3D11_TEXTURE2D_DESC copyDesc = desc;
            copyDesc.MiscFlags = 0;
            copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            copyDesc.Usage = D3D11_USAGE_DEFAULT;
            copyDesc.CPUAccessFlags = 0;
            CheckCaptureHr(device->CreateTexture2D(&copyDesc, nullptr, &texture));
            CheckCaptureHr(device->CreateShaderResourceView(texture.Get(), nullptr, &shaderResourceView));
            aspectRatio = static_cast<float>(desc.Width) / static_cast<float>(desc.Height);
        }

        context->CopyResource(texture.Get(), desktopImage.Get());
        copied = true;
    }

    duplication->ReleaseFrame();
    return copied;
}

void DesktopCapture::Reset(ID3D11DeviceContext* context)
{
    if (context)
    {
        ID3D11ShaderResourceView* nullView = nullptr;
        context->PSSetShaderResources(0, 1, &nullView);
    }

    duplication.Reset();
    shaderResourceView.Reset();
    texture.Reset();
    aspectRatio = 0.0f;
}
