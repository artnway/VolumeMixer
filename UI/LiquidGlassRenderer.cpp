#include "pch.h"
#include "LiquidGlassRenderer.h"

#include <d3dcompiler.h>
#include <windows.ui.composition.interop.h>
#include <wil/resource.h>
#include <array>
#include <cmath>
#include <cstring>
#include <string_view>

using namespace winrt;
namespace Wuc = winrt::Windows::UI::Composition;

namespace
{
    constexpr auto Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    constexpr int GlassShaderResource = 501;
    constexpr int HorizontalBlurResource = 502;
    constexpr int VerticalBlurResource = 503;

    struct GlassParameters
    {
        float refThickness = 22, refFactor = 1.4f, dispersion = 0.45f, fresnelRange = 37;
        float fresnelHardness = 0, fresnelFactor = 38, glareRange = 28, glareHardness = 10;
        float glareFactor = 23, glareConvergence = 65, oppositeGlare = 17, glareAngle = -135;
        float blur = 0, exposure = 1, tintR = 255, tintG = 255;
        float tintB = 255, tintAlpha = 0, magnification = 1, shadowFactor = 0;
        float shadowX = 0, shadowY = 0, viewportLeft = 0, thumbCoreOpacity = 0;
        float shapeRadius = 0, roundness = 3.4f, merge = 0, showShape = 0;
        float spring = 0, dispersionRange = 0.75f, step = 0, dpiScale = 1;
    };
    static_assert(sizeof(GlassParameters) == 128);

    struct BlurParameters
    {
        float amount{};
        float padding[3]{};
        float centerWeight{};
        float padding2[3]{};
        float pairs[20]{};
    };
    static_assert(sizeof(BlurParameters) == 112);

    BlurParameters GaussianKernel()
    {
        // Keep the same adjacent-tap merging and 20 sample radius as LiquidGlassWinUI.
        constexpr int radius = 20;
        constexpr double sigma = radius / 3.0;
        std::array<double, radius + 1> weights{};
        double total = 1;
        weights[0] = 1;
        for (int i = 1; i <= radius; ++i)
        {
            weights[i] = std::exp(-(i * i) / (2 * sigma * sigma));
            total += 2 * weights[i];
        }
        BlurParameters result{};
        result.centerWeight = static_cast<float>(1 / total);
        for (int i = 1; i < radius; i += 2)
        {
            const auto sum = weights[i] + weights[i + 1];
            result.pairs[i - 1] = static_cast<float>((i * weights[i] + (i + 1) * weights[i + 1]) / sum);
            result.pairs[i] = static_cast<float>(sum / total);
        }
        return result;
    }

    com_ptr<ID3DBlob> Compile(std::string_view source, char const* profile)
    {
        com_ptr<ID3DBlob> compiled;
        com_ptr<ID3DBlob> diagnostics;
        auto const status = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr,
            "Main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL2, 0, compiled.put(), diagnostics.put());
        if (FAILED(status) && diagnostics)
        {
            OutputDebugStringA(static_cast<char const*>(diagnostics->GetBufferPointer()));
        }
        check_hresult(status);
        return compiled;
    }

    com_ptr<ID3DBlob> CompileEffect(int resourceId, std::string_view wrapper)
    {
        auto const module = GetModuleHandleW(nullptr);
        auto const resource = FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
        if (!resource) throw_last_error();
        auto const loaded = LoadResource(module, resource);
        auto const bytes = static_cast<char const*>(LockResource(loaded));
        if (!bytes) throw_hresult(E_FAIL);

        // The originals export DWM shader-library functions. This host calls their bodies as pixel shaders.
        std::string source = "#define export\n";
        source.append(bytes, SizeofResource(module, resource));
        source.append(wrapper);
        return Compile(source, "ps_5_0");
    }

