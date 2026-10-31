#include "remix_ui.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <array>
#include <cstdio>
#include <string>

namespace proxy::remix_ui {
namespace {

HWND g_window = nullptr;
WNDPROC g_previous = nullptr;
WNDPROC g_gameProc = nullptr;
remixapi_Interface *g_api = nullptr;
bool g_active = false;
bool g_hovered = false;
unsigned g_injected = 0, g_rejected = 0;
char g_status[160] = "not installed";
int g_pumpError = 0;
unsigned g_toggles = 0;

// Resolve the runtime's actual SDL module; do not load a second event queue.
template <class Fn> Fn sdl(const char *name) {
  HMODULE module = GetModuleHandleW(L"SDL3_shared.dll");
  if (!module) module = GetModuleHandleW(L"SDL3.dll");
  return module ? reinterpret_cast<Fn>(GetProcAddress(module, name)) : nullptr;
}

// IVGUI surface (VGUI_Surface030) slot 52: SetCursorAlwaysVisible. While it
// is set the engine shows the OS cursor and stops mouse look (verified in
// GMod's x64 vguimatsurface.dll: stores the flag at +0x3c8, read by slot 53
// IsCursorVisible).
void setCursorVisible(bool visible) {
  using CreateInterfaceFn = void *(*)(const char *, int *);
  static void *surface = [] {
    HMODULE m = GetModuleHandleA("vguimatsurface.dll");
    auto factory = m ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(m, "CreateInterface")) : nullptr;
    return factory ? factory("VGUI_Surface030", nullptr) : nullptr;
  }();
  if (surface)
    reinterpret_cast<void (*)(void *, bool)>((*static_cast<void ***>(surface))[52])(surface, visible);
}

SDL_Scancode scancode(uint32_t vk) {
  if (vk >= 'A' && vk <= 'Z')
    return SDL_Scancode(SDL_SCANCODE_A + (vk - 'A'));
  if (vk >= '1' && vk <= '9')
    return SDL_Scancode(SDL_SCANCODE_1 + (vk - '1'));
  if (vk >= VK_F1 && vk <= VK_F12)
    return SDL_Scancode(SDL_SCANCODE_F1 + (vk - VK_F1));
  switch (vk) {
  case '0': return SDL_SCANCODE_0;
  case VK_RETURN: return SDL_SCANCODE_RETURN;
  case VK_ESCAPE: return SDL_SCANCODE_ESCAPE;
  case VK_BACK: return SDL_SCANCODE_BACKSPACE;
  case VK_TAB: return SDL_SCANCODE_TAB;
  case VK_SPACE: return SDL_SCANCODE_SPACE;
  case VK_LEFT: return SDL_SCANCODE_LEFT;
  case VK_RIGHT: return SDL_SCANCODE_RIGHT;
  case VK_UP: return SDL_SCANCODE_UP;
  case VK_DOWN: return SDL_SCANCODE_DOWN;
  case VK_HOME: return SDL_SCANCODE_HOME;
  case VK_END: return SDL_SCANCODE_END;
  case VK_PRIOR: return SDL_SCANCODE_PAGEUP;
  case VK_NEXT: return SDL_SCANCODE_PAGEDOWN;
  case VK_INSERT: return SDL_SCANCODE_INSERT;
  case VK_DELETE: return SDL_SCANCODE_DELETE;
  case VK_SHIFT: case VK_LSHIFT: return SDL_SCANCODE_LSHIFT;
  case VK_RSHIFT: return SDL_SCANCODE_RSHIFT;
  case VK_CONTROL: case VK_LCONTROL: return SDL_SCANCODE_LCTRL;
  case VK_RCONTROL: return SDL_SCANCODE_RCTRL;
  case VK_MENU: case VK_LMENU: return SDL_SCANCODE_LALT;
  case VK_RMENU: return SDL_SCANCODE_RALT;
  case VK_OEM_MINUS: return SDL_SCANCODE_MINUS;
  case VK_OEM_PLUS: return SDL_SCANCODE_EQUALS;
  case VK_OEM_PERIOD: return SDL_SCANCODE_PERIOD;
  case VK_OEM_COMMA: return SDL_SCANCODE_COMMA;
  default: return SDL_SCANCODE_UNKNOWN;
  }
}

SDL_Keymod modifier(SDL_Scancode code) {
  switch (code) {
  case SDL_SCANCODE_LSHIFT: return SDL_KMOD_LSHIFT;
  case SDL_SCANCODE_RSHIFT: return SDL_KMOD_RSHIFT;
  case SDL_SCANCODE_LCTRL: return SDL_KMOD_LCTRL;
  case SDL_SCANCODE_RCTRL: return SDL_KMOD_RCTRL;
  case SDL_SCANCODE_LALT: return SDL_KMOD_LALT;
  case SDL_SCANCODE_RALT: return SDL_KMOD_RALT;
  default: return SDL_KMOD_NONE;
  }
}

std::string utf8(uint32_t c) {
  std::string out;
  if (c < 0x80) {
    out += char(c);
  } else if (c < 0x800) {
    out += char(0xc0 | (c >> 6));
    out += char(0x80 | (c & 0x3f));
  } else {
    out += char(0xe0 | (c >> 12));
    out += char(0x80 | ((c >> 6) & 0x3f));
    out += char(0x80 | (c & 0x3f));
  }
  return out;
}

float g_x = 0, g_y = 0;
SDL_Keymod g_mods = SDL_KMOD_NONE;
SDL_MouseButtonFlags g_buttons = 0;
// SDL text events point at their text until the event is processed.
std::array<std::string, 64> g_text;
size_t g_textIndex = 0;

enum class Input { Motion, Button, Wheel, Key, Text, Focus, Hover };

// Pushes one window input event to the runtime's SDL queue.
void inject(Input type, uint32_t code, bool down, float x = 0, float y = 0, float wheel = 0) {
  static auto push = sdl<bool (*)(SDL_Event *)>("SDL_PushEvent");
  static auto windows = sdl<SDL_Window **(*)(int *)>("SDL_GetWindows");
  static auto windowId = sdl<SDL_WindowID (*)(SDL_Window *)>("SDL_GetWindowID");
  static auto properties = sdl<SDL_PropertiesID (*)(SDL_Window *)>("SDL_GetWindowProperties");
  static auto pointerProperty = sdl<void *(*)(SDL_PropertiesID, const char *, void *)>("SDL_GetPointerProperty");
  static auto sdlFree = sdl<void (*)(void *)>("SDL_free");
  static auto ticks = sdl<Uint64 (*)()>("SDL_GetTicksNS");
  static auto keyFromScancode = sdl<SDL_Keycode (*)(SDL_Scancode, SDL_Keymod, bool)>("SDL_GetKeyFromScancode");
  if (!push || !windows || !windowId || !properties || !pointerProperty || !sdlFree || !ticks || !keyFromScancode) {
    ++g_rejected;
    return;
  }
  int count = 0;
  SDL_Window **list = windows(&count);
  SDL_WindowID id = 0;
  for (int i = 0; list && i < count; ++i)
    if (pointerProperty(properties(list[i]), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr) == g_window) {
      id = windowId(list[i]);
      break;
    }
  sdlFree(list);
  if (!id) {
    ++g_rejected;
    return;
  }

  SDL_Event e = {};
  e.common.timestamp = ticks();
  switch (type) {
  case Input::Motion:
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = id;
    e.motion.state = g_buttons;
    e.motion.x = x;
    e.motion.y = y;
    e.motion.xrel = x - g_x;
    e.motion.yrel = y - g_y;
    g_x = x;
    g_y = y;
    break;
  case Input::Button: {
    const Uint8 button = code == 1 ? SDL_BUTTON_LEFT : code == 2 ? SDL_BUTTON_MIDDLE : SDL_BUTTON_RIGHT;
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = id;
    e.button.button = button;
    e.button.down = down;
    e.button.clicks = 1;
    e.button.x = x;
    e.button.y = y;
    if (down)
      g_buttons |= SDL_BUTTON_MASK(button);
    else
      g_buttons &= ~SDL_BUTTON_MASK(button);
    break;
  }
  case Input::Wheel:
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.windowID = id;
    e.wheel.y = wheel;
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    e.wheel.mouse_x = g_x;
    e.wheel.mouse_y = g_y;
    break;
  case Input::Key: {
    const SDL_Scancode sc = scancode(code);
    if (sc == SDL_SCANCODE_UNKNOWN)
      return;
    if (const SDL_Keymod mod = modifier(sc))
      g_mods = down ? SDL_Keymod(g_mods | mod) : SDL_Keymod(g_mods & ~mod);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = id;
    e.key.scancode = sc;
    e.key.key = keyFromScancode(sc, g_mods, false);
    e.key.mod = g_mods;
    e.key.down = down;
    break;
  }
  case Input::Text: {
    if (!code)
      return;
    auto &text = g_text[g_textIndex++ % g_text.size()];
    text = utf8(code);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.windowID = id;
    e.text.text = text.c_str();
    break;
  }
  case Input::Focus:
    e.type = down ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
    e.window.windowID = id;
    if (!down) {
      g_mods = SDL_KMOD_NONE;
      g_buttons = 0;
    }
    break;
  case Input::Hover:
    e.type = down ? SDL_EVENT_WINDOW_MOUSE_ENTER : SDL_EVENT_WINDOW_MOUSE_LEAVE;
    e.window.windowID = id;
    break;
  }
  if (push(&e)) ++g_injected;
  else ++g_rejected;
}

void hover(bool inside) {
  if (g_hovered == inside) return;
  g_hovered = inside;
  inject(Input::Hover, 0, inside);
  if (inside) {
    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, g_window, 0};
    TrackMouseEvent(&track);
  }
}

