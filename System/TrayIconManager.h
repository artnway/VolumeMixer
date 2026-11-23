#pragma once

class TrayIconManager final
{
public:
    static constexpr UINT CallbackMessage = WM_APP + 1;
    static constexpr UINT_PTR RetryTimer = 101;

    ~TrayIconManager();
    bool Initialize(HWND owner, HICON icon, std::wstring const& tooltip);
    void Update(HICON icon, std::wstring const& tooltip);
    void Recreate();
    void EnsureRegistered();
    void Shutdown();
    bool TryGetIconRect(RECT& rect) const;

private:
    bool Add();

    NOTIFYICONDATAW m_data{};
    UINT m_identityFlags{NIF_GUID};
    bool m_added{ false };
};
