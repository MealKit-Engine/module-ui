// [UI] panel editing: source, selection, node tree, properties, history (UiPanel.h). Drawing the preview is in UiPanel.cpp
#include "mealkit/core/Log.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/editor/UiPanel.h"
#include "mealkit/platform/Paths.h"

#include <algorithm>
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

// Walks a node and its children with the fields in scope and reports names that don't exist
void checkBindings(const ui::SourceNode& node, const reflect::TypeInfo* scope, const std::string& where, std::vector<std::string>& out) {
    std::string value;
    const std::string at = where + (node.name.empty() ? node.kind : node.name);
    for (const BindingKey& b : kBindings) {
        if (!ui::getProperty(node, b.key, value) || value.empty()) continue;
        const std::string name = b.negatable && value[0] == '!' ? value.substr(1) : value;
        if (!fieldAt(scope, name)) out.push_back(at + ": " + b.key + " " + value);
    }
    if (ui::getProperty(node, "content", value))
        for (const std::string& name : textFields(value))
            if (!fieldAt(scope, name)) out.push_back(at + ": {" + name + "}");
    if (node.kind == "use") return; // the component file has its own view model paths
    const reflect::TypeInfo* inner = scope;
    if (ui::getProperty(node, "list", value)) {
        const reflect::FieldInfo* f = fieldAt(scope, value);
        if (!f || !isList(*f)) return; // already reported; names inside can't be checked
        inner = f->value->element->type;
    }
    for (const ui::SourceNode& child : node.children) checkBindings(child, inner, at + " > ", out);
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

} // namespace

void UiPanel::loadSource() {
    m_source = {};
    m_editable = false;
    m_hasSelection = false;
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
        if (m_screen) m_ui.setDraft(m_screen, m_live == m_saved ? nullptr : &m_live);
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
    if (m_screen) m_ui.setDraft(m_screen, nullptr);
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

void UiPanel::drawEditor() {
    ImGui::SeparatorText(tr("Edit screen"));
    if (!m_editable) {
        ImGui::BeginDisabled();
        bool off = false;
        ImGui::Checkbox(tr("Edit"), &off);
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s", m_source.exported ? tr("Exported from Penpot: edit it in Penpot and export again.")
                                                    : tr("The file could not be read for editing (see the log)."));
        return;
    }
    ImGui::Checkbox(tr("Edit"), &m_editing);
    ImGui::SameLine();
    ImGui::BeginDisabled(!dirty());
    if (ImGui::Button(tr("Save"))) save();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_undo.empty());
    if (ImGui::Button(tr("Undo"))) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_redo.empty());
    if (ImGui::Button(tr("Redo"))) redo();
    ImGui::EndDisabled();
    if (dirty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", tr("Unsaved"));
    }
    if (!m_editing) return;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(tr("Click the preview to select (Ctrl+click: the box beneath), drag to move or reorder, handles resize (Ctrl: no snapping). Ctrl+S saves."));
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    // New nodes; the rest acts on the selection
    if (ImGui::SmallButton(tr("+ Frame"))) addNew("frame");
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("+ Text"))) addNew("text");
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("+ Image"))) addNew("image");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    if (ImGui::BeginCombo("##addComponent", tr("+ Component"))) {
        for (const std::string& f : m_files)
            if (ImGui::Selectable(f.c_str()) && "ui/" + f != m_path) addNew("use", "ui/" + f);
        ImGui::EndCombo();
    }
    ImGui::BeginDisabled(!m_hasSelection || m_selection.empty());
    if (ImGui::SmallButton(tr("Duplicate"))) duplicateSelected();
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Copy"))) copySelected();
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Wrap in frame"))) wrapSelected();
    ImGui::SameLine();
    ImGui::EndDisabled();
    if (ImGui::SmallButton(tr("Paste"))) paste();

    if (ImGui::BeginChild("##uiTree", ImVec2(-1.0f, ImGui::GetTextLineHeightWithSpacing() * 12.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY)) {
        if (ui::SourceNode* board = ui::findNode(m_source, {})) drawTree(*board, {});
    }
    ImGui::EndChild();

    if (m_hasSelection)
        if (ui::SourceNode* n = ui::findNode(m_source, m_selection)) {
            // Inside a flex/grid layout the parent places the node (pos is ignored unless the node is absolute)
            std::string layout, unused;
            const ui::SourceNode* parent = m_selection.empty() ? nullptr : ui::findNode(m_source, ui::SourcePath(m_selection.begin(), m_selection.end() - 1));
            const bool placed = parent && ui::getProperty(*parent, "layout", layout) && !ui::getProperty(*n, "absolute", unused);
            drawProperties(*n, placed);
        }
    syncSource();
}

