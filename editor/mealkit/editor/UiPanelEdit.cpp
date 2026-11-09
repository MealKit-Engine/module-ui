// [UI] panel editing: source, selection, node tree, properties, history (UiPanel.h). Drawing the preview is in UiPanel.cpp
#include "mealkit/core/Log.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/FuzzyMatch.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/editor/UiPanel.h"
#include "mealkit/platform/Paths.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <imgui.h>

namespace mealkit::editor {
namespace {

// Property names for the "add property" list (mkui applyProperty)
constexpr const char* kProperties[] = {
    "pos", "size", "min", "max", "margin", "padding", "gap", "layout", "justify", "align", "alignContent", "alignSelf",
    "columns", "rows", "cell", "justifyItems", "justifySelf", "constraints", "absolute", "rotation", "hidden", "clip",
    "fill", "stroke", "radius", "shadow", "opacity", "tint", "src", "slice", "key", "content", "font", "fontSize",
    "color", "span", "textAlign", "verticalAlign", "lineHeight", "wrap", "overflow", "maxLines", "letterSpacing",
    "underline", "strike", "transform", "visible", "disabled", "widthRatio", "colorField", "button", "toggle", "slider",
    "select", "scroll", "list", "textInput", "placeholder", "maxLength", "focusUp", "focusDown", "focusLeft",
    "focusRight", "on", "transition", "set", "id"};
// Board-only keys (also known, so their lines aren't marked)
constexpr const char* kBoardProperties[] = {"navigation", "firstFocus", "back"};

bool knownProperty(const std::string& key) {
    for (const char* k : kProperties)
        if (key == k) return true;
    for (const char* k : kBoardProperties)
        if (key == k) return true;
    return false;
}

// Value lists for the inspector's combos (mkui Document.cpp applyProperty)
constexpr const char* kJustify[] = {"start", "center", "end", "space-between", "space-around", "space-evenly"};
constexpr const char* kAlign[] = {"start", "center", "end", "stretch"};
constexpr const char* kTextAlign[] = {"left", "center", "right"};

// View model fields by path ("a.b": struct fields), as the runtime names them
const reflect::FieldInfo* fieldAt(const reflect::TypeInfo* type, const std::string& path) {
    size_t from = 0;
    while (type) {
        const size_t dot = path.find('.', from);
        const reflect::FieldInfo* f = type->field(path.substr(from, dot == std::string::npos ? dot : dot - from).c_str());
        if (!f || dot == std::string::npos) return f;
        type = f->value->kind == reflect::Kind::Struct ? f->value->type : nullptr;
        from = dot + 1;
    }
    return nullptr;
}

bool isList(const reflect::FieldInfo& f) {
    return f.value->kind == reflect::Kind::Array && f.value->element && f.value->element->kind == reflect::Kind::Struct && f.value->element->type;
}

using KindTest = bool (*)(const reflect::FieldInfo&);
bool anyValue(const reflect::FieldInfo& f) { return f.value->kind != reflect::Kind::Struct && f.value->kind != reflect::Kind::Array; }
bool floatValue(const reflect::FieldInfo& f) { return f.value->kind == reflect::Kind::Float; }
bool colorValue(const reflect::FieldInfo& f) { return f.value->kind == reflect::Kind::Color; }

void fieldNames(const reflect::TypeInfo& type, const std::string& prefix, KindTest test, std::vector<std::string>& out, u32 depth = 0) {
    for (const reflect::FieldInfo& f : type.fields) {
        if (test(f)) out.push_back(prefix + f.name);
        if (depth < 3 && f.value->kind == reflect::Kind::Struct && f.value->type) fieldNames(*f.value->type, prefix + f.name + ".", test, out, depth + 1);
    }
}

// Property → what its field must be. `visible` and `disabled` take a leading ! to negate
struct BindingKey {
    const char* key;
    const char* (*label)(); // translated when drawn (the language can change)
    KindTest test;
    bool negatable;
};
const BindingKey kBindings[] = {
    {"visible", [] { return tr("Visible when"); }, anyValue, true},
    {"disabled", [] { return tr("Disabled when"); }, anyValue, true},
    {"widthRatio", [] { return tr("Width from"); }, floatValue, false},
    {"colorField", [] { return tr("Color from"); }, colorValue, false},
    {"list", [] { return tr("Repeat for"); }, isList, false},
};

// {field} and {field:format} names in text
std::vector<std::string> textFields(const std::string& text) {
    std::vector<std::string> out;
    for (size_t open = text.find('{'); open != std::string::npos; open = text.find('{', open + 1)) {
        const size_t close = text.find('}', open);
        if (close == std::string::npos) break;
        const std::string inner = text.substr(open + 1, close - open - 1);
        out.push_back(inner.substr(0, inner.find(':')));
    }
    return out;
}

// Walks a node and its children with the fields in scope and reports names that don't exist: "where: problem" lines in out,
// and (when nodes is given) each node's path with its problem for the layer badges
void checkBindings(const ui::SourceNode& node, const reflect::TypeInfo* scope, const std::string& where, std::vector<std::string>& out,
                   const ui::SourcePath& path = {}, std::vector<std::pair<ui::SourcePath, std::string>>* nodes = nullptr) {
    std::string value;
    const std::string at = where + (node.name.empty() ? node.kind : node.name);
    auto report = [&](const std::string& problem) {
        out.push_back(at + ": " + problem);
        if (nodes) nodes->push_back({path, problem});
    };
    for (const BindingKey& b : kBindings) {
        if (!ui::getProperty(node, b.key, value) || value.empty()) continue;
        const std::string name = b.negatable && value[0] == '!' ? value.substr(1) : value;
        if (!fieldAt(scope, name)) report(std::string(b.key) + " " + value);
    }
    if (ui::getProperty(node, "content", value))
        for (const std::string& name : textFields(value))
            if (!fieldAt(scope, name)) report("{" + name + "}");
    if (node.kind == "use") return; // the component file has its own view model paths
    const reflect::TypeInfo* inner = scope;
    if (ui::getProperty(node, "list", value)) {
        const reflect::FieldInfo* f = fieldAt(scope, value);
        if (!f || !isList(*f)) return; // already reported; names inside can't be checked
        inner = f->value->element->type;
    }
    for (u32 c = 0; c < node.children.size(); ++c) {
        ui::SourcePath child = path;
        child.push_back(c);
        checkBindings(node.children[c], inner, at + " > ", out, child, nodes);
    }
}

std::vector<f32> numbers(const std::string& value) {
    std::vector<f32> out;
    const char* p = value.c_str();
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        char* end = nullptr;
        const f32 v = std::strtof(p, &end);
        if (end == p) return {}; // not all numbers ($token, hug ...)
        out.push_back(v);
        p = end;
    }
    return out;
}

std::string number(f32 v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    return buf;
}

bool parseHex(const std::string& s, f32 rgba[4]) {
    if (s.size() != 7 && s.size() != 9) return false;
    if (s[0] != '#') return false;
    for (size_t i = 0; i < 4; ++i) {
        if (1 + i * 2 >= s.size()) {
            rgba[i] = 1.0f;
            break;
        }
        char* end = nullptr;
        const std::string part = s.substr(1 + i * 2, 2);
        const long v = std::strtol(part.c_str(), &end, 16);
        if (*end) return false;
        rgba[i] = static_cast<f32>(v) / 255.0f;
    }
    return true;
}

std::string hex(const f32 rgba[4], bool alpha) {
    char buf[16];
    auto b = [](f32 v) { return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    if (alpha) std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", b(rgba[0]), b(rgba[1]), b(rgba[2]), b(rgba[3]));
    else std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", b(rgba[0]), b(rgba[1]), b(rgba[2]));
    return buf;
}

std::string toLf(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text)
        if (c != '\r') out += c;
    return out;
}

bool isUnder(const ui::SourcePath& path, const ui::SourcePath& ancestor) {
    return path.size() >= ancestor.size() && std::equal(ancestor.begin(), ancestor.end(), path.begin());
}


// Node kind glyphs for the layers tree and the inspector header
const char* kindGlyph(const std::string& kind) {
    if (kind == "board") return "\xEE\x9F\xB4";  // U+E7F4 screen
    if (kind == "text") return "\xEE\xA3\x92";   // U+E8D2 font
    if (kind == "image") return "\xEE\xAE\x9F";  // U+EB9F photo
    if (kind == "use") return "\xEE\x9C\x9B";    // U+E71B link (component)
    return "\xEE\x9C\xB9";                       // U+E739 box (frame and others)
}
constexpr const char* kSaveGlyph = "\xEE\x9D\x8E";  // U+E74E
constexpr const char* kUndoGlyph = "\xEE\x9E\xA7";  // U+E7A7
constexpr const char* kRedoGlyph = "\xEE\x9E\xA6";  // U+E7A6
constexpr const char* kWrapGlyph = "\xEE\xA2\xB3";  // U+E8B3 select all (a frame around)
constexpr const char* kPasteGlyph = "\xEE\x9D\xBF"; // U+E77F
constexpr const char* kHelpGlyph = "\xEE\xA2\x97";  // U+E897
constexpr const char* kCodeGlyph = "\xEE\xA5\x83";  // U+E943 code
constexpr const char* kEyeGlyph = "\xEE\x9E\xB3";   // U+E7B3 visible
constexpr const char* kHiddenGlyph = "\xEE\xB4\x9A"; // U+ED1A hidden
constexpr const char* kLockGlyph = "\xEE\x9C\xAE";  // U+E72E lock
constexpr const char* kBoundGlyph = "\xEE\x9C\x9B"; // U+E71B link (bound to the view model)
constexpr const char* kHistoryGlyph = "\xEE\xA0\x9C"; // U+E81C history
constexpr const char* kChangesGlyph = "\xEE\xA2\xA5"; // U+E8A5 document (the changed lines)
constexpr const char* kInteractGlyph = "\xEE\x9F\x89"; // U+E7C9 touch pointer

} // namespace

