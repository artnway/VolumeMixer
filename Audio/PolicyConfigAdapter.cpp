#include "pch.h"
#include "PolicyConfigAdapter.h"
#include "PlaybackRoleSwitch.h"
#include <mmreg.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wil/resource.h>

namespace
{
    struct DeviceShareMode;

    struct DECLSPEC_UUID("F8679F50-850A-41CF-9C72-430F290290C8") IPolicyConfig : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE GetMixFormat(LPCWSTR, WAVEFORMATEX**) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(LPCWSTR, INT, WAVEFORMATEX**) = 0;
        virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(LPCWSTR) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(LPCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(LPCWSTR, INT, PINT64, PINT64) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(LPCWSTR, PINT64) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetShareMode(LPCWSTR, DeviceShareMode*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetShareMode(LPCWSTR, DeviceShareMode*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(LPCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(LPCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(LPCWSTR, ERole) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(LPCWSTR, BOOL) = 0;
    };

    struct DECLSPEC_UUID("870AF99C-171D-4F9E-AF0D-E63DF40C2BC9") CPolicyConfigClient;
}

HRESULT PolicyConfigAdapter::SetDefaultPlaybackDevice(IMMDeviceEnumerator& devices, std::wstring const& deviceId, unsigned roles)
{
    if (deviceId.empty())
    {
        return E_INVALIDARG;
    }
    winrt::com_ptr<IPolicyConfig> policy;
    auto const result = CoCreateInstance(__uuidof(CPolicyConfigClient), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(policy.put()));
    if (FAILED(result))
    {
        return result;
    }
    auto const read = [&](ERole role, std::wstring& id) -> HRESULT
    {
        winrt::com_ptr<IMMDevice> device;
        auto const status = devices.GetDefaultAudioEndpoint(eRender, role, device.put());
        if (status == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) return S_OK;
        if (FAILED(status)) return status;
        wil::unique_cotaskmem_string endpointId;
        auto const result = device->GetId(endpointId.put());
        if (SUCCEEDED(result) && endpointId) id = endpointId.get();
        return result;
    };
    return PlaybackRoleSwitch::Apply(deviceId, roles, read, [&](ERole role, std::wstring const& id)
    {
        return policy->SetDefaultEndpoint(id.c_str(), role);
    },
    [] { Sleep(100); }); // The policy service publishes linked role changes asynchronously.
}
