#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "Audio/AudioStateService.h"
#include "Audio/PlaybackRoleSwitch.h"
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Composition.h>
#include <dwmapi.h>
#include <robuffer.h>
#include <cstring>
#include <DispatcherQueue.h>
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <set>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Windowing;

namespace winrt::VolumeMixer::implementation
{
    namespace
    {
        constexpr int VisibleChannels = 7;
        constexpr int FaderPadding = 97;
        constexpr int MinimumFaderHeight = 96;
        constexpr int HorizontalPadding = 36;
        constexpr int ResizeCornerReach = 16;

        HRESULT SetClearWindowBackground(HWND hwnd, bool enabled)
        {
            DWM_BLURBEHIND backdrop{};
            backdrop.dwFlags = DWM_BB_ENABLE;
            backdrop.fEnable = enabled;
            if (enabled)
            {
                MARGINS const margins{};
                DwmExtendFrameIntoClientArea(hwnd, &margins);
                // A region outside the client area enables alpha without blurring the visible surface.
                backdrop.hRgnBlur = CreateRectRgn(-2, -2, -1, -1);
                if (!backdrop.hRgnBlur) return E_OUTOFMEMORY;
                backdrop.dwFlags |= DWM_BB_BLURREGION;
            }
            auto const result = DwmEnableBlurBehindWindow(hwnd, &backdrop);
            if (backdrop.hRgnBlur) DeleteObject(backdrop.hRgnBlur);
            return result;
        }

        void SetResizeCursor(int hit)
        {
            LPCWSTR shape{};
            switch (hit)
            {
            case HTLEFT: case HTRIGHT: shape = IDC_SIZEWE; break;
            case HTTOP: case HTBOTTOM: shape = IDC_SIZENS; break;
            case HTTOPLEFT: case HTBOTTOMRIGHT: shape = IDC_SIZENWSE; break;
            case HTTOPRIGHT: case HTBOTTOMLEFT: shape = IDC_SIZENESW; break;
            }
            if (shape) SetCursor(LoadCursorW(nullptr, shape));
        }

        int ResizeHit(HWND hwnd, POINT point)
        {
            RECT rect{};
            if (!GetWindowRect(hwnd, &rect) || !PtInRect(&rect, point))
            {
                return HTCLIENT;
            }
            int const grip = MulDiv(6, GetDpiForWindow(hwnd), 96);
            int const corner = MulDiv(ResizeCornerReach, GetDpiForWindow(hwnd), 96);
            auto const left = point.x < rect.left + grip;
            auto const right = point.x >= rect.right - grip;
            if (point.y < rect.top + grip)
                return point.x < rect.left + corner ? HTTOPLEFT : point.x >= rect.right - corner ? HTTOPRIGHT : HTTOP;
            if (point.y >= rect.bottom - grip)
                return point.x < rect.left + corner ? HTBOTTOMLEFT : point.x >= rect.right - corner ? HTBOTTOMRIGHT : HTBOTTOM;
            if (left || right)
            {
                if (point.y < rect.top + corner) return left ? HTTOPLEFT : HTTOPRIGHT;
                if (point.y >= rect.bottom - corner) return left ? HTBOTTOMLEFT : HTBOTTOMRIGHT;
            }
            return left ? HTLEFT : right ? HTRIGHT : HTCLIENT;
        }

        void SetChannelTooltip(FrameworkElement const& element, std::wstring const& name, unsigned volume, bool muted)
        {
            ToolTipService::SetToolTip(element, box_value(name + L" • " + std::to_wstring(volume)
                + (muted ? L"% (Muted)" : L"%")));
        }

        Controls::Grid VerticalTrackTemplate(DependencyObject const& parent)
        {
            if (auto const grid = parent.try_as<Controls::Grid>(); grid && grid.Name() == L"VerticalTemplate")
            {
                return grid;
            }
            for (int i = 0, count = Media::VisualTreeHelper::GetChildrenCount(parent); i < count; ++i)
            {
                if (auto const grid = VerticalTrackTemplate(Media::VisualTreeHelper::GetChild(parent, i)))
                {
                    return grid;
                }
            }
            return nullptr;
        }

        void DrawPeak(Media::TranslateTransform const& offset, double height, float& displayed, float peak, float decay)
        {
            displayed = std::max(std::clamp(peak, 0.0f, 1.0f), displayed * decay);
            if (displayed < 0.005f)
            {
                displayed = 0;
            }
            auto const y = height * (1 - displayed);
            if (std::abs(offset.Y() - y) >= 0.5 || (displayed == 0 && offset.Y() != y))
            {
                offset.Y(y);
            }
        }
    }

    MainWindow::MainWindow()
    {
        InitializeComponent();

        VolumeMixer::MainWindow thisWindow = *this;
        auto windowNative = thisWindow.as<IWindowNative>();
        check_hresult(windowNative->get_WindowHandle(&m_hwnd));
        m_appWindow = AppWindow::GetFromWindowId(GetWindowIdFromWindow(m_hwnd));
        DWM_WINDOW_CORNER_PREFERENCE cornerPreference = DWMWCP_ROUND;
        DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPreference, sizeof(cornerPreference));
        COLORREF const noBorder = DWMWA_COLOR_NONE;
        DwmSetWindowAttribute(m_hwnd, DWMWA_BORDER_COLOR, &noBorder, sizeof(noBorder));