void UiPanel::loadSource() {
    m_source = {};
    m_editable = false;
    m_hasSelection = m_hasHover = m_renaming = false;
    m_undo.clear();
    m_redo.clear();
    m_live = m_committed = m_saved = {};
    m_map.clear();
    if (m_path.empty()) return;
    const std::string file = findAssetFile(m_path);
    std::string text;
    if (file.empty() || !readTextFile(file, text)) return;
    m_fileTime = fileWriteTime(file);
    m_crlf = text.find("\r\n") != std::string::npos;
    std::string error;
    if (!ui::parseSource(toLf(text), m_source, &error)) {
        MEALKIT_WARN("UI edit: %s: %s", m_path.c_str(), error.c_str());
        return;
    }
    m_editable = !m_source.exported;
    m_live = m_committed = m_saved = ui::writeSource(m_source);
    // A file not in the canonical shape (indentation) is shown as written until the first edit rewrites it
    if (m_live != toLf(text)) m_saved.clear();
    syncSource();
}

void UiPanel::syncSource() {
    const std::string text = ui::writeSource(m_source);
    if (text != m_live || m_map.empty()) {
        m_live = text;
        refreshDraft();
        ui::Document doc;
        ui::parseDocument(m_live, m_path, doc); // only the node structure is needed (no theme)
        m_map = ui::mapDocumentNodes(m_source, doc);
        if (m_hasSelection && !ui::findNode(m_source, m_selection)) m_hasSelection = false;
    }
    if (!ImGui::IsAnyItemActive() && m_drag == Drag::None && m_live != m_committed) { // a widget edit or mouse drag is one step
        m_undo.push_back(m_committed);
        m_redo.clear();
        m_committed = m_live;
    }
}

void UiPanel::setText(const std::string& text) {
    ui::Source source;
    if (!ui::parseSource(text, source)) return;
    m_source = std::move(source);
    m_committed = text;
    syncSource();
}

void UiPanel::undo() {
    if (m_undo.empty()) return;
    m_redo.push_back(m_committed);
    const std::string text = m_undo.back();
    m_undo.pop_back();
    setText(text);
}

void UiPanel::redo() {
    if (m_redo.empty()) return;
    m_undo.push_back(m_committed);
    const std::string text = m_redo.back();
    m_redo.pop_back();
    setText(text);
}

bool UiPanel::save() {
    if (!m_editable || m_path.empty()) return false;
    const std::string file = findAssetFile(m_path);
    std::string text = m_live;
    if (m_crlf) {
        text.clear();
        for (char c : m_live) {
            if (c == '\n') text += '\r';
            text += c;
        }
    }
    if (file.empty() || !writeTextFile(file, text)) {
        MEALKIT_ERROR("UI edit: cannot write %s", m_path.c_str());
        return false;
    }
    m_saved = m_live;
    m_fileTime = fileWriteTime(file);
    refreshDraft();
    MEALKIT_INFO("UI edit: saved %s", m_path.c_str());
    return true;
}

bool UiPanel::deleteSelected() {
    if (!editing() || !m_hasSelection || m_selection.empty()) return false;
    if (!ui::removeNode(m_source, m_selection)) return false;
    m_hasSelection = false;
    syncSource();
    return true;
}

bool UiPanel::insertNode(ui::SourceNode node) {
    if (!editing()) return false;
    // Into a selected frame or the board; next to a selected text, image or component
    ui::SourcePath parent;
    u32 at = 0xFFFFFFFFu;
    if (m_hasSelection) {
        const ui::SourceNode* n = ui::findNode(m_source, m_selection);
        if (n && (n->kind == "frame" || n->kind == "board")) {
            parent = m_selection;
        } else if (!m_selection.empty()) {
            parent.assign(m_selection.begin(), m_selection.end() - 1);
            at = m_selection.back() + 1;
        }
    }
    node.name = ui::uniqueName(m_source, node.name);
    m_selection = ui::addNode(m_source, parent, at, std::move(node));
    m_hasSelection = true;
    syncSource();
    return true;
}

bool UiPanel::addNew(const std::string& kind, const std::string& args) {
    ui::SourceNode n;
    n.kind = kind;
    n.name = kind;
    n.args = args;
    auto line = [&n](const char* text) { n.items.push_back({text, -1}); };
    if (kind == "frame") {
        line("size 200 100");
        line("fill #FFFFFF22");
    } else if (kind == "text") {
        line("content Text");
        line("fontSize 24");
        line("color #FFFFFF");
    } else if (kind == "image") {
        line("size 64 64");
    } else if (kind == "use") {
        const size_t slash = args.rfind('/'), dot = args.rfind('.');
        n.name = args.substr(slash == std::string::npos ? 0 : slash + 1, dot == std::string::npos ? std::string::npos : dot - (slash == std::string::npos ? 0 : slash + 1));
    }
    return insertNode(std::move(n));
}

