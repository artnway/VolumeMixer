#pragma once

#include <mmdeviceapi.h>
#include <array>
#include <string>

namespace PlaybackRoleSwitch
{
    inline constexpr HRESULT IncompleteRollback = HRESULT_FROM_WIN32(ERROR_PARTIAL_COPY);
    inline constexpr HRESULT UnselectedRoleChanged = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x201);

    template<typename ReadDefault, typename SetDefault, typename Settle>
    HRESULT Apply(std::wstring const& target, unsigned mask, ReadDefault&& read, SetDefault&& set, Settle&& settle)
    {
        constexpr std::array roles{eConsole, eMultimedia, eCommunications};
        std::array<std::wstring, roles.size()> previous;
        for (auto const role : roles)
        {
            if (auto const status = read(role, previous[role]); FAILED(status)) return status;
        }

        HRESULT failure{S_OK};
        bool attempted{};
        for (auto const role : roles)
        {
            if (!(mask & (1u << role)) || previous[role] == target) continue;
            attempted = true;
            failure = set(role, target);
            if (FAILED(failure)) break;
        }
        if (attempted) settle();
        if (SUCCEEDED(failure))
        {
            for (auto const role : roles)
            {
                std::wstring current;
                failure = read(role, current);
                if (FAILED(failure)) break;
                bool const selected = (mask & (1u << role)) != 0;
                if (current != (selected ? target : previous[role]))
                {
                    failure = selected ? HRESULT_FROM_WIN32(ERROR_NOT_FOUND) : UnselectedRoleChanged;
                    break;
                }
            }
            if (SUCCEEDED(failure)) return S_OK;
        }

        for (auto role = roles.rbegin(); role != roles.rend(); ++role)
        {
            std::wstring current;
            auto const& original = previous[*role];
            if (FAILED(read(*role, current))) continue;
            // Preserve a different device that Windows or another app selected in the meantime.
            if (current == target && current != original && !original.empty()) set(*role, original);
        }
        if (attempted) settle();
        bool restored = true;
        for (auto const role : roles)
        {
            std::wstring current;
            if (FAILED(read(role, current)) || current != previous[role]) restored = false;
        }
        return restored ? failure : IncompleteRollback;
    }

    template<typename ReadDefault, typename SetDefault>
    HRESULT Apply(std::wstring const& target, unsigned mask, ReadDefault&& read, SetDefault&& set)
    {
        return Apply(target, mask, read, set, [] {});
    }
}
