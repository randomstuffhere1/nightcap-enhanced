// Modified by vii from komaruworld/mocktail. See README "About this fork".
// Copyright 2026 Mocktail Project Authors
//
// Licensed under the Apache License, Version 2.0 (the "License")

#include "mocktail/platform/sdl_gamepad_manager.h"

#include <SDL3/SDL_gamepad.h>
#include <gtest/gtest.h>

namespace mocktail {
namespace platform {
namespace {

using platform::GamepadFamily;

// The pad families the client can be told apart. SDL has more types than the
// client has glyph sets, so these are the ones that carry through.
TEST(SdlGamepadFamilyTest, MapsEveryTypeTheClientHasArtFor) {
  EXPECT_EQ(FamilyForType(SDL_GAMEPAD_TYPE_XBOX360), GamepadFamily::kXbox);
  EXPECT_EQ(FamilyForType(SDL_GAMEPAD_TYPE_XBOXONE), GamepadFamily::kXbox);
  EXPECT_EQ(FamilyForType(SDL_GAMEPAD_TYPE_PS3), GamepadFamily::kPlayStation4);
  EXPECT_EQ(FamilyForType(SDL_GAMEPAD_TYPE_PS4), GamepadFamily::kPlayStation4);
  EXPECT_EQ(FamilyForType(SDL_GAMEPAD_TYPE_PS5), GamepadFamily::kPlayStation5);
}

// A Switch Pro and a GameCube pad are distinct hardware that SDL identifies.
// Before the fix these arrived as SDL_GAMEPAD_TYPE_STANDARD and reached the
// client as an unknown pad drawing generic glyphs.
TEST(SdlGamepadFamilyTest, IdentifiesSwitchAndGameCubePads) {
  EXPECT_EQ(FamilyForType(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO),
            GamepadFamily::kNintendoSwitch);
  EXPECT_EQ(FamilyForType(SDL_GAMEPAD_TYPE_GAMECUBE),
            GamepadFamily::kGameCube);
}

// Paired Joy-Cons use the same printed-button mapping as a Switch Pro.
TEST(SdlGamepadFamilyTest, SeparatesSingleJoyConsFromPairs) {
  EXPECT_EQ(FamilyForType(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT),
            GamepadFamily::kNintendo);
  EXPECT_EQ(FamilyForType(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT),
            GamepadFamily::kNintendo);
  EXPECT_EQ(FamilyForType(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR),
            GamepadFamily::kNintendoSwitch);
}

// An unidentifiable pad stays unknown rather than being guessed at, and the
// count of types is covered so a new SDL type cannot slip through untested.
TEST(SdlGamepadFamilyTest, UnidentifiablePadsStayUnknown) {
  EXPECT_EQ(FamilyForType(SDL_GAMEPAD_TYPE_UNKNOWN), GamepadFamily::kUnknown);
  EXPECT_EQ(FamilyForType(SDL_GAMEPAD_TYPE_STANDARD), GamepadFamily::kUnknown);
  EXPECT_EQ(FamilyForType(SDL_GAMEPAD_TYPE_COUNT), GamepadFamily::kUnknown);
  // Every type SDL defines has a defined answer. This is the test that fails
  // when SDL adds one, which is the point: a new type is a new pad family and
  // someone has to decide what the client should call it.
  for (int type = 0; type < SDL_GAMEPAD_TYPE_COUNT; ++type) {
    const GamepadFamily family =
        FamilyForType(static_cast<SDL_GamepadType>(type));
    switch (family) {
      case GamepadFamily::kUnknown:
      case GamepadFamily::kXbox:
      case GamepadFamily::kPlayStation4:
      case GamepadFamily::kPlayStation5:
      case GamepadFamily::kNintendo:
      case GamepadFamily::kGameCube:
      case GamepadFamily::kNintendoSwitch:
        break;
      default:
        ADD_FAILURE() << "type " << type << " produced an invalid family";
    }
  }
}

}  // namespace
}  // namespace platform
}  // namespace mocktail