    struct ShaderBytecode
    {
        com_ptr<ID3DBlob> vertex = Compile(R"(
float4 Main(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
})", "vs_5_0");
        com_ptr<ID3DBlob> glass = CompileEffect(GlassShaderResource, R"(
float4 Main(float4 position : SV_Position) : SV_Target
{
    uint w, h; texture0.GetDimensions(w, h);
    float2 uv = (position.xy - float2(ShapeWidth, 0)) / float2(w, h);
    float4 glass = LiquidGlassBody(uv, float4(w, h, 1.0 / w, 1.0 / h), float4(0, 0, 1, 1));
    // ShapeHeight is an unused upstream slot. Only the thumb pass sets its core opacity.
    if (ShapeHeight > 0)
    {
        float radial = length(uv - 0.5) * 2;
        float rim = smoothstep(0.38, 0.9, radial);
        glass *= lerp(ShapeHeight, 0.96, rim);
    }
    return glass;
})");
        static com_ptr<ID3DBlob> CompileBlur(int id)
        {
            return CompileEffect(id, R"(
float4 Main(float4 position : SV_Position) : SV_Target
{
    uint w, h; texture0.GetDimensions(w, h);
    float2 uv = position.xy / float2(w, h);
    return BlurCore(uv, float4(w, h, 1.0 / w, 1.0 / h));
})");
        }
        com_ptr<ID3DBlob> horizontal = CompileBlur(HorizontalBlurResource);
        com_ptr<ID3DBlob> vertical = CompileBlur(VerticalBlurResource);
    };

    D3D11_TEXTURE2D_DESC TextureDescription(UINT width, UINT height, UINT bindFlags)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = Format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = bindFlags;
        return desc;
    }
}

