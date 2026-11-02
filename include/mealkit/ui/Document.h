#pragma once

#include "mkui/Document.h"

#include <string>
#include <vector>

// .ui files (format and node properties: mkui/Document.h) read through the engine's asset paths. For the editor [UI] panel,
// layout comparison checks and tests; ui::System reads screens itself.
namespace mealkit::ui {

using mkui::Document;
using mkui::kThemeFile;
using mkui::Node;
using mkui::NodeKind;
using mkui::Theme;
using mkui::WidgetKind;

bool loadTheme(Theme& out, std::vector<std::string>* errors = nullptr);
bool loadDocument(const std::string& assetPath, Document& out, const Theme* theme = nullptr);
bool parseDocument(const std::string& text, const std::string& assetPath, Document& out, const Theme* theme = nullptr);

} // namespace mealkit::ui
