// Shared state between the user32 window layer and the D3D9 / DirectInput layers.
#pragma once
#include <SDL3/SDL.h>
#include <cstdint>

void        ui_ensure_sdl();
void        ui_pump();                       // drain SDL events into the Win32 message queue
SDL_Window *ui_sdl_window(uint32_t hwnd);    // 0 -> main window
uint32_t    ui_main_hwnd();
uint32_t    ui_dik_of(SDL_Scancode s);       // SDL scancode -> DirectInput DIK_* code
extern void (*ui_extra_event_hook)(const SDL_Event &e);
static inline void ui_extra_event(const SDL_Event &e) { if (ui_extra_event_hook) ui_extra_event_hook(e); }
bool        ui_key_injected(SDL_Scancode s);  // debug key script (FVR_KEYS)
bool        ui_scripted();                    // FVR_KEYS set: real keyboard and mouse are ignored