bool UiPanel::duplicateSelected() {
    if (!editing() || !m_hasSelection || m_selection.empty()) return false;
    const ui::SourceNode* n = ui::findNode(m_source, m_selection);
    if (!n) return false;
    ui::SourceNode copy = *n;
    copy.name = ui::uniqueName(m_source, copy.name);
    const ui::SourcePath parent(m_selection.begin(), m_selection.end() - 1);
    m_selection = ui::addNode(m_source, parent, m_selection.back() + 1, std::move(copy));
    syncSource();
    return true;
}

bool UiPanel::copySelected() {
    if (!editing() || !m_hasSelection || m_selection.empty()) return false;
    const ui::SourceNode* n = ui::findNode(m_source, m_selection);
    if (!n) return false;
    ImGui::SetClipboardText(ui::writeNodeText(*n).c_str());
    return true;
}

bool UiPanel::paste() {
    const char* text = ImGui::GetClipboardText();
    ui::SourceNode n;
    if (!editing() || !text || !ui::parseNodeText(toLf(text), n)) return false;
    return insertNode(std::move(n));
}

bool UiPanel::wrapSelected() {
    if (!editing() || !m_hasSelection || m_selection.empty()) return false;
    ui::SourceNode* n = ui::findNode(m_source, m_selection);
    ui::NodeBox b;
    if (!n || !boxOf(m_selection, b)) return false;
    // The frame takes the node's place and drawn size; a free node's pos moves to the frame
    ui::SourceNode frame;
    frame.kind = "frame";
    frame.name = ui::uniqueName(m_source, "group");
    std::string pos;
    if (ui::getProperty(*n, "pos", pos)) {
        frame.items.push_back({"pos " + pos, -1});
        ui::setProperty(*n, "pos", "0 0");
    }
    frame.items.push_back({"size " + std::to_string(static_cast<int>(b.w + 0.5f)) + " " + std::to_string(static_cast<int>(b.h + 0.5f)), -1});
    ui::SourcePath wrapped;
    if (!ui::wrapNode(m_source, m_selection, std::move(frame), &wrapped)) return false;
    m_selection = wrapped;
    syncSource();
    return true;
}

bool UiPanel::selectNode(const std::string& name) {
    m_editing = true;
    for (const ui::SourcePath& path : m_map)
        if (const ui::SourceNode* n = ui::findNode(m_source, path); n && n->name == name) {
            m_selection = path;
            m_hasSelection = true;
            return true;
        }
    return false;
}

// Toolbar row of the workspace: the screen, history and save, the add menu, then actions on the selection (icon buttons with tooltips)
void UiPanel::drawEditToolbar() {
    if (!m_screen) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", tr("No screen open"));
        return;
    }
    // Back to the screen a component was opened from
    if (!m_returnScreen.empty() && "ui/" + m_returnScreen != m_path) {
        if (ImGui::Button(("î«  " + m_returnScreen).c_str())) { // U+E72B back
            const std::string back = m_returnScreen;
            m_returnScreen.clear();
            m_restoreSelection = true;
            openFile(back);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tr("Back to the screen this component was opened from"));
        ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.5f);
    }
    // Screen name, with a dot while unsaved
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(m_path.c_str());
    if (dirty()) {
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", "\xE2\x97\x8F"); // U+25CF
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Unsaved (Ctrl+S saves)"));
    }
    if (!m_editable) {
        ImGui::SameLine(0.0f, ImGui::GetFontSize());
        ImGui::TextDisabled("%s", tr("read-only"));
        return;
    }
    toolbarGap(ImGui::GetFrameHeight() * 3.5f);
    ImGui::BeginDisabled(!dirty());
    if (iconButton(kSaveGlyph, tr("Save (Ctrl+S)"))) save();
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::BeginDisabled(m_undo.empty());
    if (iconButton(kUndoGlyph, tr("Undo (Ctrl+Z)"))) undo();
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::BeginDisabled(m_redo.empty());
    if (iconButton(kRedoGlyph, tr("Redo (Ctrl+Y)"))) redo();
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::BeginDisabled(m_undo.empty() && m_redo.empty());
    if (iconButton(kHistoryGlyph, tr("History: go back to a step"))) ImGui::OpenPopup("##uiHistory");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##uiHistory")) {
        drawHistoryMenu();
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::BeginDisabled(!dirty());
    if (iconButton(kChangesGlyph, tr("Unsaved changes: the lines against the file"))) ImGui::OpenPopup("##uiChanges");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##uiChanges")) {
        drawChanges();
        ImGui::EndPopup();
    }

    toolbarGap(ImGui::GetFontSize() * 5.0f);
    if (ImGui::Button((std::string(icon::kAdd) + "  " + tr("Add")).c_str())) ImGui::OpenPopup("##addMenu");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tr("Add a node inside the selected frame, or after the selected node"));
    if (ImGui::BeginPopup("##addMenu")) {
        drawAddMenu();
        ImGui::EndPopup();
    }

    toolbarGap(ImGui::GetFrameHeight() * 5.0f);
    const bool node = m_hasSelection && !m_selection.empty(); // the board itself can't be duplicated, wrapped or deleted
    ImGui::BeginDisabled(!node);
    if (iconButton(icon::kCopy, tr("Duplicate (Ctrl+D)"))) duplicateSelected();
    ImGui::SameLine(0.0f, 2.0f);
    if (iconButton(kWrapGlyph, tr("Wrap in frame"))) wrapSelected();
    ImGui::SameLine(0.0f, 2.0f);
    if (iconButton(icon::kTrash, tr("Delete (Del)"))) deleteSelected();
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 2.0f);
    const ui::SourceNode* sel = m_hasSelection ? ui::findNode(m_source, m_selection) : nullptr;
    ImGui::BeginDisabled(!node || !sel || sel->kind != "frame");
    if (iconButton(kindGlyph("use"), tr("Make component: the frame becomes ui/components/<name>.ui and a use of it"))) {
        m_componentName = sel->name;
        m_makeComponentOpen = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 2.0f);
    if (iconButton(kPasteGlyph, tr("Paste (Ctrl+V)"))) paste();

    toolbarGap(ImGui::GetFrameHeight() * 2.0f);
    if (iconButton(kInteractGlyph, tr("Interact: use the screen like the game (buttons, sliders) and list the actions it sends"), m_interact)) {
        m_interact = !m_interact;
        m_actionLog.clear();
        m_drag = Drag::None;
    }

    ImGui::SameLine(0.0f, ImGui::GetFontSize());
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", kHelpGlyph);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(tr("Click the preview to select (Ctrl+click: the box beneath), drag to move or reorder, handles resize (Ctrl: no snapping). Ctrl+S saves."));
        ImGui::TextUnformatted(tr("Wheel zooms around the mouse, middle or right drag moves the view. Right-click a layer or the selection for more."));
        ImGui::TextUnformatted(tr("Arrows move by 1 (Shift: 10), Alt+arrows change the order, Esc selects the parent, Enter the first child, F2 renames."));
        ImGui::TextUnformatted(tr("F or Shift+2 zooms to the selection, Shift+1 fits the screen, Shift+0 is 100%."));
        ImGui::TextUnformatted(tr("Alt shows the distances from the selection to the box under the mouse. Ctrl+Alt+C / Ctrl+Alt+V copy and paste a style."));
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void UiPanel::drawAddMenu() {
    if (ImGui::MenuItem((std::string(kindGlyph("frame")) + "  " + tr("Frame")).c_str())) addNew("frame");
    if (ImGui::MenuItem((std::string(kindGlyph("text")) + "  " + tr("Text")).c_str())) addNew("text");
    if (ImGui::MenuItem((std::string(kindGlyph("image")) + "  " + tr("Image")).c_str())) addNew("image");
    if (ImGui::BeginMenu((std::string(kindGlyph("use")) + "  " + tr("Component")).c_str())) {
        bool any = false;
        for (const std::string& f : m_files)
            if ("ui/" + f != m_path) {
                any = true;
                if (ImGui::MenuItem(f.c_str())) addNew("use", "ui/" + f);
            }
        if (!any) ImGui::TextDisabled("%s", tr("No other screens"));
        ImGui::EndMenu();
    }
}

