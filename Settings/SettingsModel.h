#pragma once
#include <array>
#include <optional>

enum class HotkeyAction { Next, Previous, Mixer, Mute, Favorite1, Favorite2, Favorite3, Count };
inline constexpr std::array HotkeyLabels{L"Next audio device", L"Previous audio device", L"Open or close mixer",
    L"Mute or unmute", L"Favorite device 1", L"Favorite device 2", L"Favorite device 3"};
using HotkeyBindings = std::array<std::wstring, static_cast<size_t>(HotkeyAction::Count)>;

enum class AppTheme
{
    System,
    Light,
    Dark
};

enum class IntegrationMode
{
    TrayOnly,
    TaskbarOnly,
    TaskbarAndTray
};

enum class TaskbarIndicator
{
    IconOnly,
    VolumePercentage
};

enum class MixerBackdrop
{
    Mica,
    Acrylic,
    ClearGlass,
    LiquidGlass
};

struct PinnedApplication
{
    std::wstring id;
    std::wstring displayName;
    std::wstring executablePath;
    int order{ 0 };
};

struct SettingsModel
{
    AppTheme theme{ AppTheme::System };
    bool launchAtStartup{ false };
    HotkeyBindings hotkeys{ L"Ctrl+Alt+V" };
    IntegrationMode integrationMode{ IntegrationMode::TaskbarAndTray };
    TaskbarIndicator taskbarIndicator{ TaskbarIndicator::VolumePercentage };
    bool showAudioActivityMeters{ true };
    bool showChannelPercentages{ false };
    bool taskbarWheelEnabled{ true };
    bool alwaysShowSpeaker{};
    bool zeroWhenMuted{ false };
    bool startMinimized{ false };
    bool hideOnFocusLoss{ true };
    bool pinnedAlwaysOnTop{ false };
    bool rememberPopupPosition{ true };
    std::optional<POINT> popupPosition;
    std::optional<SIZE> popupSize;
    int wheelStep{ 2 };
    int channelWidth{ 68 };
    int faderHeight{ 144 };
    int glassOpacity{ 46 };
    int glassFrost{ 80 };
    std::optional<COLORREF> glassTint;
    MixerBackdrop mixerBackdrop{ MixerBackdrop::Mica };
    unsigned deviceRoleMask{ 7 };
    std::vector<std::wstring> favoriteDevices;
    std::vector<std::wstring> hiddenDevices;
    std::vector<PinnedApplication> pinnedApplications;
    std::vector<std::wstring> hiddenApplications;

    COLORREF GlassTint(bool light) const
    {
        return glassTint.value_or(light ? RGB(240, 245, 252) : RGB(20, 28, 44));
    }
};