        SetWindowSubclass(m_hwnd, WindowSubclass, 1, reinterpret_cast<DWORD_PTR>(this));
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, WS_POPUP | WS_THICKFRAME);
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, (GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW);
        SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

        m_liquidGlassTimer = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
        m_liquidGlassTimer.Interval(std::chrono::milliseconds{34});
        m_liquidGlassTimer.IsRepeating(true);
        m_liquidGlassTimer.Tick([this](auto const&, auto const&) { RenderLiquidGlass(); });
        m_hideTimer = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
        m_hideTimer.Interval(std::chrono::milliseconds{ 250 });
        m_hideTimer.IsRepeating(false);
        m_hideTimer.Tick([this](auto const&, auto const&)
        {
            if (!m_pinned && !m_flyoutOpen && !m_inSizeMove)
            {
                HideMixer();
            }
        });

        TrackFlyout(MixerMenu());
        m_deviceMeter = InstallPeakMeter(DeviceSlider());
        PinDot().Fill(m_railBrush);
        MixerSurface().ActualThemeChanged([this](auto const&, auto const&) { RefreshColors(); });
        RefreshColors();
        MixerSurface().PointerPressed([this](auto const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
        {
            auto const point = args.GetCurrentPoint(MixerSurface());
            if (!point.Properties().IsLeftButtonPressed()) return;
            auto const scale = MixerSurface().XamlRoot().RasterizationScale();
            POINT pointer{std::lround(point.Position().X * scale), std::lround(point.Position().Y * scale)};
            ClientToScreen(m_hwnd, &pointer);
            auto const hit = ResizeHit(m_hwnd, pointer);
            if ((hit != HTCLIENT || args.OriginalSource() == MixerSurface() || args.OriginalSource() == MixerBody())
                && MixerSurface().CapturePointer(args.Pointer()))
            {
                BeginSizeMove(hit == HTCLIENT ? HTCAPTION : hit, pointer, false);
                args.Handled(true);
            }
        });
        MixerSurface().PointerMoved([this](auto const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
        {
            POINT cursor{};
            GetCursorPos(&cursor);
            if (m_inSizeMove && !m_nativeSizeMove)
            {
                UpdateSizeMove(cursor);
                args.Handled(true);
            }
            else
            {
                UpdateResizeCursor(ResizeHit(m_hwnd, cursor));
            }
        });
        MixerSurface().PointerReleased([this](auto const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
        {
            if (m_inSizeMove && !m_nativeSizeMove)
            {
                EndSizeMove();
                MixerSurface().ReleasePointerCapture(args.Pointer());
                args.Handled(true);
            }
        });
        MixerSurface().PointerCaptureLost([this](auto const&, auto const&)
        {
            if (m_inSizeMove && !m_nativeSizeMove)
            {
                EndSizeMove();
            }
        });
        MixerSurface().SizeChanged([this](auto const&, SizeChangedEventArgs const& args)
        {
            ResizeFaders(args.NewSize().Height);
            FitChannels(args.NewSize().Width);
            if (args.NewSize().Height != args.PreviousSize().Height)
            {
                auto const height = DividerHeight(args.NewSize().Height);
                for (auto const& [id, channel] : m_sessions)
                {
                    channel.divider.Height(height);
                }
            }
        });
        SessionScrollViewer().ViewChanged([this](auto const&, Controls::ScrollViewerViewChangedEventArgs const& args)
        {
            if (!args.IsIntermediate())
            {
                auto const offset = SessionScrollViewer().HorizontalOffset();
                auto const column = m_settings.channelWidth;
                auto const snapped = std::round(offset / column) * column;
                if (std::abs(snapped - offset) >= 0.5 && snapped <= SessionScrollViewer().ScrollableWidth() + 0.5)
                {
                    SessionScrollViewer().ChangeView(box_value(snapped).as<Windows::Foundation::IReference<double>>(), nullptr, nullptr, true);
                }
            }
        });
        EnableWheel(DeviceSlider());
        Activated({ this, &MainWindow::OnActivated });
        m_appWindow.Closing([this](Microsoft::UI::Windowing::AppWindow const&, AppWindowClosingEventArgs const& args)
        {
            if (!m_destroying)
            {
                args.Cancel(true);
                HideMixer();
            }
        });
    }

    void MainWindow::ApplySettings(::SettingsModel const& settings)
    {
        m_settings = settings;
        SettingsSaveError().IsOpen(false);
        if (settings.mixerBackdrop != MixerBackdrop::LiquidGlass) UpdateLiquidGlassTimer();
        Content().as<FrameworkElement>().RequestedTheme(settings.theme == AppTheme::Light ? ElementTheme::Light : settings.theme == AppTheme::Dark ? ElementTheme::Dark : ElementTheme::Default);
        RefreshBackdrop();
        m_showMeters = settings.showAudioActivityMeters;
        MasterColumnWidth().Width({static_cast<double>(settings.channelWidth), GridUnitType::Pixel});
        ResizeFaders(MixerSurface().ActualHeight());
        FitChannels(MixerSurface().ActualWidth());
        for (auto const& [id, channel] : m_sessions)
        {
            channel.root.Width(settings.channelWidth);
        }
        RefreshColors();
        UpdateLiquidGlassTimer();
        DevicePercent().Visibility(settings.showChannelPercentages ? Visibility::Visible : Visibility::Collapsed);
        if (IsWindowVisible(m_hwnd))
        {
            PositionAt(m_anchor, false);
        }
        UpdateMeterVisibility();
        if (m_hasSnapshot)
        {
            ApplySnapshot(m_snapshot);
        }
    }

    void MainWindow::RefreshBackdrop()
    {
        using namespace Microsoft::UI::Composition::SystemBackdrops;
        VolumeMixer::MainWindow window = *this;
        auto const target = window.as<Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop>();
        auto const closeAcrylic = [&]
        {
            if (m_acrylicController)
            {
                m_acrylicController.Close();
                m_acrylicController = nullptr;
                m_backdropConfiguration = nullptr;
            }
        };
        if (m_settings.mixerBackdrop == MixerBackdrop::ClearGlass || m_settings.mixerBackdrop == MixerBackdrop::LiquidGlass)
        {
            if (m_clearGlassActive) return;
            closeAcrylic();
            if (SUCCEEDED(SetClearWindowBackground(m_hwnd, true)))
            {
                try
                {
                    if (!Windows::System::DispatcherQueue::GetForCurrentThread())
                    {
                        DispatcherQueueOptions const options{sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
                        check_hresult(CreateDispatcherQueueController(options,
                            reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(put_abi(m_systemDispatcher))));
                    }
                    if (!m_glassCompositor)
                    {
                        m_glassCompositor = Windows::UI::Composition::Compositor();
                        m_clearGlassBrush = m_glassCompositor.CreateColorBrush(Windows::UI::Color{});
                    }
                    SystemBackdrop(nullptr);
                    target.SystemBackdrop(m_clearGlassBrush);
                    m_clearGlassActive = true;
                    RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE);
                    return;
                }
                catch (hresult_error const&)
                {
                    SetClearWindowBackground(m_hwnd, false);
                }
            }
        }
        else if (m_clearGlassActive)
        {
            target.SystemBackdrop(nullptr);
            SetClearWindowBackground(m_hwnd, false);
            m_clearGlassActive = false;
        }
        if (m_settings.mixerBackdrop != MixerBackdrop::Mica && DesktopAcrylicController::IsSupported())
        {
            if (!m_acrylicController)
            {
                SystemBackdrop(nullptr);
                m_backdropConfiguration = SystemBackdropConfiguration();
                m_backdropConfiguration.IsInputActive(true);
                m_acrylicController = DesktopAcrylicController();
                m_acrylicController.Kind(DesktopAcrylicKind::Thin);
                m_acrylicController.AddSystemBackdropTarget(target);
                m_acrylicController.SetSystemBackdropConfiguration(m_backdropConfiguration);
            }
            m_backdropConfiguration.Theme(m_settings.theme == AppTheme::Light ? SystemBackdropTheme::Light
                : m_settings.theme == AppTheme::Dark ? SystemBackdropTheme::Dark : SystemBackdropTheme::Default);
            return;
        }
        closeAcrylic();
        MixerSurface().Background(Media::SolidColorBrush(Windows::UI::Color{}));
        if (!SystemBackdrop())
        {
            Media::MicaBackdrop backdrop;
            backdrop.Kind(MicaKind::BaseAlt);
            SystemBackdrop(backdrop);
        }
    }

    void MainWindow::UpdateLiquidGlassTimer()
    {
        if (m_settings.mixerBackdrop == MixerBackdrop::LiquidGlass && m_clearGlassActive && IsWindowVisible(m_hwnd))
        {
            if (!m_liquidGlassTimer.IsRunning()) m_liquidGlassTimer.Start();
        }
        else
        {
            m_liquidGlassTimer.Stop();
            ReleaseLiquidGlass();
            m_liquidRetryAt = 0;
        }
    }

    void MainWindow::ReleaseLiquidGlass()
    {
        ResetGlassThumbs();
        if (m_liquidGlassBrush)
        {
            VolumeMixer::MainWindow window = *this;
            window.as<Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop>().SystemBackdrop(m_clearGlassBrush);
            m_liquidGlassBrush = nullptr;
        }
        m_liquidGlass.reset();
        if (m_excludedFromCapture)
        {
            SetWindowDisplayAffinity(m_hwnd, WDA_NONE);
            m_excludedFromCapture = false;
        }
    }

    void MainWindow::RenderLiquidGlass()
    {
        auto const now = GetTickCount64();
        if (now < m_liquidRetryAt) return;
        RECT client{};
        POINT origin{};
        if (!GetClientRect(m_hwnd, &client) || !ClientToScreen(m_hwnd, &origin) || IsRectEmpty(&client)) return;
        OffsetRect(&client, origin.x, origin.y);
        try
        {
            auto const monitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO info{sizeof(info)};
            RECT covered{};
            if (!GetMonitorInfoW(monitor, &info) || !IntersectRect(&covered, &client, &info.rcMonitor)
                || !EqualRect(&client, &covered))
            {
                if (m_liquidGlass || m_excludedFromCapture) ReleaseLiquidGlass();
                return;
            }
            if (m_liquidGlass && m_liquidGlass->Monitor() != monitor) ReleaseLiquidGlass();
            if (!m_liquidGlass)
            {
                check_bool(SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE));
                m_excludedFromCapture = true;
                m_liquidGlass = std::make_unique<::LiquidGlassRenderer>(m_hwnd);
            }
            auto const scale = GetDpiForWindow(m_hwnd) / 96.0f;
            auto const brush = m_liquidGlass->Render(client, scale, m_settings.glassFrost, m_glassCompositor);
            if (brush != m_liquidGlassBrush)
            {
                VolumeMixer::MainWindow window = *this;
                auto const target = window.as<Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop>();
                target.SystemBackdrop(brush ? Windows::UI::Composition::CompositionBrush(brush) : m_clearGlassBrush);
                m_liquidGlassBrush = brush;
            }
            if (brush) RenderGlassThumbs(client, scale);
            else ResetGlassThumbs();
        }
        catch (hresult_error const& error)
        {
            OutputDebugStringW(error.message().c_str());
            ReleaseLiquidGlass();
            m_liquidRetryAt = now + 2000;
        }
    }

    void MainWindow::RenderGlassThumbs(RECT const& client, float scale)
    {
        auto const diameter = static_cast<UINT>(std::lround(14 * scale));
        auto const surface = MixerSurface();
        std::vector<::LiquidGlassRenderer::ThumbSample> samples;
        std::vector<GlassThumb*> visible;
        auto const collect = [&](MeterVisual const& meter, RECT const& clip)
        {
            auto& lens = *meter.thumb;
            auto const element = lens.element;
            if (!element || !element.XamlRoot()) return;
            auto const center = element.TransformToVisual(surface).TransformPoint(
                {static_cast<float>(element.ActualWidth() / 2), static_cast<float>(element.ActualHeight() / 2)});
            POINT const screen{client.left + std::lround(center.X * scale), client.top + std::lround(center.Y * scale)};
            if (PtInRect(&clip, screen))
            {
                samples.push_back({lens.id, screen});
                visible.push_back(&lens);
            }
            else if (lens.brush)
            {
                auto const dot = element.as<Shapes::Ellipse>();
                if (dot.Fill() == lens.brush) dot.Fill(m_thumbBrush);
            }
        };
        collect(m_deviceMeter, client);
        auto const scroller = SessionScrollViewer();
        if (scroller.XamlRoot())
        {
            auto const origin = scroller.TransformToVisual(surface).TransformPoint({0, 0});
            RECT const viewport{
                client.left + std::lround(origin.X * scale), client.top + std::lround(origin.Y * scale),
                client.left + std::lround((origin.X + scroller.ActualWidth()) * scale),
                client.top + std::lround((origin.Y + scroller.ActualHeight()) * scale)};
            RECT clip{};
            IntersectRect(&clip, &viewport, &client);
            for (auto const& [id, session] : m_sessions) collect(session.meter, clip);
        }
        auto const tiles = m_liquidGlass->RenderThumbs(samples, diameter, scale);
        auto const tileSize = static_cast<size_t>(diameter) * diameter;
        for (size_t i = 0; i < tiles.ids.size(); ++i)
        {
            auto const found = std::ranges::find(visible, tiles.ids[i], &GlassThumb::id);
            if (found == visible.end()) continue;
            auto& lens = **found;
            auto const element = lens.element;
            if (!lens.bitmap || lens.bitmap.PixelWidth() != static_cast<int>(diameter))
            {
                lens.bitmap = Media::Imaging::WriteableBitmap(diameter, diameter);
                lens.brush = Media::ImageBrush();
                lens.brush.Stretch(Media::Stretch::Fill);
                lens.brush.ImageSource(lens.bitmap);
            }
            BYTE* pixels{};
            check_hresult(lens.bitmap.PixelBuffer().as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&pixels));
            std::memcpy(pixels, tiles.pixels.data() + i * tileSize, tileSize * sizeof(std::uint32_t));
            lens.bitmap.Invalidate();
            auto const dot = element.as<Shapes::Ellipse>();
            if (dot.Fill() != lens.brush) dot.Fill(lens.brush);
        }
    }

    void MainWindow::ResetGlassThumbs()
    {
        auto const reset = [&](MeterVisual const& meter)
        {
            auto& lens = *meter.thumb;
            if (!lens.brush) return;
            if (lens.element) lens.element.as<Shapes::Ellipse>().Fill(m_thumbBrush);
            lens.bitmap = nullptr;
            lens.brush = nullptr;
        };
        reset(m_deviceMeter);
        for (auto const& [id, session] : m_sessions) reset(session.meter);
    }

    int MainWindow::HeightForTracks(::SettingsModel const& settings)
    {
        return FaderPadding + settings.faderHeight + (settings.showChannelPercentages ? 24 : 0);
    }

    void MainWindow::ResizeFaders(double height)
    {
        auto const target = m_settings.popupSize || (m_inSizeMove && m_sizeMoveHit != HTCAPTION)
            ? std::max<double>(MinimumFaderHeight, height - FaderPadding - (m_settings.showChannelPercentages ? 24 : 0))
            : m_settings.faderHeight;
        if (DeviceSlider().Height() != target)
        {
            DeviceSlider().Height(target);
            for (auto const& [id, channel] : m_sessions)
            {
                channel.slider.Height(target);
            }
        }
    }

    double MainWindow::DividerHeight(double windowHeight)
    {
        return std::max(0.0, windowHeight - 2 * MixerBody().Padding().Top);
    }

    int MainWindow::MinimumWidth() const
    {
        return HorizontalPadding + (m_sessions.empty() ? 1 : 2) * m_settings.channelWidth;
    }

    void MainWindow::FitChannels(double width)
    {
        auto const column = m_settings.channelWidth;
        auto const count = std::max(0.0, std::floor((width - 32 - column + 0.01) / column));
        SessionScrollViewer().Width(count * column);
    }

    void MainWindow::RefreshColors()
    {
        bool const light = MixerSurface().ActualTheme() == ElementTheme::Light;
        using namespace Windows::UI::ViewManagement;
        m_peakBrush.Color(UISettings().GetColorValue(UIColorType::Accent));
        m_railBrush.Color(light ? Windows::UI::Color{36, 0, 0, 0} : Windows::UI::Color{32, 255, 255, 255});
        m_sourcePeakBrush.Color(light ? Windows::UI::Color{126, 0, 0, 0} : Windows::UI::Color{106, 255, 255, 255});
        auto const tint = m_settings.GlassTint(light);
        auto const opacity = m_acrylicController || m_clearGlassActive || m_settings.glassTint ? m_settings.glassOpacity : 0;
        auto const alpha = static_cast<uint8_t>(std::lround(255 * opacity / 100.0));
        m_backdropTint.Color(Windows::UI::Color{alpha, GetRValue(tint), GetGValue(tint), GetBValue(tint)});
        if (MixerSurface().Background() != m_backdropTint)
        {
            MixerSurface().Background(m_backdropTint);
        }
        if (m_acrylicController)
        {
            m_acrylicController.TintColor(light ? Windows::UI::Color{255, 248, 249, 252} : Windows::UI::Color{255, 30, 31, 36});
            m_acrylicController.TintOpacity(0.08f);
            m_acrylicController.LuminosityOpacity(0.16f);
            m_acrylicController.FallbackColor(light ? Windows::UI::Color{255, 228, 231, 236} : Windows::UI::Color{255, 54, 57, 63});
        }
    }

    void MainWindow::MeterVisual::Update(::AudioMeterLevels const& levels, float decay)
    {
        if (displayedSource == 0 && displayedOutput == 0 && levels.source == 0 && levels.output == 0) return;
        auto const height = layers.ActualHeight();
        DrawPeak(sourceOffset, height, displayedSource, levels.source, decay);
        DrawPeak(outputOffset, height, displayedOutput, levels.output, decay);
    }

    void MainWindow::MeterVisual::Clear()
    {
        displayedSource = displayedOutput = 0;
        sourceOffset.Y(layers.ActualHeight());
        outputOffset.Y(layers.ActualHeight());
    }

    MainWindow::MeterVisual MainWindow::InstallPeakMeter(Controls::Slider const& slider)
    {
        MeterVisual visual;
        visual.thumb = std::make_shared<GlassThumb>();
        visual.thumb->id = ++m_nextGlassThumbId;
        visual.layers = Controls::Grid();
        auto const meter = visual.layers;
        Media::RectangleGeometry clip;
        meter.Clip(clip);
        meter.Visibility(m_showMeters ? Visibility::Visible : Visibility::Collapsed);
        auto const addLevel = [&](Media::SolidColorBrush const& brush, Media::TranslateTransform& offset)
        {
            Shapes::Rectangle level;
            level.Fill(brush);
            level.RadiusY(1.5);
            offset = Media::TranslateTransform();
            level.RenderTransform(offset);
            meter.Children().Append(level);
        };
        addLevel(m_sourcePeakBrush, visual.sourceOffset);
        addLevel(m_peakBrush, visual.outputOffset);
        meter.SizeChanged([clip, source = visual.sourceOffset, output = visual.outputOffset](auto const&, SizeChangedEventArgs const& args)
        {
            auto const oldHeight = args.PreviousSize().Height;
            auto const size = args.NewSize();
            source.Y(oldHeight > 0 ? source.Y() / oldHeight * size.Height : size.Height);
            output.Y(oldHeight > 0 ? output.Y() / oldHeight * size.Height : size.Height);
            clip.Rect({0, 0, size.Width, size.Height});
        });
        slider.Loaded([meter, lens = visual.thumb, railBrush = m_railBrush, white = m_thumbBrush, master = slider == DeviceSlider(),
            weak = get_weak()](IInspectable const& sender, RoutedEventArgs const&)
        {
            auto const grid = VerticalTrackTemplate(sender.as<Controls::Slider>());
            auto const previousSlot = meter.Parent().try_as<Controls::Grid>();
            if (!grid || (previousSlot && previousSlot.Parent() == grid))
            {
                return;
            }
            auto const children = grid.Children();
            FrameworkElement track{nullptr};
            FrameworkElement decrease{nullptr};
            for (uint32_t i = 0; i < children.Size(); ++i)
            {
                auto const part = children.GetAt(i).try_as<FrameworkElement>();
                if (part && part.Name() == L"VerticalTrackRect")
                {
                    track = part;
                }
                if (part && part.Name() == L"VerticalDecreaseRect")
                {
                    decrease = part;
                }
                if (track && decrease && part && part.Name() == L"VerticalThumb")
                {
                    if (previousSlot)
                    {
                        uint32_t index{};
                        if (previousSlot.Children().IndexOf(meter, index))
                        {
                            previousSlot.Children().RemoveAt(index);
                        }
                    }
                    auto const thumb = part.as<Controls::Primitives::Thumb>();
                    thumb.ApplyTemplate();
                    if (Media::VisualTreeHelper::GetChildrenCount(thumb))
                    {
                        if (auto const visibleThumb = Media::VisualTreeHelper::GetChild(thumb, 0).try_as<Controls::Border>())
                        {
                            // Fit the circle inside the native thumb cell at both ends of the track.
                            thumb.Width(22);
                            thumb.Height(22);
                            visibleThumb.Margin(Thickness{0});
                            visibleThumb.Background(nullptr);
                            visibleThumb.BorderThickness(Thickness{0});
                            if (auto const dot = visibleThumb.Child().try_as<Shapes::Ellipse>())
                            {
                                dot.Width(14);
                                dot.Height(14);
                                dot.Fill(white);
                                lens->element = dot;
                            }
                        }
                    }
                    // Keep the native rows for input and measure the visible rail between the thumb centers.
                    auto const halfThumb = thumb.Height() / 2;
                    Controls::Grid railSlot;
                    Controls::Grid::SetRowSpan(railSlot, 3);
                    Controls::Grid::SetColumn(railSlot, Controls::Grid::GetColumn(thumb));
                    Controls::Grid::SetColumnSpan(railSlot, Controls::Grid::GetColumnSpan(thumb));
                    railSlot.HorizontalAlignment(thumb.HorizontalAlignment());
                    railSlot.UseLayoutRounding(thumb.UseLayoutRounding());
                    railSlot.Width(thumb.Width());
                    railSlot.Margin(Thickness{0, halfThumb, 0, halfThumb});
                    railSlot.IsHitTestVisible(false);
                    // Match the thumb's layout width and narrow the rail about their common center.
                    Media::ScaleTransform narrow;
                    auto const scaleX = track.Width() / thumb.Width();
                    narrow.ScaleX(scaleX);
                    railSlot.RenderTransformOrigin({0.5, 0.5});
                    railSlot.RenderTransform(narrow);
                    track.Opacity(0);
                    decrease.Opacity(0);
                    Shapes::Rectangle rail;
                    rail.Fill(railBrush);
                    rail.RadiusX(2 / scaleX);
                    rail.RadiusY(2);
                    for (auto const& level : meter.Children())
                    {
                        level.as<Shapes::Rectangle>().RadiusX(1.5 / scaleX);
                    }
                    railSlot.Children().Append(rail);
                    railSlot.Children().Append(meter);
                    children.Append(railSlot);
                    Controls::Canvas::SetZIndex(part, 2);
                    part.FocusVisualMargin(Thickness{-4, -8, -4, -8});
                    if (auto const tip = ToolTipService::GetToolTip(part).try_as<Controls::ToolTip>())
                    {
                        auto const placeAbove = [](Controls::ToolTip const& tooltip)
                        {
                            tooltip.Placement(Controls::Primitives::PlacementMode::Top);
                            tooltip.HorizontalOffset(0);
                            tooltip.VerticalOffset(0);
                        };
                        placeAbove(tip);
                        tip.Opened([placeAbove](auto const& sender, auto const&)
                        {
                            placeAbove(sender.template as<Controls::ToolTip>());
                        });
                    }
                    if (master)
                    {
                        thumb.DragStarted([weak](auto const&, auto const&)
                        {
                            if (auto self = weak.get()) self->m_deviceChanging = true;
                        });
                        auto finishDrag = [weak](auto const&, auto const&)
                        {
                            if (auto self = weak.get())
                            {
                                self->m_deviceChanging = false;
                                self->ApplyMasterVolume(self->m_snapshot);
                            }
                        };
                        thumb.DragCompleted(finishDrag);
                        thumb.PointerCaptureLost(finishDrag);
                    }
                    break;
                }
            }
        });
        return visual;
    }

    void MainWindow::ApplySnapshot(::AudioSnapshot const& snapshot)
    {
        if (&snapshot != &m_snapshot)
        {
            if (snapshot.deviceId != m_snapshot.deviceId)
            {
                m_requestedMasterWrite = 0;
                m_deviceChanging = false;
            }
            m_snapshot = snapshot;
        }
        m_hasSnapshot = true;
        m_updating = true;
        auto const deviceIcon = snapshot.deviceIconPath.empty() ? nullptr
            : m_icons.Load(snapshot.deviceIconPath, MulDiv(24, GetDpiForWindow(m_hwnd), 96));
        if (DeviceIcon().Source() != deviceIcon) DeviceIcon().Source(deviceIcon);
        DeviceFallback().Visibility(deviceIcon ? Visibility::Collapsed : Visibility::Visible);
        ApplyMasterVolume(snapshot);
        if (FAILED(snapshot.deviceSwitchResult))
        {
            wchar_t status[96]{};
            if (snapshot.deviceSwitchResult == ::PlaybackRoleSwitch::IncompleteRollback)
            {
                StringCchCopyW(status, ARRAYSIZE(status), L"Device switch failed. Some playback roles could not be restored.");
            }
            else if (snapshot.deviceSwitchResult == ::PlaybackRoleSwitch::UnselectedRoleChanged)
            {
                StringCchCopyW(status, ARRAYSIZE(status),
                    L"Windows changed an unselected playback role. The previous output was restored.");
            }
            else
            {
                StringCchPrintfW(status, ARRAYSIZE(status), L"Device switch failed (0x%08X)", static_cast<unsigned>(snapshot.deviceSwitchResult));
            }
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetHelpText(DeviceButton(), status);
            ToolTipService::SetToolTip(DeviceButton(), box_value(std::wstring(status)
                + (snapshot.deviceName.empty() ? L"" : L" — " + snapshot.deviceName)));
        }
        else
        {
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetHelpText(DeviceButton(), std::to_wstring(snapshot.devices.size()) + L" playback devices");
        }

        std::wstring deviceMenuSignature;
        for (auto const& device : snapshot.devices)
        {
            deviceMenuSignature += device.id + L'\n' + device.name + (device.isDefault ? L"\n1\n" : L"\n0\n");
        }
        for (auto const& id : m_settings.favoriteDevices)
        {
            deviceMenuSignature += L"favorite:" + id + L'\n';
        }
        for (auto const& id : m_settings.hiddenDevices)
        {
            deviceMenuSignature += L"hidden:" + id + L'\n';
        }
        if (deviceMenuSignature != m_deviceMenuSignature)
        {
            Controls::MenuFlyout menu;
            Controls::MenuFlyoutSubItem favorites;
            favorites.Text(L"Favorite devices");
            for (auto const& device : snapshot.devices)
            {
                if (std::ranges::find(m_settings.hiddenDevices, device.id) != m_settings.hiddenDevices.end())
                {
                    continue;
                }
                Controls::ToggleMenuFlyoutItem item;
                item.Text(device.name);
                item.IsChecked(device.isDefault);
                item.Click([this, deviceId = device.id](IInspectable const&, RoutedEventArgs const&)
                {
                    if (m_audio)
                    {
                        m_audio->SwitchToDevice(deviceId);
                    }
                });
                menu.Items().Append(item);
                Controls::ToggleMenuFlyoutItem favorite;
                favorite.Text(device.name);
                favorite.IsChecked(std::ranges::find(m_settings.favoriteDevices, device.id) != m_settings.favoriteDevices.end());
                favorite.Click([this, id = device.id](IInspectable const&, RoutedEventArgs const&)
                {
                    if (!std::erase(m_settings.favoriteDevices, id))
                    {
                        m_settings.favoriteDevices.push_back(id);
                    }
                    LayoutChanged();
                });
                favorites.Items().Append(favorite);
            }
            if (menu.Items().Size() == 0)
            {
                Controls::MenuFlyoutItem unavailable;
                unavailable.Text(L"No selectable playback device");
                unavailable.IsEnabled(false);
                menu.Items().Append(unavailable);
            }
            if (snapshot.deviceAvailable)
            {
                menu.Items().Append(Controls::MenuFlyoutSeparator());
                Controls::MenuFlyoutItem precise;
                precise.Text(L"Set master volume…");
                precise.Click([this](auto const&, auto const&) { SetVolumePrecisely({}); });
                menu.Items().Append(precise);
            }
            if (favorites.Items().Size())
            {
                menu.Items().Append(favorites);
            }
            DeviceButton().ContextFlyout(menu);
            TrackFlyout(menu);
            m_deviceMenuSignature = std::move(deviceMenuSignature);
        }

        bool migratedSettings{};
        std::vector<AudioSessionModel> sessions;
        for (auto const& session : snapshot.sessions)
        {
            if (session.appId.starts_with(L"aumid:") && !session.executablePath.empty())
            {
                auto legacy = L"path:" + session.executablePath;
                std::transform(legacy.begin() + 5, legacy.end(), legacy.begin() + 5,
                    [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
                if (auto pinned = std::ranges::find(m_settings.pinnedApplications, legacy, &PinnedApplication::id);
                    pinned != m_settings.pinnedApplications.end())
                {
                    if (PinnedIndex(session.appId) < 0)
                    {
                        pinned->id = session.appId;
                    }
                    else
                    {
                        m_settings.pinnedApplications.erase(pinned);
                    }
                    migratedSettings = true;
                }
                if (auto hidden = std::ranges::find(m_settings.hiddenApplications, legacy); hidden != m_settings.hiddenApplications.end())
                {
                    *hidden = session.appId;
                    migratedSettings = true;
                }
            }
            auto group = std::ranges::find(sessions, session.appId, &AudioSessionModel::appId);
            if (group == sessions.end())
            {
                sessions.push_back(session);
            }
            else
            {
                // Show the loudest configured session and mark mute only when the whole group is muted.
                group->volume = std::max(group->volume, session.volume);
                group->muted = group->muted && session.muted;
            }
        }
        if (migratedSettings)
        {
            SaveSettings();
        }
        for (auto const& pinned : m_settings.pinnedApplications)
        {
            auto const exists = std::any_of(sessions.begin(), sessions.end(), [&](AudioSessionModel const& session) { return session.appId == pinned.id; });
            if (!exists)
            {
                AudioSessionModel inactive;
                inactive.appId = pinned.id;
                inactive.displayName = pinned.displayName;
                inactive.executablePath = pinned.executablePath;
                sessions.push_back(std::move(inactive));
            }
        }
        std::erase_if(sessions, [this](AudioSessionModel const& session)
        {
            return std::find(m_settings.hiddenApplications.begin(), m_settings.hiddenApplications.end(), session.appId) != m_settings.hiddenApplications.end();
        });
        std::stable_sort(sessions.begin(), sessions.end(), [this](AudioSessionModel const& left, AudioSessionModel const& right)
        {
            auto const leftPinned = PinnedIndex(left.appId);
            auto const rightPinned = PinnedIndex(right.appId);
            if (leftPinned < 0 || rightPinned < 0)
            {
                return leftPinned >= 0 && rightPinned < 0;
            }
            return leftPinned < rightPinned;
        });

        std::set<std::wstring> visibleIds;
        for (auto const& session : sessions)
        {
            visibleIds.insert(session.appId);
        }
        auto items = ApplicationColumns().Items();
        auto const oldCount = items.Size();
        for (auto iterator = m_sessions.begin(); iterator != m_sessions.end();)
        {
            if (!visibleIds.contains(iterator->first))
            {
                uint32_t index{};
                if (items.IndexOf(iterator->second.root, index))
                {
                    items.RemoveAt(index);
                }
                iterator = m_sessions.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }

        for (uint32_t index = 0; index < sessions.size(); ++index)
        {
            auto const& session = sessions[index];
            auto const& channelId = session.appId;
            auto& controls = EnsureSession(channelId, session.appId, session.displayName);
            uint32_t currentIndex{};
            auto const found = items.IndexOf(controls.root, currentIndex);
            if (!found || currentIndex != index)
            {
                if (found)
                {
                    items.RemoveAt(currentIndex);
                }
                items.InsertAt(index, controls.root);
            }
            auto const iconSize = MulDiv(24, GetDpiForWindow(m_hwnd), 96);
            auto const aumid = std::wstring_view(session.appId);
            auto const icon = m_icons.Load(session.executablePath, iconSize,
                aumid.starts_with(L"aumid:") ? aumid.substr(6) : std::wstring_view{});
            if (controls.icon.Source() != icon) controls.icon.Source(icon);
            bool const available = snapshot.deviceAvailable && !session.instanceId.empty();
            controls.available = available;
            if (available)
            {
                SetChannelTooltip(controls.optionsButton, session.displayName, session.volume, session.muted);
            }
            else
            {
                ToolTipService::SetToolTip(controls.optionsButton, box_value(session.displayName + L" (Inactive)"));
            }
            controls.percent.Text(available ? std::to_wstring(session.volume) + L"%" : L"\u2014");
            controls.percent.Visibility(m_settings.showChannelPercentages ? Visibility::Visible : Visibility::Collapsed);
            controls.muted = session.muted;
            controls.muteGlyph.Glyph(session.muted ? L"\uE74F" : L"\uE767");
            controls.root.Opacity(available ? 1.0 : 0.55);
            controls.slider.IsEnabled(available);
            controls.muteButton.IsEnabled(available);
            controls.resetItem.IsEnabled(available);
            auto const pinnedIndex = PinnedIndex(session.appId);
            controls.pinItem.Text(pinnedIndex < 0 ? L"Pin" : L"Unpin");
            controls.optionsButton.CanDrag(pinnedIndex >= 0);
            if (!controls.changing && available)
            {
                controls.slider.Value(session.volume);
            }
        }
        m_updating = false;
        if (oldCount != items.Size() && IsWindowVisible(m_hwnd))
        {
            PositionAt(m_anchor, false);
        }
    }

    void MainWindow::ApplyMasterVolume(::MasterVolumeState const& state)
    {
        static_cast<::MasterVolumeState&>(m_snapshot) = state;
        if (DeviceSlider().IsEnabled() != state.deviceAvailable)
        {
            DeviceSlider().IsEnabled(state.deviceAvailable);
            DeviceMuteButton().IsEnabled(state.deviceAvailable);
        }
        auto const glyph = state.masterMuted ? L"\uE74F" : L"\uE767";
        if (DeviceMuteGlyph().Glyph() != glyph)
        {
            DeviceMuteGlyph().Glyph(glyph);
        }
        if (SUCCEEDED(m_snapshot.deviceSwitchResult))
        {
            SetChannelTooltip(DeviceButton(), state.deviceAvailable ? m_snapshot.deviceName : L"No playback device",
                state.masterVolume, state.masterMuted);
        }
        if (!state.deviceAvailable)
        {
            DevicePercent().Text(L"\u2014");
        }
        else if (!m_deviceChanging && state.masterWrite >= m_requestedMasterWrite)
        {
            auto const wasUpdating = std::exchange(m_updating, true);
            DeviceSlider().Value(state.masterVolume);
            m_updating = wasUpdating;
            DevicePercent().Text(std::to_wstring(state.masterVolume) + L"%");
        }
    }

    void MainWindow::ApplyPeaks(::AudioPeakSnapshot const& snapshot)
    {
        if (!m_showMeters || snapshot.deviceId != m_snapshot.deviceId)
        {
            return;
        }
        // Keep the previous decay curve even when the timer is late or the UI skips samples.
        using Milliseconds = std::chrono::duration<float, std::milli>;
        auto const elapsed = m_lastPeakSample.time_since_epoch().count() == 0
            ? Milliseconds{20} : Milliseconds{snapshot.sampledAt - m_lastPeakSample};
        m_lastPeakSample = snapshot.sampledAt;
        auto const decay = std::pow(0.72f, elapsed.count() / 40.0f);
        m_deviceMeter.Update(snapshot.master, decay);
        for (auto const& peak : snapshot.sessions)
        {
            auto const found = m_sessions.find(peak.appId);
            if (found != m_sessions.end())
            {
                found->second.meter.Update(peak.levels, decay);
            }
        }
    }

    MainWindow::SessionControls& MainWindow::EnsureSession(std::wstring const& channelId, std::wstring const& appId, std::wstring const& displayName)
    {
        auto const existing = m_sessions.find(channelId);
        if (existing != m_sessions.end())
        {
            return existing->second;
        }

        SessionControls controls;
        controls.root = Controls::Border();
        controls.root.Width(m_settings.channelWidth);
        Controls::Grid layout;
        controls.root.Child(layout);
        controls.divider = Shapes::Rectangle();
        controls.divider.Width(1);
        controls.divider.Height(DividerHeight(MixerSurface().ActualHeight()));
        controls.divider.HorizontalAlignment(HorizontalAlignment::Left);
        controls.divider.VerticalAlignment(VerticalAlignment::Top);
        controls.divider.Fill(MixerSurface().BorderBrush());
        controls.divider.IsHitTestVisible(false);
        layout.Children().Append(controls.divider);
        Controls::StackPanel column;
        column.Margin(Thickness{1, 0, 0, 0});
        layout.Children().Append(column);

        controls.optionsButton = Controls::Button();
        auto iconButton = controls.optionsButton;
        iconButton.Width(32);
        iconButton.Height(32);
        iconButton.Padding(Thickness{ 0 });
        iconButton.Background(Media::SolidColorBrush(Windows::UI::Color{0, 0, 0, 0}));
        iconButton.BorderThickness(Thickness{0});
        iconButton.HorizontalAlignment(HorizontalAlignment::Center);
        ToolTipService::SetToolTip(iconButton, box_value(L"Channel options"));
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(iconButton, displayName + L" options");
        controls.icon = Controls::Image();
        controls.icon.Width(24);
        controls.icon.Height(24);
        iconButton.Content(controls.icon);

        controls.slider = Controls::Slider();
        controls.slider.Orientation(Controls::Orientation::Vertical);
        controls.slider.Minimum(0);
        controls.slider.Maximum(100);
        controls.slider.SmallChange(1);
        controls.slider.LargeChange(10);
        controls.slider.Height(DeviceSlider().Height());
        controls.slider.Margin(DeviceSlider().Margin());
        controls.slider.FocusVisualMargin(Thickness{-4, -8, -4, -8});
        controls.slider.Width(32);
        controls.slider.HorizontalAlignment(HorizontalAlignment::Center);
        Automation::AutomationProperties::SetName(controls.slider, displayName + L" volume");
        EnableWheel(controls.slider);

        controls.meter = InstallPeakMeter(controls.slider);

        controls.percent = Controls::TextBlock();
        controls.percent.Margin(DevicePercent().Margin());
        controls.percent.FontSize(12);
        controls.percent.TextAlignment(TextAlignment::Center);

        controls.muteButton = Controls::Button();
        controls.muteButton.Width(32);
        controls.muteButton.Height(32);
        controls.muteButton.Padding(Thickness{ 0 });
        controls.muteButton.Background(Media::SolidColorBrush(Windows::UI::Color{0, 0, 0, 0}));
        controls.muteButton.BorderThickness(Thickness{0});
        controls.muteButton.HorizontalAlignment(HorizontalAlignment::Center);
        Automation::AutomationProperties::SetName(controls.muteButton, L"Mute " + displayName);
        controls.muteGlyph = Controls::FontIcon();
        controls.muteGlyph.FontSize(16);
        controls.muteButton.Content(controls.muteGlyph);

        controls.menu = Controls::MenuFlyout();
        auto menu = controls.menu;
        controls.root.ContextFlyout(menu);
        controls.root.AllowDrop(true);
        controls.root.DragOver([this](IInspectable const&, DragEventArgs const& args)
        {
            if (!m_draggedAppId.empty())
            {
                args.AcceptedOperation(Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move);
            }
        });
        controls.root.Drop([this, targetAppId = appId](IInspectable const&, DragEventArgs const& args)
        {
            if (!m_draggedAppId.empty())
            {
                ReorderPinned(m_draggedAppId, targetAppId);
            }
            m_draggedAppId.clear();
            args.Handled(true);
        });
        controls.pinItem = Controls::MenuFlyoutItem();
        controls.pinItem.Text(L"Pin");
        Controls::MenuFlyoutItem hideItem;
        hideItem.Text(L"Hide");
        controls.resetItem = Controls::MenuFlyoutItem();
        controls.resetItem.Text(L"Reset Volume (100%)");
        Controls::MenuFlyoutItem precise;
        precise.Text(L"Set volume…");
        precise.Click([this, channelId](auto const&, auto const&) { SetVolumePrecisely(channelId); });
        menu.Items().Append(controls.pinItem);
        menu.Items().Append(hideItem);
        menu.Items().Append(precise);
        menu.Items().Append(controls.resetItem);
        for (int direction : {-1, 1})
        {
            Controls::MenuFlyoutItem move;
            move.Text(direction < 0 ? L"Move left" : L"Move right");
            move.Click([this, appId, direction](auto const&, auto const&)
            {
                auto const index = PinnedIndex(appId);
                if (index >= 0)
                {
                    auto const target = std::ranges::find(m_settings.pinnedApplications, index + direction, &PinnedApplication::order);
                    if (target != m_settings.pinnedApplications.end())
                    {
                        ReorderPinned(appId, target->id);
                    }
                }
            });
            menu.Items().Append(move);
        }
        iconButton.DragStarting([this, appId](IInspectable const&, DragStartingEventArgs const& args)
        {
            m_draggedAppId = appId;
            args.Data().SetText(appId);
        });
        iconButton.DropCompleted([this](IInspectable const&, DropCompletedEventArgs const&)
        {
            m_draggedAppId.clear();
        });
        iconButton.Click([this, channelId](IInspectable const&, RoutedEventArgs const&)
        {
            auto const iterator = m_sessions.find(channelId);
            if (iterator != m_sessions.end())
            {
                iterator->second.menu.ShowAt(iterator->second.optionsButton);
            }
        });
        TrackFlyout(menu);
        column.Children().Append(iconButton);
        column.Children().Append(controls.slider);
        column.Children().Append(controls.muteButton);
        column.Children().Append(controls.percent);

        controls.slider.ValueChanged([this, channelId](IInspectable const&, Controls::Primitives::RangeBaseValueChangedEventArgs const& args)
        {
            auto const iterator = m_sessions.find(channelId);
            if (iterator != m_sessions.end())
            {
                iterator->second.percent.Text(std::to_wstring(static_cast<unsigned>(std::lround(args.NewValue()))) + L"%");
                if (!m_updating && m_audio && iterator->second.available)
                {
                    m_audio->SetApplicationVolume(channelId, args.NewValue() / 100.0, m_snapshot.deviceId);
                }
            }
        });
        controls.slider.PointerPressed([this, channelId](IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
        {
            auto const iterator = m_sessions.find(channelId);
            if (iterator != m_sessions.end())
            {
                iterator->second.changing = true;
            }
        });
        auto endChange = [this, channelId](IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
        {
            auto const iterator = m_sessions.find(channelId);
            if (iterator != m_sessions.end())
            {
                iterator->second.changing = false;
            }
        };
        controls.slider.PointerReleased(endChange);
        controls.slider.PointerCaptureLost(endChange);
        controls.muteButton.Click([this, channelId](IInspectable const&, RoutedEventArgs const&)
        {
            auto const iterator = m_sessions.find(channelId);
            if (iterator != m_sessions.end() && m_audio && iterator->second.available)
            {
                m_audio->SetApplicationMute(channelId, !iterator->second.muted);
            }
        });
        controls.pinItem.Click([this, appId](IInspectable const&, RoutedEventArgs const&) { TogglePin(appId); });
        hideItem.Click([this, appId](IInspectable const&, RoutedEventArgs const&) { HideApplication(appId); });
        controls.resetItem.Click([this, channelId](IInspectable const&, RoutedEventArgs const&)
        {
            auto const iterator = m_sessions.find(channelId);
            if (iterator != m_sessions.end() && m_audio && iterator->second.available)
            {
                m_audio->SetApplicationVolume(channelId, 1.0, m_snapshot.deviceId);
            }
        });

        return m_sessions.emplace(channelId, std::move(controls)).first->second;
    }

    int MainWindow::PinnedIndex(std::wstring const& appId) const
    {
        auto const found = std::find_if(m_settings.pinnedApplications.begin(), m_settings.pinnedApplications.end(), [&](PinnedApplication const& item) { return item.id == appId; });
        return found == m_settings.pinnedApplications.end() ? -1 : static_cast<int>(std::count_if(m_settings.pinnedApplications.begin(), m_settings.pinnedApplications.end(), [&](PinnedApplication const& item) { return item.order < found->order; }));
    }

    void MainWindow::TogglePin(std::wstring const& appId)
    {
        auto const found = std::find_if(m_settings.pinnedApplications.begin(), m_settings.pinnedApplications.end(), [&](PinnedApplication const& item) { return item.id == appId; });
        if (found != m_settings.pinnedApplications.end())
        {
            m_settings.pinnedApplications.erase(found);
        }
        else if (auto const session = std::find_if(m_snapshot.sessions.begin(), m_snapshot.sessions.end(), [&](AudioSessionModel const& item) { return item.appId == appId; }); session != m_snapshot.sessions.end())
        {
            PinnedApplication pinned;
            pinned.id = appId;
            pinned.displayName = session->displayName;
            pinned.executablePath = session->executablePath;
            pinned.order = static_cast<int>(m_settings.pinnedApplications.size());
            m_settings.pinnedApplications.push_back(std::move(pinned));
        }
        LayoutChanged();
    }

    void MainWindow::ReorderPinned(std::wstring const& sourceAppId, std::wstring const& targetAppId)
    {
        if (sourceAppId == targetAppId)
        {
            return;
        }
        std::sort(m_settings.pinnedApplications.begin(), m_settings.pinnedApplications.end(), [](PinnedApplication const& left, PinnedApplication const& right) { return left.order < right.order; });
        auto const source = std::find_if(m_settings.pinnedApplications.begin(), m_settings.pinnedApplications.end(), [&](PinnedApplication const& item) { return item.id == sourceAppId; });
        auto const target = std::find_if(m_settings.pinnedApplications.begin(), m_settings.pinnedApplications.end(), [&](PinnedApplication const& item) { return item.id == targetAppId; });
        if (source == m_settings.pinnedApplications.end() || target == m_settings.pinnedApplications.end())
        {
            return;
        }
        auto const insertAfterTarget = source < target;
        auto moved = *source;
        m_settings.pinnedApplications.erase(source);
        auto const newTarget = std::find_if(m_settings.pinnedApplications.begin(), m_settings.pinnedApplications.end(), [&](PinnedApplication const& item) { return item.id == targetAppId; });
        m_settings.pinnedApplications.insert(insertAfterTarget ? std::next(newTarget) : newTarget, std::move(moved));
        for (size_t index = 0; index < m_settings.pinnedApplications.size(); ++index)
        {
            m_settings.pinnedApplications[index].order = static_cast<int>(index);
        }
        LayoutChanged();
    }

    void MainWindow::HideApplication(std::wstring const& appId)
    {
        if (std::find(m_settings.hiddenApplications.begin(), m_settings.hiddenApplications.end(), appId) == m_settings.hiddenApplications.end())
        {
            m_settings.hiddenApplications.push_back(appId);
        }
        std::erase_if(m_settings.pinnedApplications, [&](PinnedApplication const& item) { return item.id == appId; });
        LayoutChanged();
    }

    void MainWindow::LayoutChanged()
    {
        std::sort(m_settings.pinnedApplications.begin(), m_settings.pinnedApplications.end(), [](PinnedApplication const& left, PinnedApplication const& right) { return left.order < right.order; });
        for (size_t index = 0; index < m_settings.pinnedApplications.size(); ++index)
        {
            m_settings.pinnedApplications[index].order = static_cast<int>(index);
        }
        SaveSettings();
        if (m_hasSnapshot)
        {
            ApplySnapshot(m_snapshot);
            SessionScrollViewer().ScrollToHorizontalOffset(0.0);
        }
    }

    void MainWindow::ShowAt(POINT anchor)
    {
        m_hideTimer.Stop();
        m_anchor = anchor;
        PositionAt(anchor, true);
        UpdateLiquidGlassTimer();
        SetForegroundWindow(m_hwnd);
        if (m_audio && m_showMeters)
        {
            m_audio->StartMetering();
        }
    }

    void MainWindow::PositionAt(POINT anchor, bool opening)
    {
        if (m_inSizeMove)
        {
            return;
        }
        auto const remembered = m_settings.popupPosition;
        MONITORINFO monitor{ sizeof(monitor) };
        auto const monitorHandle = MonitorFromPoint(remembered.value_or(anchor), MONITOR_DEFAULTTONEAREST);
        if (!GetMonitorInfoW(monitorHandle, &monitor))
        {
            return;
        }
        auto const& work = monitor.rcWork;
        auto const taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
        UINT dpi = GetDpiForWindow(m_hwnd);
        if (taskbar && MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST) == monitorHandle)
        {
            dpi = GetDpiForWindow(taskbar);
        }
        for (HWND bar{}; (bar = FindWindowExW(nullptr, bar, L"Shell_SecondaryTrayWnd", nullptr));)
        {
            if (MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST) == monitorHandle)
            {
                dpi = GetDpiForWindow(bar);
                break;
            }
        }
        auto const scale = [dpi](int value) { return MulDiv(value, dpi, 96); };
        auto const margin = scale(4);
        auto const channels = std::min(static_cast<int>(ApplicationColumns().Items().Size()) + 1, VisibleChannels);
        auto const defaultHeight = HeightForTracks(m_settings);
        auto const size = m_settings.popupSize.value_or(SIZE{HorizontalPadding + channels * m_settings.channelWidth, defaultHeight});
        auto const width = std::min(scale(std::max<LONG>(size.cx, MinimumWidth())), static_cast<int>(work.right - work.left));
        auto const minHeight = FaderPadding + MinimumFaderHeight + (m_settings.showChannelPercentages ? 24 : 0);
        auto const height = std::min(scale(std::max<LONG>(size.cy, minHeight)), static_cast<int>(work.bottom - work.top));
        auto const x = remembered ? std::clamp(remembered->x, work.left, work.right - width) : work.right - width;
        auto const y = remembered ? std::clamp(remembered->y, work.top, work.bottom - height)
            : anchor.y < work.top ? work.top + margin : std::max(work.top, work.bottom - height - margin);
        bool const onTop = m_pinned ? m_settings.pinnedAlwaysOnTop : m_settings.hideOnFocusLoss;
        SetWindowPos(m_hwnd, onTop ? HWND_TOPMOST : HWND_NOTOPMOST,
            x, y, width, height, opening ? SWP_SHOWWINDOW : SWP_NOACTIVATE);
    }

    void MainWindow::HideMixer()
    {
        m_liquidGlassTimer.Stop();
        ReleaseLiquidGlass();
        m_liquidRetryAt = 0;
        m_hideTimer.Stop();
        m_deviceChanging = false;
        if (m_audio)
        {
            m_audio->StopMetering();
        }
        m_lastPeakSample = {};
        m_deviceMeter.Clear();
        for (auto& [id, channel] : m_sessions)
        {
            channel.meter.Clear();
        }
        if (!m_destroying)
        {
            ShowWindow(m_hwnd, SW_HIDE);
        }
    }

    void MainWindow::Destroy()
    {
        m_destroying = true;
        m_liquidGlassTimer.Stop();
        ReleaseLiquidGlass();
        RemoveWindowSubclass(m_hwnd, WindowSubclass, 1);
        m_hideTimer.Stop();
        if (m_audio)
        {
            m_audio->StopMetering();
        }
        m_sessions.clear();
        m_audio = nullptr;
        if (m_acrylicController)
        {
            m_acrylicController.RemoveAllSystemBackdropTargets();
            m_acrylicController.Close();
        }
        if (m_clearGlassActive)
        {
            VolumeMixer::MainWindow window = *this;
            window.as<Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop>().SystemBackdrop(nullptr);
        }
        Close();
        m_clearGlassBrush = nullptr;
        m_glassCompositor = nullptr;
        if (m_systemDispatcher)
        {
            m_systemDispatcher.ShutdownQueueAsync();
        }
    }

    void MainWindow::UpdateMeterVisibility()
    {
        auto const visibility = m_showMeters ? Visibility::Visible : Visibility::Collapsed;
        m_deviceMeter.layers.Visibility(visibility);
        for (auto const& [id, controls] : m_sessions)
        {
            controls.meter.layers.Visibility(visibility);
        }
        if (m_audio && IsWindowVisible(m_hwnd))
        {
            if (m_showMeters)
            {
                m_audio->StartMetering();
            }
            else
            {
                m_audio->StopMetering();
            }
        }
    }

    void MainWindow::PinButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_hideTimer.Stop();
        m_pinned = !m_pinned;
        auto const label = m_pinned ? L"Unpin mixer window" : L"Pin mixer window";
        PinDot().Fill(m_pinned ? m_thumbBrush : m_railBrush);
        ToolTipService::SetToolTip(PinButton(), box_value(label));
        Automation::AutomationProperties::SetName(PinButton(), label);
        PositionAt(m_anchor, false);
    }

    void MainWindow::SettingsButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_settingsHandler)
        {
            m_settingsHandler();
        }
    }

    void MainWindow::DeviceSlider_ValueChanged(IInspectable const&, Controls::Primitives::RangeBaseValueChangedEventArgs const& args)
    {
        DevicePercent().Text(std::to_wstring(static_cast<unsigned>(std::lround(args.NewValue()))) + L"%");
        if (!m_updating && m_audio)
        {
            m_requestedMasterWrite = m_audio->SetMasterVolume(args.NewValue() / 100.0, m_snapshot.deviceId);
        }
    }

    void MainWindow::DeviceMuteButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_audio)
        {
            m_audio->ToggleMasterMute();
        }
    }

    void MainWindow::DeviceButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_audio)
        {
            m_audio->CycleDevice(m_settings.favoriteDevices);
        }
    }

    void MainWindow::TrackFlyout(Controls::Primitives::FlyoutBase const& menu)
    {
        menu.Opened([this](auto const&, auto const&)
        {
            m_flyoutOpen = true;
            m_hideTimer.Stop();
        });
        menu.Closed([this](auto const&, auto const&)
        {
            m_flyoutOpen = false;
            if (m_settings.hideOnFocusLoss && !m_pinned && GetForegroundWindow() != m_hwnd)
            {
                m_hideTimer.Start();
            }
        });
    }

    void MainWindow::EnableWheel(Controls::Slider const& slider)
    {
        slider.PointerWheelChanged([this, remainder = 0](IInspectable const& sender,
                Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) mutable
            {
                auto const control = sender.as<Controls::Slider>();
                if (!control.IsEnabled())
                {
                    return;
                }
                remainder += args.GetCurrentPoint(control).Properties().MouseWheelDelta();
                int const steps = remainder / WHEEL_DELTA;
                remainder %= WHEEL_DELTA;
                if (steps)
                {
                    control.Value(std::clamp(control.Value() + steps * m_settings.wheelStep, 0.0, 100.0));
                }
                args.Handled(true);
            });
    }

    void MainWindow::SetVolumePrecisely(std::wstring const& appId)
    {
        auto const channel = m_sessions.find(appId);
        if (!m_audio || (appId.empty() ? !m_snapshot.deviceAvailable : channel == m_sessions.end() || !channel->second.available))
        {
            return;
        }
        Controls::NumberBox value;
        value.Header(box_value(L"Volume (%)"));
        value.Minimum(0);
        value.Maximum(100);
        value.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Inline);
        value.Value(appId.empty() ? DeviceSlider().Value() : channel->second.slider.Value());
        Controls::StackPanel fields;
        fields.Spacing(8);
        Controls::Button apply;
        apply.Content(box_value(L"Apply"));
        fields.Children().Append(value);
        fields.Children().Append(apply);
        m_precisionFlyout = Controls::Flyout();
        m_precisionFlyout.Content(fields);
        TrackFlyout(m_precisionFlyout);
        apply.Click([this, appId, value](auto const&, auto const&)
        {
            if (m_audio && std::isfinite(value.Value()))
            {
                if (appId.empty())
                {
                    m_requestedMasterWrite = m_audio->SetMasterVolume(value.Value() / 100.0, m_snapshot.deviceId);
                }
                else
                {
                    m_audio->SetApplicationVolume(appId, value.Value() / 100.0, m_snapshot.deviceId);
                }
                m_precisionFlyout.Hide();
            }
        });
        DispatcherQueue().TryEnqueue([weak = get_weak(), anchor = appId.empty() ? DeviceButton() : channel->second.optionsButton]
        {
            if (auto self = weak.get(); self && !self->m_destroying)
            {
                self->m_precisionFlyout.ShowAt(anchor);
            }
        });
    }

    void MainWindow::BeginSizeMove(int hit, POINT cursor, bool native)
    {
        if (!m_inSizeMove && GetWindowRect(m_hwnd, &m_sizeMoveBounds))
        {
            m_inSizeMove = true;
            m_nativeSizeMove = native;
            m_sizeMoveHit = hit;
            m_sizeMoveStart = cursor;
            m_hideTimer.Stop();
            UpdateResizeCursor(hit);
        }
    }

    void MainWindow::UpdateResizeCursor(int hit)
    {
        SetResizeCursor(hit);
        if (m_cursorHit == hit) return;
        m_cursorHit = hit;

        using namespace Microsoft::UI::Input;
        auto shape = InputSystemCursorShape::Arrow;
        switch (hit)
        {
        case HTLEFT: case HTRIGHT: shape = InputSystemCursorShape::SizeWestEast; break;
        case HTTOP: case HTBOTTOM: shape = InputSystemCursorShape::SizeNorthSouth; break;
        case HTTOPLEFT: case HTBOTTOMRIGHT: shape = InputSystemCursorShape::SizeNorthwestSoutheast; break;
        case HTTOPRIGHT: case HTBOTTOMLEFT: shape = InputSystemCursorShape::SizeNortheastSouthwest; break;
        }
        if (auto const element = MixerSurface().try_as<IUIElementProtected>())
        {
            element.ProtectedCursor(shape == InputSystemCursorShape::Arrow ? nullptr : InputSystemCursor::Create(shape));
        }
    }

    void MainWindow::UpdateSizeMove(POINT cursor)
    {
        if (!m_inSizeMove)
        {
            return;
        }
        auto bounds = m_sizeMoveBounds;
        auto const dx = cursor.x - m_sizeMoveStart.x;
        auto const dy = cursor.y - m_sizeMoveStart.y;
        auto const hit = m_sizeMoveHit;
        SetResizeCursor(hit);
        if (hit == HTCAPTION)
        {
            OffsetRect(&bounds, dx, dy);
        }
        else
        {
            auto const dpi = GetDpiForWindow(m_hwnd);
            LONG const minimumWidth = MulDiv(MinimumWidth(), dpi, 96);
            LONG const minimumHeight = MulDiv(FaderPadding + MinimumFaderHeight + (m_settings.showChannelPercentages ? 24 : 0), dpi, 96);
            if (hit == HTLEFT || hit == HTTOPLEFT || hit == HTBOTTOMLEFT)
            {
                bounds.left = std::min(bounds.left + dx, bounds.right - minimumWidth);
            }
            if (hit == HTRIGHT || hit == HTTOPRIGHT || hit == HTBOTTOMRIGHT)
            {
                bounds.right = std::max(bounds.right + dx, bounds.left + minimumWidth);
            }
            if (hit == HTTOP || hit == HTTOPLEFT || hit == HTTOPRIGHT)
            {
                bounds.top = std::min(bounds.top + dy, bounds.bottom - minimumHeight);
            }
            if (hit == HTBOTTOM || hit == HTBOTTOMLEFT || hit == HTBOTTOMRIGHT)
            {
                bounds.bottom = std::max(bounds.bottom + dy, bounds.top + minimumHeight);
            }
        }
        SetWindowPos(m_hwnd, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
            SWP_NOACTIVATE | SWP_NOZORDER);
    }

    void MainWindow::EndSizeMove()
    {
        m_inSizeMove = false;
        UpdateResizeCursor(HTCLIENT);
        RECT bounds{};
        if (!GetWindowRect(m_hwnd, &bounds) || EqualRect(&bounds, &m_sizeMoveBounds))
        {
            return;
        }
        if (m_sizeMoveHit == HTCAPTION || m_settings.popupPosition)
        {
            m_settings.popupPosition = POINT{bounds.left, bounds.top};
        }
        if (m_sizeMoveHit != HTCAPTION)
        {
            auto const dpi = GetDpiForWindow(m_hwnd);
            m_settings.popupSize = SIZE{MulDiv(bounds.right - bounds.left, 96, dpi),
                MulDiv(bounds.bottom - bounds.top, 96, dpi)};
            ResizeFaders(MixerSurface().ActualHeight());
        }
        SaveSettings();
    }

    void MainWindow::SaveSettings()
    {
        if (m_settingsChangedHandler)
        {
            SettingsSaveError().IsOpen(!m_settingsChangedHandler(m_settings));
        }
    }

    void MainWindow::RetrySettingsSave_Click(IInspectable const&, RoutedEventArgs const&)
    {
        SaveSettings();
    }

    LRESULT CALLBACK MainWindow::WindowSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR, DWORD_PTR data)
    {
        auto self = reinterpret_cast<MainWindow*>(data);
        switch (message)
        {
        case WM_ERASEBKGND:
            if (self->m_clearGlassActive)
            {
                RECT client{};
                if (GetClientRect(hwnd, &client))
                {
                    FillRect(reinterpret_cast<HDC>(wParam), &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                    return TRUE;
                }
            }
            break;
        case WM_DWMCOMPOSITIONCHANGED:
            if (self->m_clearGlassActive) SetClearWindowBackground(hwnd, true);
            break;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCPAINT:
            return 0;
        case WM_NCACTIVATE:
            return DefSubclassProc(hwnd, message, wParam, -1);
        case WM_STYLECHANGING:
            if (wParam == static_cast<WPARAM>(GWL_STYLE))
            {
                reinterpret_cast<STYLESTRUCT*>(lParam)->styleNew &= ~WS_CAPTION;
            }
            break;
        case WM_NCHITTEST:
            return ResizeHit(hwnd, {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        case WM_SETCURSOR:
        {
            auto const hit = self->m_inSizeMove ? self->m_sizeMoveHit : LOWORD(lParam);
            if (hit >= HTLEFT && hit <= HTBOTTOMRIGHT)
            {
                SetResizeCursor(hit);
                return TRUE;
            }
            break;
        }
        case WM_NCLBUTTONDOWN:
            if (wParam >= HTLEFT && wParam <= HTBOTTOMRIGHT)
            {
                self->BeginSizeMove(static_cast<int>(wParam), {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, true);
                SetCapture(hwnd);
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
            if (self->m_inSizeMove && self->m_nativeSizeMove)
            {
                POINT cursor{};
                GetCursorPos(&cursor);
                self->UpdateSizeMove(cursor);
                return 0;
            }
            break;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (self->m_inSizeMove && self->m_nativeSizeMove)
            {
                self->EndSizeMove();
                if (GetCapture() == hwnd) ReleaseCapture();
                return 0;
            }
            break;
        case WM_GETMINMAXINFO:
        {
            auto const info = reinterpret_cast<MINMAXINFO*>(lParam);
            auto const dpi = GetDpiForWindow(hwnd);
            auto const padding = FaderPadding + (self->m_settings.showChannelPercentages ? 24 : 0);
            info->ptMinTrackSize.x = MulDiv(self->MinimumWidth(), dpi, 96);
            info->ptMinTrackSize.y = MulDiv(padding + MinimumFaderHeight, dpi, 96);
            return 0;
        }
        default:
            break;
        }
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }

    void MainWindow::OnActivated(IInspectable const&, WindowActivatedEventArgs const& args)
    {
        m_hideTimer.Stop();
        if (m_settings.hideOnFocusLoss && !m_pinned && !m_flyoutOpen && !m_inSizeMove && args.WindowActivationState() == WindowActivationState::Deactivated)
        {
            m_hideTimer.Start();
        }
    }
}