void UiPanel::drawNodeMenu(const ui::SourcePath& path) {
    m_selection = path;
    m_hasSelection = true;
    const bool node = !path.empty();
    if (ImGui::BeginMenu(tr("Add"))) {
        drawAddMenu();
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(tr("Duplicate"), "Ctrl+D", false, node)) duplicateSelected();
    if (ImGui::MenuItem(tr("Copy"), "Ctrl+C", false, node)) copySelected();
    if (ImGui::MenuItem(tr("Paste"), "Ctrl+V")) paste();
    if (ImGui::MenuItem(tr("Wrap in frame"), nullptr, false, node)) wrapSelected();
    const ui::SourceNode* picked = ui::findNode(m_source, path);
    if (ImGui::MenuItem(tr("Make component..."), nullptr, false, node && picked && picked->kind == "frame")) {
        m_componentName = picked->name;
        m_makeComponentOpen = true;
    }
    if (ImGui::MenuItem(tr("Edit component"), nullptr, false, canEditComponent())) editComponent();
    ImGui::Separator();
    if (ImGui::MenuItem(tr("Copy style"), "Ctrl+Alt+C")) copyStyle();
    if (ImGui::MenuItem(tr("Paste style"), "Ctrl+Alt+V", false, !m_styleClipboard.empty())) pasteStyle();
    if (const ui::SourceNode* n = ui::findNode(m_source, path); n && !n->name.empty()) {
        const bool hidden = std::find(m_hiddenNodes.begin(), m_hiddenNodes.end(), n->name) != m_hiddenNodes.end();
        const bool locked = std::find(m_lockedNodes.begin(), m_lockedNodes.end(), n->name) != m_lockedNodes.end();
        if (ImGui::MenuItem(tr("Hide in the editor"), nullptr, hidden)) {
            toggleName(m_hiddenNodes, n->name);
            refreshDraft();
        }
        if (ImGui::MenuItem(tr("Lock (the canvas doesn't pick it)"), nullptr, locked)) toggleName(m_lockedNodes, n->name);
    }
    ImGui::Separator();
    if (ImGui::MenuItem(tr("Delete"), "Del", false, node)) deleteSelected();
}

