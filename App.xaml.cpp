#include "pch.h"
#include "App.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::VolumeMixer::implementation
{
    App::App()
    {
#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const&)
        {
            if (IsDebuggerPresent())
            {
                __debugbreak();
            }
        });
#endif
    }

    void App::OnLaunched(LaunchActivatedEventArgs const&)
    {
        try
        {
            m_controller = std::make_unique<::ApplicationController>();
            if (!m_controller->Initialize())
            {
                m_controller.reset();
                Exit();
                PostQuitMessage(0);
            }
        }
        catch (winrt::hresult_error const& error)
        {
            MessageBoxW(nullptr, error.message().c_str(), L"Volume Mixer startup error", MB_OK | MB_ICONERROR);
            Exit();
            PostQuitMessage(0);
        }
        catch (std::exception const& error)
        {
            MessageBoxW(nullptr, winrt::to_hstring(error.what()).c_str(), L"Volume Mixer startup error", MB_OK | MB_ICONERROR);
            Exit();
            PostQuitMessage(0);
        }
    }
}
