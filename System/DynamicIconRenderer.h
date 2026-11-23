#pragma once

#include <windows.h>
#include <map>
#include <tuple>

class DynamicIconRenderer final
{
public:
    DynamicIconRenderer() = default;
    ~DynamicIconRenderer();

    DynamicIconRenderer(DynamicIconRenderer const&) = delete;
    DynamicIconRenderer& operator=(DynamicIconRenderer const&) = delete;

    HICON Get(HICON baseIcon, unsigned volume, bool muted, int size, bool light);

    static constexpr wchar_t SpeakerGlyph(unsigned volume, bool muted)
    {
        constexpr wchar_t levels[]{L'\uE992', L'\uE993', L'\uE994', L'\uE995'};
        unsigned const clamped = volume > 100 ? 100 : volume;
        return muted ? L'\uE74F' : levels[(clamped * 3 + 99) / 100];
    }

private:
    using CacheKey = std::tuple<wchar_t, int, bool>;

    std::map<CacheKey, HICON> m_cache;
};
