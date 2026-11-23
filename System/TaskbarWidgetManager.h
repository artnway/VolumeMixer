#pragma once

#include <windows.h>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class TaskbarWidgetManager final
{
public:
    static constexpr UINT LayoutMessage = WM_APP + 5;
    static constexpr UINT InputMessage = WM_APP + 6;
    static constexpr UINT_PTR LayoutTimer = 100;
    enum class Input { ToggleMixer, Menu, Mute, Volume, Device };

    ~TaskbarWidgetManager();
    void Initialize(HWND host);
    void SetEnabled(bool enabled);
    void RequestStop();
    bool IsStopped();
    void Stop();
    void Reset();
    void RecheckLayout();
    void SetLightTheme(bool light) { m_light = light; }
    bool ApplyLayout();
    void Update(bool available, unsigned volume, bool muted, bool headphones, bool showNumber, bool zeroWhenMuted,
        std::wstring const& tooltip);
    bool TryGetRect(RECT& rect) const;

private:
    struct Placement
    {
        HWND taskbar;
        RECT bar;
        RECT slot;
        UINT dpi;
        ULONGLONG measuredAt;
        RECT pendingCloserSlot{};
        bool provisional{};
    };

    struct Widget
    {
        TaskbarWidgetManager* manager;
        HWND taskbar;
        HWND window{};
        HWND tooltip{};
        RECT slot{};
        RECT taskbarBounds{};
        ULONGLONG lastPlacement{};
        UINT dpi{};
        bool hover{};
        bool light{};
        bool provisional{};
        int wheelRemainder{};
        int deviceWheelRemainder{};

        ~Widget();
        bool Paint() const;
        void UpdateTooltip() const;
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void Measure(std::stop_token stop);

    HWND m_host{};
    HWND m_primaryTaskbar{};
    RECT m_primaryTray{};
    std::vector<RECT> m_primaryCompanions;
    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::jthread m_worker;
    std::vector<Placement> m_placements;
    std::vector<HWND> m_taskbarsWithoutSpace;
    ULONGLONG m_measuredAt{};
    unsigned m_layoutRevision{};
    bool m_layoutCheckRequested{};
    bool m_enabled{};
    bool m_light{};
    bool m_available{};
    bool m_muted{};
    bool m_showNumber{true};
    bool m_headphones{};
    bool m_zeroWhenMuted{};
    unsigned m_volume{};
    std::wstring m_tooltip;
    std::vector<std::unique_ptr<Widget>> m_widgets;
};