// Opens or closes the panel on the proxy's side: input capture and cursor.
void setActive(bool active) {
  g_active = active;
  ++g_toggles;
  if (g_active) {
    // Drop what SDL's own window procedure queued while nobody pumped.
    if (auto flush = sdl<void (*)(Uint32, Uint32)>("SDL_FlushEvents"))
      flush(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    POINT p;
    if (GetCursorPos(&p) && ScreenToClient(g_window, &p)) {
      g_x = float(p.x);
      g_y = float(p.y);
    }
  }
  hover(g_active);
  inject(Input::Focus, 0, g_active);
  if (g_active) inject(Input::Motion, 0, false, g_x, g_y);
  setCursorVisible(g_active);
}

// The runtime handles its own shortcuts (the panel's, screenshots, scene
// captures) while pumping: follow the panel state it leaves.
void pump() {
  const remixapi_ErrorCode r = g_api->PumpEvents();
  if (r != REMIXAPI_ERROR_CODE_SUCCESS)
    g_pumpError = int(r);
  const bool open = g_api->GetUIState && g_api->GetUIState() != REMIXAPI_UI_STATE_NONE;
  if (open != g_active)
    setActive(open);
}

void toggle() {
  setActive(!g_active);
  g_api->SetUIState(g_active ? REMIXAPI_UI_STATE_ADVANCED : REMIXAPI_UI_STATE_NONE);
  pump();
}

// True: the message was taken for the panel.
bool forward(UINT message, WPARAM w, LPARAM l) {
  const float x = float(short(LOWORD(l))), y = float(short(HIWORD(l)));
  switch (message) {
  case WM_MOUSEMOVE:
    // We consume WM_MOUSEMOVE before SDL's window procedure can emit enter.
    // Without it ImGui falls back to polling the host's warped mouse position.
    hover(true);
    inject(Input::Motion, 0, false, x, y);
    break;
  case WM_MOUSELEAVE: hover(false); break;
  case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: inject(Input::Button, 1, true, x, y); break;
  case WM_LBUTTONUP: inject(Input::Button, 1, false, x, y); break;
  case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: inject(Input::Button, 2, true, x, y); break;
  case WM_MBUTTONUP: inject(Input::Button, 2, false, x, y); break;
  case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: inject(Input::Button, 3, true, x, y); break;
  case WM_RBUTTONUP: inject(Input::Button, 3, false, x, y); break;
  case WM_MOUSEWHEEL: inject(Input::Wheel, 0, false, 0, 0, float(GET_WHEEL_DELTA_WPARAM(w)) / WHEEL_DELTA); break;
  case WM_KEYDOWN: case WM_SYSKEYDOWN: inject(Input::Key, uint32_t(w), true); break;
  case WM_KEYUP: case WM_SYSKEYUP:
    inject(Input::Key, uint32_t(w), false);
    pump();
    return false; // the game still sees key-ups (no stuck keys)
  case WM_CHAR: inject(Input::Text, uint32_t(w), true); break;
  case WM_INPUT: return true; // raw mouse would turn the camera
  default: return false;
  }
  pump();
  return true;
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == kToggleMessage) {
    toggle();
    return 0;
  }
  const bool key = message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP;
  const bool repeat = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && (l & (1 << 30));
  if (!g_active && key && !repeat) {
    // The game keeps its keys; the runtime sees them for its shortcuts, and a
    // key that opened the panel is the runtime's alone.
    inject(Input::Key, uint32_t(w), message == WM_KEYDOWN || message == WM_SYSKEYDOWN);
    pump();
    if (g_active)
      return 0;
  } else if (g_active) {
    if (message == WM_KILLFOCUS || message == WM_SETFOCUS) {
      if (message == WM_KILLFOCUS) hover(false);
      inject(Input::Focus, 0, message == WM_SETFOCUS);
      pump();
    } else if (forward(message, w, l)) {
      return 0;
    }
  } else if (message == WM_KILLFOCUS) {
    g_mods = SDL_KMOD_NONE; // Alt+Tab: Alt's key-up goes to the next window
  } else if (message == WM_SETCURSOR && g_gameProc) {
    // SDL's window procedure answers WM_SETCURSOR with its own cursor (an
    // arrow) on every mouse move over the client area, which the game then
    // hides again: a flickering cursor during mouse look on Windows. With the
    // panel closed the cursor is the game's alone.
    return CallWindowProcW(g_gameProc, window, message, w, l);
  }
  return CallWindowProcW(g_previous, window, message, w, l);
}

} // namespace

