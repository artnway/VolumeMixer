#pragma once

#include <map>

class AppIconCache final
{
public:
    winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapSource Load(
        std::wstring const& executablePath, int size, std::wstring_view aumid = {});

private:
    std::map<std::wstring, winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap> m_cache;
};