// One layer row: kind glyph, name (the kind when unnamed), marks at its end (red ! binding problems, a link for bound nodes, eye and
// lock while hovered or set); right-click for the node menu, F2 or double-click renames in place, hovering outlines the node on the
// canvas, dragging moves it (above/below a row: before/after it, the middle of a frame: into it), a dropped view model field binds it.
// The filter keeps matches and their ancestors
void UiPanel::drawTree(ui::SourceNode& node, const ui::SourcePath& path) {
    if (!treeMatches(node)) return;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen |
                               ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_AllowOverlap;
    if (node.children.empty() || node.kind == "use") flags |= ImGuiTreeNodeFlags_Leaf;
    if (m_hasSelection && m_selection == path) flags |= ImGuiTreeNodeFlags_Selected;
    if ((m_hasSelection && isUnder(m_selection, path) && m_selection != path) || !m_layerFilter.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    else if (m_openAll != 0) ImGui::SetNextItemOpen(m_openAll > 0, ImGuiCond_Always);
    ImGui::PushID(static_cast<int>(path.empty() ? -1 : static_cast<int>(path.back())));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 2.0f));
    const bool renaming = m_renaming && m_renamePath == path;
    std::string value;
    const bool fileHidden = ui::getProperty(node, "hidden", value);
    const bool hiddenHere = editorHidden(path), lockedHere = editorLocked(path);
    if (fileHidden || hiddenHere) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const std::string label = std::string(kindGlyph(node.kind)) + "  " + (renaming ? std::string() : node.name.empty() ? node.kind : node.name);
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s", label.c_str());
    if (fileHidden || hiddenHere) ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    const ImVec2 rowMin = ImGui::GetItemRectMin(), rowMax = ImGui::GetItemRectMax();
    const bool rowHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (!renaming) {
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            m_selection = path;
            m_hasSelection = true;
        }
        if (m_editable && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
            m_renaming = m_renameFocus = true;
            m_renamePath = path;
            m_renameText = node.name;
        }
        if (m_editable && ImGui::BeginPopupContextItem("##nodeMenu")) {
            drawNodeMenu(path);
            ImGui::EndPopup();
        }
        if (rowHovered) {
            m_hover = path;
            m_hasHover = m_treeHovered = true;
        }
        // Drag a row onto another to move it
        if (m_editable && !path.empty() && ImGui::BeginDragDropSource()) {
            m_dragNode = true;
            m_dragFrom = path;
            ImGui::SetDragDropPayload("MK_UI_NODE", nullptr, 0);
            ImGui::TextUnformatted(label.c_str());
            ImGui::EndDragDropSource();
        }
        if (m_editable && ImGui::BeginDragDropTarget()) {
            const f32 y = ImGui::GetMousePos().y, h = rowMax.y - rowMin.y;
            const bool container = node.kind == "frame" || node.kind == "board";
            // Zone: top quarter before, bottom quarter after, middle into (frames), else the nearer half
            int zone = container ? (y < rowMin.y + h * 0.25f ? -1 : y > rowMax.y - h * 0.25f ? 1 : 0) : (y < rowMin.y + h * 0.5f ? -1 : 1);
            if (path.empty()) zone = 0; // the board: only into
            ImDrawList* draw = ImGui::GetWindowDrawList();
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("MK_UI_NODE", ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                const ImU32 c = IM_COL32(255, 200, 60, 255);
                if (zone == 0) draw->AddRect(rowMin, rowMax, c, 3.0f, 0, 2.0f);
                else draw->AddLine(ImVec2(rowMin.x, zone < 0 ? rowMin.y : rowMax.y), ImVec2(rowMax.x, zone < 0 ? rowMin.y : rowMax.y), c, 2.0f);
                if (p->IsDelivery() && m_dragNode && !isUnder(path, m_dragFrom)) {
                    m_moveQueued = true;
                    m_moveFrom = m_dragFrom;
                    if (zone == 0) {
                        m_moveParent = path;
                        m_moveAt = static_cast<u32>(node.children.size()); // the end (clamped after the removal)
                    } else {
                        m_moveParent.assign(path.begin(), path.end() - 1);
                        const bool sameParent = m_dragFrom.size() == path.size() && std::equal(m_moveParent.begin(), m_moveParent.end(), m_dragFrom.begin());
                        u32 at = path.back() + (zone > 0 ? 1u : 0u);
                        if (sameParent && m_dragFrom.back() < at) --at; // the removal shifts later siblings down
                        m_moveAt = at;
                    }
                    m_dragNode = false;
                }
            }
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("MK_UI_FIELD", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
                if (p->IsDelivery()) {
                    m_bindQueued = true;
                    m_bindPath = path;
                    m_bindField = static_cast<const char*>(p->Data);
                }
            }
            ImGui::EndDragDropTarget();
        }
    }
    // Marks at the row's end, right to left: lock and eye (while hovered or set), binding problems (red !), bound (link)
    std::string problems;
    for (const auto& [at, problem] : m_nodeProblems)
        if (at == path) problems += (problems.empty() ? "" : "\n") + problem;
    const f32 glyph = ImGui::GetFontSize() * 1.1f;
    f32 right = rowMax.x - 2.0f;
    bool movedCursor = renaming;
    if (!node.name.empty() && !renaming && (rowHovered || hiddenHere || lockedHere)) {
        const bool ownLock = std::find(m_lockedNodes.begin(), m_lockedNodes.end(), node.name) != m_lockedNodes.end();
        const bool ownHide = std::find(m_hiddenNodes.begin(), m_hiddenNodes.end(), node.name) != m_hiddenNodes.end();
        movedCursor = true;
        for (int i = 0; i < 2; ++i) {
            right -= glyph;
            ImGui::SetCursorScreenPos(ImVec2(right, rowMin.y));
            const bool lock = i == 0;
            const bool on = lock ? ownLock : ownHide;
            ImGui::PushStyleColor(ImGuiCol_Text, on ? ImVec4(1.0f, 0.8f, 0.35f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.0f, 1.0f));
            if (ImGui::SmallButton(lock ? (std::string(kLockGlyph) + "##lock").c_str() : (std::string(on ? kHiddenGlyph : kEyeGlyph) + "##eye").c_str())) {
                toggleName(lock ? m_lockedNodes : m_hiddenNodes, node.name);
                if (!lock) refreshDraft();
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            ImGui::SetItemTooltip("%s", lock ? tr("Lock in the editor (the canvas doesn't pick it)") : tr("Hide in the editor (not saved)"));
        }
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const f32 textY = rowMin.y + (rowMax.y - rowMin.y - ImGui::GetTextLineHeight()) * 0.5f;
    if (!problems.empty()) {
        right -= glyph * 0.7f;
        draw->AddText(ImVec2(right, textY), IM_COL32(255, 96, 96, 255), "!");
    }
    static const char* const kBindingKeys[] = {"visible", "disabled", "widthRatio", "colorField", "list", "button", "toggle", "slider", "select", "textInput"};
    bool bound = ui::getProperty(node, "content", value) && value.find('{') != std::string::npos;
    for (const char* k : kBindingKeys) bound = bound || ui::getProperty(node, k, value);
    if (bound) {
        right -= glyph;
        draw->AddText(ImVec2(right, textY), ImGui::GetColorU32(ImGuiCol_TextDisabled), kBoundGlyph);
    }
    if (!renaming && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsAnyItemActive()) {
        std::string tip = node.kind == "use" ? node.kind + " " + node.args : node.kind;
        if (fileHidden) tip += std::string("\n") + tr("hidden in the file");
        if (!problems.empty()) tip += "\n" + trf("No such field: %s", problems.c_str());
        ImGui::SetTooltip("%s", tip.c_str());
    }
    if (renaming) { // the name field over the row, after the glyph; Enter or a click elsewhere keeps it, Esc cancels
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x + ImGui::GetTreeNodeToLabelSpacing() + ImGui::GetFontSize() * 1.6f, rowMin.y));
        ImGui::SetNextItemWidth(rowMax.x - ImGui::GetCursorScreenPos().x);
        if (m_renameFocus) {
            ImGui::SetKeyboardFocusHere();
            m_renameFocus = false;
        }
        const bool enter = uiInputText("##rename", "", m_renameText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (enter || ImGui::IsItemDeactivated()) {
            std::string name;
            for (char c : m_renameText) name += c == ' ' ? '_' : c; // a name is one word on the head line
            if (!ImGui::IsKeyPressed(ImGuiKey_Escape) && !name.empty() && name != node.name) {
                node.name = ui::uniqueName(m_source, name);
                syncSource();
            }
            m_renaming = false;
        }
    }
    if (movedCursor) { // back under the row (the marks or the name field moved the cursor)
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMax.y));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }
    if (open) {
        if (node.kind != "use")
            for (u32 c = 0; c < node.children.size(); ++c) {
                ui::SourcePath child = path;
                child.push_back(c);
                drawTree(node.children[c], child);
            }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void UiPanel::drawProperties(ui::SourceNode& node, bool placedByLayout) {
    // Header: kind glyph and kind, then the name filling the row
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(kindGlyph(node.kind));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", node.kind.c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
    inputString("##name", node.name);
    ImGui::SameLine();
    if (iconButton(kCodeGlyph, tr("Open this node in the code editor"))) openInCodeEditor(m_path, nodeLine(m_selection));
    if (node.kind == "use") {
        const bool editable = canEditComponent();
        ImGui::SetNextItemWidth(editable ? -ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x : -1.0f);
        inputString("##component", node.args);
        if (editable) {
            ImGui::SameLine();
            if (iconButton(kindGlyph("use"), tr("Edit component (opens its file, with a button back here)"))) editComponent();
        }
    }

    // Common properties with their own widgets (the lines below show every property as text)
    std::string value;
    ImGui::SeparatorText(tr("Layout"));
    if (placedByLayout) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", tr("Placed by the parent's layout: drag it in the preview to change its order."));
        ImGui::PopTextWrapPos();
    } else if (node.kind != "board") {
        std::vector<f32> pos = ui::getProperty(node, "pos", value) ? numbers(value) : std::vector<f32>{0.0f, 0.0f};
        if (pos.size() == 2 && ImGui::DragFloat2(propertyRow(tr("Position")).c_str(), pos.data(), 1.0f, 0.0f, 0.0f, "%.0f"))
            ui::setProperty(node, "pos", number(pos[0]) + " " + number(pos[1]));
    }
    if (ui::getProperty(node, "size", value)) {
        const std::vector<f32> size = numbers(value);
        if (size.size() == 2) {
            f32 v[2] = {size[0], size[1]};
            if (ImGui::DragFloat2(propertyRow(tr("Size")).c_str(), v, 1.0f, 0.0f, 100000.0f, "%.0f")) ui::setProperty(node, "size", number(v[0]) + " " + number(v[1]));
        }
    }
    // Where the layout put it (read-only): what the game draws at the chosen resolution
    if (ui::NodeBox drawn; boxOf(m_selection, drawn)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", trf("Drawn at %.0f, %.0f  size %.0f x %.0f", drawn.x, drawn.y, drawn.w, drawn.h).c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tr("Result of the layout at the canvas resolution (logical units)"));
    }

    // A number property: a drag box when the value is numbers (count = how many), the raw text dimmed when it is a $token or a word
    f32 rowReserve = 0.0f; // room kept at a row's end (the token combo)
    auto numberRow = [&](const char* label, const char* key, f32 fallback, f32 speed, f32 lo, f32 hi, const char* format) {
        std::string v;
        const bool has = ui::getProperty(node, key, v);
        std::vector<f32> n = has ? numbers(v) : std::vector<f32>{fallback};
        if (n.size() != 1) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s  %s %s", label, key, v.c_str());
            return;
        }
        const std::string id = propertyRow(label);
        if (rowReserve > 0.0f) ImGui::SetNextItemWidth(-rowReserve);
        if (ImGui::DragFloat(id.c_str(), &n[0], speed, lo, hi, format, ImGuiSliderFlags_AlwaysClamp)) ui::setProperty(node, key, number(n[0]));
    };
    // A word property from a list; (default) removes the line
    auto comboRow = [&](const char* label, const char* key, const char* const* options, size_t count) {
        std::string v;
        const bool has = ui::getProperty(node, key, v);
        if (beginCombo(propertyRow(label).c_str(), has ? v.c_str() : tr("(default)"))) {
            if (ImGui::Selectable(tr("(default)"), !has)) ui::removeProperty(node, key);
            for (size_t i = 0; i < count; ++i)
                if (ImGui::Selectable(options[i], has && v == options[i])) ui::setProperty(node, key, options[i]);
            ImGui::EndCombo();
        }
    };

    // Children's layout (frames and the board): kind, then the flex/grid settings that go with it
    if (node.kind == "frame" || node.kind == "board") {
        std::string layout;
        const bool hasLayout = ui::getProperty(node, "layout", layout);
        const std::vector<std::string> parts = [&] {
            std::vector<std::string> w;
            size_t a = 0;
            while ((a = layout.find_first_not_of(' ', a)) != std::string::npos) {
                const size_t b = layout.find(' ', a);
                w.push_back(layout.substr(a, b == std::string::npos ? std::string::npos : b - a));
                a = b;
            }
            return w;
        }();
        const std::string kind = parts.empty() ? std::string() : parts[0];
        const char* kinds[] = {"row", "column", "grid"};
        if (beginCombo(propertyRow(tr("Children"), tr("How the children are placed (layout). Free: each by its own pos")).c_str(),
                       hasLayout ? kind.c_str() : tr("free (pos)"))) {
            if (ImGui::Selectable(tr("free (pos)"), !hasLayout)) ui::removeProperty(node, "layout");
            for (const char* k : kinds)
                if (ImGui::Selectable(k, kind == k)) {
                    // row <-> column keeps reverse/wrap; grid starts plain
                    std::string line = k;
                    if (std::string(k) != "grid" && kind != "grid")
                        for (size_t i = 1; i < parts.size(); ++i) line += " " + parts[i];
                    ui::setProperty(node, "layout", line);
                }
            ImGui::EndCombo();
        }
        if (hasLayout && kind != "grid") {
            bool wrap = std::find(parts.begin(), parts.end(), "wrap") != parts.end();
            if (ImGui::Checkbox(tr("Wrap"), &wrap)) {
                std::string line = kind;
                for (size_t i = 1; i < parts.size(); ++i)
                    if (parts[i] != "wrap") line += " " + parts[i];
                ui::setProperty(node, "layout", wrap ? line + " wrap" : line);
            }
        }
        if (hasLayout) {
            comboRow(tr("Justify"), "justify", kJustify, std::size(kJustify));
            comboRow(tr("Align"), "align", kAlign, std::size(kAlign));
            numberRow(tr("Gap"), "gap", 0.0f, 0.5f, 0.0f, 10000.0f, "%.0f");
        }
        // Padding: one number for all sides, or top right bottom left
        std::string pad;
        std::vector<f32> p = ui::getProperty(node, "padding", pad) ? numbers(pad) : std::vector<f32>{0.0f};
        if (p.size() == 2) p = {p[0], p[1], p[0], p[1]};
        if (p.size() == 1) p = {p[0], p[0], p[0], p[0]};
        if (p.size() == 4) {
            if (ImGui::DragFloat4(propertyRow(tr("Padding"), tr("top, right, bottom, left")).c_str(), p.data(), 0.5f, 0.0f, 10000.0f, "%.0f")) {
                const bool same = p[0] == p[1] && p[1] == p[2] && p[2] == p[3];
                ui::setProperty(node, "padding", same ? number(p[0]) : number(p[0]) + " " + number(p[1]) + " " + number(p[2]) + " " + number(p[3]));
            }
        } else {
            ImGui::TextDisabled("%s  padding %s", tr("Padding"), pad.c_str());
        }
    }

    // Style: fill and text color with a picker (or the $token they use) and a token combo beside them
    if (node.kind != "board" || ui::getProperty(node, "fill", value)) ImGui::SeparatorText(tr("Style"));
    for (const char* key : {"fill", "color"}) {
        if (!ui::getProperty(node, key, value)) continue;
        const std::string label = std::string(key == std::string("fill") ? tr("Fill") : tr("Text color")) + "##" + key;
        f32 rgba[4];
        const f32 combo = ImGui::GetFrameHeight() * 1.6f + ImGui::GetStyle().ItemSpacing.x;
        if (parseHex(value, rgba)) {
            const std::string id = propertyRow(label.c_str());
            ImGui::SetNextItemWidth(-combo);
            if (colorEdit(id.c_str(), rgba, true)) ui::setProperty(node, key, hex(rgba, value.size() == 9 || rgba[3] < 1.0f));
        } else {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label.substr(0, label.find("##")).c_str());
            ImGui::SameLine(ImGui::GetContentRegionMax().x * 0.4f);
            ImGui::TextDisabled("%s", value.c_str());
        }
        ImGui::SameLine(ImGui::GetContentRegionMax().x - combo + ImGui::GetStyle().ItemSpacing.x);
        tokenCombo(node, key, true);
    }
    if (node.kind != "board") {
        if (node.kind == "frame" || node.kind == "image") {
            rowReserve = ImGui::GetFrameHeight() * 1.6f + ImGui::GetStyle().ItemSpacing.x;
            numberRow(tr("Corner radius"), "radius", 0.0f, 0.5f, 0.0f, 10000.0f, "%.0f");
            rowReserve = 0.0f;
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight() * 1.6f);
            tokenCombo(node, "radius", false);
        }
        numberRow(tr("Opacity"), "opacity", 1.0f, 0.01f, 0.0f, 1.0f, "%.2f");
    }

    // Text: the content on several lines, size and alignment
    if (node.kind == "text") {
        ImGui::SeparatorText(tr("Text"));
        std::string content;
        if (ui::getProperty(node, "content", content)) {
            ImGui::TextDisabled("%s", tr("Content ({field} inserts a view model value)"));
            ImGui::SetNextItemWidth(-1.0f);
            if (inputString("##content", content)) ui::setProperty(node, "content", content);
        } else if (ui::getProperty(node, "key", content)) {
            ImGui::TextDisabled("%s", trf("Translated text: key %s", content.c_str()).c_str());
        }
        numberRow(tr("Font size"), "fontSize", 24.0f, 0.25f, 1.0f, 1000.0f, "%.0f");
        comboRow(tr("Text align"), "textAlign", kTextAlign, std::size(kTextAlign));
    }

    ImGui::SeparatorText(tr("State"));
    const char* const states[] = {tr("Normal"), tr("Hover"), tr("Pressed"), tr("Focus"), tr("Disabled"), tr("Checked")};
    if (beginCombo(propertyRow(tr("Show state"), tr("Draw the selected node as if hovered, pressed ...")).c_str(), states[std::clamp(m_previewState, 0, 5)])) {
        for (int i = 0; i < 6; ++i)
            if (ImGui::Selectable(states[i], m_previewState == i)) m_previewState = i;
        ImGui::EndCombo();
    }
    drawBindings(node);

    // Every line of the node: edit as text, remove with x. Comments are shown dimmed
    ImGui::SeparatorText(tr("All lines"));
    for (size_t i = 0; i < node.items.size(); ++i) {
        ui::SourceItem& item = node.items[i];
        if (item.child >= 0 || item.line.empty()) continue;
        ImGui::PushID(static_cast<int>(i));
        if (item.line[0] == '#') {
            ImGui::TextDisabled("%s", item.line.c_str());
        } else {
            // A key mkui doesn't know (typo) is red; the file's problems say more
            const std::string key = item.line.substr(0, item.line.find(' '));
            const bool unknown = !knownProperty(key);
            if (unknown) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 96, 96, 255));
            ImGui::SetNextItemWidth(-28.0f);
            inputString("##line", item.line);
            if (unknown) {
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", trf("Unknown property '%s'", key.c_str()).c_str());
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                node.items.erase(node.items.begin() + static_cast<std::ptrdiff_t>(i));
                ImGui::PopID();
                break;
            }
        }
        ImGui::PopID();
    }
    // Add a property: pick the name, type the value
    ImGui::SetNextItemWidth(120.0f);
    if (beginCombo("##addKey", tr("Add..."), ImGuiComboFlags_HeightLarge)) {
        for (const char* key : kProperties)
            if (ImGui::Selectable(key)) m_newProperty = std::string(key) + " ";
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-60.0f);
    const bool enter = ImGui::InputText("##newLine", m_newProperty.data(), m_newProperty.capacity() + 1,
                                        ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_EnterReturnsTrue,
                                        [](ImGuiInputTextCallbackData* d) {
                                            if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
                                                std::string& s = *static_cast<std::string*>(d->UserData);
                                                s.resize(static_cast<size_t>(d->BufTextLen));
                                                d->Buf = s.data();
                                            }
                                            return 0;
                                        },
                                        &m_newProperty);
    ImGui::SameLine();
    if ((ImGui::Button(tr("Add")) || enter) && !m_newProperty.empty()) {
        const std::vector<std::string> words = [&] {
            std::vector<std::string> w;
            size_t a = 0;
            while ((a = m_newProperty.find_first_not_of(' ', a)) != std::string::npos) {
                const size_t b = m_newProperty.find(' ', a);
                w.push_back(m_newProperty.substr(a, b - a));
                a = b;
            }
            return w;
        }();
        if (!words.empty()) {
            // A repeatable line (on/set/span/shadow) is added; others replace the same property
            const std::string& k = words[0];
            if (k == "on" || k == "set" || k == "span" || k == "shadow") {
                ui::SourceItem line{m_newProperty, -1};
                size_t at = node.items.size();
                for (size_t i = 0; i < node.items.size(); ++i)
                    if (node.items[i].child >= 0) {
                        at = i;
                        break;
                    }
                node.items.insert(node.items.begin() + static_cast<std::ptrdiff_t>(at), line);
            } else {
                std::string rest;
                for (size_t i = 1; i < words.size(); ++i) rest += (i > 1 ? " " : "") + words[i];
                ui::setProperty(node, k, rest);
            }
        }
        m_newProperty.clear();
    }
}

