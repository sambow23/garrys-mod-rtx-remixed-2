#pragma once

#include <windows.h>

#include <remix/remix_c.h>

#include <cstdio>

namespace proxy::remix_ui {

// OpenRemix's developer panel on the game window. Remix runs in process on
// the game window (SDL wraps it), but nothing pumps SDL's queue for a
// borrowed window: the game window is subclassed here, its keys reach the
// runtime for its own shortcuts (the panel's, set in its header), and while
// the panel is open its input goes to the panel's SDL queue instead of the
// game (as l4d2-rtx + bridge do).
// Call on the thread that ran remixapi Startup (it owns the window's events).
// `gameProc`: the window procedure the game had before SDL wrapped the window.
void install(HWND window, remixapi_Interface *api, WNDPROC gameProc);

// Restores the window procedure (before the runtime releases its window).
void uninstall();

// Whether the panel is open.
bool active();

// Opens the panel unless it is open. On the thread that ran
// remixapi Startup.
void open();

// Posted to the game window, toggles the panel (dev aid).
constexpr UINT kToggleMessage = WM_APP + 0x7e1;

void dumpStatistics(FILE *out);

} // namespace proxy::remix_ui
