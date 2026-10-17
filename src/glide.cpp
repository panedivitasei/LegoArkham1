#include "glide.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <string>

#include "frame.h"
#include "hook.h"

namespace glide {
namespace {

// LEGOBatman.exe addresses. The exe has no ASLR, so these are absolute.
constexpr uintptr_t kBuildAbilitiesCallSite = 0x427157;  // call Character_BuildAbilities(object)
constexpr uintptr_t kBuildAbilitiesThunk = 0x4024B9;
constexpr uintptr_t kPlayerObjects = 0xAB3960;  // eight character pointers, player 1 first
constexpr uintptr_t kFrameTime = 0xA95FE0;      // float seconds

constexpr int kAbilitiesOffset = 1164;  // the mask the game consults for moves
constexpr DWORD kCanGlide = 4;          // what a character's `glide_anytime` property sets
constexpr int kDefinitionFlags = 324;   // on the character definition; the move check reads this one
constexpr DWORD kGlideAnytime = 0x40000;

constexpr int kYawOffset = 88;  // int16, 65536 per turn
constexpr int kPositionOffset = 92;
constexpr int kVelocityOffset = 104;
constexpr int kContextOffset = 2523;
constexpr uint8_t kGlideState = 79;  // the game's glide context (dword_960478 at run time)
constexpr float kPi = 3.14159265f;

using BuildFn = int(__cdecl*)(uint8_t* object);

float boost;  // extra forward speed while gliding, units per second

template <class T>
T& At(uintptr_t address) {
  return *reinterpret_cast<T*>(address);
}

template <class T>
T& Field(uint8_t* object, int offset) {
  return *reinterpret_cast<T*>(object + offset);
}

// A character's definition sits behind object+84, then +36; both players' current characters
// share it with any object of the same type, so a rebuilt object gets the glide as well.
void* Definition(uint8_t* object) {
  auto instance = Field<uint8_t*>(object, 84);
  return instance ? *reinterpret_cast<void**>(instance + 36) : nullptr;
}

// The mod's glide is for the listed characters in their default suits. Every suit entry in the
// game's table carries a suit id except the default ones, so "no id" is the default suit. Other
// suits keep the game's own air moves, and the Glide Suit its own glide, which its entry grants
// itself. Characters not on the list, gliders or not, are left entirely to the game.
constexpr int kSuitOffset = 4420;  // the worn suit's table entry; +12 is its id
constexpr int kCharacterIndexOffset = 5552;  // int16 into the character table
constexpr uintptr_t kCharacterTablePtr = 0xACB81C;  // 72-byte entries from chars.txt; +8 is the name
constexpr int kCharacterStride = 72;
constexpr int kCharacterName = 8;
std::string characters;  // ",name,name,": the whitelist, lower case

bool DefaultSuit(uint8_t* object) {
  auto suit = Field<uint8_t*>(object, kSuitOffset);
  return !suit || *reinterpret_cast<const char**>(suit + 12) == nullptr;
}

bool Listed(uint8_t* object) {
  auto table = At<uintptr_t>(kCharacterTablePtr);
  int index = Field<int16_t>(object, kCharacterIndexOffset);
  if (!table || index < 0) return false;
  auto name = *reinterpret_cast<const char**>(table + kCharacterStride * index + kCharacterName);
  if (!name) return false;
  std::string key = ",";
  for (const char* c = name; *c; ++c) key += static_cast<char>(tolower(static_cast<unsigned char>(*c)));
  key += ",";
  return characters.find(key) != std::string::npos;
}

// Grants or revokes the mod's glide on a listed player character: the definition flag the move
// check reads, and the ability bit, which a suit's own entry may also grant (the Glide Suit's does).
void EnforceGlide(uint8_t* object) {
  if (!Listed(object)) return;
  bool grant = DefaultSuit(object);
  if (auto definition = Definition(object)) {
    auto& flags = *reinterpret_cast<DWORD*>(static_cast<uint8_t*>(definition) + kDefinitionFlags);
    flags = grant ? flags | kGlideAnytime : flags & ~kGlideAnytime;
  }
  auto suit = Field<uint8_t*>(object, kSuitOffset);
  DWORD fromSuit = suit ? *reinterpret_cast<DWORD*>(suit + 68) & kCanGlide : 0;
  auto& abilities = Field<DWORD>(object, kAbilitiesOffset);
  abilities = (abilities & ~kCanGlide) | (grant ? kCanGlide : fromSuit);
}

int __cdecl HookBuildAbilities(uint8_t* object) {
  for (int player = 0; player < 2; ++player)
    if (object == At<uint8_t*>(kPlayerObjects + 4 * player)) EnforceGlide(object);
  return reinterpret_cast<BuildFn>(kBuildAbilitiesThunk)(object);  // rebuilds the mask from the flags
}

// The glide flag lives on the character definition, which both players share when they play
// the same character in different suits (Free Play allows it). The glide decision reads it off
// the definition during the character's own main update (jump held in the air: Character_Update
// at 0x4D7AE0, traced), and the animation update runs the state code, so before each of those
// the flag is set to that character's own answer.
constexpr uintptr_t kCharacterUpdateThunk = 0x401212;  // jmp Character_Update(object)
constexpr uintptr_t kCharacterUpdate = 0x4D7AE0;
constexpr uintptr_t kAnimUpdateThunk = 0x40119F;  // jmp Character_UpdateAnim(object): poses and runs the state
constexpr uintptr_t kAnimUpdate = 0x424090;
using UpdateFn = void(__cdecl*)(uint8_t* object);

void EnforceIfPlayer(uint8_t* object) {
  for (int player = 0; player < 2; ++player)
    if (object == At<uint8_t*>(kPlayerObjects + 4 * player)) EnforceGlide(object);
}

void __cdecl HookCharacterUpdate(uint8_t* object) {
  EnforceIfPlayer(object);
  reinterpret_cast<UpdateFn>(kCharacterUpdate)(object);
}

void __cdecl HookAnimUpdate(uint8_t* object) {
  EnforceIfPlayer(object);
  reinterpret_cast<UpdateFn>(kAnimUpdate)(object);
}

// The decision itself: Character_CheckGlide(object), called from three character update
// variants. Hooked at every call, so the flag is right whichever update a character takes.
constexpr uintptr_t kCheckGlide = 0x5D0400;
constexpr uintptr_t kCheckGlideCallSites[] = {0x4D7E2E, 0x4D8D0C, 0x4DB02E};
using CheckGlideFn = int(__cdecl*)(uint8_t* object);

int __cdecl HookCheckGlide(uint8_t* object) {
  EnforceIfPlayer(object);
  return reinterpret_cast<CheckGlideFn>(kCheckGlide)(object);
}

// The glide's motion: Character_GlideMove sets the horizontal pace along the facing and a sink
// rate (0.375/s for a player, ramped in over the first half second) that the movement eases the
// vertical velocity toward. With the default suits gliding, the Glide Suit climbs at that rate
// instead, so holding jump flies and letting go drops out of the glide as before.
constexpr uintptr_t kGlideMove = 0x5D0910;
constexpr uintptr_t kGlideMoveCallSites[] = {0x4BB6EB, 0x4D2C58};
constexpr int kVerticalTargetOffset = 5188;
constexpr int kSuitFlagsOffset = 20;  // on the suit's table entry
constexpr DWORD kSuitGlides = 2;      // the Glide Suit's own glide grant
using GlideMoveFn = int(__cdecl*)(uint8_t* object);
bool suitFlight = true;

bool IsPlayer(uint8_t* object) {
  for (int player = 0; player < 2; ++player)
    if (object == At<uint8_t*>(kPlayerObjects + 4 * player)) return true;
  return false;
}

bool GlideSuit(uint8_t* object) {
  auto suit = Field<uint8_t*>(object, kSuitOffset);
  return suit && *reinterpret_cast<const char**>(suit + 12) != nullptr &&
         (*reinterpret_cast<DWORD*>(suit + kSuitFlagsOffset) & kSuitGlides);
}

int __cdecl HookGlideMove(uint8_t* object) {
  int result = reinterpret_cast<GlideMoveFn>(kGlideMove)(object);
  auto& vertical = Field<float>(object, kVerticalTargetOffset);
  if (suitFlight && vertical < 0.0f && IsPlayer(object) && GlideSuit(object)) vertical = -vertical;
  return result;
}

constexpr uintptr_t kGamePads = 0xA96388;  // GAMEPAD[64], 96 bytes each: +0 NUPAD*, whose +112 is buttons
constexpr DWORD kJumpButton = 0x40;

bool JumpHeld(int player) {
  auto pad = At<uint8_t*>(kGamePads + 96 * player);
  return pad && (*reinterpret_cast<DWORD*>(pad + 112) & kJumpButton);
}

// Mod glide only (jump state, jump held), so the boost skips other suits and characters.
bool IsGliding(uint8_t* object, int player) {
  return Listed(object) && DefaultSuit(object) && Field<uint8_t>(object, kContextOffset) == kGlideState &&
         (JumpHeld(player) || Field<float>(object, kVelocityOffset + 4) < 0.3f);
}

// The game's glide keeps running pace. Push a gliding character along its facing so it carries.
void LevelUpdate() {
  float dt = At<float>(kFrameTime);
  for (int player = 0; player < 2; ++player) {
    auto object = At<uint8_t*>(kPlayerObjects + 4 * player);
    if (!object) continue;
    EnforceGlide(object);  // every frame: a suit change doesn't rebuild the abilities by itself
    if (!IsGliding(object, player)) continue;
    float yaw = Field<int16_t>(object, kYawOffset) * (2.0f * kPi / 65536.0f);
    Field<float>(object, kPositionOffset) += std::sin(yaw) * boost * dt;
    Field<float>(object, kPositionOffset + 8) += std::cos(yaw) * boost * dt;
  }
}

}  // namespace

void Install(float glideBoost, const std::string& ini) {
  boost = glideBoost;
  char list[512];
  GetPrivateProfileStringA("Gameplay", "GlideCharacters", "Batman,Robin,Batgirl,Azrael,Huntress", list, sizeof(list),
                           ini.c_str());
  characters = ",";
  for (const char* c = list; *c; ++c) {
    if (*c == ' ') continue;
    characters += static_cast<char>(tolower(static_cast<unsigned char>(*c)));
  }
  characters += ",";
  hook::PatchCall(reinterpret_cast<void*>(kBuildAbilitiesCallSite), reinterpret_cast<void*>(kBuildAbilitiesThunk),
                  HookBuildAbilities);
  hook::PatchCall(reinterpret_cast<void*>(kCharacterUpdateThunk), reinterpret_cast<void*>(kCharacterUpdate),
                  HookCharacterUpdate);
  hook::PatchCall(reinterpret_cast<void*>(kAnimUpdateThunk), reinterpret_cast<void*>(kAnimUpdate), HookAnimUpdate);
  for (uintptr_t site : kCheckGlideCallSites)
    hook::PatchCall(reinterpret_cast<void*>(site), reinterpret_cast<void*>(kCheckGlide), HookCheckGlide);
  suitFlight = GetPrivateProfileIntA("Gameplay", "GlideSuitFlight", 1, ini.c_str()) != 0;
  for (uintptr_t site : kGlideMoveCallSites)
    hook::PatchCall(reinterpret_cast<void*>(site), reinterpret_cast<void*>(kGlideMove), HookGlideMove);
  frame::OnLevelUpdate(LevelUpdate);
}

}  // namespace glide