const reflect::TypeInfo* UiPanel::bindingScope(const ui::SourcePath& path) const {
    const reflect::TypeInfo* scope = m_vmType;
    std::string value;
    for (size_t depth = 0; depth < path.size() && scope; ++depth) { // every ancestor: a list changes the scope for what's inside it
        const ui::SourceNode* parent = ui::findNode(m_source, ui::SourcePath(path.begin(), path.begin() + static_cast<std::ptrdiff_t>(depth)));
        if (!parent || !ui::getProperty(*parent, "list", value)) continue;
        const reflect::FieldInfo* f = fieldAt(scope, value);
        scope = f && isList(*f) ? f->value->element->type : nullptr;
    }
    return scope;
}

void UiPanel::collectBindingProblems(std::vector<std::string>& out) const {
    if (!m_vmType || m_path.empty()) return;
    if (const ui::SourceNode* board = ui::findNode(m_source, {})) checkBindings(*board, m_vmType, m_path + ": ", out);
}

void UiPanel::collectNodeProblems() {
    m_nodeProblems.clear();
    if (!m_vmType || !m_editable) return;
    std::vector<std::string> lines;
    if (const ui::SourceNode* board = ui::findNode(m_source, {})) checkBindings(*board, m_vmType, {}, lines, {}, &m_nodeProblems);
}

