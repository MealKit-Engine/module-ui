// [UI] workspace tools (UiPanel.h): view guides (outlines, safe area, rulers, grid, distances, overflow), editor-only hide and lock,
// binding by drag and drop, theme tokens, style copy, preview value sets, history and changes, screen duplicate and rename, canvas
// capture and the game's live values while playing
#include "mealkit/core/Log.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/editor/UiPanel.h"
#include "mealkit/platform/Image.h"
#include "mealkit/platform/Paths.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <imgui.h>

namespace mealkit::editor {
namespace {

constexpr text::Format kValuesFormat{"uivalues", 1};

std::vector<std::string> splitWords(const std::string& s) {
    std::vector<std::string> out;
    size_t a = 0;
    while ((a = s.find_first_not_of(' ', a)) != std::string::npos) {
        const size_t b = s.find(' ', a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        a = b;
    }
    return out;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
        if (end == text.size()) break;
        start = end + 1;
    }
    if (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

const reflect::FieldInfo* fieldByPath(const reflect::TypeInfo* type, const std::string& path) {
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

bool listField(const reflect::FieldInfo& f) {
    return f.value->kind == reflect::Kind::Array && f.value->element && f.value->element->kind == reflect::Kind::Struct && f.value->element->type;
}

// Style keys: what Copy style takes and Paste style replaces
bool styleKey(const std::string& key) {
    for (const char* k : {"fill", "stroke", "radius", "shadow", "opacity", "tint", "font", "fontSize", "color", "letterSpacing", "lineHeight",
                          "textAlign", "verticalAlign", "transform", "underline", "strike"})
        if (key == k) return true;
    return false;
}

std::string firstWord(const std::string& line) { return line.substr(0, line.find(' ')); }

// Line diff (longest common subsequence): ' ' same, '-' only in a, '+' only in b
std::vector<std::pair<char, std::string>> diffLines(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    const size_t n = a.size(), m = b.size();
    std::vector<std::vector<u32>> lcs(n + 1, std::vector<u32>(m + 1, 0));
    for (size_t i = n; i-- > 0;)
        for (size_t j = m; j-- > 0;) lcs[i][j] = a[i] == b[j] ? lcs[i + 1][j + 1] + 1 : std::max(lcs[i + 1][j], lcs[i][j + 1]);
    std::vector<std::pair<char, std::string>> out;
    size_t i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && a[i] == b[j]) out.push_back({' ', a[i++]}), ++j;
        else if (j < m && (i == n || lcs[i][j + 1] >= lcs[i + 1][j])) out.push_back({'+', b[j++]});
        else out.push_back({'-', a[i++]});
    }
    return out;
}

// One history step named after its first changed line ("pos 40 32")
std::string stepName(const std::string& before, const std::string& after) {
    const std::vector<std::string> a = splitLines(before), b = splitLines(after);
    for (const auto& [kind, line] : diffLines(a, b))
        if (kind != ' ') {
            const size_t first = line.find_first_not_of(' ');
            return (kind == '-' ? "- " : "") + (first == std::string::npos ? std::string() : line.substr(first));
        }
    return "-";
}

void listProjectFiles(const std::string& dir, std::vector<std::string>& out, u32 depth = 0) {
    if (depth > 8) return;
    for (const char* ext : {".cpp", ".h", ".ui"})
        for (const std::string& f : listFiles(dir, ext)) out.push_back(dir + "/" + f);
    for (const std::string& d : listDirectories(dir))
        if (d != "build" && d != "export" && d[0] != '.' && d != "third_party") listProjectFiles(dir + "/" + d, out, depth + 1);
}


} // namespace

// ---------------------------------------------------------------- Editor-only hide and lock

void UiPanel::toggleName(std::vector<std::string>& names, const std::string& name) {
    const auto it = std::find(names.begin(), names.end(), name);
    if (it == names.end()) names.push_back(name);
    else names.erase(it);
}

bool UiPanel::editorHidden(const ui::SourcePath& path) const {
    for (size_t depth = 0; depth <= path.size(); ++depth)
        if (const ui::SourceNode* n = ui::findNode(m_source, ui::SourcePath(path.begin(), path.begin() + static_cast<std::ptrdiff_t>(depth))))
            if (!n->name.empty() && std::find(m_hiddenNodes.begin(), m_hiddenNodes.end(), n->name) != m_hiddenNodes.end()) return true;
    return false;
}

bool UiPanel::editorLocked(const ui::SourcePath& path) const {
    for (size_t depth = 0; depth <= path.size(); ++depth)
        if (const ui::SourceNode* n = ui::findNode(m_source, ui::SourcePath(path.begin(), path.begin() + static_cast<std::ptrdiff_t>(depth))))
            if (!n->name.empty() && std::find(m_lockedNodes.begin(), m_lockedNodes.end(), n->name) != m_lockedNodes.end()) return true;
    return false;
}

// The preview draws m_draft: the unsaved text, with a "hidden" line added to every node hidden in the editor (never saved)
void UiPanel::refreshDraft() {
    if (!m_screen) return;
    if (m_hiddenNodes.empty()) {
        m_ui.setDraft(m_screen, m_live == m_saved ? nullptr : &m_live);
        return;
    }
    ui::Source copy = m_source;
    std::vector<ui::SourceNode*> stack{&copy.root};
    while (!stack.empty()) {
        ui::SourceNode* n = stack.back();
        stack.pop_back();
        if (!n->name.empty() && std::find(m_hiddenNodes.begin(), m_hiddenNodes.end(), n->name) != m_hiddenNodes.end()) ui::setProperty(*n, "hidden", "");
        for (ui::SourceNode& c : n->children) stack.push_back(&c);
    }
    m_draft = ui::writeSource(copy);
    m_ui.setDraft(m_screen, &m_draft);
}

// ---------------------------------------------------------------- View menu and guides

void UiPanel::toggleView(const std::string& which) {
    if (which == "layout") m_showLayout = !m_showLayout;
    else if (which == "safe") m_showSafeArea = !m_showSafeArea;
    else if (which == "rulers") m_showRulers = !m_showRulers;
    else if (which == "grid") m_showGrid = !m_showGrid;
    else if (which == "snap") m_snapGrid = !m_snapGrid;
    else if (which == "pseudo") m_ui.setPseudoText(!m_ui.pseudoText());
    else if (which == "interact") {
        m_interact = !m_interact;
        m_actionLog.clear();
        m_drag = Drag::None;
    }
}

void UiPanel::drawViewMenu() {
    ImGui::Checkbox(tr("Layout outlines"), &m_showLayout);
    ImGui::SetItemTooltip("%s", tr("Every box's outline, the direction of flex layouts and their gaps"));
    ImGui::Checkbox(tr("Safe area"), &m_showSafeArea);
    ImGui::SetItemTooltip("%s", tr("Action safe (90%) and title safe (80%) frames for TVs"));
    ImGui::Checkbox(tr("Rulers"), &m_showRulers);
    ImGui::Checkbox(tr("Grid"), &m_showGrid);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
    ImGui::DragFloat("##gridSize", &m_gridSize, 0.25f, 2.0f, 256.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Checkbox(tr("Snap to grid"), &m_snapGrid);
    ImGui::SetItemTooltip("%s", tr("Moving and resizing round to the grid (Ctrl: free)"));
    ImGui::Checkbox(tr("Mark text that doesn't fit"), &m_showOverflow);
    bool pseudo = m_ui.pseudoText();
    if (ImGui::Checkbox(tr("Pseudo-localize"), &pseudo)) m_ui.setPseudoText(pseudo);
    ImGui::SetItemTooltip("%s", tr("String table text 40% longer and accented, to find text that won't fit translations"));
}

void UiPanel::drawCanvasGuides() {
    if (!m_screen || m_view.width <= 0.0f) return;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const f32 k = m_scaleK > 0.0f ? m_scaleK : 1.0f;
    auto px = [&](f32 lx, f32 ly) { return ImVec2(m_originX + lx * k, m_originY + ly * k); };
    const Size r = resolution();
    draw->PushClipRect(ImVec2(m_canvas.x, m_canvas.y), ImVec2(m_canvas.x + m_canvas.width, m_canvas.y + m_canvas.height), true);

    if (m_showGrid) { // lines at least 6 px apart (multiples of the grid size when zoomed out)
        f32 step = std::max(m_gridSize, 1.0f);
        while (step * k < 6.0f) step *= 2.0f;
        for (f32 x = step; x < r.w; x += step) draw->AddLine(px(x, 0), px(x, r.h), IM_COL32(255, 255, 255, 18));
        for (f32 y = step; y < r.h; y += step) draw->AddLine(px(0, y), px(r.w, y), IM_COL32(255, 255, 255, 18));
    }
    if (m_showSafeArea)
        for (const f32 inset : {0.05f, 0.10f}) {
            const ImVec2 a = px(r.w * inset, r.h * inset), c = px(r.w * (1.0f - inset), r.h * (1.0f - inset));
            draw->AddRect(a, c, inset < 0.07f ? IM_COL32(255, 200, 60, 150) : IM_COL32(255, 120, 60, 150), 0.0f, 0, 1.0f);
            draw->AddText(ImVec2(a.x + 3.0f, a.y + 1.0f), IM_COL32(255, 200, 60, 170), inset < 0.07f ? tr("action safe") : tr("title safe"));
        }
    if (m_showLayout) {
        for (const ui::NodeBox& b : m_boxes)
            if (b.visible && b.node < m_map.size() && !m_map[b.node].empty()) draw->AddRect(px(b.x, b.y), px(b.x + b.w, b.y + b.h), IM_COL32(120, 200, 255, 70));
        // Flex frames: an arrow along the main axis and the gaps between their children tinted
        for (const ui::NodeBox& b : m_boxes) {
            if (!b.visible || b.node >= m_map.size()) continue;
            const ui::SourceNode* n = ui::findNode(m_source, m_map[b.node]);
            std::string layout;
            if (!n || !ui::getProperty(*n, "layout", layout) || layout.rfind("grid", 0) == 0) continue;
            const bool column = layout.rfind("column", 0) == 0;
            const ImVec2 c = px(b.x + b.w * 0.5f, b.y + b.h * 0.5f);
            const f32 len = std::min(column ? b.h * k : b.w * k, 40.0f) * 0.5f;
            const ImVec2 from = column ? ImVec2(c.x, c.y - len) : ImVec2(c.x - len, c.y), to = column ? ImVec2(c.x, c.y + len) : ImVec2(c.x + len, c.y);
            const ImU32 arrow = IM_COL32(120, 200, 255, 140);
            draw->AddLine(from, to, arrow, 1.5f);
            draw->AddTriangleFilled(to, column ? ImVec2(to.x - 4, to.y - 6) : ImVec2(to.x - 6, to.y - 4), column ? ImVec2(to.x + 4, to.y - 6) : ImVec2(to.x - 6, to.y + 4), arrow);
            // Children in order: the space between one's end and the next one's start
            std::vector<ui::NodeBox> kids;
            for (const ui::NodeBox& cb : m_boxes)
                if (cb.visible && cb.node < m_map.size() && m_map[cb.node].size() == m_map[b.node].size() + 1 &&
                    std::equal(m_map[b.node].begin(), m_map[b.node].end(), m_map[cb.node].begin()))
                    kids.push_back(cb);
            std::sort(kids.begin(), kids.end(), [column](const ui::NodeBox& x, const ui::NodeBox& y) { return column ? x.y < y.y : x.x < y.x; });
            for (size_t i = 1; i < kids.size(); ++i) {
                const ui::NodeBox& p = kids[i - 1];
                const ui::NodeBox& q = kids[i];
                if (column && q.y > p.y + p.h + 0.5f) draw->AddRectFilled(px(b.x, p.y + p.h), px(b.x + b.w, q.y), IM_COL32(255, 120, 200, 45));
                if (!column && q.x > p.x + p.w + 0.5f) draw->AddRectFilled(px(p.x + p.w, b.y), px(q.x, b.y + b.h), IM_COL32(255, 120, 200, 45));
            }
        }
    }
    if (m_showOverflow)
        for (const ui::NodeBox& b : m_boxes)
            if (b.overflow) {
                draw->AddRect(px(b.x, b.y), px(b.x + b.w, b.y + b.h), IM_COL32(255, 80, 80, 220), 0.0f, 0, 1.5f);
                draw->AddText(ImVec2(px(b.x + b.w, b.y).x - 8.0f, px(b.x, b.y).y - 2.0f), IM_COL32(255, 80, 80, 255), "!");
            }
    // Alt: distances from the selection to the hovered box (inside it: to its edges; beside it: the gaps)
    ui::NodeBox s, h;
    if (ImGui::GetIO().KeyAlt && m_hasSelection && m_hasHover && m_hover != m_selection && boxOf(m_selection, s) && boxOf(m_hover, h)) {
        const ImU32 red = IM_COL32(255, 70, 90, 255);
        auto measure = [&](f32 x0, f32 y0, f32 x1, f32 y1) {
            const f32 d = std::fabs(x1 - x0) + std::fabs(y1 - y0);
            if (d < 0.5f) return;
            draw->AddLine(px(x0, y0), px(x1, y1), red, 1.0f);
            char label[16];
            std::snprintf(label, sizeof label, "%.0f", static_cast<double>(d));
            const ImVec2 m = px((x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
            const ImVec2 size = ImGui::CalcTextSize(label);
            draw->AddRectFilled(ImVec2(m.x - size.x * 0.5f - 3, m.y - size.y * 0.5f - 1), ImVec2(m.x + size.x * 0.5f + 3, m.y + size.y * 0.5f + 1), red, 3.0f);
            draw->AddText(ImVec2(m.x - size.x * 0.5f, m.y - size.y * 0.5f), IM_COL32(255, 255, 255, 255), label);
        };
        const f32 scx = s.x + s.w * 0.5f, scy = s.y + s.h * 0.5f;
        const bool inside = s.x >= h.x && s.y >= h.y && s.x + s.w <= h.x + h.w && s.y + s.h <= h.y + h.h;
        if (inside) {
            measure(h.x, scy, s.x, scy);
            measure(s.x + s.w, scy, h.x + h.w, scy);
            measure(scx, h.y, scx, s.y);
            measure(scx, s.y + s.h, scx, h.y + h.h);
        } else {
            draw->AddRect(px(h.x, h.y), px(h.x + h.w, h.y + h.h), red, 0.0f, 0, 1.0f);
            if (h.x >= s.x + s.w) measure(s.x + s.w, scy, h.x, scy);
            else if (h.x + h.w <= s.x) measure(h.x + h.w, scy, s.x, scy);
            if (h.y >= s.y + s.h) measure(scx, s.y + s.h, scx, h.y);
            else if (h.y + h.h <= s.y) measure(scx, h.y + h.h, scx, s.y);
        }
    }
    draw->PopClipRect();

    if (m_showRulers) { // along the canvas's top and left edges: a tick every 10 units (labels every 100, fewer when zoomed out)
        const f32 t = 14.0f;
        const ImU32 bg = IM_COL32(30, 31, 35, 235), tick = IM_COL32(160, 165, 175, 200);
        draw->AddRectFilled(ImVec2(m_canvas.x, m_canvas.y), ImVec2(m_canvas.x + m_canvas.width, m_canvas.y + t), bg);
        draw->AddRectFilled(ImVec2(m_canvas.x, m_canvas.y), ImVec2(m_canvas.x + t, m_canvas.y + m_canvas.height), bg);
        f32 label = 100.0f;
        while (label * k < 50.0f) label *= 2.0f;
        const f32 minor = label / 10.0f;
        for (f32 v = 0.0f; v <= std::max(r.w, r.h); v += minor) {
            const bool major = std::fmod(v, label) < 0.01f;
            const f32 x = m_originX + v * k, y = m_originY + v * k;
            if (v <= r.w && x >= m_canvas.x + t && x <= m_canvas.x + m_canvas.width) {
                draw->AddLine(ImVec2(x, m_canvas.y + t - (major ? t : 4.0f)), ImVec2(x, m_canvas.y + t), tick);
                if (major) draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.7f, ImVec2(x + 2.0f, m_canvas.y), tick, std::to_string(static_cast<int>(v)).c_str());
            }
            if (v <= r.h && y >= m_canvas.y + t && y <= m_canvas.y + m_canvas.height) {
                draw->AddLine(ImVec2(m_canvas.x + t - (major ? t : 4.0f), y), ImVec2(m_canvas.x + t, y), tick);
                if (major) draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.7f, ImVec2(m_canvas.x + 1.0f, y + 1.0f), tick, std::to_string(static_cast<int>(v)).c_str());
            }
        }
        // The selection's extent on both rulers
        ui::NodeBox b;
        if (m_hasSelection && boxOf(m_selection, b)) {
            draw->AddRectFilled(ImVec2(m_originX + b.x * k, m_canvas.y + t - 3.0f), ImVec2(m_originX + (b.x + b.w) * k, m_canvas.y + t), IM_COL32(80, 170, 255, 220));
            draw->AddRectFilled(ImVec2(m_canvas.x + t - 3.0f, m_originY + b.y * k), ImVec2(m_canvas.x + t, m_originY + (b.y + b.h) * k), IM_COL32(80, 170, 255, 220));
        }
    }
}

// ---------------------------------------------------------------- Binding by drag and drop

void UiPanel::drawFieldList() {
    if (!m_vmType || !m_editable) return;
    ImGui::SeparatorText(tr("Fields"));
    ImGui::SetItemTooltip("%s", tr("Drag a field onto a layer or the canvas to bind it"));
    // name, kind; struct fields open their members
    struct Walk {
        UiPanel* self;
        void fields(const reflect::TypeInfo& type, const std::string& prefix, u32 depth) {
            for (const reflect::FieldInfo& f : type.fields) {
                const std::string name = prefix + f.name;
                const bool sub = depth < 3 && f.value->kind == reflect::Kind::Struct && f.value->type;
                ImGui::PushID(name.c_str());
                if (sub) {
                    if (ImGui::TreeNodeEx("##s", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", f.name)) {
                        fields(*f.value->type, name + ".", depth + 1);
                        ImGui::TreePop();
                    }
                } else {
                    const char* kind = listField(f) ? "list" : f.value->kind == reflect::Kind::Bool ? "bool" : f.value->kind == reflect::Kind::Float ? "float"
                                     : f.value->kind == reflect::Kind::Color ? "color" : f.value->kind == reflect::Kind::String ? "text" : "value";
                    ImGui::Selectable((std::string(f.name) + "##f").c_str(), false);
                    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(kind).x));
                    ImGui::TextDisabled("%s", kind);
                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                        self->m_dragField = name;
                        ImGui::SetDragDropPayload("MK_UI_FIELD", name.c_str(), name.size() + 1);
                        ImGui::TextUnformatted(name.c_str());
                        ImGui::EndDragDropSource();
                    }
                }
                ImGui::PopID();
            }
        }
    };
    if (ImGui::BeginChild("##fields", ImVec2(-1.0f, ImGui::GetTextLineHeightWithSpacing() * 4.0f), ImGuiChildFlags_ResizeY | ImGuiChildFlags_FrameStyle))
        Walk{this}.fields(*m_vmType, "", 0);
    ImGui::EndChild();
}

// Text: {field} appended to its content. Frame/image: a list field repeats it, a bool shows it, a float sets its width ratio, a color its color
bool UiPanel::bindField(const ui::SourcePath& path, const std::string& field) {
    ui::SourceNode* n = ui::findNode(m_source, path);
    const reflect::TypeInfo* scope = bindingScope(path);
    const reflect::FieldInfo* f = fieldByPath(scope ? scope : m_vmType, field);
    if (!n || !f || !m_editable) return false;
    std::string value;
    if (n->kind == "text" && !listField(*f) && f->value->kind != reflect::Kind::Color) {
        if (ui::getProperty(*n, "content", value)) ui::setProperty(*n, "content", value + "{" + field + "}");
        else {
            ui::removeProperty(*n, "key");
            ui::setProperty(*n, "content", "{" + field + "}");
        }
    } else if (listField(*f)) {
        ui::setProperty(*n, "list", field);
    } else if (f->value->kind == reflect::Kind::Color) {
        ui::setProperty(*n, n->kind == "text" ? "colorField" : "colorField", field);
    } else if (f->value->kind == reflect::Kind::Float) {
        ui::setProperty(*n, "widthRatio", field);
    } else {
        ui::setProperty(*n, "visible", field);
    }
    m_selection = path;
    m_hasSelection = true;
    syncSource();
    notice(trf("Bound %s to %s", field.c_str(), n->name.empty() ? n->kind.c_str() : n->name.c_str()));
    return true;
}

// ---------------------------------------------------------------- Theme tokens

void UiPanel::loadTheme() {
    m_tokens.clear();
    m_themeText.clear();
    m_themeDirty = false;
    const std::string file = findAssetFile(ui::kThemeFile);
    if (file.empty() || !readTextFile(file, m_themeText)) return;
    for (const std::string& line : splitLines(m_themeText)) {
        const std::vector<std::string> w = splitWords(line);
        if (w.size() >= 3 && (w[0] == "color" || w[0] == "number")) m_tokens.push_back({w[0] == "color", w[1], w[2], 0});
    }
    // Uses: $name in every screen (and component) file
    const std::string dir = findAsset("assets/ui");
    std::vector<std::string> files;
    if (!dir.empty()) listProjectFiles(dir, files);
    for (const std::string& path : files) {
        std::string text;
        if (!readTextFile(path, text)) continue;
        for (Token& t : m_tokens) {
            const std::string needle = "$" + t.name;
            for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
                const size_t end = at + needle.size();
                if (end >= text.size() || !(std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) ++t.uses;
            }
        }
    }
}

void UiPanel::drawThemeSection() {
    if (!sectionHeader((std::string(tr("Theme")) + (m_themeDirty ? "  *" : "") + "###theme").c_str())) return;
    if (m_tokens.empty() && m_themeText.empty()) loadTheme();
    if (m_tokens.empty()) {
        ImGui::TextDisabled("%s", tr("No theme tokens (assets/ui/theme.ui)."));
        return;
    }
    for (Token& t : m_tokens) {
        ImGui::PushID(t.name.c_str());
        const std::string label = "$" + t.name;
        if (t.color) {
            f32 rgba[4] = {1, 1, 1, 1};
            const std::string& v = t.value;
            if (v.size() >= 7 && v[0] == '#')
                for (int i = 0; i < 4 && 1 + i * 2 + 1 < static_cast<int>(v.size()) + 1; ++i)
                    if (1 + i * 2 + 2 <= static_cast<int>(v.size())) rgba[i] = static_cast<f32>(std::strtol(v.substr(1 + i * 2, 2).c_str(), nullptr, 16)) / 255.0f;
            if (colorEdit(propertyRow(label.c_str()).c_str(), rgba, true)) {
                char buf[16];
                auto b = [](f32 x) { return static_cast<int>(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
                if (v.size() == 9 || rgba[3] < 1.0f) std::snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", b(rgba[0]), b(rgba[1]), b(rgba[2]), b(rgba[3]));
                else std::snprintf(buf, sizeof buf, "#%02X%02X%02X", b(rgba[0]), b(rgba[1]), b(rgba[2]));
                t.value = buf;
                m_themeDirty = true;
            }
        } else {
            f32 v = static_cast<f32>(std::atof(t.value.c_str()));
            if (ImGui::DragFloat(propertyRow(label.c_str()).c_str(), &v, 0.25f, -10000.0f, 10000.0f, "%g")) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
                t.value = buf;
                m_themeDirty = true;
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", trf("Used %u times in assets/ui", t.uses).c_str());
        ImGui::PopID();
    }
    ImGui::BeginDisabled(!m_themeDirty);
    if (ImGui::SmallButton(tr("Save theme"))) {
        // Same lines, values replaced: comments and order stay
        std::string out;
        for (const std::string& line : splitLines(m_themeText)) {
            const std::vector<std::string> w = splitWords(line);
            std::string written = line;
            if (w.size() >= 3 && (w[0] == "color" || w[0] == "number"))
                for (const Token& t : m_tokens)
                    if (t.name == w[1]) written = w[0] + " " + w[1] + " " + t.value;
            out += written + (m_themeText.find("\r\n") != std::string::npos ? "\r\n" : "\n");
        }
        const std::string file = findAssetFile(ui::kThemeFile);
        if (!file.empty() && writeTextFile(file, out)) {
            notice(tr("Theme saved: every screen uses the new values"));
            loadTheme();
        } else {
            MEALKIT_ERROR("UI: cannot write %s", ui::kThemeFile);
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Revert"))) loadTheme();
    ImGui::EndDisabled();
}

// A small $ combo after a color or number property: picks a token for it, or back to a plain value
bool UiPanel::tokenCombo(ui::SourceNode& node, const char* key, bool color) {
    if (m_tokens.empty() && m_themeText.empty()) loadTheme();
    std::string value;
    const bool has = ui::getProperty(node, key, value);
    bool changed = false;
    ImGui::PushID(key);
    ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 1.6f);
    if (ImGui::BeginCombo("##token", "$", ImGuiComboFlags_NoArrowButton | ImGuiComboFlags_HeightLarge)) {
        for (const Token& t : m_tokens) {
            if (t.color != color) continue;
            const std::string name = "$" + t.name;
            if (ImGui::Selectable((name + "  " + t.value).c_str(), has && value == name)) {
                ui::setProperty(node, key, name);
                changed = true;
            }
        }
        if (has && !value.empty() && value[0] == '$') {
            ImGui::Separator();
            if (ImGui::Selectable(tr("Plain value (the token's)"))) {
                for (const Token& t : m_tokens)
                    if ("$" + t.name == value) ui::setProperty(node, key, t.value);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", tr("Theme token"));
    ImGui::PopID();
    return changed;
}

// ---------------------------------------------------------------- Style copy

void UiPanel::copyStyle() {
    const ui::SourceNode* n = m_hasSelection ? ui::findNode(m_source, m_selection) : nullptr;
    if (!n) return;
    m_styleClipboard.clear();
    for (const ui::SourceItem& item : n->items)
        if (item.child < 0 && !item.line.empty() && styleKey(firstWord(item.line))) m_styleClipboard.push_back(item.line);
    notice(trf("Style copied (%zu lines)", m_styleClipboard.size()));
}

void UiPanel::pasteStyle() {
    ui::SourceNode* n = m_hasSelection && m_editable ? ui::findNode(m_source, m_selection) : nullptr;
    if (!n || m_styleClipboard.empty()) return;
    // The node's own style lines go, the copied ones come in where its properties end (before its children)
    std::vector<ui::SourceItem> kept;
    for (const ui::SourceItem& item : n->items)
        if (item.child >= 0 || item.line.empty() || !styleKey(firstWord(item.line))) kept.push_back(item);
    size_t at = kept.size();
    for (size_t i = 0; i < kept.size(); ++i)
        if (kept[i].child >= 0) {
            at = i;
            break;
        }
    for (const std::string& line : m_styleClipboard) kept.insert(kept.begin() + static_cast<std::ptrdiff_t>(at++), ui::SourceItem{line, -1});
    n->items = std::move(kept);
    syncSource();
}

// ---------------------------------------------------------------- History and changes

void UiPanel::drawHistoryMenu() {
    // Oldest first: the undo steps, the current text, then the redo steps (newest redo last)
    std::vector<std::string> steps = m_undo;
    const size_t current = steps.size();
    steps.push_back(m_committed);
    for (size_t i = m_redo.size(); i-- > 0;) steps.push_back(m_redo[i]);
    ImGui::TextDisabled("%s", tr("Click a step to go back or forward to it"));
    if (ImGui::Selectable(tr("Opened"), current == 0) && current != 0)
        while (!m_undo.empty()) undo();
    for (size_t i = 1; i < steps.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const std::string name = stepName(steps[i - 1], steps[i]);
        if (i > current) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable(name.c_str(), i == current) && i != current) {
            while (m_undo.size() > i) undo();
            while (m_undo.size() < i && !m_redo.empty()) redo();
        }
        if (i > current) ImGui::PopStyleColor();
        ImGui::PopID();
    }
}

void UiPanel::drawChanges() {
    std::string saved = m_saved;
    if (saved.empty()) { // the file isn't in the canonical shape: compare with it as written
        const std::string file = findAssetFile(m_path);
        std::string text;
        if (!file.empty() && readTextFile(file, text)) saved = text;
    }
    const std::vector<std::pair<char, std::string>> diff = diffLines(splitLines(saved), splitLines(m_live));
    u32 added = 0, removed = 0;
    for (const auto& d : diff) added += d.first == '+', removed += d.first == '-';
    ImGui::TextUnformatted(trf("%s: %u lines added, %u removed", m_path.c_str(), added, removed).c_str());
    if (ImGui::BeginChild("##diff", ImVec2(ImGui::GetFontSize() * 34.0f, ImGui::GetTextLineHeightWithSpacing() * std::min<f32>(20.0f, static_cast<f32>(diff.size()) + 1.0f)),
                          ImGuiChildFlags_FrameStyle)) {
        // Changed lines with two lines of context around them
        for (size_t i = 0; i < diff.size(); ++i) {
            bool near = false;
            for (size_t j = (i >= 2 ? i - 2 : 0); j < std::min(diff.size(), i + 3); ++j) near = near || diff[j].first != ' ';
            if (!near) continue;
            if (diff[i].first == '+') ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1.0f), "+ %s", diff[i].second.c_str());
            else if (diff[i].first == '-') ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "- %s", diff[i].second.c_str());
            else ImGui::TextDisabled("  %s", diff[i].second.c_str());
        }
    }
    ImGui::EndChild();
    if (ImGui::Button(tr("Save"))) {
        save();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Revert"))) {
        loadSource(); // rereads the file (history cleared)
        ImGui::CloseCurrentPopup();
    }
}

// ---------------------------------------------------------------- Screens: duplicate and rename

void UiPanel::duplicateScreen(const std::string& file) {
    const std::string dir = findAsset("assets/ui");
    std::string text;
    if (dir.empty() || !readTextFile(dir + "/" + file, text)) return;
    std::string base = file.substr(0, file.size() - 3) + "_copy", name = base;
    for (int i = 2; fileExists(dir + "/" + name + ".ui"); ++i) name = base + std::to_string(i);
    if (!writeTextFile(dir + "/" + name + ".ui", text)) return;
    MEALKIT_INFO("UI: duplicated ui/%s as ui/%s.ui", file.c_str(), name.c_str());
    notice(trf("Duplicated as %s.ui", name.c_str()));
    if (!dirty()) select(name + ".ui");
    else refresh();
}

void UiPanel::startScreenRename(const std::string& file) {
    m_screenFrom = file.substr(0, file.size() - 3);
    m_screenTo = m_screenFrom;
    m_screenRefs.clear();
    // Project files that name ui/<screen>.ui (game code, other screens using it as a component)
    std::vector<std::string> files;
    listProjectFiles(projectRoot(), files);
    const std::string needle = "ui/" + file;
    for (const std::string& path : files) {
        std::string text;
        if (readTextFile(path, text) && text.find(needle) != std::string::npos) m_screenRefs.push_back(path);
    }
    m_screenRenameOpen = true;
}

void UiPanel::drawScreenTools() {
    if (m_screenRenameOpen) {
        ImGui::OpenPopup("##screenRename");
        m_screenRenameOpen = false;
    }
    if (!ImGui::BeginPopup("##screenRename")) return;
    ImGui::TextUnformatted(trf("Rename %s.ui", m_screenFrom.c_str()).c_str());
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    uiInputText("##screenTo", "", m_screenTo);
    bool valid = !m_screenTo.empty() && m_screenTo != m_screenFrom;
    for (char c : m_screenTo) valid = valid && (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-');
    const std::string dir = findAsset("assets/ui");
    const bool exists = valid && fileExists(dir + "/" + m_screenTo + ".ui");
    if (exists) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", tr("A screen with this name exists"));
    if (m_screenRefs.empty()) ImGui::TextDisabled("%s", tr("No project file names this screen."));
    else {
        ImGui::TextUnformatted(trf("Also replaces ui/%s.ui in %zu files:", m_screenFrom.c_str(), m_screenRefs.size()).c_str());
        for (const std::string& f : m_screenRefs) ImGui::BulletText("%s", f.size() > projectRoot().size() ? f.substr(projectRoot().size() + 1).c_str() : f.c_str());
    }
    const bool open = m_path == "ui/" + m_screenFrom + ".ui";
    if (open && dirty()) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", tr("Save or revert the screen first"));
    ImGui::BeginDisabled(!valid || exists || (open && dirty()));
    if (ImGui::Button(tr("Rename"))) {
        const std::string from = "ui/" + m_screenFrom + ".ui", to = "ui/" + m_screenTo + ".ui";
        if (moveFile(dir + "/" + m_screenFrom + ".ui", dir + "/" + m_screenTo + ".ui")) {
            u32 changed = 0;
            for (std::string path : m_screenRefs) {
                if (path == dir + "/" + m_screenFrom + ".ui") path = dir + "/" + m_screenTo + ".ui"; // the renamed file itself
                std::string text;
                if (!readTextFile(path, text)) continue;
                for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
                if (writeTextFile(path, text)) ++changed;
            }
            MEALKIT_INFO("UI: renamed %s to %s (%u files updated)", from.c_str(), to.c_str(), changed);
            notice(trf("Renamed to %s (%u files updated)", to.c_str(), changed));
            if (open) select(m_screenTo + ".ui");
            else refresh();
        } else {
            MEALKIT_ERROR("UI: cannot rename %s", from.c_str());
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- Notices and capture

void UiPanel::notice(const std::string& text) {
    m_notice = text;
    m_noticeTime = ImGui::GetTime(); // shown translated; the actions log in English themselves
}

void UiPanel::finishCapture() {
    const std::string tmp = projectRoot() + "/build/editor/ui_capture.png";
    Image whole;
    if (!loadImage(tmp, 0, whole) || whole.width == 0) {
        MEALKIT_WARN("UI: capture failed");
        return;
    }
    removeFile(tmp);
    // The board's rectangle in back buffer pixels (ImGui coordinates are the window's pixels)
    const u32 x0 = static_cast<u32>(std::clamp(m_view.x, 0.0f, static_cast<f32>(whole.width)));
    const u32 y0 = static_cast<u32>(std::clamp(m_view.y, 0.0f, static_cast<f32>(whole.height)));
    const u32 x1 = static_cast<u32>(std::clamp(m_view.x + m_view.width, 0.0f, static_cast<f32>(whole.width)));
    const u32 y1 = static_cast<u32>(std::clamp(m_view.y + m_view.height, 0.0f, static_cast<f32>(whole.height)));
    if (x1 <= x0 || y1 <= y0) return;
    Image crop;
    crop.width = x1 - x0;
    crop.height = y1 - y0;
    crop.rgba.resize(static_cast<size_t>(crop.width) * crop.height * 4);
    for (u32 y = 0; y < crop.height; ++y)
        std::copy_n(whole.rgba.data() + (static_cast<size_t>(y0 + y) * whole.width + x0) * 4, static_cast<size_t>(crop.width) * 4,
                    crop.rgba.data() + static_cast<size_t>(y) * crop.width * 4);
    std::string screen = m_path.substr(m_path.rfind('/') + 1);
    screen.resize(screen.size() - 3);
    const Size r = resolution();
    const std::string dir = projectRoot() + "/art/ui/captures";
    createDirectories(dir);
    const std::string out = dir + "/" + screen + "_" + std::to_string(static_cast<int>(r.w)) + "x" + std::to_string(static_cast<int>(r.h)) + "_" + m_strings.language() + ".png";
    if (savePng(out, crop)) {
        MEALKIT_INFO("UI: saved %s (%ux%u)", out.c_str(), crop.width, crop.height);
        ImGui::SetClipboardText(out.c_str());
        notice(trf("Saved %s (path copied)", out.c_str()));
    }
}

// ---------------------------------------------------------------- Live values while playing

void UiPanel::drawPlayValues(ui::System& gameUi) {
    ImGui::SetCursorPos(ImVec2(16.0f, 12.0f));
    ImGui::BeginGroup();
    ImGui::TextDisabled("%s", tr("Game screens are edited while not playing. The game's open screens and their values:"));
    bool any = false;
    for (const ui::ScreenId id : gameUi.screens()) {
        const ui::ScreenInfo info = gameUi.info(id);
        const reflect::View vm = gameUi.viewModel(id);
        ImGui::PushID(static_cast<int>(id));
        any = true;
        const std::string title = info.path + (vm.type ? std::string("  (") + vm.type->name + ")" : std::string());
        if (sectionHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen) && vm.type) {
            drawFieldsReadOnly(*vm.type, vm.data);
            // Keep as the preview values of that screen: art/ui/samples/<screen>.<Type>.values (the default set)
            if (ImGui::SmallButton(tr("Keep as preview values"))) {
                std::string screen = info.path.substr(info.path.rfind('/') + 1);
                if (screen.size() > 3) screen.resize(screen.size() - 3);
                createDirectories(projectRoot() + "/art/ui/samples");
                const std::string file = projectRoot() + "/art/ui/samples/" + screen + "." + vm.type->name + ".values";
                if (reflect::saveTextFile(*vm.type, vm.data, file, kValuesFormat, "# Preview values for the editor [UI] panel (not shipped), kept from a play session")) {
                    MEALKIT_INFO("UI: kept the game's values in %s", file.c_str());
                    notice(trf("Kept %s", file.c_str()));
                }
            }
            ImGui::SetItemTooltip("%s", tr("Saves these values for the [UI] workspace's preview of this screen (art/ui/samples)"));
        }
        ImGui::PopID();
    }
    if (!any) ImGui::TextDisabled("%s", tr("(no screens open)"));
    ImGui::EndGroup();
}

} // namespace mealkit::editor
