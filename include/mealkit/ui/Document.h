#pragma once

#include "mkui/Document.h"
#include "mkui/Source.h"

#include <string>
#include <vector>

// .ui files (format and node properties: mkui/Document.h) read through the engine's asset paths. For the editor [UI] panel,
// layout comparison checks and tests; ui::System reads screens itself.
namespace mealkit::ui {

using mkui::Document;
using mkui::kThemeFile;
using mkui::Node;
using mkui::Source;
using mkui::SourceItem;
using mkui::SourceNode;
using mkui::SourcePath;
using mkui::NodeKind;
using mkui::Theme;
using mkui::WidgetKind;
using mkui::addNode;
using mkui::findNode;
using mkui::getProperty;
using mkui::mapDocumentNodes;
using mkui::extractComponent;
using mkui::isPlacementLine;
using mkui::moveNode;
using mkui::parseNodeText;
using mkui::parseSource;
using mkui::removeNode;
using mkui::removeProperty;
using mkui::setProperty;
using mkui::uniqueName;
using mkui::wrapNode;
using mkui::writeNodeText;
using mkui::writeSource;

bool loadTheme(Theme& out, std::vector<std::string>* errors = nullptr);
bool loadDocument(const std::string& assetPath, Document& out, const Theme* theme = nullptr);
bool parseDocument(const std::string& text, const std::string& assetPath, Document& out, const Theme* theme = nullptr);

} // namespace mealkit::ui