void install(HWND window, remixapi_Interface *api, WNDPROC gameProc) {
  if (g_previous || !window || !api)
    return;
  g_window = window;
  g_api = api;
  g_gameProc = gameProc;
  g_previous = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&windowProc)));
  std::snprintf(g_status, sizeof(g_status), "window %p subclassed %d (window thread %lu, startup thread %lu)",
                static_cast<void *>(window), g_previous != nullptr, GetWindowThreadProcessId(window, nullptr),
                GetCurrentThreadId());
}

void uninstall() {
  if (!g_previous)
    return;
  // Only when still on top; otherwise a later subclass chains through ours.
  if (GetWindowLongPtrW(g_window, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&windowProc))
    SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_previous));
  if (g_active)
    setCursorVisible(false);
  g_active = false;
  g_hovered = false;
  g_gameProc = nullptr;
  std::snprintf(g_status, sizeof(g_status), "uninstalled");
}

bool active() { return g_active; }

void open() {
  if (g_api && !g_active)
    toggle();
}

void dumpStatistics(FILE *out) {
  std::fprintf(out, "   Remix panel: %s; open %d, toggles %u, PumpEvents error %d; shared SDL %d, "
                   "injected %u, rejected %u, hovered %d\n", g_status, int(g_active),
               g_toggles, g_pumpError, GetModuleHandleW(L"SDL3_shared.dll") != nullptr,
               g_injected, g_rejected, int(g_hovered));
}

} // namespace proxy::remix_ui
