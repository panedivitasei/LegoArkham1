#include <windows.h>
#include <unknwn.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include "alt_f4.h"
#include "borderless.h"
#include "clips.h"
#include "device_scan.h"
#include "glide.h"
#include "grapple.h"
#include "keybinds.h"
#include "mouse_controls.h"
#include "orbit_camera.h"
#include "pause_save.h"
#include "player2.h"
#include "studs.h"
#include "vehicle.h"
#include "vsync_default.h"

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID riid, LPVOID* out,
                                             LPUNKNOWN outer) {
  static const auto real = [] {
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    strcat_s(path, "\\dinput8.dll");
    return reinterpret_cast<decltype(&DirectInput8Create)>(GetProcAddress(LoadLibraryA(path), "DirectInput8Create"));
  }();
  HRESULT hr = real ? real(instance, version, riid, out, outer) : E_FAIL;
  if (SUCCEEDED(hr) && riid.Data1 == 0xBF798030) devscan::Attach(*out);  // IID_IDirectInput8A
  return hr;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
  if (reason != DLL_PROCESS_ATTACH) return TRUE;
  DisableThreadLibraryCalls(self);

  std::string ini(MAX_PATH, '\0');
  ini.resize(GetModuleFileNameA(self, ini.data(), MAX_PATH));
  ini = ini.substr(0, ini.find_last_of('\\') + 1) + "LegoArkham.ini";

  HMODULE game = GetModuleHandleA(nullptr);
  clips::Install(ini);
  altf4::Install(game);
  vsync::DefaultOn(game);
  devscan::Install();
  if (GetPrivateProfileIntA("Display", "Borderless", 1, ini.c_str())) borderless::Install(game);
  if (GetPrivateProfileIntA("Players", "F2Toggle", 1, ini.c_str())) player2::Install();
  if (GetPrivateProfileIntA("Controls", "Mouse", 1, ini.c_str())) mousectl::Install(ini);
  if (GetPrivateProfileIntA("Controls", "ArkhamKeys", 1, ini.c_str())) keybinds::Install();
  if (GetPrivateProfileIntA("Controls", "Grapple", 1, ini.c_str())) grapple::Install(ini);
  if (GetPrivateProfileIntA("Vehicle", "Enabled", 1, ini.c_str())) vehicle::Install(ini);
  studs::Install();
  if (GetPrivateProfileIntA("Gameplay", "PauseMenuSave", 1, ini.c_str())) pausesave::Install();
  if (GetPrivateProfileIntA("Gameplay", "Glide", 1, ini.c_str())) {
    char boost[32];
    GetPrivateProfileStringA("Gameplay", "GlideBoost", "1.0", boost, sizeof(boost), ini.c_str());
    glide::Install(std::clamp(static_cast<float>(atof(boost)), 0.0f, 10.0f), ini);
  }
  if (GetPrivateProfileIntA("Camera", "Orbit", 1, ini.c_str())) {
    char distance[32];
    GetPrivateProfileStringA("Camera", "Distance", "2.0", distance, sizeof(distance), ini.c_str());
    orbit::Install(std::clamp(static_cast<float>(atof(distance)), 0.5f, 10.0f),
                   GetPrivateProfileIntA("Camera", "Collision", 1, ini.c_str()) != 0, ini);
  }

  return TRUE;
}