bool UiPanel::treeMatches(const ui::SourceNode& node) const {
    if (m_layerFilter.empty() || fuzzyScore(node.name, m_layerFilter) >= 0 || fuzzyScore(node.kind, m_layerFilter) >= 0) return true;
    if (node.kind == "use") return false;
    for (const ui::SourceNode& child : node.children)
        if (treeMatches(child)) return true;
    return false;
}

int UiPanel::nodeLine(const ui::SourcePath& path) const {
    std::string text;
    const std::string file = findAssetFile(m_path);
    if (file.empty() || !readTextFile(file, text)) return 1;
    // The nearest named node on the path (names are unique in a screen): its head line "<kind> <name> ..."
    for (size_t depth = path.size() + 1; depth-- > 0;) {
        const ui::SourceNode* n = ui::findNode(m_source, ui::SourcePath(path.begin(), path.begin() + static_cast<std::ptrdiff_t>(depth)));
        if (!n || n->name.empty()) continue;
        const std::string head = n->kind + " " + n->name;
        int line = 1;
        for (size_t start = 0; start < text.size(); ++line) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            const size_t first = text.find_first_not_of(" \t", start);
            if (first != std::string::npos && first < end && text.compare(first, head.size(), head) == 0 &&
                (first + head.size() >= end || text[first + head.size()] == ' ' || text[first + head.size()] == '\r'))
                return line;
            start = end + 1;
        }
    }
    return 1;
}

void UiPanel::handleKeys() {
    if (!editing() || !m_screen || m_renaming) return;
    if (!m_hasSelection) return;
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
        if (const ui::SourceNode* n = ui::findNode(m_source, m_selection)) {
            m_renaming = m_renameFocus = true;
            m_renamePath = m_selection;
            m_renameText = n->name;
        }
        return;
    }
    if (io.KeyCtrl && io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_C, false)) return copyStyle();
    if (io.KeyCtrl && io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_V, false)) return pasteStyle();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { // up one level; from the board, nothing
        if (m_selection.empty()) m_hasSelection = false;
        else m_selection.pop_back();
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) { // down to the first child
        const ui::SourceNode* n = ui::findNode(m_source, m_selection);
        if (n && n->kind != "use" && !n->children.empty()) m_selection.push_back(0);
        return;
    }
    if (io.KeyCtrl) return;
    const int dx = (ImGui::IsKeyPressed(ImGuiKey_RightArrow) ? 1 : 0) - (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ? 1 : 0);
    const int dy = (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : 0) - (ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? 1 : 0);
    if (!dx && !dy) return;
    if (io.KeyAlt) moveInOrder(dx + dy > 0 ? 1 : -1);
    else nudge(static_cast<f32>(dx) * (io.KeyShift ? 10.0f : 1.0f), static_cast<f32>(dy) * (io.KeyShift ? 10.0f : 1.0f));
}

// Arrow keys: a free node's pos by whole units (nodes a layout places don't move; $token positions aren't touched)
void UiPanel::nudge(f32 dx, f32 dy) {
    if (m_selection.empty() || !layoutOf(m_selection).empty()) return;
    ui::SourceNode* n = ui::findNode(m_source, m_selection);
    if (!n) return;
    std::string value;
    const std::vector<f32> pos = ui::getProperty(*n, "pos", value) ? numbers(value) : std::vector<f32>{0.0f, 0.0f};
    if (pos.size() != 2) return;
    ui::setProperty(*n, "pos", number(pos[0] + dx) + " " + number(pos[1] + dy));
    syncSource();
}