LiquidGlassRenderer::LiquidGlassRenderer(HWND window) : m_monitor(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST))
{
    check_hresult(CreateDXGIFactory1(__uuidof(IDXGIFactory2), m_factory.put_void()));
    com_ptr<IDXGIAdapter1> matchingAdapter;
    com_ptr<IDXGIOutput1> matchingOutput;
    for (UINT adapterIndex = 0; !matchingOutput; ++adapterIndex)
    {
        com_ptr<IDXGIAdapter1> adapter;
        auto const status = m_factory->EnumAdapters1(adapterIndex, adapter.put());
        if (status == DXGI_ERROR_NOT_FOUND) break;
        check_hresult(status);
        for (UINT outputIndex = 0; ; ++outputIndex)
        {
            com_ptr<IDXGIOutput> output;
            auto const outputStatus = adapter->EnumOutputs(outputIndex, output.put());
            if (outputStatus == DXGI_ERROR_NOT_FOUND) break;
            check_hresult(outputStatus);
            DXGI_OUTPUT_DESC description{};
            if (SUCCEEDED(output->GetDesc(&description)) && description.Monitor == m_monitor
                && description.Rotation == DXGI_MODE_ROTATION_IDENTITY)
            {
                matchingAdapter = adapter;
                matchingOutput = output.as<IDXGIOutput1>();
                m_desktopBounds = description.DesktopCoordinates;
                break;
            }
        }
    }
    if (!matchingOutput) throw_hresult(DXGI_ERROR_UNSUPPORTED);

    check_hresult(D3D11CreateDevice(matchingAdapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        m_device.put(), nullptr, m_context.put()));
    check_hresult(matchingOutput->DuplicateOutput(m_device.get(), m_capture.put()));

    static ShaderBytecode const shaders;
    check_hresult(m_device->CreateVertexShader(shaders.vertex->GetBufferPointer(),
        shaders.vertex->GetBufferSize(), nullptr, m_vertex.put()));
    auto const createPixel = [&](com_ptr<ID3DBlob> const& code, com_ptr<ID3D11PixelShader>& shader)
    {
        check_hresult(m_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader.put()));
    };
    createPixel(shaders.glass, m_glass);
    createPixel(shaders.horizontal, m_blurHorizontal);
    createPixel(shaders.vertical, m_blurVertical);

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    check_hresult(m_device->CreateSamplerState(&sampler, m_sampler.put()));

    D3D11_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    com_ptr<ID3D11RasterizerState> rasterizer;
    check_hresult(m_device->CreateRasterizerState(&rasterizerDesc, rasterizer.put()));
    m_context->RSSetState(rasterizer.get());

    D3D11_BUFFER_DESC constants{};
    constants.Usage = D3D11_USAGE_DEFAULT;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constants.ByteWidth = sizeof(GlassParameters);
    check_hresult(m_device->CreateBuffer(&constants, nullptr, m_glassConstants.put()));
    constants.ByteWidth = sizeof(BlurParameters);
    check_hresult(m_device->CreateBuffer(&constants, nullptr, m_blurConstants.put()));

    auto* samplerState = m_sampler.get();
    m_context->PSSetSamplers(0, 1, &samplerState);
    m_context->VSSetShader(m_vertex.get(), nullptr, 0);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void LiquidGlassRenderer::UpdateDesktop(RECT const& sampleBounds)
{
    DXGI_OUTDUPL_FRAME_INFO info{};
    com_ptr<IDXGIResource> frame;
    auto const status = m_capture->AcquireNextFrame(0, &info, frame.put());
    if (status == DXGI_ERROR_WAIT_TIMEOUT) return;
    check_hresult(status);
    auto const release = wil::scope_exit([&] { m_capture->ReleaseFrame(); });
    if (m_desktop && info.LastPresentTime.QuadPart == 0) return;
    auto const source = frame.as<ID3D11Texture2D>();
    RECT const desktop{0, 0, m_desktopBounds.right - m_desktopBounds.left, m_desktopBounds.bottom - m_desktopBounds.top};
    auto const copyWholeFrame = [&]
    {
        m_context->CopyResource(m_desktop.get(), source.get());
        ++m_desktopFrame;
    };
    if (!m_desktop)
    {
        D3D11_TEXTURE2D_DESC original{};
        source->GetDesc(&original);
        auto const desc = TextureDescription(original.Width, original.Height, 0);
        check_hresult(m_device->CreateTexture2D(&desc, nullptr, m_desktop.put()));
        copyWholeFrame();
        return;
    }

    if (!info.TotalMetadataBufferSize)
    {
        copyWholeFrame();
        return;
    }

    m_moves.resize((info.TotalMetadataBufferSize + sizeof(DXGI_OUTDUPL_MOVE_RECT) - 1) / sizeof(DXGI_OUTDUPL_MOVE_RECT));
    m_dirty.resize((info.TotalMetadataBufferSize + sizeof(RECT) - 1) / sizeof(RECT));
    UINT moveBytes{}, dirtyBytes{};
    auto const movesStatus = m_capture->GetFrameMoveRects(info.TotalMetadataBufferSize, m_moves.data(), &moveBytes);
    auto const dirtyStatus = m_capture->GetFrameDirtyRects(info.TotalMetadataBufferSize, m_dirty.data(), &dirtyBytes);
    if (movesStatus == DXGI_ERROR_ACCESS_LOST || dirtyStatus == DXGI_ERROR_ACCESS_LOST)
    {
        check_hresult(DXGI_ERROR_ACCESS_LOST);
    }
    auto const moveCount = moveBytes / sizeof(DXGI_OUTDUPL_MOVE_RECT);
    auto const dirtyCount = dirtyBytes / sizeof(RECT);
    if (FAILED(movesStatus) || FAILED(dirtyStatus) || moveCount + dirtyCount == 0 || moveCount + dirtyCount > 64)
    {
        copyWholeFrame();
        return;
    }
    auto focus = sampleBounds;
    OffsetRect(&focus, -m_desktopBounds.left, -m_desktopBounds.top);
    bool affectsMixer{};
    auto const copyRect = [&](RECT const& rect)
    {
        RECT pixels{};
        if (!IntersectRect(&pixels, &rect, &desktop)) return;
        D3D11_BOX const box{static_cast<UINT>(pixels.left), static_cast<UINT>(pixels.top), 0,
            static_cast<UINT>(pixels.right), static_cast<UINT>(pixels.bottom), 1};
        m_context->CopySubresourceRegion(m_desktop.get(), 0, box.left, box.top, 0, source.get(), 0, &box);
        RECT overlap{};
        affectsMixer |= IntersectRect(&overlap, &pixels, &focus) != FALSE;
    };
    // The acquired texture already contains the final pixels at each move destination.
    for (size_t i = 0; i < moveCount; ++i) copyRect(m_moves[i].DestinationRect);
    for (size_t i = 0; i < dirtyCount; ++i) copyRect(m_dirty[i]);
    if (affectsMixer) ++m_desktopFrame;
}

