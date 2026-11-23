#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdint>
#include <span>
#include <vector>
#include <winrt/Windows.UI.Composition.h>

class LiquidGlassRenderer final
{
public:
    explicit LiquidGlassRenderer(HWND window);
    HMONITOR Monitor() const { return m_monitor; }
    winrt::Windows::UI::Composition::CompositionSurfaceBrush Render(
        RECT const& bounds, float scale, int frost,
        winrt::Windows::UI::Composition::Compositor const& compositor);
    struct ThumbSample
    {
        std::uint64_t id;
        POINT center;
    };

    struct ThumbFrame
    {
        std::vector<std::uint64_t> ids;
        std::vector<std::uint32_t> pixels;
    };

    ThumbFrame RenderThumbs(std::span<ThumbSample const> samples, UINT diameter, float scale);

private:
    struct Surface
    {
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<ID3D11ShaderResourceView> source;
        winrt::com_ptr<ID3D11RenderTargetView> target;
    };

    void Resize(UINT width, UINT height, winrt::Windows::UI::Composition::Compositor const& compositor);
    void UpdateDesktop(RECT const& sampleBounds);
    void Draw(ID3D11PixelShader* shader, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target);

    HMONITOR m_monitor{};
    RECT m_desktopBounds{};
    RECT m_lastBounds{};
    int m_lastFrost{-1};
    float m_lastScale{};
    bool m_presented{};
    UINT m_width{};
    UINT m_height{};
    winrt::com_ptr<IDXGIFactory2> m_factory;
    winrt::com_ptr<IDXGIOutputDuplication> m_capture;
    std::vector<DXGI_OUTDUPL_MOVE_RECT> m_moves;
    std::vector<RECT> m_dirty;
    winrt::com_ptr<ID3D11Device> m_device;
    winrt::com_ptr<ID3D11DeviceContext> m_context;
    winrt::com_ptr<ID3D11Texture2D> m_desktop;
    winrt::com_ptr<ID3D11VertexShader> m_vertex;
    winrt::com_ptr<ID3D11PixelShader> m_glass;
    winrt::com_ptr<ID3D11PixelShader> m_blurHorizontal;
    winrt::com_ptr<ID3D11PixelShader> m_blurVertical;
    winrt::com_ptr<ID3D11SamplerState> m_sampler;
    winrt::com_ptr<ID3D11Buffer> m_glassConstants;
    winrt::com_ptr<ID3D11Buffer> m_blurConstants;
    winrt::com_ptr<IDXGISwapChain1> m_swapChain;
    winrt::com_ptr<ID3D11RenderTargetView> m_backBuffer;
    Surface m_region;
    Surface m_horizontal;
    Surface m_vertical;
    Surface m_thumbRegion;
    Surface m_thumbAtlas;
    winrt::com_ptr<ID3D11Texture2D> m_thumbReadback;
    std::vector<ThumbSample> m_thumbSamples;
    UINT m_thumbDiameter{};
    bool m_thumbPending{};
    std::uint64_t m_desktopFrame{};
    std::uint64_t m_presentedFrame{};
    std::uint64_t m_thumbFrame{};
    winrt::Windows::UI::Composition::CompositionSurfaceBrush m_brush{nullptr};
};