// Alt+arrows: one place earlier or later among the siblings (the order a layout places them in, or drawing order for free nodes)
void UiPanel::moveInOrder(int step) {
    if (m_selection.empty()) return;
    const ui::SourcePath parent(m_selection.begin(), m_selection.end() - 1);
    const ui::SourceNode* pn = ui::findNode(m_source, parent);
    const int to = static_cast<int>(m_selection.back()) + step;
    if (!pn || to < 0 || to >= static_cast<int>(pn->children.size())) return;
    ui::SourcePath moved;
    if (ui::moveNode(m_source, m_selection, parent, static_cast<u32>(to), &moved)) {
        m_selection = moved;
        syncSource();
    }
}

void UiPanel::drawBindings(ui::SourceNode& node) {
    ImGui::SeparatorText(tr("Bindings"));
    const reflect::TypeInfo* scope = bindingScope(m_selection);
    if (!scope) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", tr("Pick a view model under Preview values to choose fields here."));
        ImGui::PopTextWrapPos();
        return;
    }
    std::string value;
    for (const BindingKey& b : kBindings) {
        const bool has = ui::getProperty(node, b.key, value);
        const bool negated = has && b.negatable && !value.empty() && value[0] == '!';
        const std::string current = !has ? std::string() : negated ? value.substr(1) : value;
        const bool unknown = has && !fieldAt(scope, current);
        ImGui::PushID(b.key);
        if (unknown) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 96, 96, 255));
        const std::string preview = !has ? std::string(tr("(none)")) : unknown ? trf("%s (no such field)", current.c_str()) : current;
        const bool open = beginCombo(propertyRow(b.label()).c_str(), preview.c_str());
        if (unknown) ImGui::PopStyleColor();
        if (open) {
            if (ImGui::Selectable(tr("(none)"), !has)) ui::removeProperty(node, b.key);
            std::vector<std::string> names;
            fieldNames(*scope, "", b.test, names);
            for (const std::string& name : names)
                if (ImGui::Selectable(name.c_str(), name == current)) ui::setProperty(node, b.key, (negated ? "!" : "") + name);
            ImGui::EndCombo();
        }
        if (b.negatable && has) {
            bool invert = negated;
            if (ImGui::Checkbox(tr("Invert"), &invert)) ui::setProperty(node, b.key, (invert ? "!" : "") + current);
        }
        ImGui::PopID();
    }
    if (node.kind == "text" && ui::getProperty(node, "content", value)) {
        if (beginCombo(propertyRow(tr("Insert field")).c_str(), tr("Choose..."))) {
            std::vector<std::string> names;
            fieldNames(*scope, "", anyValue, names);
            for (const std::string& name : names)
                if (ImGui::Selectable(name.c_str())) ui::setProperty(node, "content", value + "{" + name + "}");
            ImGui::EndCombo();
        }
        for (const std::string& name : textFields(value))
            if (!fieldAt(scope, name)) ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.38f, 1.0f), "%s", trf("%s (no such field)", ("{" + name + "}").c_str()).c_str());
    }
}

void UiPanel::drawSavePrompt() {
    if (m_pendingScreen >= 0 && !ImGui::IsPopupOpen("##uiSave")) ImGui::OpenPopup("##uiSave");
    if (!ImGui::BeginPopupModal("##uiSave", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("%s", trf("%s has unsaved changes.", m_path.c_str()).c_str());
    auto openPending = [this] {
        m_selected = m_pendingScreen;
        m_pendingScreen = -1;
        openSelected();
        ImGui::CloseCurrentPopup();
    };
    if (ImGui::Button(tr("Save"))) {
        if (save()) openPending();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Discard"))) {
        m_saved = m_live; // so openSelected doesn't ask again
        openPending();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) {
        m_pendingScreen = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Components ------------------------------------------------------------------------------------------------

bool UiPanel::makeComponent(const std::string& name) {
    if (!editing() || !m_hasSelection || m_selection.empty()) return false;
    const ui::SourceNode* n = ui::findNode(m_source, m_selection);
    ui::NodeBox b;
    const std::string dir = findAsset("assets/ui");
    if (!n || n->kind != "frame" || dir.empty() || !boxOf(m_selection, b)) return false;
    const std::string file = dir + "/components/" + name + ".ui";
    if (fileExists(file)) {
        notice(trf("ui/components/%s.ui exists already", name.c_str()));
        return false;
    }
    const std::string before = m_live;
    ui::Source component;
    if (!ui::extractComponent(m_source, m_selection, name, "ui/components/" + name + ".ui", static_cast<i32>(b.w + 0.5f),
                              static_cast<i32>(b.h + 0.5f), component))
        return false;
    createDirectories(dir + "/components");
    if (!writeTextFile(file, ui::writeSource(component))) {
        MEALKIT_ERROR("UI edit: cannot write %s", file.c_str());
        setText(before);
        return false;
    }
    MEALKIT_INFO("UI edit: component %s from %s", file.c_str(), m_path.c_str());
    refresh();
    syncSource();
    notice(trf("Component ui/components/%s.ui (save the screen to keep the use)", name.c_str()));
    return true;
}

bool UiPanel::canEditComponent() const {
    if (!m_hasSelection) return false;
    const ui::SourceNode* n = ui::findNode(m_source, m_selection);
    if (!n || n->kind != "use" || n->args.rfind("ui/", 0) != 0) return false;
    return std::find(m_files.begin(), m_files.end(), n->args.substr(3)) != m_files.end();
}

void UiPanel::editComponent() {
    if (canEditComponent()) m_editComponentQueued = true;
}

void UiPanel::openFile(const std::string& file) {
    for (size_t i = 0; i < m_files.size(); ++i)
        if (m_files[i] == file) {
            if (dirty()) {
                m_pendingScreen = static_cast<int>(i); // drawSavePrompt asks, then opens it
            } else {
                m_selected = static_cast<int>(i);
                zoomFit();
                openSelected();
            }
            return;
        }
}

void UiPanel::drawMakeComponent() {
    if (m_editComponentQueued) {
        m_editComponentQueued = false;
        if (canEditComponent() && m_selected >= 0 && m_selected < static_cast<int>(m_files.size())) {
            const std::string target = ui::findNode(m_source, m_selection)->args.substr(3);
            m_returnScreen = m_files[static_cast<size_t>(m_selected)];
            m_returnSelection = m_selection;
            openFile(target);
        }
    }
    if (m_makeComponentOpen) {
        ImGui::OpenPopup("##makeComponent");
        m_makeComponentOpen = false;
    }
    if (!ImGui::BeginPopup("##makeComponent")) return;
    ImGui::TextUnformatted(tr("Make component"));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    const bool enter = uiInputText("##componentName", tr("name (ui/components/<name>.ui)"), m_componentName, ImGuiInputTextFlags_EnterReturnsTrue);
    bool valid = !m_componentName.empty();
    for (char c : m_componentName) valid = valid && (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-');
    const std::string dir = findAsset("assets/ui");
    const bool exists = valid && !dir.empty() && fileExists(dir + "/components/" + m_componentName + ".ui");
    if (!m_componentName.empty() && !valid) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", tr("Letters, digits, _ and - only"));
    if (exists) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", tr("A component with this name exists"));
    ImGui::TextDisabled("%s", tr("Where it sits (pos, size, constraints ...) stays on the use, the rest moves to the component."));
    ImGui::BeginDisabled(!valid || exists);
    if (ImGui::Button(tr("Create")) || (enter && valid && !exists)) {
        makeComponent(m_componentName);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace mealkit::editor