void LiquidGlassRenderer::Resize(UINT width, UINT height, Wuc::Compositor const& compositor)
{
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_backBuffer = nullptr;
    if (!m_swapChain)
    {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = Format;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        check_hresult(m_factory->CreateSwapChainForComposition(m_device.get(), &desc, nullptr, m_swapChain.put()));
        Wuc::ICompositionSurface surface{nullptr};
        auto const interop = compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>();
        check_hresult(interop->CreateCompositionSurfaceForSwapChain(m_swapChain.get(),
            reinterpret_cast<ABI::Windows::UI::Composition::ICompositionSurface**>(put_abi(surface))));
        m_brush = compositor.CreateSurfaceBrush(surface);
        m_brush.Stretch(Wuc::CompositionStretch::Fill);
    }
    else
    {
        check_hresult(m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0));
    }
    com_ptr<ID3D11Texture2D> buffer;
    check_hresult(m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), buffer.put_void()));
    check_hresult(m_device->CreateRenderTargetView(buffer.get(), nullptr, m_backBuffer.put()));

    m_region = {};
    auto const desc = TextureDescription(width, height, D3D11_BIND_SHADER_RESOURCE);
    check_hresult(m_device->CreateTexture2D(&desc, nullptr, m_region.texture.put()));
    check_hresult(m_device->CreateShaderResourceView(m_region.texture.get(), nullptr, m_region.source.put()));
    m_horizontal = {};
    m_vertical = {};
    m_width = width;
    m_height = height;
    m_presented = false;
    D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    m_context->RSSetViewports(1, &viewport);
}

void LiquidGlassRenderer::Draw(ID3D11PixelShader* shader, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target)
{
    m_context->PSSetShader(shader, nullptr, 0);
    m_context->PSSetShaderResources(0, 1, &source);
    m_context->OMSetRenderTargets(1, &target, nullptr);
    m_context->Draw(3, 0);
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* none = nullptr;
    m_context->PSSetShaderResources(0, 1, &none);
}

