#include "pch.h"
#include "DynamicIconRenderer.h"
#include <wil/resource.h>

namespace
{
    HICON RenderSpeaker(wchar_t glyph, int size, bool light)
    {
        wil::unique_hdc dc(CreateCompatibleDC(nullptr));
        wil::unique_hfont font(CreateFontW(-MulDiv(size, 16, 20), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe Fluent Icons"));
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
        void* data{};
        wil::unique_hbitmap bitmap(CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &data, nullptr, 0));
        if (!dc || !font || !bitmap || !data) return nullptr;

        auto const oldBitmap = SelectObject(dc.get(), bitmap.get());
        auto const oldFont = SelectObject(dc.get(), font.get());
        PatBlt(dc.get(), 0, 0, size, size, BLACKNESS);
        SetBkMode(dc.get(), TRANSPARENT);
        SetTextColor(dc.get(), RGB(255, 255, 255));
        WORD index{};
        GetGlyphIndicesW(dc.get(), &glyph, 1, &index, GGI_MARK_NONEXISTING_GLYPHS);
        if (index == 0xffff) glyph = L'\uE767';
        RECT rect{0, 0, size, size};
        DrawTextW(dc.get(), &glyph, 1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();
        SelectObject(dc.get(), oldFont);
        SelectObject(dc.get(), oldBitmap);

        auto pixels = static_cast<uint32_t*>(data);
        bool hasInk{};
        auto const stride = (size + 15) / 16 * 2;
        std::vector<BYTE> maskBits(static_cast<size_t>(size) * stride, 0);
        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                auto& pixel = pixels[y * size + x];
                auto const alpha = pixel & 0xffu;
                hasInk |= alpha != 0;
                auto const shade = light ? alpha * 28 / 255 : alpha;
                pixel = (alpha << 24) | (shade * 0x010101u);
                if (alpha == 0) maskBits[y * stride + x / 8] |= static_cast<BYTE>(0x80u >> (x % 8));
            }
        }
        if (!hasInk) return nullptr;
        wil::unique_hbitmap mask(CreateBitmap(size, size, 1, 1, maskBits.data()));
        if (!mask) return nullptr;
        ICONINFO icon{TRUE, 0, 0, mask.get(), bitmap.get()};
        return CreateIconIndirect(&icon);
    }
}

DynamicIconRenderer::~DynamicIconRenderer()
{
    for (auto const& [key, icon] : m_cache)
    {
        DestroyIcon(icon);
    }
}

HICON DynamicIconRenderer::Get(HICON baseIcon, unsigned volume, bool muted, int size, bool light)
{
    if (size <= 0 || size > 256) return baseIcon;
    wchar_t const glyph = SpeakerGlyph(volume, muted);
    CacheKey const key{glyph, size, light};
    if (auto const found = m_cache.find(key); found != m_cache.end()) return found->second;
    if (auto const icon = RenderSpeaker(glyph, size, light))
    {
        m_cache.emplace(key, icon);
        return icon;
    }
    return baseIcon;
}

