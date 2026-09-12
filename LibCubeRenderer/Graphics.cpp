#include "pch.h"
#include "include/Graphics.h"
//#include "Camera.h"

namespace CubeRenderer {

	//Graphics::Graphics() 

	Scene* Graphics::Init(HWND window) {

		Scene* scene = CreateScene();

		CreateDeviceAndSwapChain(window);

		InitializeBlendState();

		//CreateRenderTarget();

		if (window) Resize(window);
		else Resize(100, 100);

		CreatePixelShader();
		CreateVertexShader();

		CreateInputLayout();

		context->PSSetSamplers(0, 1, sampler.GetAddressOf());

		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		return scene;
	}

	void Graphics::CreateDeviceAndSwapChain(HWND window) {
		D3D_DRIVER_TYPE driverTypes[] = {
			D3D_DRIVER_TYPE_HARDWARE,
			D3D_DRIVER_TYPE_WARP,
			D3D_DRIVER_TYPE_REFERENCE
		};

		unique_ptr<runtime_error> error;
		for (UINT i = 0; i < ARRAYSIZE(driverTypes); i++)
			try {
				CreateDeviceAndSwapChain(driverTypes[i], window);
				return;
			}
			catch (const runtime_error& e) {
				OutputDebugStringA(e.what());
				error = make_unique<runtime_error>(e);
			}
		throw runtime_error(error ? error->what() : "Failed to create device and swap chain");
	}

	void Graphics::CreateDevice() {
		D3D_DRIVER_TYPE driverTypes[] = {
			D3D_DRIVER_TYPE_HARDWARE,
			D3D_DRIVER_TYPE_WARP,
			D3D_DRIVER_TYPE_REFERENCE
		};

		unique_ptr<runtime_error> error;
		for (UINT i = 0; i < ARRAYSIZE(driverTypes); i++)
			try {
				CreateDevice(driverTypes[i]);
				return;
			}
			catch (const runtime_error& e) {
				OutputDebugStringA(e.what());
				error = make_unique<runtime_error>(e);
			}
		throw runtime_error(error ? error->what() : "Failed to create device");
	}

	void Graphics::CreateDevice(D3D_DRIVER_TYPE driverType) {
		UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

#ifdef _DEBUG
		creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

		D3D_FEATURE_LEVEL featureLevels[] = {
			D3D_FEATURE_LEVEL_12_2,
			D3D_FEATURE_LEVEL_12_1,
			D3D_FEATURE_LEVEL_12_0,
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0,
			D3D_FEATURE_LEVEL_10_1,
			D3D_FEATURE_LEVEL_10_0,
			D3D_FEATURE_LEVEL_9_3,
			D3D_FEATURE_LEVEL_9_2,
			D3D_FEATURE_LEVEL_9_1
		};

		D3D_FEATURE_LEVEL selectedFeatureLevel;
		ThrowIfFailed(D3D11CreateDevice(nullptr, driverType, 0, creationFlags, featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, &device, &selectedFeatureLevel, &context));

		ThrowIfFailed(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)));

#ifdef _DEBUG
		ComPtr<ID3D11Debug> debug;
		device.As<ID3D11Debug>(&debug);
		ComPtr<ID3D11InfoQueue> infoQueue;
		debug.As<ID3D11InfoQueue>(&infoQueue);

		if (IsDebuggerPresent()) {
			infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, true);
			infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, true);
		}

		D3D11_MESSAGE_ID hide[] = { D3D11_MESSAGE_ID_DEVICE_DRAW_RENDERTARGETVIEW_NOT_SET };
		D3D11_INFO_QUEUE_FILTER filter = {};
		filter.DenyList.NumIDs = _countof(hide);
		filter.DenyList.pIDList = hide;
		infoQueue->AddStorageFilterEntries(&filter);
