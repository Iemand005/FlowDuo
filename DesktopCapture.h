#pragma once

#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>

class DesktopCapture {
public:
    bool Update(ID3D11Device* device, ID3D11DeviceContext* context);
    void Reset(ID3D11DeviceContext* context);

    ID3D11ShaderResourceView* GetShaderResourceView() const { return shaderResourceView.Get(); }
    float GetAspectRatio() const { return aspectRatio; }

private:
    bool Initialize(ID3D11Device* device);

    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shaderResourceView;
    float aspectRatio = 0.0f;
};