Wuc::CompositionSurfaceBrush LiquidGlassRenderer::Render(
    RECT const& bounds, float scale, int frost, Wuc::Compositor const& compositor)
{
    frost = std::clamp(frost, 0, 100);
    RECT covered{};
    if (!IntersectRect(&covered, &bounds, &m_desktopBounds) || !EqualRect(&bounds, &covered)) return nullptr;
    auto const changed = !EqualRect(&bounds, &m_lastBounds) || frost != m_lastFrost || scale != m_lastScale;
    auto sampleBounds = bounds;
    auto const thumbRadius = static_cast<int>(std::ceil(7 * scale));
    InflateRect(&sampleBounds, thumbRadius, thumbRadius);
    UpdateDesktop(sampleBounds);
    if (!m_desktop) return nullptr;
    if (m_desktopFrame == m_presentedFrame && !changed && m_presented) return m_brush;

    const UINT width = bounds.right - bounds.left;
    const UINT height = bounds.bottom - bounds.top;
    if (width != m_width || height != m_height) Resize(width, height, compositor);
    D3D11_BOX region{static_cast<UINT>(bounds.left - m_desktopBounds.left),
        static_cast<UINT>(bounds.top - m_desktopBounds.top), 0,
        static_cast<UINT>(bounds.right - m_desktopBounds.left),
        static_cast<UINT>(bounds.bottom - m_desktopBounds.top), 1};
    m_context->CopySubresourceRegion(m_region.texture.get(), 0, 0, 0, 0, m_desktop.get(), 0, &region);
    auto* input = m_region.source.get();
    if (frost > 0)
    {
        if (!m_horizontal.texture)
        {
            auto const makePass = [&](Surface& surface)
            {
                auto const desc = TextureDescription(width, height, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
                check_hresult(m_device->CreateTexture2D(&desc, nullptr, surface.texture.put()));
                check_hresult(m_device->CreateShaderResourceView(surface.texture.get(), nullptr, surface.source.put()));
                check_hresult(m_device->CreateRenderTargetView(surface.texture.get(), nullptr, surface.target.put()));
            };
            makePass(m_horizontal);
            makePass(m_vertical);
        }
        static BlurParameters const kernel = GaussianKernel();
        auto params = kernel;
        // Full travel uses the former 25% blur amount.
        params.amount = scale * frost / 120.0f;
        m_context->UpdateSubresource(m_blurConstants.get(), 0, nullptr, &params, 0, 0);
        auto* constants = m_blurConstants.get();
        m_context->PSSetConstantBuffers(0, 1, &constants);
        Draw(m_blurHorizontal.get(), input, m_horizontal.target.get());
        Draw(m_blurVertical.get(), m_horizontal.source.get(), m_vertical.target.get());
        input = m_vertical.source.get();
    }
    GlassParameters glass{};
    glass.dpiScale = scale;
    glass.shapeRadius = 36 * scale / std::min(width, height);
    m_context->UpdateSubresource(m_glassConstants.get(), 0, nullptr, &glass, 0, 0);
    auto* constants = m_glassConstants.get();
    m_context->PSSetConstantBuffers(0, 1, &constants);
    Draw(m_glass.get(), input, m_backBuffer.get());
    auto const status = m_swapChain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
    if (status == DXGI_ERROR_WAS_STILL_DRAWING) return m_presented ? m_brush : nullptr;
    check_hresult(status);
    m_lastBounds = bounds;
    m_lastFrost = frost;
    m_lastScale = scale;
    m_presentedFrame = m_desktopFrame;
    m_presented = true;
    return m_brush;
}

LiquidGlassRenderer::ThumbFrame LiquidGlassRenderer::RenderThumbs(std::span<ThumbSample const> samples, UINT diameter, float scale)
{
    ThumbFrame ready;
    if (!m_desktop || samples.empty() || !diameter) return ready;

    auto const count = static_cast<UINT>(samples.size());
    auto const atlasWidth = count * diameter;
    auto const previousCount = m_thumbSamples.size();
    auto const sameSize = diameter == m_thumbDiameter && count == previousCount;
    if (m_thumbPending)
    {
        D3D11_MAPPED_SUBRESOURCE map{};
        auto const result = m_context->Map(m_thumbReadback.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
        if (result == DXGI_ERROR_WAS_STILL_DRAWING) return ready;
        check_hresult(result);
        auto const unmap = wil::scope_exit([&] { m_context->Unmap(m_thumbReadback.get(), 0); });
        if (diameter == m_thumbDiameter)
        {
            // Return the IDs from the submitted frame, even if the visible channels have changed.
            ready.ids.reserve(previousCount);
            ready.pixels.resize(previousCount * diameter * diameter);
            auto const pixels = static_cast<std::uint8_t const*>(map.pData);
            for (size_t index = 0; index < previousCount; ++index)
            {
                ready.ids.push_back(m_thumbSamples[index].id);
                for (UINT y = 0; y < diameter; ++y)
                {
                    std::memcpy(ready.pixels.data() + (index * diameter + y) * diameter,
                        pixels + y * map.RowPitch + index * diameter * sizeof(std::uint32_t),
                        diameter * sizeof(std::uint32_t));
                }
            }
        }
        m_thumbPending = false;
    }
    if (sameSize && m_thumbFrame == m_desktopFrame &&
        std::ranges::equal(samples, m_thumbSamples, [](ThumbSample const& a, ThumbSample const& b)
        {
            return a.id == b.id && a.center.x == b.center.x && a.center.y == b.center.y;
        }))
    {
        return ready;
    }
    if (!sameSize)
    {
        m_thumbRegion = {};
        auto const source = TextureDescription(diameter, diameter, D3D11_BIND_SHADER_RESOURCE);
        check_hresult(m_device->CreateTexture2D(&source, nullptr, m_thumbRegion.texture.put()));
        check_hresult(m_device->CreateShaderResourceView(m_thumbRegion.texture.get(), nullptr, m_thumbRegion.source.put()));
        m_thumbAtlas = {};
        auto atlas = TextureDescription(atlasWidth, diameter, D3D11_BIND_RENDER_TARGET);
        check_hresult(m_device->CreateTexture2D(&atlas, nullptr, m_thumbAtlas.texture.put()));
        check_hresult(m_device->CreateRenderTargetView(m_thumbAtlas.texture.get(), nullptr, m_thumbAtlas.target.put()));
        atlas.Usage = D3D11_USAGE_STAGING;
        atlas.BindFlags = 0;
        atlas.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        m_thumbReadback = nullptr;
        check_hresult(m_device->CreateTexture2D(&atlas, nullptr, m_thumbReadback.put()));
    }

    float const transparent[4]{};
    m_context->ClearRenderTargetView(m_thumbAtlas.target.get(), transparent);
    D3D11_VIEWPORT const backdropViewport{0, 0, static_cast<float>(m_width), static_cast<float>(m_height), 0, 1};
    auto const restoreViewport = wil::scope_exit([&] { m_context->RSSetViewports(1, &backdropViewport); });
    GlassParameters glass{};
    glass.dpiScale = scale;
    glass.shapeRadius = 1;
    glass.roundness = 2;
    glass.refThickness = 3;
    glass.refFactor = 1.65f;
    glass.fresnelFactor = 52;
    glass.glareFactor = 36;
    glass.magnification = 1.25f;
    glass.tintAlpha = 0.03f;
    glass.thumbCoreOpacity = 0.30f;
    auto* constants = m_glassConstants.get();
    m_context->PSSetConstantBuffers(0, 1, &constants);
    auto const side = static_cast<LONG>(diameter);
    for (UINT index = 0; index < count; ++index)
    {
        auto const center = samples[index].center;
        auto const left = center.x - side / 2 - m_desktopBounds.left;
        auto const top = center.y - side / 2 - m_desktopBounds.top;
        if (left < 0 || top < 0 || left + side > m_desktopBounds.right - m_desktopBounds.left ||
            top + side > m_desktopBounds.bottom - m_desktopBounds.top) continue;

        D3D11_BOX const crop{static_cast<UINT>(left), static_cast<UINT>(top), 0,
            static_cast<UINT>(left + side), static_cast<UINT>(top + side), 1};
        m_context->CopySubresourceRegion(m_thumbRegion.texture.get(), 0, 0, 0, 0, m_desktop.get(), 0, &crop);
        glass.viewportLeft = static_cast<float>(index * diameter);
        m_context->UpdateSubresource(m_glassConstants.get(), 0, nullptr, &glass, 0, 0);
        D3D11_VIEWPORT const tile{glass.viewportLeft, 0, static_cast<float>(diameter), static_cast<float>(diameter), 0, 1};
        m_context->RSSetViewports(1, &tile);
        Draw(m_glass.get(), m_thumbRegion.source.get(), m_thumbAtlas.target.get());
    }
    m_context->CopyResource(m_thumbReadback.get(), m_thumbAtlas.texture.get());
    // Submit the atlas even when the unchanged window backdrop does not need a Present.
    m_context->Flush();
    m_thumbPending = true;
    m_thumbDiameter = diameter;
    m_thumbSamples.assign(samples.begin(), samples.end());
    m_thumbFrame = m_desktopFrame;
    return ready;
}
