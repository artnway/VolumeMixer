#include "pch.h"
#include "AppIconCache.h"
#include <robuffer.h>
#include <shlobj.h>
#include <cstring>
#include <cstdlib>
#include <wil/resource.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

namespace
{
    winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap RenderIcon(HICON icon, int size)
    {
        using winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap;
        if (!icon || size <= 0)
        {
            return nullptr;
        }

        wil::unique_hdc dc(CreateCompatibleDC(nullptr));
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = size;
        info.bmiHeader.biHeight = -size;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels{};
        wil::unique_hbitmap bitmap(CreateDIBSection(dc.get(), &info, DIB_RGB_COLORS, &pixels, nullptr, 0));
        if (!dc || !bitmap || !pixels)
        {
            return nullptr;
        }
        auto* previous = SelectObject(dc.get(), bitmap.get());
        std::memset(pixels, 0, static_cast<size_t>(size) * size * 4);
        auto const drawn = DrawIconEx(dc.get(), 0, 0, icon, size, size, 0, nullptr, DI_NORMAL) != FALSE;

        WriteableBitmap result(size, size);
        if (drawn)
        {
            GdiFlush();
            auto* bytes = static_cast<byte*>(pixels);
            bool hasAlpha = false;
            for (int index = 3; index < size * size * 4; index += 4)
            {
                hasAlpha = hasAlpha || bytes[index] != 0;
            }
            if (!hasAlpha)
            {
                for (int index = 3; index < size * size * 4; index += 4)
                {
                    bytes[index] = 255;
                }
            }
            auto buffer = result.PixelBuffer().as<Windows::Storage::Streams::IBufferByteAccess>();
            byte* destination{};
            winrt::check_hresult(buffer->Buffer(&destination));
            std::memcpy(destination, pixels, static_cast<size_t>(size) * size * 4);
            result.Invalidate();
        }
        SelectObject(dc.get(), previous);
        return drawn ? result : nullptr;
    }
}

winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapSource AppIconCache::Load(
    std::wstring const& executablePath, int size, std::wstring_view aumid)
{
    auto path = executablePath;
    if (path.empty())
    {
        wchar_t systemDirectory[MAX_PATH]{};
        if (!GetSystemDirectoryW(systemDirectory, ARRAYSIZE(systemDirectory)))
        {
            return nullptr;
        }
        path = std::wstring(systemDirectory) + L"\\SndVol.exe";
    }
    auto const key = std::wstring(aumid) + L'|' + path + L'#' + std::to_wstring(size);
    if (auto const cached = m_cache.find(key); cached != m_cache.end())
    {
        return cached->second;
    }
    if (!aumid.empty())
    {
        auto const location = L"shell:AppsFolder\\" + std::wstring(aumid);
        PIDLIST_ABSOLUTE identifier{};
        auto const result = SHParseDisplayName(location.c_str(), nullptr, &identifier, 0, nullptr);
        wil::unique_cotaskmem_ptr<void> item(identifier);
        if (SUCCEEDED(result) && item)
        {
            SHFILEINFOW fileInfo{};
            if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(item.get()), 0, &fileInfo, sizeof(fileInfo),
                    SHGFI_PIDL | SHGFI_ICON | (size <= 16 ? SHGFI_SMALLICON : SHGFI_LARGEICON)))
            {
                wil::unique_hicon icon(fileInfo.hIcon);
                if (auto const bitmap = RenderIcon(icon.get(), size))
                {
                    m_cache.emplace(key, bitmap);
                    return bitmap;
                }
            }
        }
    }

    int iconIndex = 0;
    if (auto const comma = path.rfind(L','); comma != std::wstring::npos)
    {
        wchar_t* end{};
        auto const index = std::wcstol(path.c_str() + comma + 1, &end, 10);
        if (end && *end == L'\0')
        {
            iconIndex = static_cast<int>(index);
            path.resize(comma);
        }
    }
    auto const expandedSize = ExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
    if (expandedSize)
    {
        std::wstring expanded(expandedSize, L'\0');
        if (ExpandEnvironmentStringsW(path.c_str(), expanded.data(), expandedSize))
        {
            path = expanded.c_str();
        }
    }

    HICON largeIcon{};
    HICON smallIcon{};
    if (PrivateExtractIconsW(path.c_str(), iconIndex, size, size, &largeIcon, nullptr, 1, 0) == 0)
    {
        largeIcon = nullptr;
    }
    if (!largeIcon && ExtractIconExW(path.c_str(), iconIndex, &largeIcon, &smallIcon, 1) == 0)
    {
        SHFILEINFOW fileInfo{};
        if (SHGetFileInfoW(path.c_str(), 0, &fileInfo, sizeof(fileInfo), SHGFI_ICON | SHGFI_LARGEICON))
        {
            largeIcon = fileInfo.hIcon;
        }
        if (SHGetFileInfoW(path.c_str(), 0, &fileInfo, sizeof(fileInfo), SHGFI_ICON | SHGFI_SMALLICON))
        {
            smallIcon = fileInfo.hIcon;
        }
    }
    if (!largeIcon && !smallIcon)
    {
        SHSTOCKICONINFO stockInfo{ sizeof(stockInfo) };
        if (SUCCEEDED(SHGetStockIconInfo(SIID_APPLICATION, SHGSI_ICON | SHGSI_LARGEICON, &stockInfo)))
        {
            largeIcon = stockInfo.hIcon;
        }
    }
    wil::unique_hicon largeOwner(largeIcon);
    wil::unique_hicon smallOwner(smallIcon);
    auto bitmap = RenderIcon(size <= 16 && smallIcon ? smallIcon : (largeIcon ? largeIcon : smallIcon), size);
    if (bitmap)
    {
        m_cache.emplace(key, bitmap);
    }
    return bitmap;
}
