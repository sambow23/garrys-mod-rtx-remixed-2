#include "remix_ui.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
HMODULE module;
template <class T> T function(const char *name) {
  const auto result = reinterpret_cast<T>(GetProcAddress(module, name));
  if (!result) std::abort();
  return result;
}
std::vector<SDL_Event> events;
remixapi_UIState state = REMIXAPI_UI_STATE_NONE;
remixapi_ErrorCode REMIXAPI_CALL setState(remixapi_UIState value) {
  state = value;
  return REMIXAPI_ERROR_CODE_SUCCESS;
}
remixapi_ErrorCode REMIXAPI_CALL pump() {
  static const auto poll = function<decltype(&SDL_PollEvent)>("SDL_PollEvent");
  SDL_Event event;
  while (poll(&event)) {
    events.push_back(event);
    // Simulate a runtime-selected shortcut, not an adapter-owned fixed F12 key.
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_F6)
      state = state == REMIXAPI_UI_STATE_NONE ? REMIXAPI_UI_STATE_ADVANCED : REMIXAPI_UI_STATE_NONE;
  }
  return REMIXAPI_ERROR_CODE_SUCCESS;
}
void require(bool value, const char *message) {
  if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
}
int main(int argc, char **argv) {
  require(argc == 2, "Supply the packaged SDL3_shared.dll");
  module = LoadLibraryA(argv[1]);
  require(module && GetModuleHandleW(L"SDL3_shared.dll") && !GetModuleHandleW(L"SDL3.dll"),
          "Test the actual packaged module name, without a legacy SDL3.dll masking the bug");
  const auto init = function<decltype(&SDL_Init)>("SDL_Init");
  const auto create = function<decltype(&SDL_CreateWindow)>("SDL_CreateWindow");
  const auto destroy = function<decltype(&SDL_DestroyWindow)>("SDL_DestroyWindow");
  const auto quit = function<decltype(&SDL_Quit)>("SDL_Quit");
  const auto properties = function<decltype(&SDL_GetWindowProperties)>("SDL_GetWindowProperties");
  const auto pointer = function<decltype(&SDL_GetPointerProperty)>("SDL_GetPointerProperty");
  const auto id = function<decltype(&SDL_GetWindowID)>("SDL_GetWindowID");
  require(init(SDL_INIT_VIDEO), "Initialize packaged SDL video");
  auto *target = create("Input bridge test", 320, 240, SDL_WINDOW_HIDDEN);
  auto *unrelated = create("Unrelated SDL window", 64, 64, SDL_WINDOW_HIDDEN);
  require(target && unrelated, "Create hidden test windows");
  const auto window = static_cast<HWND>(pointer(properties(target), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  require(window != nullptr, "Resolve the actual native target");
  const auto original = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));
  remixapi_Interface api{};
  api.SetUIState = setState;
  api.PumpEvents = pump;
  api.GetUIState = []() -> remixapi_UIState { return state; };
  proxy::remix_ui::install(window, &api, original);
  SendMessageW(window, WM_KEYDOWN, VK_F6, 0);
  require(proxy::remix_ui::active() && state == REMIXAPI_UI_STATE_ADVANCED, "Runtime shortcut opens the panel");
  SendMessageW(window, WM_KEYUP, VK_F6, 0);
  bool entered = false;
  for (const auto &event : events)
    entered |= event.type == SDL_EVENT_WINDOW_MOUSE_ENTER && event.window.windowID == id(target);
  require(entered, "Opening supplies hover state so ImGui trusts injected motion instead of host cursor warps");
  events.clear();
  SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(23, 31));
  SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(23, 31));
  SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(23, 31));
  bool motion = false, down = false, up = false;
  for (const auto &event : events) {
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
      require(event.motion.windowID == id(target) && event.motion.x == 23 && event.motion.y == 31,
              "Motion reaches the wrapped game window, never the first unrelated SDL window");
      motion = true;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
      require(event.button.windowID == id(target) && event.button.button == SDL_BUTTON_LEFT &&
                  event.button.x == 23 && event.button.y == 31, "Clicks preserve target and coordinates");
      (event.button.down ? down : up) = true;
    }
  }
  require(motion && down && up, "Packaged SDL receives motion and both click edges");
  events.clear();
  SendMessageW(window, WM_MOUSELEAVE, 0, 0);
  // Drain SDL's poll sentinel before injecting the next message. Production
  // pumps continuously on the render thread; this hidden-window harness does not.
  pump();
  SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(80, 40));
  pump();
  bool reentered = false;
  for (const auto &event : events) {
    if (event.type == SDL_EVENT_WINDOW_MOUSE_ENTER) reentered = event.window.windowID == id(target);
    if (event.type == SDL_EVENT_MOUSE_MOTION)
      require(reentered && event.motion.x == 80 && event.motion.y == 40,
              "Window re-entry restores hover before forwarding motion");
  }
  // SDL can coalesce a leave/enter pair for the same window while polling.
  require(reentered, "Leaving and returning cannot strand the panel in cursor-poll fallback");
  SendMessageW(window, WM_KEYDOWN, VK_F6, 0);
  require(!proxy::remix_ui::active() && state == REMIXAPI_UI_STATE_NONE, "Runtime shortcut restores the closed state");
  proxy::remix_ui::uninstall();
  require(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) == original,
          "Uninstall restores the previous window procedure");
  destroy(unrelated);
  destroy(target);
  quit();
  std::puts("Runtime shortcut bridge: packaged SDL module, native-window ownership, motion, click edges and teardown passed");
}
