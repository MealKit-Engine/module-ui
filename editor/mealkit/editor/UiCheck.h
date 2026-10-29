#pragma once

#include <string>

// Penpot layout comparison (ROADMAP "Game screen UI part 2" verification): for each screen exported from Penpot (assets/ui/*.ui, first line "# MealKit UI exported from Penpot"),
// matches the boxes the plugin exported with it (art/ui/boxes/<screen>.boxes, per-node position/size computed by Penpot) against the engine ui::System layout
// by id at board size and scale 1. Tolerance is 0.5 logical pixels; for text nodes, due to font shaping differences, the larger of kTextTolerance and
// kTextToleranceRatio of the width (Penpot itself measures text width 1~2 pixels differently on every export).
// Text with bindings ({field}), nodes inside lists, and nodes invisible in the engine (visible binding) are skipped.
//   MealKitEditor.exe --project <folder> --ui-check   → a "UI-CHECK ..." line per screen, exit code 2 on differences
namespace mealkit::editor {

inline constexpr float kBoxTolerance = 0.5f;
inline constexpr float kTextTolerance = 2.0f;
inline constexpr float kTextToleranceRatio = 0.04f;

// 0 if no screens were compared ("UI-CHECK none")
int runUiCheck(const std::string& projectDir, const std::string& language);

// For pixel comparison (secondary check): draws just one screen (assets-relative "ui/x.ui") in a window half the board size and saves a PNG.
// Compared with art/ui/preview/<screen>.png exported by the plugin at the same scale using tools\compare_images.ps1 (check.ps1)
//   MealKitEditor.exe --project <folder> --ui-render ui/x.ui out.png [--lang en]
int runUiRender(const std::string& projectDir, const std::string& language, const std::string& uiFile, const std::string& pngPath);

} // namespace mealkit::editor
