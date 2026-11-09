// Editor [UI] workspace for people new to it: the widget library (ready-made nodes to click or drag in), the inspector rows that
// replace typing property lines (size modes, anchors, image file, font, behaviour), and the guide on an empty screen.
#include "mealkit/core/Log.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/editor/UiPanel.h"
#include "mealkit/platform/Paths.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <imgui.h>

namespace mealkit::editor {
namespace {

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> w;
    size_t a = 0;
    while ((a = s.find_first_not_of(' ', a)) != std::string::npos) {
        const size_t b = s.find(' ', a);
        w.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        a = b;
    }
    return w;
}

std::string num(f32 v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    return buf;
}

bool isNumber(const std::string& s, f32& out) {
    char* end = nullptr;
    out = std::strtof(s.c_str(), &end);
    return !s.empty() && end && *end == 0;
}

// A node from lines: "frame name" then property lines, children between "  frame ..." and "end" (two spaces per depth)
ui::SourceNode nodeFrom(const char* text) {
    ui::SourceNode n;
    if (!ui::parseNodeText(text, n)) MEALKIT_ERROR("UI library: bad template\n%s", text);
    return n;
}

// Marks English source text for the string table (tools\check_strings.ps1 reads trn("..."); tr() translates it where it is shown)
constexpr const char* trn(const char* text) { return text; }

struct Widget {
    const char* command; // editor command (palette): "Add UI Button"
    const char* glyph;
    const char* label;   // English source text (tr)
    const char* tooltip; // English source text (tr)
    const char* text;    // the node as .ui lines
};

// Plain colors (not $tokens) so every project's theme can take them; field and action names are placeholders the Behaviour
// section shows for editing (a missing field is listed under Problems)
const Widget kWidgets[] = {
    {trn("Add UI Panel"), "\xEE\x9C\xB9", trn("Panel"), trn("A box that lines its children up in a column (padding, gap, rounded corners)"),
     "frame panel\n  size 420 hug\n  layout column\n  gap 12\n  padding 24\n  fill #1B2130E6\n  radius 14\nend\n"},
    {trn("Add UI Text"), "\xEE\xA3\x92", trn("Text"), trn("A line of text. {field} in it shows a view model value"),
     "text label\n  content Text\n  fontSize 28\n  color #FFFFFF\nend\n"},
    {trn("Add UI Image"), "\xEE\xAE\x9F", trn("Image"), trn("A picture from assets (pick the file in the inspector)"), "image image\n  size 96 96\nend\n"},
    {trn("Add UI Button"), "\xEE\x9F\x89", trn("Button"), trn("A button that sends an action when pressed (change the action in Behaviour)"),
     "frame button\n  size 280 60\n  layout row\n  justify center\n  align center\n  fill #3B6FE0\n  radius 10\n  button confirm\n"
     "  on hover fill #5585F0\n  on pressed fill #2C58B8\n  on focus stroke 3 #FFFFFF\n  text buttonLabel\n    content Button\n    fontSize 26\n"
     "    color #FFFFFF\n  end\nend\n"},
    {trn("Add UI Toggle"), "\xEE\xA7\xB5", trn("Toggle"), trn("An on/off switch for a bool field"),
     "frame toggleRow\n  size hug hug\n  layout row\n  align center\n  gap 14\n  frame toggle\n    size 64 32\n    radius 16\n    fill #FFFFFF26\n"
     "    toggle enabled\n    on checked fill #3B6FE0\n    frame knobOff\n      pos 4 4\n      size 24 24\n      radius 12\n      fill #FFFFFF\n"
     "      visible !enabled\n    end\n    frame knobOn\n      pos 36 4\n      size 24 24\n      radius 12\n      fill #FFFFFF\n      visible enabled\n"
     "    end\n  end\n  text toggleLabel\n    content Option\n    fontSize 24\n    color #FFFFFF\n  end\nend\n"},
    {trn("Add UI Slider"), "\xEE\xB1\x8A", trn("Slider"), trn("Drag to set a float field between a minimum and a maximum"),
     "frame slider\n  size 320 28\n  radius 14\n  fill #FFFFFF26\n  clip\n  slider value01 0 1 0.01\n  on hover stroke 2 #FFFFFF40\n"
     "  frame sliderFill\n    size fill fill\n    widthRatio value01\n    radius 14\n    fill #3B6FE0\n  end\nend\n"},
    {trn("Add UI Progress bar"), "\xEE\xA7\x99", trn("Progress bar"), trn("A bar filled by a float field from 0 to 1 (health, loading)"),
     "frame progress\n  size 320 20\n  radius 10\n  fill #FFFFFF1A\n  clip\n  frame progressFill\n    size fill fill\n    widthRatio progress\n"
     "    radius 10\n    fill #4CC38A\n  end\nend\n"},
    {trn("Add UI Text input"), "\xEE\xA8\xB7", trn("Text input"), trn("A box the player types into (a string field)"),
     "frame inputBox\n  size 360 52\n  layout row\n  align center\n  padding 0 16\n  fill #00000055\n  stroke 1 #FFFFFF33\n  radius 10\n"
     "  text input\n    size fill hug\n    textInput text\n    maxLength 32\n    fontSize 24\n    color #FFFFFF\n    maxLines 1\n  end\nend\n"},
    {trn("Add UI List"), "\xEE\xA3\xBD", trn("List"), trn("Repeats its first child for every element of a list field"),
     "frame list\n  size 420 hug\n  layout column\n  gap 6\n  list items\n  frame row\n    size fill 48\n    layout row\n    align center\n"
     "    padding 0 14\n    radius 8\n    fill #FFFFFF0A\n    text rowName\n      content {name}\n      fontSize 22\n      color #FFFFFF\n    end\n  end\nend\n"},
    {trn("Add UI Scroll area"), "\xEE\x9C\x8D", trn("Scroll area"), trn("Clips its content and scrolls it with the wheel or the gamepad"),
     "frame scrollArea\n  size 420 300\n  scroll\n  frame content\n    size fill hug\n    layout column\n    gap 8\n  end\nend\n"},
};

// Images and fonts the project has (asset paths), for the pickers
void collectFiles(const std::string& dir, const std::string& prefix, std::vector<std::string>& out, int depth) {
    for (const std::string& f : listFiles(dir)) {
        const size_t dot = f.rfind('.');
        const std::string ext = dot == std::string::npos ? "" : f.substr(dot);
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") out.push_back(prefix + f);
    }
    if (depth < 4)
        for (const std::string& d : listDirectories(dir))
            if (d != "preview" && d != "boxes" && d != "captures") collectFiles(dir + "/" + d, prefix + d + "/", out, depth + 1);
}

// Where a library widget may go inside: the board, or a frame with a layout that is not itself a widget (a button, a toggle row,
// a text box). Anything else gets the widget beside it, so clicking Button twice gives two buttons, not a button in a button
bool isContainer(const ui::SourceNode& n) {
    if (n.kind == "board") return true;
    std::string v;
    if (n.kind != "frame" || !ui::getProperty(n, "layout", v)) return false;
    for (const char* w : {"button", "toggle", "slider", "select", "list"})
        if (ui::getProperty(n, w, v)) return false;
    for (const ui::SourceNode& c : n.children)
        for (const char* w : {"toggle", "textInput", "widthRatio", "select"})
            if (ui::getProperty(c, w, v)) return false;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------------------------------- library

void UiPanel::drawLibrary() {
    if (!editing()) return;
    if (!ImGui::CollapsingHeader(tr("Library"), ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::SetItemTooltip("%s", tr("Click to add inside the selected frame (or after the selected node), or drag onto the canvas or a layer"));
    // Buttons in rows that wrap with the column width
    const f32 width = ImGui::GetContentRegionAvail().x;
    const f32 cell = std::max(ImGui::GetFontSize() * 6.2f, (width - 4.0f) / std::max(1.0f, std::floor(width / (ImGui::GetFontSize() * 6.2f))) - 4.0f);
    f32 x = 0.0f;
    for (int i = 0; i < static_cast<int>(std::size(kWidgets)); ++i) {
        const Widget& w = kWidgets[i];
        if (x > 0.0f && x + cell <= width) ImGui::SameLine(0.0f, 4.0f);
        else x = 0.0f;
        ImGui::PushID(i);
        const std::string label = std::string(w.glyph) + "  " + tr(w.label);
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        if (ImGui::Button(label.c_str(), ImVec2(cell, 0.0f))) defer([this, i] { insertWidget(i, nullptr, nullptr); });
        ImGui::PopStyleVar();
        ImGui::SetItemTooltip("%s", tr(w.tooltip));
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("MK_UI_WIDGET", &i, sizeof(i));
            ImGui::TextUnformatted(label.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();
        x += cell + 4.0f;
    }
}

void UiPanel::drawLibraryMenu() {
    for (int i = 0; i < static_cast<int>(std::size(kWidgets)); ++i) {
        if (ImGui::MenuItem((std::string(kWidgets[i].glyph) + "  " + tr(kWidgets[i].label)).c_str())) defer([this, i] { insertWidget(i, nullptr, nullptr); });
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tr(kWidgets[i].tooltip));
    }
}

int UiPanel::widgetCount() { return static_cast<int>(std::size(kWidgets)); }
const char* UiPanel::widgetCommand(int index) { return index >= 0 && index < widgetCount() ? kWidgets[index].command : ""; }

// parent == nullptr: like Add (inside the selected frame, else after the selection). pos: logical point inside parent (free layouts)
bool UiPanel::insertWidget(int index, const ui::SourcePath* parent, const f32* pos) {
    if (!editing() || index < 0 || index >= widgetCount()) return false;
    ui::SourceNode n = nodeFrom(kWidgets[index].text);
    if (n.kind.empty()) return false;
    struct Rename {
        const ui::Source& source;
        std::vector<std::string> taken;
        void run(ui::SourceNode& node) {
            std::string name = ui::uniqueName(source, node.name);
            for (int k = 2; std::find(taken.begin(), taken.end(), name) != taken.end(); ++k) name = ui::uniqueName(source, node.name + std::to_string(k));
            taken.push_back(name);
            node.name = name;
            for (ui::SourceNode& c : node.children) run(c);
        }
    } rename{m_source, {}};
    rename.run(n);
    if (!parent) { // inside the selected container, else after the selected node, else at the end of the board
        ui::SourcePath into;
        u32 at = 0xFFFFFFFFu;
        if (m_hasSelection) {
            const ui::SourceNode* sel = ui::findNode(m_source, m_selection);
            if (sel && isContainer(*sel)) {
                into = m_selection;
            } else if (!m_selection.empty()) {
                into.assign(m_selection.begin(), m_selection.end() - 1);
                at = m_selection.back() + 1;
            }
        }
        m_selection = ui::addNode(m_source, into, at, std::move(n));
        m_hasSelection = true;
        syncSource();
        return true;
    }
    const ui::SourceNode* p = ui::findNode(m_source, *parent);
    if (!p) return false;
    std::string layout;
    if (pos && !ui::getProperty(*p, "layout", layout)) ui::setProperty(n, "pos", num(std::round(pos[0])) + " " + num(std::round(pos[1]))); // free: where it was dropped
    m_selection = ui::addNode(m_source, *parent, static_cast<u32>(p->children.size()), std::move(n));
    m_hasSelection = true;
    syncSource();
    return true;
}

// A library widget dropped on the canvas: into the deepest frame (or the board) under the mouse, at the mouse in free layouts
void UiPanel::dropWidgetOnCanvas(int index, f32 lx, f32 ly) {
    const ui::NodeBox* best = nullptr;
    for (const ui::NodeBox& b : m_boxes) {
        if (!b.visible || b.node >= m_map.size() || lx < b.x || ly < b.y || lx >= b.x + b.w || ly >= b.y + b.h) continue;
        const ui::SourceNode* n = ui::findNode(m_source, m_map[b.node]);
        if (!n || !isContainer(*n) || editorLocked(m_map[b.node])) continue;
        if (!best || b.depth >= best->depth) best = &b;
    }
    const ui::SourcePath parent = best ? m_map[best->node] : ui::SourcePath{};
    const f32 at[2] = {lx - (best ? best->x : 0.0f), ly - (best ? best->y : 0.0f)};
    defer([this, index, parent, at] {
        const f32 p[2] = {at[0], at[1]};
        insertWidget(index, &parent, p);
    });
}

// ---------------------------------------------------------------------------------------------------- inspector rows

// Width and height each as fixed / fit content (hug) / fill the parent's room, with the number for fixed
void UiPanel::drawSizeRows(ui::SourceNode& node) {
    if (node.kind == "board" || node.kind == "use") {
        std::string v;
        if (node.kind == "board" || !ui::getProperty(node, "size", v)) return;
    }
    std::string value;
    std::vector<std::string> parts = ui::getProperty(node, "size", value) ? words(value) : std::vector<std::string>{};
    ui::NodeBox drawn{};
    const bool hasBox = boxOf(m_selection, drawn);
    if (parts.size() != 2) parts = {node.kind == "text" ? "hug" : num(hasBox ? std::round(drawn.w) : 100.0f), node.kind == "text" ? "hug" : num(hasBox ? std::round(drawn.h) : 100.0f)};
    const char* modes[] = {tr("Fixed"), tr("Fit content"), tr("Fill")};
    const char* tips[] = {tr("A set number of units"), tr("As large as its children or text (hug)"), tr("Takes the room left in the parent's layout (fill)")};
    bool changed = false;
    for (int axis = 0; axis < 2; ++axis) {
        ImGui::PushID(axis);
        std::string& p = parts[static_cast<size_t>(axis)];
        f32 v = 0.0f;
        const int mode = p == "hug" ? 1 : p == "fill" ? 2 : 0;
        const std::string label = propertyRow(axis == 0 ? tr("Width") : tr("Height"));
        const f32 comboW = ImGui::GetFontSize() * 6.5f;
        ImGui::SetNextItemWidth(mode == 0 ? comboW : -1.0f);
        if (beginCombo(mode == 0 ? (label + "##mode").c_str() : label.c_str(), modes[mode])) {
            for (int m = 0; m < 3; ++m) {
                if (ImGui::Selectable(modes[m], m == mode) && m != mode) {
                    const f32 current = axis == 0 ? drawn.w : drawn.h;
                    p = m == 1 ? "hug" : m == 2 ? "fill" : num(std::round(hasBox ? current : 100.0f));
                    changed = true;
                }
                ImGui::SetItemTooltip("%s", tips[m]);
            }
            ImGui::EndCombo();
        }
        if (mode == 0) {
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::SetNextItemWidth(-1.0f);
            if (!isNumber(p, v)) {
                ImGui::TextDisabled("%s", p.c_str()); // a $token
            } else if (ImGui::DragFloat("##value", &v, 1.0f, 0.0f, 100000.0f, "%.0f")) {
                p = num(std::round(v));
                changed = true;
            }
        }
        ImGui::PopID();
    }
    if (changed) ui::setProperty(node, "size", parts[0] + " " + parts[1]);
}

// Anchors for nodes outside a layout: which screen edges it keeps its distance to when the resolution changes (constraints)
void UiPanel::drawAnchors(ui::SourceNode& node) {
    if (node.kind == "board") return;
    std::string value;
    std::vector<std::string> c = ui::getProperty(node, "constraints", value) ? words(value) : std::vector<std::string>{};
    if (c.size() != 2) c = {"left", "top"};
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Anchor"));
    ImGui::SetItemTooltip("%s", tr("The edge or center it stays attached to when the screen is wider, taller or scaled"));
    ImGui::SameLine(ImGui::GetContentRegionMax().x * 0.4f);
    // 3x3 grid: column = horizontal, row = vertical
    const char* hs[] = {"left", "center", "right"};
    const char* vs[] = {"top", "center", "bottom"};
    const f32 s = ImGui::GetFrameHeight() * 0.8f;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) {
            ImGui::SetCursorScreenPos(ImVec2(start.x + col * (s + 2.0f), start.y + row * (s + 2.0f)));
            ImGui::PushID(row * 3 + col);
            const bool on = c[0] == hs[col] && c[1] == vs[row];
            ImGui::PushStyleColor(ImGuiCol_Button, on ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
            if (ImGui::Button("##a", ImVec2(s, s))) {
                if (col == 0 && row == 0) ui::removeProperty(node, "constraints"); // the default
                else ui::setProperty(node, "constraints", std::string(hs[col]) + " " + vs[row]);
            }
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s %s", hs[col], vs[row]);
            ImGui::PopID();
        }
    // Stretching and scaling: two combos beside the grid
    const char* hAll[] = {"left", "center", "right", "leftright", "scale"};
    const char* vAll[] = {"top", "center", "bottom", "topbottom", "scale"};
    const f32 x = start.x + 3.0f * (s + 2.0f) + 8.0f;
    const f32 w = std::max(40.0f, ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x - x);
    for (int axis = 0; axis < 2; ++axis) {
        ImGui::SetCursorScreenPos(ImVec2(x, start.y + axis * (ImGui::GetFrameHeight() + 4.0f)));
        ImGui::SetNextItemWidth(w);
        ImGui::PushID(10 + axis);
        if (beginCombo("##stretch", c[static_cast<size_t>(axis)].c_str())) {
            for (const char* o : axis == 0 ? hAll : vAll)
                if (ImGui::Selectable(o, c[static_cast<size_t>(axis)] == o)) {
                    c[static_cast<size_t>(axis)] = o;
                    if (c[0] == "left" && c[1] == "top") ui::removeProperty(node, "constraints");
                    else ui::setProperty(node, "constraints", c[0] + " " + c[1]);
                }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("%s", axis == 0 ? tr("leftright: keeps both side margins (stretches), scale: grows with the screen")
                                              : tr("topbottom: keeps both margins (stretches), scale: grows with the screen"));
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + 3.0f * (s + 2.0f) + 4.0f));
    ImGui::Dummy(ImVec2(1.0f, 1.0f));
}

void UiPanel::drawImageRow(ui::SourceNode& node) {
    if (node.kind != "image") return;
    if (m_imageFilesTime <= 0.0 || ImGui::GetTime() - m_imageFilesTime > 5.0) { // the project's pictures, reread now and then
        m_imageFiles.clear();
        for (const char* folder : {"ui", "textures"})
            if (const std::string dir = findAsset(std::string("assets/") + folder); !dir.empty()) collectFiles(dir, std::string(folder) + "/", m_imageFiles, 0);
        m_imageFilesTime = ImGui::GetTime();
    }
    std::string src;
    const bool has = ui::getProperty(node, "src", src);
    if (beginCombo(propertyRow(tr("Image file"), tr("PNG or JPG in assets/ui or assets/textures")).c_str(), has ? src.c_str() : tr("(none)"), ImGuiComboFlags_HeightLarge)) {
        if (m_imageFiles.empty()) ImGui::TextDisabled("%s", tr("No pictures in assets/ui or assets/textures yet"));
        for (const std::string& f : m_imageFiles)
            if (ImGui::Selectable(f.c_str(), has && f == src)) ui::setProperty(node, "src", f);
        ImGui::EndCombo();
    }
    if (!has) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", tr("Copy a PNG into assets/ui (for example assets/ui/icons/) and pick it here."));
        ImGui::PopTextWrapPos();
    }
}

void UiPanel::drawFontRow(ui::SourceNode& node) {
    if (node.kind != "text") return;
    if (m_fontFiles.empty() || ImGui::GetTime() - m_fontFilesTime > 5.0) {
        m_fontFiles.clear();
        if (const std::string dir = findAsset("assets/fonts"); !dir.empty())
            for (const std::string& f : listFiles(dir)) {
                const size_t dot = f.rfind('.');
                const std::string ext = dot == std::string::npos ? "" : f.substr(dot);
                if (ext == ".ttf" || ext == ".otf") m_fontFiles.push_back(f.substr(0, dot));
            }
        m_fontFilesTime = ImGui::GetTime();
    }
    std::string font;
    const bool has = ui::getProperty(node, "font", font);
    if (beginCombo(propertyRow(tr("Font"), tr("A font file in assets/fonts")).c_str(), has ? font.c_str() : tr("(default)"))) {
        if (ImGui::Selectable(tr("(default)"), !has)) ui::removeProperty(node, "font");
        for (const std::string& f : m_fontFiles)
            if (ImGui::Selectable(f.c_str(), has && f == font)) ui::setProperty(node, "font", f);
        ImGui::EndCombo();
    }
    std::string color;
    if (!ui::getProperty(node, "color", color) && ImGui::SmallButton(tr("Set text color"))) ui::setProperty(node, "color", "#FFFFFF");
}

// What the node does: none, button (sends an action), toggle (bool field), slider (float field), list (repeats a child), scroll, text input
void UiPanel::drawBehaviour(ui::SourceNode& node) {
    if (node.kind == "board" || node.kind == "use") return;
    struct Kind {
        const char* key;
        const char* label;
        const char* fresh; // the line a new choice writes
        bool text;         // text nodes only
    };
    const Kind kinds[] = {{"button", tr("Button (sends an action)"), "button confirm", false},
                          {"toggle", tr("Toggle (bool field)"), "toggle enabled", false},
                          {"slider", tr("Slider (float field)"), "slider value01 0 1 0.01", false},
                          {"list", tr("List (repeats the first child)"), "list items", false},
                          {"scroll", tr("Scroll area"), "scroll", false},
                          {"textInput", tr("Text input (string field)"), "textInput text", true}};
    int current = -1;
    std::string line;
    for (int i = 0; i < static_cast<int>(std::size(kinds)); ++i)
        if (ui::getProperty(node, kinds[i].key, line)) {
            current = i;
            break;
        }
    ImGui::SeparatorText(tr("Behaviour"));
    ImGui::SetItemTooltip("%s", tr("What the node does when the player uses it. Plain boxes and texts do nothing"));
    if (beginCombo(propertyRow(tr("Kind")).c_str(), current < 0 ? tr("None (just drawn)") : kinds[current].label)) {
        if (ImGui::Selectable(tr("None (just drawn)"), current < 0) && current >= 0) ui::removeProperty(node, kinds[current].key);
        for (int i = 0; i < static_cast<int>(std::size(kinds)); ++i) {
            if (kinds[i].text != (node.kind == "text")) continue;
            if (ImGui::Selectable(kinds[i].label, i == current) && i != current) {
                if (current >= 0) ui::removeProperty(node, kinds[current].key);
                const std::string fresh = kinds[i].fresh;
                const size_t sp = fresh.find(' ');
                ui::setProperty(node, kinds[i].key, sp == std::string::npos ? std::string() : fresh.substr(sp + 1));
            }
        }
        ImGui::EndCombo();
    }
    if (current < 0) return;
    const std::string key = kinds[current].key;
    std::vector<std::string> w = words(line);
    // Labels for the words after the key; edit each in its own box, written back as one line
    std::vector<const char*> names;
    if (key == "button") names = {tr("Action")};
    else if (key == "toggle" || key == "textInput") names = {tr("Field"), tr("Action (optional)")};
    else if (key == "slider") names = {tr("Field"), tr("Minimum"), tr("Maximum"), tr("Step"), tr("Action (optional)")};
    else if (key == "list") names = {tr("Field")};
    else if (key == "scroll") names = {tr("Direction (horizontal or empty)")};
    if (w.size() < names.size()) w.resize(names.size());
    bool changed = false;
    for (size_t i = 0; i < names.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (inputString(propertyRow(names[i]).c_str(), w[i])) changed = true;
        ImGui::PopID();
    }
    if (key == "button" || ((key == "toggle" || key == "slider" || key == "textInput") && names.size() > 1)) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", key == "button" ? tr("Game code reads the action from ctx.ui.actions() (for example \"play\" or \"settings.open\").")
                                                  : tr("The field is a member of the screen's view model (MEALKIT_VIEWMODEL)."));
        ImGui::PopTextWrapPos();
    }
    if (changed) {
        while (!w.empty() && w.back().empty()) w.pop_back(); // optional words left empty
        std::string out;
        for (const std::string& s : w) out += (out.empty() ? "" : " ") + (s.empty() ? std::string("-") : s);
        ui::setProperty(node, key, out);
    }
}

// ---------------------------------------------------------------------------------------------------- guide

// An empty screen (only the board) tells where to start, in the middle of the canvas
void UiPanel::drawEmptyGuide() {
    // An image without a file draws nothing in the game: the editor outlines it and says so, or it would look missing
    if (editing() && m_scaleK > 0.0f) {
        ImDrawList* list = ImGui::GetWindowDrawList();
        for (const ui::NodeBox& b : m_boxes) {
            if (!b.visible || b.node >= m_map.size()) continue;
            const ui::SourceNode* n = ui::findNode(m_source, m_map[b.node]);
            std::string src;
            if (!n || n->kind != "image" || ui::getProperty(*n, "src", src)) continue;
            const ImVec2 a(m_originX + b.x * m_scaleK, m_originY + b.y * m_scaleK), z(m_originX + (b.x + b.w) * m_scaleK, m_originY + (b.y + b.h) * m_scaleK);
            list->AddRectFilled(a, z, IM_COL32(255, 255, 255, 18));
            list->AddRect(a, z, IM_COL32(255, 255, 255, 90), 0.0f, 0, 1.0f);
            list->AddLine(a, z, IM_COL32(255, 255, 255, 50));
            list->AddLine(ImVec2(a.x, z.y), ImVec2(z.x, a.y), IM_COL32(255, 255, 255, 50));
            const char* label = tr("no image file");
            const ImVec2 size = ImGui::CalcTextSize(label);
            if (size.x < z.x - a.x) list->AddText(ImVec2((a.x + z.x - size.x) * 0.5f, (a.y + z.y - size.y) * 0.5f), IM_COL32(255, 255, 255, 170), label);
        }
    }
    const ui::SourceNode* board = ui::findNode(m_source, {});
    if (!editing() || !board || !board->children.empty() || m_canvas.width < 200.0f) return;
    const char* lines[] = {tr("This screen is empty. To start:"), tr("1. Click Panel, Button or Text in the Library on the left (or drag it here)."),
                           tr("2. Click a box to select it, drag it to move it, pull the handles to resize it."),
                           tr("3. Change it on the right: size, anchor, colors, and what it does (Behaviour)."), tr("4. Ctrl+S saves. The game shows the screen with ctx.ui.open(\"ui/<screen>.ui\").")};
    ImDrawList* draw = ImGui::GetWindowDrawList(); // over the board drawn on the back buffer, under popups
    f32 w = 0.0f, h = 0.0f;
    const f32 line = ImGui::GetTextLineHeightWithSpacing();
    for (const char* l : lines) w = std::max(w, ImGui::CalcTextSize(l).x);
    h = line * static_cast<f32>(std::size(lines));
    const ImVec2 at(m_canvas.x + (m_canvas.width - w) * 0.5f, m_canvas.y + (m_canvas.height - h) * 0.5f);
    draw->AddRectFilled(ImVec2(at.x - 16.0f, at.y - 12.0f), ImVec2(at.x + w + 16.0f, at.y + h + 12.0f), IM_COL32(20, 24, 32, 230), 10.0f);
    for (size_t i = 0; i < std::size(lines); ++i)
        draw->AddText(ImVec2(at.x, at.y + line * static_cast<f32>(i)), i == 0 ? IM_COL32(255, 255, 255, 255) : IM_COL32(190, 200, 215, 255), lines[i]);
}

} // namespace mealkit::editor