#endif
	}

	void Graphics::CreateSwapChain(HWND window) {

		ComPtr<IDXGIAdapter1> dxgiAdapter;
		dxgiDevice->GetParent(IID_PPV_ARGS(&dxgiAdapter));

		ComPtr<IDXGIFactory2> dxgiFactory2;
		dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory2));

		if (swapChain)
		{
			swapChain->Release();
			swapChain = nullptr;
		}

		DXGI_SWAP_CHAIN_DESC1 sd = {};
		sd.Width = 1000;
		sd.Height = 1000;
		sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.Stereo = FALSE;
		sd.SampleDesc.Count = 1;
		sd.SampleDesc.Quality = 0;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
		sd.BufferCount = 2;
		sd.Scaling = DXGI_SCALING_STRETCH;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
		sd.Flags = 0;

		if (!window) {
			ThrowIfFailed(dxgiFactory2->CreateSwapChainForComposition(device.Get(), &sd, nullptr, &swapChain));
		}
		else {
			sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
			ThrowIfFailed(dxgiFactory2->CreateSwapChainForHwnd(device.Get(), window, &sd, nullptr, nullptr, &swapChain));
		}
	}

	

	void Graphics::CreateDeviceAndSwapChain(D3D_DRIVER_TYPE driverType, HWND window) {
		CreateDevice(driverType);
		CreateSwapChain(window);
	}

	void Graphics::CreateRenderTarget(UINT sampleCount, UINT sampleQuality)
	{

		sampleCount = max(min(sampleCount, 8), 1);


		if (sampleCount > 1)
		{
			if (sampleQuality == 0)
				sampleQuality = sampleCount - 1;
			UINT maxQuality = 0;
			device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, sampleCount, &maxQuality);
			if (maxQuality != 0) {
				if (sampleQuality >= maxQuality)
					sampleQuality = maxQuality - 1;
			} else sampleQuality = 0, sampleCount = 1;
		}

		if (renderTargetView) {
			renderTargetView.Reset();
		}

		sampleDesc.Count = sampleCount;
		sampleDesc.Quality = sampleQuality;

		antiAliasing = sampleDesc.Count > 1;

		ID3D11Texture2D* backBuffer;

		if (antiAliasing || superSampling) {
			DXGI_SWAP_CHAIN_DESC swapChainDesc;
			swapChain->GetDesc(&swapChainDesc);

			UINT width = swapChainDesc.BufferDesc.Width;
			UINT height = swapChainDesc.BufferDesc.Height;

			D3D11_TEXTURE2D_DESC texDesc = {};
			texDesc.Width = width;
			texDesc.Height = height;
			texDesc.MipLevels = 1;
			texDesc.ArraySize = 1;
			texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			texDesc.SampleDesc = sampleDesc;
			texDesc.Usage = D3D11_USAGE_DEFAULT;
			texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

			if (superSampling) {
				texDesc.Width *= scalingFactor;
				texDesc.Height *= scalingFactor;
			}

			ThrowIfFailed(device->CreateTexture2D(&texDesc, NULL, &backBuffer));

			if (superSampling) {
				texDesc.Width = width;
				texDesc.Height = height;
				texDesc.SampleDesc.Count = 1;
				texDesc.SampleDesc.Quality = 0;
				texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

				ID3D11Texture2D* pResolveTexture = nullptr;
				device->CreateTexture2D(&texDesc, nullptr, &pResolveTexture);
			}

			if (msaaTexture) {
				msaaTexture->Release();
				msaaTexture = nullptr;
			}
			msaaTexture = backBuffer;
		} else ThrowIfFailed(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));


		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		rtvDesc.ViewDimension = antiAliasing ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;

		ThrowIfFailed(device->CreateRenderTargetView(backBuffer, &rtvDesc, &renderTargetView));

		if (!antiAliasing) backBuffer->Release();
		

		D3D11_BUFFER_DESC cbd = {};
		cbd.ByteWidth = sizeof(ConstantBuffer);
		cbd.Usage = D3D11_USAGE_DYNAMIC;
		cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		ThrowIfFailed(device->CreateBuffer(&cbd, NULL, &constantBuffer));

		context->VSSetConstantBuffers(0, 1, constantBuffer.GetAddressOf());


		D3D11_SAMPLER_DESC sampDesc = {};
		sampDesc.Filter = D3D11_FILTER::D3D11_FILTER_MIN_MAG_MIP_POINT;
		//sampDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sampDesc.MinLOD = 0;
		sampDesc.MaxLOD = 0/*D3D11_FLOAT32_MAX*/;

		ThrowIfFailed(device->CreateSamplerState(&sampDesc, &sampler));
		context->PSSetSamplers(0, 1, sampler.GetAddressOf());

		D3D11_RASTERIZER_DESC rasterDesc = {};
		rasterDesc.FillMode = D3D11_FILL_SOLID;
		rasterDesc.CullMode = D3D11_CULL_BACK;
		rasterDesc.FrontCounterClockwise = false;
		rasterDesc.DepthBias = 0;
		rasterDesc.SlopeScaledDepthBias = 0.0f;
		rasterDesc.DepthBiasClamp = 0.0f;
		rasterDesc.MultisampleEnable = false;
		rasterDesc.AntialiasedLineEnable = true;

		ID3D11RasterizerState* pRasterizerState = nullptr;
		ThrowIfFailed(device->CreateRasterizerState(&rasterDesc, &pRasterizerState));

		context->RSSetState(pRasterizerState);
	}

	void Graphics::CreateRenderTexture(UINT width, UINT height) {
		D3D11_TEXTURE2D_DESC textureDesc;
		ZeroMemory(&textureDesc, sizeof(textureDesc));
		textureDesc.Width = width;
		textureDesc.Height = height;
		textureDesc.MipLevels = 1;
		textureDesc.ArraySize = 1;
		textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.Usage = D3D11_USAGE_DEFAULT;
		textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		textureDesc.CPUAccessFlags = 0;
		textureDesc.MiscFlags = 0;

		ThrowIfFailed(device->CreateTexture2D(&textureDesc, nullptr, renderTexture.GetAddressOf()));
		ThrowIfFailed(device->CreateRenderTargetView(renderTexture.Get(), nullptr, textureRTV.GetAddressOf()));
		ThrowIfFailed(device->CreateShaderResourceView(renderTexture.Get(), nullptr, textureSRV.GetAddressOf()));
	}

	void Graphics::CreateDepthStencil() {

		if (depthStencilBuffer) {
			depthStencilBuffer.Reset();
			depthStencilView.Reset();
			depthStencilState.Reset();
		}

		DXGI_SWAP_CHAIN_DESC swapChainDesc;
		swapChain->GetDesc(&swapChainDesc);

		D3D11_TEXTURE2D_DESC depthDesc = {};

		depthDesc.Width = swapChainDesc.BufferDesc.Width;
		depthDesc.Height = swapChainDesc.BufferDesc.Height;

		depthDesc.MipLevels = 1;
		depthDesc.ArraySize = 1;
		depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		depthDesc.SampleDesc = sampleDesc;
		depthDesc.Usage = D3D11_USAGE_DEFAULT;
		depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

		ThrowIfFailed(device->CreateTexture2D(&depthDesc, nullptr, &depthStencilBuffer));

		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
		dsvDesc.Format = depthDesc.Format;
		dsvDesc.ViewDimension = antiAliasing ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
		dsvDesc.Texture2D.MipSlice = 0;

		ThrowIfFailed(device->CreateDepthStencilView(depthStencilBuffer.Get(), &dsvDesc, &depthStencilView));

		D3D11_DEPTH_STENCIL_DESC dsDesc = {};
		dsDesc.DepthEnable = TRUE;
		dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dsDesc.DepthFunc = D3D11_COMPARISON_LESS;

		ThrowIfFailed(device->CreateDepthStencilState(&dsDesc, &depthStencilState));
	}

	void Graphics::CreateInputLayout() {

		D3D11_INPUT_ELEMENT_DESC layout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXTURECOORDINATE", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, textureCoordinate), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};

		ComPtr<ID3D11InputLayout> pInputLayout;

		ThrowIfFailed(device->CreateInputLayout(layout, std::size(layout), vertexShaderBlob->GetBufferPointer(), vertexShaderBlob->GetBufferSize(), &pInputLayout));

		context->IASetInputLayout(pInputLayout.Get());
	}

	void Graphics::CreateVertexShader() {

		std::wstring fullPath = GetExecutableDirectory() / L"VertexShader.cso";
		ThrowIfFailed(D3DReadFileToBlob(fullPath.c_str(), &vertexShaderBlob));
		ThrowIfFailed(device->CreateVertexShader(vertexShaderBlob->GetBufferPointer(), vertexShaderBlob->GetBufferSize(), nullptr, &vertexShader));
		context->VSSetShader(vertexShader.Get(), nullptr, 0);
	}

	void Graphics::CreatePixelShader() {

		std::wstring fullPath = GetExecutableDirectory() / L"PixelShader.cso";
		ThrowIfFailed(D3DReadFileToBlob(fullPath.c_str(), &pixelShaderBlob));
		ThrowIfFailed(device->CreatePixelShader(pixelShaderBlob->GetBufferPointer(), pixelShaderBlob->GetBufferSize(), nullptr, &pixelShader));
		context->PSSetShader(pixelShader.Get(), nullptr, 0);
	}

	

	void Graphics::SetScene(std::unique_ptr<Scene> newScene) {
		scene = std::move(newScene);
		UpdateScene();
	}

	void Graphics::UpdateScene() {
		if (scene) {
			auto vertices = scene->GetVertices();
			auto verticesSize = scene->GetVerticesSize();
			auto indices = scene->GetIndices();
			auto indicesSize = scene->GetIndicesSize();
			SetVertexBuffer(vertices, verticesSize);
			SetIndexBuffer(indices, indicesSize);
			indexCount = scene->GetIndexCount();

			Texture* texture = scene->GetTexture();
			if (texture) {
				ThrowIfFailed(device->CreateShaderResourceView(texture->GetResource(), nullptr, textureView.GetAddressOf()));
				context->PSSetShaderResources(0, 1, textureView.GetAddressOf());
			}
		}
	}

	Scene* Graphics::CreateScene() {
		scene = make_unique<Scene>();
		return GetScene();
	}

	void Graphics::SetVertexBuffer(Vertex* vertices, size_t size) {
		vertexBuffer = nullptr;
		D3D11_BUFFER_DESC bufferDescription = {};
		D3D11_SUBRESOURCE_DATA subresourceData = {};

		bufferDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		bufferDescription.Usage = D3D11_USAGE_DEFAULT;
		bufferDescription.CPUAccessFlags = 0;
		bufferDescription.MiscFlags = 0u;
		bufferDescription.ByteWidth = size;
		bufferDescription.StructureByteStride = sizeof(Vertex);
		subresourceData.pSysMem = vertices;
		ThrowIfFailed(device->CreateBuffer(&bufferDescription, &subresourceData, &vertexBuffer));

		const UINT stride = sizeof(Vertex);
		const UINT offset = 0;

		context->IASetVertexBuffers(0, 1, vertexBuffer.GetAddressOf(), &stride, &offset);
	}

	void Graphics::SetIndexBuffer(unsigned short* indices, size_t size) {
		indexBuffer = nullptr;

		D3D11_BUFFER_DESC indexBufferDescription = {};
		indexBufferDescription.Usage = D3D11_USAGE_DEFAULT;
		indexBufferDescription.ByteWidth = size;
		indexBufferDescription.BindFlags = D3D11_BIND_INDEX_BUFFER;
		indexBufferDescription.CPUAccessFlags = 0;
		indexBufferDescription.MiscFlags = 0;

		D3D11_SUBRESOURCE_DATA indexSubresourceData = {};
		indexSubresourceData.pSysMem = indices;

		ThrowIfFailed(device->CreateBuffer(&indexBufferDescription, &indexSubresourceData, &indexBuffer));
		context->IASetIndexBuffer(indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
	}

	void Graphics::Resize(HWND window) {
		RECT rect;
		GetClientRect(window, &rect);

		Resize(rect.right - rect.left, rect.bottom - rect.top);
	}

	void Graphics::Resize(UINT width, UINT height) {
		//RenderTargetView.c(); reset this one
		if (width == 0 || height == 0) return;

		context->OMSetRenderTargets(0, NULL, NULL);
		renderTargetView.Reset();
		depthStencilView.Reset();
		
		if (backBuffer) {
			backBuffer->Release();
			backBuffer = nullptr;
		}
		if (dxgiBackBuffer) {
			dxgiBackBuffer->Release();
			dxgiBackBuffer = nullptr;
		}

		ThrowIfFailed(swapChain->ResizeBuffers(2, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0));

		CreateRenderTarget();
		CreateDepthStencil();
		
		swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));

		backBuffer->QueryInterface(IID_PPV_ARGS(&dxgiBackBuffer));

		UpdateViewport(width, height);
	}

	void Graphics::UpdateViewport(FLOAT width, FLOAT height) {

		if (width == 0 || height == 0) return;

		FLOAT aspectRatio = width / height;

		D3D11_VIEWPORT viewport;
		viewport.TopLeftX = 0;
		viewport.TopLeftY = 0;
		viewport.Width = width;
		viewport.Height = height;
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		context->RSSetViewports(1, &viewport);

		projectionMatrix = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspectRatio, 0.1f, 500.0f);

		viewMatrix = XMMatrixTranslation(0.0f, 0.0f, 100.0f);
	}

	void Graphics::Clear() {
		const float color[] = { 1.0f, 1.0f, 1.0f, 0.0f };
		context->ClearRenderTargetView(renderTargetView.Get(), color);
		if (depthStencilView)
			context->ClearDepthStencilView(depthStencilView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
	}

	void Graphics::Render(float angle, float x, float y, float z) {

		Camera cam(0, 0, 0, x, y, z);
		Render(&cam);
	}

	void Graphics::Render(Camera *camera) {

		Clear();

		if (!camera) camera = this->camera.Get();

		context->OMSetRenderTargets(1, renderTargetView.GetAddressOf(), depthStencilView.Get());
		
		const ConstantBuffer cb = { {
			XMMatrixTranspose(
				XMMatrixRotationY(camera->rotation.x) *
				XMMatrixRotationX(camera->rotation.y) *
				XMMatrixRotationZ(camera->rotation.z) *
				XMMatrixTranslation(camera->position.x, camera->position.y, camera->position.z) *
				viewMatrix * projectionMatrix
			)
		} };

		D3D11_MAPPED_SUBRESOURCE mappedResource;
		context->Map(constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
		memcpy(mappedResource.pData, &cb, sizeof(cb));
		context->Unmap(constantBuffer.Get(), 0);


		context->DrawIndexed(indexCount, 0, 0);

		if (antiAliasing) {

			ID3D11Texture2D* backBuffer = nullptr;
			swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));

			context->ResolveSubresource(backBuffer, 0, msaaTexture.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);

			backBuffer->Release();
		}

		Present();
	}

	void Graphics::Present() {
		ThrowIfFailed(swapChain->Present(1, 0));
	}

	Scene* Graphics::GetScene() {
		return scene.get();
	}

	void Graphics::InitializeBlendState() {
		D3D11_BLEND_DESC blendDesc = {};
		blendDesc.RenderTarget[0].BlendEnable = FALSE;
		blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
		blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

		ID3D11BlendState* blendState = nullptr;
		ThrowIfFailed(device->CreateBlendState(&blendDesc, &blendState));

		float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

		context->OMSetBlendState(blendState, blendFactor, 0xFFFFFFFF);
	}

	IDXGISwapChain* Graphics::GetSwapChain() {
		return swapChain.Get();
	}

	ID3D11Device* Graphics::GetDevice() {
		return device.Get();
	}

	ID3D11DeviceContext* Graphics::GetContext() {
		return context.Get();
	}

	int prevX, prevY;
	bool isMouseDown = false;

	void Graphics::MouseDown(int x, int y) {

		prevX = x;
		prevY = y;
		isMouseDown = true;
	}

	void Graphics::MouseUp() {

		isMouseDown = false;
	}

	void Graphics::MouseMove(int x, int y) {

		// TODO: Move player head
		if (!isMouseDown) return;

		int deltaX = prevX - x;
		int deltaY = prevY - y;

		camera->rotation.x += deltaX;

		prevX = x, prevY = y;
	}

	Texture* Graphics::CreateTexture(const path& filename) {
		Gdiplus::GdiplusStartupInput gdiplusStartupInput;
		ULONG_PTR gdiplusToken;
		Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

		std::unique_ptr<Gdiplus::Bitmap> pBitmap(Gdiplus::Bitmap::FromFile(filename.c_str()));
		if (!pBitmap || pBitmap->GetLastStatus() != Gdiplus::Ok) {
			Gdiplus::GdiplusShutdown(gdiplusToken);
			ThrowIfFailed(E_FAIL);
		}

		UINT width = pBitmap->GetWidth();
		UINT height = pBitmap->GetHeight();

		std::unique_ptr<BYTE[]> pPixels(new BYTE[(size_t)width * height * 4]);

		Gdiplus::BitmapData bitmapData;
		Gdiplus::Rect rect(0, 0, width, height);
		pBitmap->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bitmapData);
		memcpy(pPixels.get(), bitmapData.Scan0, (size_t)width * height * 4);
		pBitmap->UnlockBits(&bitmapData);

		Gdiplus::GdiplusShutdown(gdiplusToken);

		D3D11_TEXTURE2D_DESC textureDesc = {};
		textureDesc.Width = width;
		textureDesc.Height = height;
		textureDesc.MipLevels = 1;
		textureDesc.ArraySize = 1;
		textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.Usage = D3D11_USAGE_DEFAULT;
		textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		textureDesc.CPUAccessFlags = 0;

		D3D11_SUBRESOURCE_DATA subresourceData = {};
		subresourceData.pSysMem = pPixels.get();
		subresourceData.SysMemPitch = width * 4;
		subresourceData.SysMemSlicePitch = 0;

		ID3D11Texture2D* texture = nullptr;
		ThrowIfFailed(device->CreateTexture2D(&textureDesc, &subresourceData, &texture));

		return new Texture(texture);
	}

}