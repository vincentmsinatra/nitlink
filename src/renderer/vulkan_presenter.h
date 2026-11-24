#pragma once

#include <d3d11.h>
#include <memory>

namespace NitLink {

// Presents the Direct3D video and overlay through a Vulkan swap chain.
class VulkanPresenter {
public:
    VulkanPresenter();
    ~VulkanPresenter();

    bool Initialize(HWND window, const LUID& adapter);
    bool Resize(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format, bool vsync);
    void WaitForImage();
    HRESULT Present(ID3D11DeviceContext* context, ID3D11Texture2D* source);
    void RequestResize();
    bool NeedsResize() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_state;
};

} // namespace NitLink
