#include "clips.h"

#include <windows.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <vector>

#include "hook.h"

namespace clips {
namespace {

// Clips and txts load from .fpk packs in both packed and extracted games, so the swap sits on pack find/fetch.
constexpr uintptr_t kPackFind = 0x6DCF10;  // (pack, name): entry index + 1, or 0
constexpr uintptr_t kPackData = 0x6D3D10;  // (pack, index, &data, &size): 1 on success
constexpr uintptr_t kPackFindCallSites[] = {0x599847, 0x5998B3, 0x599976, 0x5999E2, 0x599B36, 0x599B94, 0x599C5F, 0x599CEB,  // animations
                                            0x6293D7, 0x6294B0, 0x6295FD};                                                  // character txt
constexpr uintptr_t kPackDataCallSites[] = {0x599B4E, 0x599BAC, 0x599C77, 0x599D03, 0x6293F8, 0x6294C8, 0x629619};

// Levels flagged to skip packs (dword_A94750) load clips one by one through the file layer.
constexpr uintptr_t kFileExists = 0x6E3070;  // (name): nonzero if the DAT or the disk has it
constexpr uintptr_t kFileLoad = 0x737880;    // (name, &pool, poolEnd): the clip, loaded and fixed up
constexpr uintptr_t kFileExistsCallSite = 0x598F8B;
constexpr uintptr_t kFileLoadCallSite = 0x598FCA;

// Appended to a clip owner's txt when it has no glide anim (Robin).
constexpr char kGlideBlock[] = "\r\nanim_start=\"glide\"\r\naction=\"glide\"\r\nspeed=1.6\r\nno_headturn\r\nanim_end\r\n";

using PackFindFn = int(__cdecl*)(void* pack, const char* name);
using PackDataFn = int(__cdecl*)(void* pack, int index, void** data, DWORD* size);
using FileExistsFn = int(__cdecl*)(const char* name);
using FileLoadFn = void*(__cdecl*)(const char* name, void* pool, void* poolEnd);

// The game keeps pointers into these buffers and fixes them up in place, so they never move or free.
struct Clip {
  std::string game;  // the game's name for it, chars\<name>\glide_pc.an3
  std::string file;  // relative to the game folder, for the file layer
  std::string path;  // absolute
  std::vector<uint8_t> data;
};

struct Txt {
  std::string game;  // chars\<name>\<name>.txt
  std::vector<char> data;
};

std::vector<Clip> clips;
std::vector<Txt> txts;
Clip* pendingClip;  // target of the next pack fetch
Txt* pendingTxt;

bool Same(const char* a, const std::string& b) {
  if (!a) return false;
  size_t i = 0;
  for (; a[i] && i < b.size(); ++i) {
    char x = a[i] == '/' ? '\\' : static_cast<char>(tolower(static_cast<unsigned char>(a[i])));
    if (x != b[i]) return false;
  }
  return !a[i] && i == b.size();
}

Clip* FindClip(const char* name) {
  for (Clip& clip : clips)
    if (Same(name, clip.game)) return &clip;
  return nullptr;
}

Txt* FindTxt(const char* name) {
  for (Txt& txt : txts)
    if (Same(name, txt.game)) return &txt;
  return nullptr;
}

bool Load(Clip& clip) {
  if (!clip.data.empty()) return true;
  HANDLE file = CreateFileA(clip.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD size = GetFileSize(file, nullptr), read = 0;
  std::vector<uint8_t> data(size + 16);  // slack for the game's terminator write
  bool ok = size && ReadFile(file, data.data(), size, &read, nullptr) && read == size;
  CloseHandle(file);
  if (ok) clip.data = std::move(data);
  return ok;
}

bool HasGlide(const char* text, DWORD size) {
  std::string lower(text, size);
  for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return lower.find("action=\"glide\"") != std::string::npos;
}

int __cdecl HookPackFind(void* pack, const char* name) {
  int index = reinterpret_cast<PackFindFn>(kPackFind)(pack, name);
  pendingClip = nullptr;
  pendingTxt = nullptr;
  if (Clip* clip = FindClip(name); clip && Load(*clip)) {
    pendingClip = clip;
    return index ? index : 1;  // fetch is ours, pack entry optional
  }
  if (index) pendingTxt = FindTxt(name);
  return index;
}

int __cdecl HookPackData(void* pack, int index, void** data, DWORD* size) {
  Clip* clip = pendingClip;
  Txt* txt = pendingTxt;
  pendingClip = nullptr;
  pendingTxt = nullptr;
  if (clip) {
    if (data) *data = clip->data.data();
    if (size) *size = static_cast<DWORD>(clip->data.size() - 16);
    return 1;
  }
  void* original = nullptr;
  DWORD originalSize = 0;
  int ok = reinterpret_cast<PackDataFn>(kPackData)(pack, index, &original, &originalSize);
  if (ok && txt && txt->data.empty() && originalSize && !HasGlide(static_cast<char*>(original), originalSize)) {
    txt->data.assign(static_cast<char*>(original), static_cast<char*>(original) + originalSize);
    txt->data.insert(txt->data.end(), kGlideBlock, kGlideBlock + sizeof(kGlideBlock) - 1);
    txt->data.resize(txt->data.size() + 16);
  }
  if (ok && txt && !txt->data.empty()) {
    original = txt->data.data();
    originalSize = static_cast<DWORD>(txt->data.size() - 16);
  }
  if (data) *data = original;
  if (size) *size = originalSize;
  return ok;
}

int __cdecl HookFileExists(const char* name) {
  Clip* clip = FindClip(name);
  return reinterpret_cast<FileExistsFn>(kFileExists)(clip ? clip->file.c_str() : name);
}

void* __cdecl HookFileLoad(const char* name, void* pool, void* poolEnd) {
  Clip* clip = FindClip(name);
  return reinterpret_cast<FileLoadFn>(kFileLoad)(clip ? clip->file.c_str() : name, pool, poolEnd);
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

void Install(const std::string& ini) {
  std::string dir = ini.substr(0, ini.find_last_of('\\') + 1);
  std::vector<char> section(4096);
  DWORD n = GetPrivateProfileSectionA("GlideClips", section.data(), static_cast<DWORD>(section.size()), ini.c_str());
  for (const char* line = section.data(); line < section.data() + n && *line; line += strlen(line) + 1) {
    std::string entry = line;
    size_t eq = entry.find('=');
    if (eq == std::string::npos || *line == ';') continue;
    std::string name = Lower(entry.substr(0, eq)), file = entry.substr(eq + 1);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!file.empty() && file.front() == ' ') file.erase(0, 1);
    if (name.empty() || file.empty()) continue;
    DWORD attributes = GetFileAttributesA((dir + file).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    clips.push_back({"chars\\" + name + "\\glide_pc.an3", file, dir + file, {}});
    txts.push_back({"chars\\" + name + "\\" + name + ".txt", {}});
  }
  if (clips.empty()) return;
  for (uintptr_t site : kPackFindCallSites)
    hook::PatchCall(reinterpret_cast<void*>(site), reinterpret_cast<void*>(kPackFind), HookPackFind);
  for (uintptr_t site : kPackDataCallSites)
    hook::PatchCall(reinterpret_cast<void*>(site), reinterpret_cast<void*>(kPackData), HookPackData);
  hook::PatchCall(reinterpret_cast<void*>(kFileExistsCallSite), reinterpret_cast<void*>(kFileExists), HookFileExists);
  hook::PatchCall(reinterpret_cast<void*>(kFileLoadCallSite), reinterpret_cast<void*>(kFileLoad), HookFileLoad);
}

}  // namespace clips
