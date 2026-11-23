# Implementation notes

## Mixer and taskbar fixes

- The signal meter uses the Windows accent color and is centered in the fader track. The white volume fill meets a small white knob. Core Audio peaks are displayed on a linear scale with a smooth falloff.
- Acrylic opacity affects the background. Values from 0% to 100% were tested with Apply and by comparing screenshots.
- The widget preserves a safe position across layout measurements and restores its stacking order if the taskbar covers it. In a 16-second check, a layout notification was received and the widget retained its window handle.
- Settings has five tabs and a fixed footer with Apply. A setting change was verified on disk while the window stayed open.
- The knobs fit inside the native control cells. The mixer uses a single rounded outline and has a Pin control in the corner.
- The tray icon renderer validates alpha. Tray registration is retried, with a window-based identifier as a fallback if Explorer rejects the GUID. Explorer accepted the GUID in testing.
- When **Taskbar + Tray** was enabled, both the widget and the tray icon were present. Horizontal and vertical resize cursors were tested interactively.
- Device volume changes are delivered through separate messages. During a fast drag, the audio thread applies the latest pending value and the fader ignores acknowledgments for older commands.
- The widget retains its last measured position through brief UI Automation failures and tray size changes while that position remains free.
- The mixer opens at the right edge of the work area and saves its position and size. Knobs render above the tracks, and the viewport fits whole fader columns.
- The tray uses Segoe Fluent Icons for volume and mute. The default backdrop is Mica Alt, with Acrylic available in Settings.
- Release testing covered master fader dragging, external volume synchronization, left and top edge resizing, dragging the window by a blank area, and restoring its position after restart. In **Taskbar only** mode, the widget stayed visible with the same HWND for the six-second observation.

## Tests performed

- The Win32 prototype was tested on Windows 11 x64 26H2, build 26300.9974, at 125% scaling. Explorer rejected one external child window approach. A nonactivating Win32 window appeared in the free taskbar space and passed mouse hit testing.
- An x64 Release build was produced with the Windows App SDK deployed next to the executable.
- Release testing covered app startup, widget availability, showing and hiding the mixer, native sliders, external master volume synchronization, and per-app fader persistence.
- Windows routed a mouse wheel event that changed the output volume from 30% to 32%. The original volume was restored. Right-click opened the system menu, and the widget was recreated after a `TaskbarCreated` message.
- A quiet test signal verified the accent-colored meters inside the output and active application tracks.

## Checks after the code audit

- The previous settings file survived a write failure. After moving the mixer, Retry saved the updated coordinates.
- The mixer updates the affected Core Audio sessions when changing volume. In a test of 150 changes, the final command reached the audio session during shutdown. The meters ran at 49 Hz. When the UI was delayed, one notification remained pending.
- Switching the physical output was tested for the Console and Multimedia roles. Selecting Console alone changed both roles in Windows. The mixer detected the extra change and restored the original output. Communications stayed on its own device when switching the other two roles.
- After an Explorer restart, the widget returned in under a second and could open the mixer. The tray icon returned when the notification area finished loading. The widget remained accessible next to Wireless Battery in 58 checks.
- The Release x64 build occupied about 121 MiB at the time of the audit. Debug symbols and linker files were in the intermediate build directory.

## Scenarios pending verification

- Finding available favorite devices and recovering from an audio service failure.
- Hiding and reordering outputs in Settings and retrieving packaged app icons from AppsFolder.
- Multiple monitors with different DPI settings, scaling from 100% to 200%, resuming from sleep, and physically disconnecting USB, Bluetooth, and HDMI outputs.
- Long-running CPU, RAM, and handle measurements with the mixer visible and hidden. The Core Audio peak meter timer stops when the mixer is hidden.
- Testing on more Windows 11 configurations. The current checks used one monitor and one active output device.

## Technical limitations

- Windows 11 offers no public extension point for this type of widget. Placement depends on Explorer's taskbar, its UI Automation tree, and available space. Tray access is used for unsupported layouts, auto-hide, or full-screen mode.
- Mouse wheel routing depends on the Windows setting for scrolling inactive windows. Standard tray icons receive a limited set of events.
- Default output switching depends on the undocumented PolicyConfig interface. The UI displays the actual Core Audio state.
- The meter uses parts of the standard vertical Slider template to align with the track. Recheck these part names when updating the Windows App SDK.