void UiPanel::drawTree(ui::SourceNode& node, const ui::SourcePath& path) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
    if (node.children.empty() || node.kind == "use") flags |= ImGuiTreeNodeFlags_Leaf;
    if (m_hasSelection && m_selection == path) flags |= ImGuiTreeNodeFlags_Selected;
    if (m_hasSelection && isUnder(m_selection, path) && m_selection != path) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    ImGui::PushID(static_cast<int>(path.empty() ? -1 : static_cast<int>(path.back())));
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s  %s", node.kind.c_str(), node.name.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        m_selection = path;
        m_hasSelection = true;
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
    ImGui::SeparatorText(tr("Properties"));
    ImGui::TextDisabled("%s", node.kind.c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    inputString("##name", node.name);
    if (node.kind == "use") {
        ImGui::SetNextItemWidth(-1.0f);
        inputString("##component", node.args);
    }

    // Common properties with their own widgets (the lines below show every property as text)
    std::string value;
    if (placedByLayout) ImGui::TextDisabled("%s", tr("Placed by the parent's layout: drag it in the preview to change its order."));
    else if (node.kind != "board") {
        std::vector<f32> pos = ui::getProperty(node, "pos", value) ? numbers(value) : std::vector<f32>{0.0f, 0.0f};
        if (pos.size() == 2 && ImGui::DragFloat2(tr("Position"), pos.data(), 1.0f, 0.0f, 0.0f, "%.0f"))
            ui::setProperty(node, "pos", number(pos[0]) + " " + number(pos[1]));
    }
    if (ui::getProperty(node, "size", value)) {
        const std::vector<f32> size = numbers(value);
        if (size.size() == 2) {
            f32 v[2] = {size[0], size[1]};
            if (ImGui::DragFloat2(tr("Size"), v, 1.0f, 0.0f, 100000.0f, "%.0f")) ui::setProperty(node, "size", number(v[0]) + " " + number(v[1]));
        }
    }
    for (const char* key : {"fill", "color"}) {
        f32 rgba[4];
        if (ui::getProperty(node, key, value) && parseHex(value, rgba)) {
            const std::string label = std::string(key == std::string("fill") ? tr("Fill") : tr("Text color")) + "##" + key;
            if (ImGui::ColorEdit4(label.c_str(), rgba, ImGuiColorEditFlags_AlphaBar)) ui::setProperty(node, key, hex(rgba, value.size() == 9 || rgba[3] < 1.0f));
        }
    }

    const char* const states[] = {tr("Normal"), tr("Hover"), tr("Pressed"), tr("Focus"), tr("Disabled"), tr("Checked")};
    ImGui::Combo(tr("Show state"), &m_previewState, states, 6);
    drawBindings(node);

    // Every line of the node: edit as text, remove with x. Comments are shown dimmed
    ImGui::SeparatorText(tr("Lines"));
    for (size_t i = 0; i < node.items.size(); ++i) {
        ui::SourceItem& item = node.items[i];
        if (item.child >= 0 || item.line.empty()) continue;
        ImGui::PushID(static_cast<int>(i));
        if (item.line[0] == '#') {
            ImGui::TextDisabled("%s", item.line.c_str());
        } else {
            ImGui::SetNextItemWidth(-28.0f);
            inputString("##line", item.line);
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
    if (ImGui::BeginCombo("##addKey", tr("Add..."), ImGuiComboFlags_HeightLarge)) {
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

void UiPanel::drawBindings(ui::SourceNode& node) {
    ImGui::SeparatorText(tr("Bindings"));
    const reflect::TypeInfo* scope = bindingScope(m_selection);
    if (!scope) {
        ImGui::TextDisabled("%s", tr("Pick a view model under Preview values to choose fields here."));
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
        const bool open = ImGui::BeginCombo(propertyRow(b.label()).c_str(), preview.c_str());
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
        if (ImGui::BeginCombo(propertyRow(tr("Insert field")).c_str(), tr("Choose..."))) {
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

} // namespace mealkit::editor
