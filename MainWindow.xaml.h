#pragma once

#include "MainWindow.g.h"
#include "UI/AppIconCache.h"
#include "UI/LiquidGlassRenderer.h"
#include "Settings/SettingsModel.h"
#include "Audio/AudioModels.h"
#include <functional>
#include <map>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.System.h>

class AudioStateService;

namespace winrt::VolumeMixer::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        HWND Handle() const { return m_hwnd; }
        void SetAudioService(::AudioStateService* service) { m_audio = service; }
        void SetSettingsHandler(std::function<void()> handler) { m_settingsHandler = std::move(handler); }
        void SetSettingsChangedHandler(std::function<bool(::SettingsModel const&)> handler) { m_settingsChangedHandler = std::move(handler); }
        void RetrySettingsSave_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ApplySettings(::SettingsModel const& settings);
        void ApplySnapshot(::AudioSnapshot const& snapshot);
        void ApplyMasterVolume(::MasterVolumeState const& state);
        void ApplyPeaks(::AudioPeakSnapshot const& snapshot);
        void RefreshColors();
        static int HeightForTracks(::SettingsModel const& settings);
        void ShowAt(POINT anchor);
        void HideMixer();
        void Destroy();
        void PinButton_Click(IInspectable const&, RoutedEventArgs const&);
        void DeviceSlider_ValueChanged(IInspectable const&, Controls::Primitives::RangeBaseValueChangedEventArgs const& args);
        void DeviceMuteButton_Click(IInspectable const&, RoutedEventArgs const&);
        void DeviceButton_Click(IInspectable const&, RoutedEventArgs const&);
        void SettingsButton_Click(IInspectable const&, RoutedEventArgs const&);

    private:
        struct GlassThumb
        {
            std::uint64_t id{};
            FrameworkElement element{ nullptr };
            Media::Imaging::WriteableBitmap bitmap{ nullptr };
            Media::ImageBrush brush{ nullptr };
        };

        struct MeterVisual
        {
            Controls::Grid layers{ nullptr };
            Media::TranslateTransform sourceOffset{ nullptr };
            Media::TranslateTransform outputOffset{ nullptr };
            float displayedSource{};
            float displayedOutput{};
            std::shared_ptr<GlassThumb> thumb;

            void Update(::AudioMeterLevels const& levels, float decay);
            void Clear();
        };

        struct SessionControls
        {
            Controls::Border root{ nullptr };
            Shapes::Rectangle divider{ nullptr };
            Controls::Image icon{ nullptr };
            Controls::Slider slider{ nullptr };
            MeterVisual meter;
            Controls::TextBlock percent{ nullptr };
            Controls::FontIcon muteGlyph{ nullptr };
            Controls::Button muteButton{ nullptr };
            Controls::Button optionsButton{ nullptr };
            Controls::MenuFlyout menu{ nullptr };
            Controls::MenuFlyoutItem pinItem{ nullptr };
            Controls::MenuFlyoutItem resetItem{ nullptr };
            bool available{ false };
            bool muted{ false };
            bool changing{ false };
        };

        void OnActivated(IInspectable const&, WindowActivatedEventArgs const&);
        void TrackFlyout(Controls::Primitives::FlyoutBase const& menu);
        void PositionAt(POINT anchor, bool opening);
        void RefreshBackdrop();
        void UpdateLiquidGlassTimer();
        void RenderLiquidGlass();
        void ReleaseLiquidGlass();
        void RenderGlassThumbs(RECT const& client, float scale);
        void ResetGlassThumbs();
        void ResizeFaders(double height);
        double DividerHeight(double windowHeight);
        void FitChannels(double width);
        int MinimumWidth() const;
        void BeginSizeMove(int hit, POINT cursor, bool native);
        void UpdateSizeMove(POINT cursor);
        void EndSizeMove();
        void UpdateResizeCursor(int hit);
        void EnableWheel(Controls::Slider const& slider);
        MeterVisual InstallPeakMeter(Controls::Slider const& slider);
        void SetVolumePrecisely(std::wstring const& appId);
        static LRESULT CALLBACK WindowSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data);
        SessionControls& EnsureSession(std::wstring const& channelId, std::wstring const& appId, std::wstring const& displayName);
        void UpdateMeterVisibility();
        int PinnedIndex(std::wstring const& appId) const;
        void TogglePin(std::wstring const& appId);
        void ReorderPinned(std::wstring const& sourceAppId, std::wstring const& targetAppId);
        void HideApplication(std::wstring const& appId);
        void LayoutChanged();

        Microsoft::UI::Windowing::AppWindow m_appWindow{ nullptr };
        Microsoft::UI::Composition::SystemBackdrops::DesktopAcrylicController m_acrylicController{ nullptr };
        Microsoft::UI::Composition::SystemBackdrops::SystemBackdropConfiguration m_backdropConfiguration{ nullptr };
        Windows::System::DispatcherQueueController m_systemDispatcher{ nullptr };
        Windows::UI::Composition::Compositor m_glassCompositor{ nullptr };
        Windows::UI::Composition::CompositionColorBrush m_clearGlassBrush{ nullptr };
        Windows::UI::Composition::CompositionSurfaceBrush m_liquidGlassBrush{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_liquidGlassTimer{ nullptr };
        std::unique_ptr<::LiquidGlassRenderer> m_liquidGlass;
        std::uint64_t m_nextGlassThumbId{};
        ULONGLONG m_liquidRetryAt{};
        bool m_excludedFromCapture{};
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_hideTimer{ nullptr };
        Media::SolidColorBrush m_peakBrush;
        Media::SolidColorBrush m_sourcePeakBrush;
        Media::SolidColorBrush m_railBrush;
        Media::SolidColorBrush m_thumbBrush{ Windows::UI::Color{255, 255, 255, 255} };
        Media::SolidColorBrush m_backdropTint;
        MeterVisual m_deviceMeter;
        std::chrono::steady_clock::time_point m_lastPeakSample{};
        Controls::Flyout m_precisionFlyout{ nullptr };
        ::AudioStateService* m_audio{ nullptr };
        std::function<void()> m_settingsHandler;
        void SaveSettings();
        std::function<bool(::SettingsModel const&)> m_settingsChangedHandler;
        AppIconCache m_icons;
        HWND m_hwnd{ nullptr };
        std::map<std::wstring, SessionControls> m_sessions;
        ::SettingsModel m_settings;
        ::AudioSnapshot m_snapshot;
        std::wstring m_deviceMenuSignature;
        std::wstring m_draggedAppId;
        POINT m_anchor{};
        uint64_t m_requestedMasterWrite{};
        bool m_hasSnapshot{ false };
        bool m_updating{ false };
        bool m_deviceChanging{ false };
        bool m_pinned{ false };
        bool m_destroying{ false };
        bool m_clearGlassActive{ false };
        bool m_showMeters{ true };
        bool m_flyoutOpen{ false };
        bool m_inSizeMove{};
        bool m_nativeSizeMove{};
        int m_sizeMoveHit{};
        int m_cursorHit{HTCLIENT};
        POINT m_sizeMoveStart{};
        RECT m_sizeMoveBounds{};
    };
}

namespace winrt::VolumeMixer::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
