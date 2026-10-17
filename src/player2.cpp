#include "player2.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "grapple.h"
#include "hook.h"
#include "mouse_controls.h"
#include "vehicle.h"

namespace player2 {
namespace {

// LEGOBatman.exe addresses. The exe has no ASLR, so these are absolute.
constexpr uintptr_t kUpdateCallSite = 0x5A3585;  // call Player_UpdateDropInDropOut, once per frame
constexpr uintptr_t kUpdateThunk = 0x401037;
constexpr uintptr_t kPadJoinCallSite = 0x42E833;  // its Player_SetDevice for a pad that pressed any button
constexpr uintptr_t kKeyboardJoinKey = 0x42E774;  // its `cmp key, DIK_F2` for the keyboard joining player 2
constexpr uintptr_t kFindDeviceCallSite = 0x42E70E;  // its Input_FindDeviceWithEvent before a join
constexpr uintptr_t kFindDeviceWithEvent = 0x522300;  // (skip mask, event, &value, &code, flags): first slot with it
// Its other callers; the controls menu's input gate (0x4048CB) waits for a frame where it finds nothing.
constexpr uintptr_t kFindDeviceCallSites[] = {0x4048CB, 0x406373, 0x406466, 0x42E68E, 0x4ACBF6, 0x4B3CBD, 0x5A3A08};
constexpr uintptr_t kReadSlotCallSite = 0x6DE4AD;  // Input_UpdateDeviceAssignments reading one device slot
constexpr uintptr_t kReadSlot = 0x524070;
// Every caller of Input_UpdateDeviceAssignments: the main loop, the loading screen and the FMV player.
constexpr uintptr_t kUpdateDevicesCallSites[] = {0x6E0484, 0x4B3CDB, 0x593ABE, 0x593C32, 0x593D16};
constexpr uintptr_t kUpdateDevices = 0x6DE440;

constexpr uintptr_t kPlayerSetDevice = 0x6D5D00;
constexpr uintptr_t kPlayerGetDevice = 0x6D5DF0;
constexpr uintptr_t kPlayerDropOut = 0x42E280;
constexpr uintptr_t kPrimaryKeyboardSlot = 0x51E240;
constexpr uintptr_t kKeyboardSlotForPlayer = 0x5226F0;
constexpr uintptr_t kMenuState = 0x60CEE0;

constexpr uintptr_t kPlayerTable = 0xAECB64;      // per player: device slot, joined, spare
constexpr uintptr_t kPlayerLastDevice = 0x93428C;  // per player: the slot a pending drop-in uses
constexpr uintptr_t kPlayerObjects = 0xAB3960;     // eight character pointers, player 1 first
constexpr uintptr_t kPlayer2Object = 0xAB3964;
constexpr uintptr_t kPendingDropIn = 0x936B7C;  // int8 player index the game should (re)join, -1 if none
constexpr uintptr_t kDropInDisabled = 0x9CAE58;
constexpr uintptr_t kDropInDelayTimer = 0x9C600C;  // float, counts down to the drop-in spawn
constexpr DWORD kPendingDropInFlag = 0x2000000;  // on a character at +5136
constexpr uintptr_t kGamePads = 0xA96388;      // 96 bytes per port: +0 NUPAD (+112 held, +120 pressed), +36 read flag
constexpr DWORD kStartButton = 0x800;
constexpr DWORD kGrappleButton = 0x400;  // bit 10: the game's unused stick-click bit, the grapple's row
constexpr DWORD kPlayer2ToggleButton = 0x100;  // bit 8: "Player 2 toggle", F2 by default
constexpr DWORD kCameraToggleButton = 0x200;   // bit 9: "Camera toggle", F1 by default
constexpr DWORD kToggleUpButton = 0x8;   // bit 3: the right bumper by default, shared with the grapple
constexpr DWORD kUseButton = 0x20;       // the special button: grab, use, what starts the game's grapple
constexpr uintptr_t kDeviceStates = 0xAF8870;  // 132 bytes per slot: +0 present, +4 owning player or -1
constexpr uintptr_t kInputBlock = 0x9CFAF4;    // pointer to the input manager's block
constexpr int kRecordsOffset = 20020;          // its device records: 380 bytes per slot, +40 keyboard active
constexpr int kRecordStride = 380;
constexpr int kRecordActive = 40;

constexpr int kSlots = 14;     // 0-9 pads, 10-13 keyboard
constexpr int kXInputSlots = 4;  // named with the Xbox button names

// The HUD's "Press F2 or any button to start" over a player who's out. The game keeps the text
// as pointers it looks up once from its PC strings, so they get pointed at a line naming the
// "Player 2 toggle" binding instead, worded the way the controls menu words it.
constexpr uintptr_t kJoinPromptP1 = 0x99E4AC;  // GEN_PCPRESSSTARTP1
constexpr uintptr_t kJoinPromptP2 = 0x99E4B0;  // GEN_PCPRESSSTARTP2
constexpr uintptr_t kKeyNames = 0x94AA98;      // pointer to the key names, one char* per scancode
constexpr uintptr_t kInputNames = 0x9CFB1C;    // pointer to the localized input-name table (char* cells)
constexpr uintptr_t kPadButtonNames = 0x94BA50;  // XInput button code -> input-name cell
constexpr uintptr_t kPadAxisNames = 0x94BA40;    // XInput axis code * 2 (+1 for its high end) -> cell
constexpr int kSlotBindings = 110;  // in a device record: {kind, pad, code16} per output entry
constexpr int kEntryTypes = 26608;  // in the block: 6 bytes per output entry, its type first
constexpr int kPlayer2ToggleEntry = 22;
constexpr int kKind_PadButton = 1, kKind_PadAxis = 2, kKind_PadPov = 3, kKind_Mouse = 4, kKind_Key = 7;
constexpr int kCell_Button = 4, kCell_Low = 5, kCell_High = 6, kCell_Analogue = 7, kCell_Axes = 8;
constexpr int kCell_MouseButtons = 43, kCell_MouseButtonN = 46, kCell_NoInput = 47;
constexpr int kPadSlots = 10;
constexpr int kDeviceStride = 132;
constexpr BYTE kAxisCentre = 0x80;

using UpdateFn = int(__cdecl*)();
using PlayerSlotFn = int(__cdecl*)(int player, int slot);
using PlayerFn = int(__cdecl*)(int player);
using DropOutFn = int(__cdecl*)(int player, int, int);
using SlotFn = int(__cdecl*)();
using SlotPlayerFn = int(__cdecl*)(int slot, int player);
using FindDeviceFn = int(__cdecl*)(int excludeMask, int event, void* value, int* code, int flags);
using ReadSlotFn = int(__cdecl*)(DWORD slot, BYTE*, BYTE*, BYTE*, BYTE*, BYTE*, BYTE*, BYTE*, BYTE*, DWORD* buttons,
                                 BYTE*, DWORD*);

template <class T>
T& At(uintptr_t address) {
  return *reinterpret_cast<T*>(address);
}

template <class Fn>
Fn Game(uintptr_t address) {
  return reinterpret_cast<Fn>(address);
}

int& PlayerSlot(int player) { return At<int>(kPlayerTable + 12 * player); }
int& PlayerJoined(int player) { return At<int>(kPlayerTable + 12 * player + 4); }
bool SlotPresent(int slot) { return At<int>(kDeviceStates + slot * kDeviceStride) != 0; }
int& SlotOwner(int slot) { return At<int>(kDeviceStates + slot * kDeviceStride + 4); }
bool IsKeyboardSlot(int slot) { return slot >= kPadSlots; }

// The engine tags idle players with whatever device last showed activity; only a joined owner counts.
bool SlotIsFree(int slot) {
  int owner = SlotOwner(slot);
  return SlotPresent(slot) && (owner < 0 || !PlayerJoined(owner));
}

bool pendingJoin;  // the game itself asked for player 2 to (re)join this frame, e.g. from the pause menu

// The two toggle buttons, read off the slots as the game reads them (either player's), and
// turned into one press per frame for the drop-in update and the camera.
bool toggleHeld[2], toggleWas[2], togglePressed[2];  // 0 = player 2, 1 = camera

bool refusedJoin;  // the game tried to join player 2 on its own this frame

// The drop-in routine asks for a device with a pending button press before joining a player.
// With player 1 in and player 2 out, the only join it could make is player 2's, so it finds nothing.
int __cdecl StuckAwareFindDevice(int excludeMask, int event, void* value, int* code, int flags);

int __cdecl FilteredFindDeviceWithEvent(int excludeMask, int event, void* value, int* code, int flags) {
  if (PlayerJoined(0) && !PlayerJoined(1)) return -1;
  return StuckAwareFindDevice(excludeMask, event, value, code, flags);
}

// Player 2's any-button pad join comes through here; only the game's own requests get through.
int __cdecl FilteredSetDevice(int player, int slot) {
  if (player == 1 && !pendingJoin) {
    refusedJoin = true;
    return 0;
  }
  return Game<PlayerSlotFn>(kPlayerSetDevice)(player, slot);
}

int FreePadSlot() {
  for (int slot = 0; slot < kPadSlots; ++slot)
    if (SlotIsFree(slot)) return slot;
  return -1;
}

// The game keeps a per-player "active" bit on the character (bit 7 at +508) that it restores at
// level start from saved progress, independently of any device.
bool Player2Active() {
  auto object = At<uint8_t*>(kPlayer2Object);
  return object && (object[508] & 0x80);
}

bool joinPending;    // asked the game to join player 2; cleared once it has, or after a few frames
int joinFrames;
bool droppingOut;    // asked the game to drop player 2; cleared once the character is AI again
int slotWarming = -1;  // keyboard slot marked active, waiting for the engine to read it as present
int warmFrames;

// Joins through the game's own pending-drop-in path (the one its rejoin dialog uses), so the
// game runs its whole drop-in sequence: device assignment, the two-second delay, the spawn.
void RequestJoin(int slot) {
  // Player 2's pad record still says "read" from their last session, while the new device isn't
  // read for them until next frame; the game's pad-lost check would pause on that. Start fresh.
  At<uint8_t>(kGamePads + 96 + 36) = 0;
  At<int>(kPlayerLastDevice + 4) = slot;
  At<int8_t>(kPendingDropIn) = 1;
  pendingJoin = true;  // let the game's device assignment through the filter this frame
  joinPending = true;
  joinFrames = 0;
}

int PresentKeyboardSlot() {
  for (int slot = kPadSlots; slot < kSlots; ++slot)
    if (SlotPresent(slot)) return slot;
  return -1;
}

bool Join() {
  int p1 = Game<PlayerFn>(kPlayerGetDevice)(0);
  int slot = FreePadSlot();
  // Player 2 gets the pad. If player 1 holds the only one, player 1 moves to the keyboard first.
  if (slot < 0 && !IsKeyboardSlot(p1)) {
    if (int keyboard = PresentKeyboardSlot(); keyboard >= 0) {
      Game<PlayerSlotFn>(kPlayerSetDevice)(0, keyboard);
      slot = p1;
    } else {
      slot = Game<SlotPlayerFn>(kKeyboardSlotForPlayer)(Game<SlotFn>(kPrimaryKeyboardSlot)(), 1);
    }
  }
  if (slot < 0) return false;  // nothing left for player 2 to use

  // The engine only reads a keyboard slot once it is marked active, and a player whose device
  // doesn't read as present gets the "controller removed" pause. So mark it first and join
  // once it has been read.
  if (IsKeyboardSlot(slot) && !SlotPresent(slot)) {
    At<uint8_t>(At<uintptr_t>(kInputBlock) + kRecordsOffset + slot * kRecordStride + kRecordActive) = 1;
    slotWarming = slot;
    warmFrames = 0;
    return true;
  }
  RequestJoin(slot);
  return true;
}

// What the pause menu's drop out does: clear every character's pending drop-in, then drop out.
void DropOut() {
  for (int i = 0; i < 8; ++i)
    if (auto object = At<uint8_t*>(kPlayerObjects + 4 * i))
      At<DWORD>(reinterpret_cast<uintptr_t>(object) + 5136) &= ~kPendingDropInFlag;
  Game<DropOutFn>(kPlayerDropOut)(1, 1, 0);
  droppingOut = true;
}

// F2 is also Start on the keyboard's player 2 layout, so the press that joins player 2 would
// open their pause menu on the frame they get a device. Hide Start while the toggle is down.
void SwallowStart() {
  for (int port = 0; port < 2; ++port) {
    if (auto pad = At<BYTE*>(kGamePads + 96 * port)) {
      At<DWORD>(reinterpret_cast<uintptr_t>(pad) + 112) &= ~kStartButton;
      At<DWORD>(reinterpret_cast<uintptr_t>(pad) + 120) &= ~kStartButton;
    }
  }
}

int __cdecl HookUpdate() {
  pendingJoin = At<int8_t>(kPendingDropIn) == 1;
  if (joinPending && (IsHuman() || ++joinFrames > 30)) joinPending = false;
  if (droppingOut && !Player2Active() && !IsHuman()) droppingOut = false;
  if (slotWarming >= 0) {
    if (SlotPresent(slotWarming)) RequestJoin(slotWarming), slotWarming = -1;
    else if (++warmFrames > 60) slotWarming = -1;
  }
  bool inPlay = !At<int>(kDropInDisabled) && Game<SlotFn>(kMenuState)() == -1 && At<int>(kPlayer2Object);
  bool ready = inPlay && !joinPending && !droppingOut && slotWarming < 0;

  if (togglePressed[0]) {
    // Dropping out the only human player is something the game never does, and it crashes.
    if (ready) {
      if (!IsHuman()) Join();
      else if (PlayerJoined(0)) DropOut();
    }
  }

  // An active player 2 without a device is a ghost: no input, but the game treats them as human
  // and asks for their controller. Give them the spare device, or turn them back into the AI.
  if (ready && Player2Active() && !IsHuman()) {
    if (!Join() && PlayerJoined(0)) DropOut();
  }

  int result = Game<UpdateFn>(kUpdateThunk)();
  if (toggleHeld[0]) SwallowStart();  // the pause check runs right after this hook
  // Refusing the device assignment leaves the rest of the game's drop-in sequence armed: a delay,
  // then it activates the character anyway. Disarm it the way the pause menu's drop-out does.
  if (refusedJoin) {
    refusedJoin = false;
    if (auto object = At<uint8_t*>(kPlayer2Object))
      At<DWORD>(reinterpret_cast<uintptr_t>(object) + 5136) &= ~kPendingDropInFlag;
    At<float>(kDropInDelayTimer) = 0.0f;
  }
  return result;
}

// Keyboard and pad both drive player 1 until a second player takes one of them. The engine reads
// the slots in order once per frame, so each read is cached and merged into player 1's slot as it
// is read; a slot that comes later in the order contributes its previous frame. Merging happens
// before the engine derives presses and analog values, so everything downstream sees one device.
struct SlotSample {
  unsigned frame = 0;
  bool present = false;
  bool moved = false;  // an axis was off rest
  BYTE axes[8];
  DWORD buttons;
  DWORD changedAt;  // tick of the last button or stick change
};
SlotSample samples[kSlots];
unsigned frame;
constexpr int kMoveThreshold = 24;  // axis counts from rest that count as being used
constexpr DWORD kStuckMs = 1000;

// Input held unchanged this long is a jammed or phantom device, which would otherwise lock the controls menu.
bool Stuck(int slot) {
  const SlotSample& s = samples[slot];
  return s.present && frame - s.frame <= 1 && (s.buttons || s.moved) && GetTickCount() - s.changedAt >= kStuckMs;
}

int __cdecl StuckAwareFindDevice(int excludeMask, int event, void* value, int* code, int flags) {
  for (int slot = 0; slot < kSlots; ++slot)
    if (Stuck(slot)) excludeMask |= 1 << slot;
  return Game<FindDeviceFn>(kFindDeviceWithEvent)(excludeMask, event, value, code, flags);
}
int onsetSlot = -1;  // the slot that most recently went from idle to used
unsigned onsetFrame;
int pendingSwitch = -1;  // the device player 1 moves to once it's let go of

// Fresh use of a device: a button that wasn't down, or a stick that was at rest.
bool Onset(const SlotSample& before, DWORD buttons, bool moved) {
  return (buttons & ~before.buttons) != 0 || (moved && !before.moved);
}

int FreeKeyboardSlot() {
  for (int slot = kPadSlots; slot < kSlots; ++slot)
    if (SlotIsFree(slot)) return slot;
  return -1;
}

// The game starts with mouse control off, which puts player 1 on the plain keyboard profile
// where mouse buttons can't be bound. Once per session, hand the mouse to player 1 and wake the
// keyboard-and-mouse slot; the controls menu's "Mouse Control" row can still change it.
constexpr int kMouseOwnerOffset = 26120;
bool mouseGiven;
int mouseSlotFrames;  // waiting for the keyboard-and-mouse slot to read as present

void GiveMouseToPlayer1() {
  uintptr_t block = At<uintptr_t>(kInputBlock);
  if (!block) return;
  int keyboard = Game<SlotFn>(kPrimaryKeyboardSlot)();
  if (!mouseGiven) {
    mouseGiven = true;
    if (At<int>(block + kMouseOwnerOffset) >= 0) return;
    At<int>(block + kMouseOwnerOffset) = 0;
    mouseSlotFrames = 1;
  }
  if (mouseSlotFrames <= 0 || ++mouseSlotFrames > 120) return;
  // The engine only reads a keyboard slot once it's marked active; once it's present, a player 1
  // sitting on the plain keyboard moves over so the mouse buttons are theirs.
  At<uint8_t>(block + kRecordsOffset + keyboard * kRecordStride + kRecordActive) = 1;
  if (!SlotPresent(keyboard)) return;
  int p1 = PlayerSlot(0);
  if (PlayerJoined(0) && IsKeyboardSlot(p1) && p1 != keyboard && SlotIsFree(keyboard)) {
    Game<PlayerSlotFn>(kPlayerSetDevice)(0, keyboard);
    if (SlotOwner(p1) == 0) SlotOwner(p1) = -1;
  }
  mouseSlotFrames = 0;
}

// Player 1's device follows whichever of their devices was used last, pad or keyboard, so the
// controls menu shows the one in hand and an unplugged pad hands over to the keyboard instead
// of pausing the game. Both keep driving the character through the merge below. The press that
// asks for the hand-over reaches the game through the merge; the hand-over itself waits until
// that device is let go of, since the game's press edges come from the player's device.
void FollowActiveDevice() {
  if (!PlayerJoined(0) || IsHuman() || joinPending || droppingOut || slotWarming >= 0) return;
  int p1 = PlayerSlot(0);
  if (p1 < 0) return;
  int want = -1;
  if (!SlotPresent(p1)) {
    pendingSwitch = -1;
    want = IsKeyboardSlot(p1) ? FreePadSlot() : FreeKeyboardSlot();
    if (want < 0 && !IsKeyboardSlot(p1)) {
      // No keyboard slot is read until one is marked active; mark the primary and come back.
      int keyboard = Game<SlotFn>(kPrimaryKeyboardSlot)();
      At<uint8_t>(At<uintptr_t>(kInputBlock) + kRecordsOffset + keyboard * kRecordStride + kRecordActive) = 1;
      return;
    }
  } else {
    if (onsetSlot >= 0 && onsetFrame == frame && onsetSlot != p1 &&
        (IsKeyboardSlot(onsetSlot) != IsKeyboardSlot(p1) || Stuck(p1)))
      pendingSwitch = onsetSlot;
    else if (onsetSlot == p1 && onsetFrame == frame)
      pendingSwitch = -1;  // back on the current device: stay
    if (pendingSwitch < 0) return;
    const SlotSample& s = samples[pendingSwitch];
    if (!SlotIsFree(pendingSwitch) || frame - s.frame > 1 || Stuck(pendingSwitch)) {
      pendingSwitch = -1;
      return;
    }
    if (s.buttons != 0 || s.moved) return;  // still in use; the merge carries it meanwhile
    want = pendingSwitch;
    pendingSwitch = -1;
  }
  if (want < 0 || want == p1) return;
  Game<PlayerSlotFn>(kPlayerSetDevice)(0, want);
  if (SlotOwner(p1) == 0) SlotOwner(p1) = -1;  // the old device is spare again, so the merge can use it
}

int __cdecl HookReadSlot(DWORD slot, BYTE* x0, BYTE* x1, BYTE* x2, BYTE* x3, BYTE* x4, BYTE* x5, BYTE* x6, BYTE* x7,
                         DWORD* buttons, BYTE* extra, DWORD* extra2) {
  int result = Game<ReadSlotFn>(kReadSlot)(slot, x0, x1, x2, x3, x4, x5, x6, x7, buttons, extra, extra2);
  if (slot >= kSlots) return result;

  BYTE* axes[8] = {x0, x1, x2, x3, x4, x5, x6, x7};
  SlotSample& mine = samples[slot];
  bool moved = false;
  for (int i = 0; i < 8; ++i) moved |= abs(*axes[i] - (i < 4 ? kAxisCentre : 0)) > kMoveThreshold;
  if (result && Onset(mine, *buttons, moved)) onsetSlot = slot, onsetFrame = frame;
  if (*buttons != mine.buttons || moved != mine.moved || !mine.present) mine.changedAt = GetTickCount();
  mine.frame = frame;
  mine.present = result != 0;
  mine.moved = moved;
  for (int i = 0; i < 8; ++i) mine.axes[i] = *axes[i];
  mine.buttons = *buttons;
  // Player 2's grapple: their slot's bit goes to the grapple, never to the game, and the launch
  // presses their use button once.
  if (IsHuman() && int(slot) == PlayerSlot(1)) {
    if (*buttons & kPlayer2ToggleButton) toggleHeld[0] = true;
    *buttons &= ~(kPlayer2ToggleButton | kCameraToggleButton);  // the camera is player 1's
    bool grappleDown = (*buttons & kGrappleButton) != 0;
    grapple::SetButton(1, grappleDown);
    *buttons &= ~kGrappleButton;
    if (grappleDown && grapple::Ready(1)) *buttons &= ~kToggleUpButton;
    if (grapple::TakeFire(1)) *buttons |= kUseButton;
    return result;
  }
  if (!PlayerJoined(0) || int(slot) != PlayerSlot(0)) return result;

  bool keyboard = IsKeyboardSlot(slot);
  for (int other = 0; other < kSlots && !IsHuman(); ++other) {
    const SlotSample& s = samples[other];
    if (other == int(slot) || IsKeyboardSlot(other) == keyboard || !s.present || frame - s.frame > 1) continue;
    if (!SlotIsFree(other) || Stuck(other)) continue;
    // Sticks (0-3) rest at 0x80, triggers (4-7) at 0; the stronger input wins either way.
    for (int i = 0; i < 8; ++i) {
      int rest = i < 4 ? kAxisCentre : 0;
      if (abs(s.axes[i] - rest) > abs(*axes[i] - rest)) *axes[i] = s.axes[i];
    }
    *buttons |= s.buttons;
  }
  // The grapple's bit never reaches the game. When it's about to grapple, the bumper it shares
  // by default doesn't either, so one press doesn't also toggle the character.
  bool grappleDown = (*buttons & kGrappleButton) != 0;
  grapple::SetButton(0, grappleDown);
  *buttons &= ~kGrappleButton;
  if (*buttons & kPlayer2ToggleButton) toggleHeld[0] = true;
  if (*buttons & kCameraToggleButton) toggleHeld[1] = true;
  *buttons &= ~(kPlayer2ToggleButton | kCameraToggleButton);
  if (grappleDown && grapple::Ready(0)) *buttons &= ~kToggleUpButton;
  mousectl::Apply(axes, buttons);  // on top of whatever player 1's devices say
  vehicle::SteerToView(axes);
  return result;
}

// The name the controls menu would show for a slot's binding of an output entry.
void BindingName(int slot, int entry, char* out, size_t size) {
  out[0] = 0;
  uintptr_t block = At<uintptr_t>(kInputBlock);
  auto binding = reinterpret_cast<const uint8_t*>(block + kRecordsOffset + slot * kRecordStride + kSlotBindings + 4 * entry);
  int kind = binding[0];
  int code = *reinterpret_cast<const int16_t*>(binding + 2);
  auto names = At<const char* const*>(kInputNames);
  auto copy = [&](const char* name) { snprintf(out, size, "%s", name ? name : ""); };
  bool xinput = slot < kXInputSlots;
  switch (kind) {
    case kKind_Key:
      if (code >= 1) copy(At<const char* const*>(kKeyNames)[code]);
      return;
    case kKind_Mouse:
      if (code >= 0 && code <= 2) copy(names[kCell_MouseButtons + code]);
      else snprintf(out, size, names[kCell_MouseButtonN], code + 1);
      return;
    case kKind_PadButton:
      if (code < 0) return;
      if (xinput && code <= 10) copy(names[At<uint8_t>(kPadButtonNames + code)]);
      else if (code >= 128 && code <= 131) copy(names[code - 128]);  // the hat
      else snprintf(out, size, "%s %d", names[kCell_Button], code + 1);
      return;
    case kKind_PadAxis:
    case kKind_PadPov: {
      if (code < 0) return;
      int entryType = At<uint8_t>(block + kEntryTypes + 6 * entry);
      int highType = kind == kKind_PadPov ? 1 : (entryType == 3 ? 3 : 2);
      bool high = entryType == highType;
      if (xinput && code < 8) copy(names[At<uint8_t>(kPadAxisNames + 2 * code + (high ? 1 : 0))]);
      else if (code < 6) snprintf(out, size, "%s %s", names[kCell_Axes + code], names[high ? kCell_High : kCell_Low]);
      else snprintf(out, size, names[kCell_Analogue], code, names[high ? kCell_High : kCell_Low]);
      return;
    }
    default:
      copy(names[kCell_NoInput]);
  }
}

char joinPrompt[96];
const char* gameJoinPrompt[2];

// Player 2 joins with the keyboard's toggle when there is a keyboard, so that's the one named;
// player 1's own binding otherwise.
void UpdateJoinPrompt() {
  if (!At<uintptr_t>(kInputBlock)) return;
  auto& p1 = At<const char*>(kJoinPromptP1);
  auto& p2 = At<const char*>(kJoinPromptP2);
  if (p1 != joinPrompt) gameJoinPrompt[0] = p1;
  if (p2 != joinPrompt) gameJoinPrompt[1] = p2;
  int slot = PlayerSlot(0);
  if (!IsKeyboardSlot(slot)) {
    int keyboard = Game<SlotFn>(kPrimaryKeyboardSlot)();
    if (keyboard >= 0 && keyboard < kSlots && SlotPresent(keyboard)) slot = keyboard;
  }
  char name[64];
  if (slot >= 0 && slot < kSlots) BindingName(slot, kPlayer2ToggleEntry, name, sizeof name);
  else name[0] = 0;
  if (!name[0]) {  // unbound: the game's own line
    p1 = gameJoinPrompt[0];
    p2 = gameJoinPrompt[1];
    return;
  }
  snprintf(joinPrompt, sizeof joinPrompt, "Press %s to start", name);
  p1 = p2 = joinPrompt;
}

// The engine hands an idle player whatever device last showed activity and routes its input to
// them without a join. Player 2 only gets a device by joining. An active character keeps its
// device while the game drops it out, or the game would report the controller as removed.
int __cdecl HookUpdateDevices() {
  ++frame;
  toggleHeld[0] = toggleHeld[1] = false;  // the slot reads inside set them again
  int result = Game<UpdateFn>(kUpdateDevices)();
  for (int i = 0; i < 2; ++i) {
    togglePressed[i] = toggleHeld[i] && !toggleWas[i];
    toggleWas[i] = toggleHeld[i];
  }
  GiveMouseToPlayer1();
  FollowActiveDevice();
  UpdateJoinPrompt();
  if (!PlayerJoined(1) && !Player2Active() && PlayerSlot(1) >= 0) {
    if (SlotOwner(PlayerSlot(1)) == 1) SlotOwner(PlayerSlot(1)) = -1;
    PlayerSlot(1) = -1;
  }
  return result;
}

}  // namespace

bool IsHuman() { return PlayerJoined(1) != 0; }

bool CameraTogglePressed() { return togglePressed[1]; }

void Install() {
  hook::PatchCall(reinterpret_cast<void*>(kUpdateCallSite), reinterpret_cast<void*>(kUpdateThunk), HookUpdate);
  hook::PatchCall(reinterpret_cast<void*>(kPadJoinCallSite), reinterpret_cast<void*>(kPlayerSetDevice),
                  FilteredSetDevice);
  hook::PatchCall(reinterpret_cast<void*>(kFindDeviceCallSite), reinterpret_cast<void*>(kFindDeviceWithEvent),
                  FilteredFindDeviceWithEvent);
  for (uintptr_t site : kFindDeviceCallSites)
    hook::PatchCall(reinterpret_cast<void*>(site), reinterpret_cast<void*>(kFindDeviceWithEvent), StuckAwareFindDevice);
  hook::PatchCall(reinterpret_cast<void*>(kReadSlotCallSite), reinterpret_cast<void*>(kReadSlot), HookReadSlot);
  for (uintptr_t site : kUpdateDevicesCallSites)
    hook::PatchCall(reinterpret_cast<void*>(site), reinterpret_cast<void*>(kUpdateDevices), HookUpdateDevices);
  // The keyboard branch compares the pressed key with F2 for player 2; make it never match.
  hook::PatchBytes(reinterpret_cast<void*>(kKeyboardJoinKey), {0x83, 0x7C, 0x24, 0x1C, 0x3C},
                   {0x83, 0x7C, 0x24, 0x1C, 0xFF});
}

}  // namespace player2
