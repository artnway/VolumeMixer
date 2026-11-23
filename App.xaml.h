#pragma once

#include "App.xaml.g.h"
#include "App/ApplicationController.h"

namespace winrt::VolumeMixer::implementation
{
    struct App : AppT<App>
    {
        App();
        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

    private:
        std::unique_ptr<::ApplicationController> m_controller;
    };
}
