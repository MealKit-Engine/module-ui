// Editor [UI] workspace: several nodes selected at once (ROADMAP "UI workspace: multi-select and align"). m_selection stays the primary
// node (the last one picked: the inspector, handles and the path strip follow it); m_multi holds every selected path, primary included,
// and is empty for a single selection. Group edits work on names (unique in a file), because paths shift as nodes move.
#include "mealkit/core/Log.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/editor/UiPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <imgui.h>

namespace mealkit::editor {
namespace {

std::vector<f32> numbersOf(const std::string& s) {
    std::vector<f32> out;
    size_t a = 0;
    while ((a = s.find_first_not_of(' ', a)) != std::string::npos) {
        const size_t b = s.find(' ', a);
        const std::string w = s.substr(a, b == std::string::npos ? std::string::npos : b - a);
        char* end = nullptr;
        const f32 v = std::strtof(w.c_str(), &end);
        if (end == w.c_str() || *end) return {};
        out.push_back(v);
        a = b;
    }
    return out;
}

std::string whole(f32 v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(v)));
    return buf;
}

bool under(const ui::SourcePath& path, const ui::SourcePath& ancestor) { // path is ancestor or inside it
    return path.size() >= ancestor.size() && std::equal(ancestor.begin(), ancestor.end(), path.begin());
}

bool findByName(const ui::SourceNode& n, const std::string& name, ui::SourcePath& path) {
    if (n.name == name) return true;
    for (u32 c = 0; c < n.children.size(); ++c) {
        path.push_back(c);
        if (findByName(n.children[c], name, path)) return true;
        path.pop_back();
    }
    return false;
}

} // namespace

bool UiPanel::pathOfName(const std::string& name, ui::SourcePath& out) const {
    out.clear();
    const ui::SourceNode* board = ui::findNode(m_source, {});
    return board && !name.empty() && findByName(*board, name, out);
}

std::vector<ui::SourcePath> UiPanel::selectedPaths() const {
    if (!m_hasSelection) return {};
    if (m_multi.size() > 1 && std::find(m_multi.begin(), m_multi.end(), m_selection) != m_multi.end()) {
        std::vector<ui::SourcePath> out;
        for (const ui::SourcePath& p : m_multi)
            if (ui::findNode(m_source, p)) out.push_back(p);
        if (out.size() > 1) return out;
    }
    return {m_selection};
}

bool UiPanel::isSelected(const ui::SourcePath& path) const {
    const std::vector<ui::SourcePath> all = selectedPaths();
    return std::find(all.begin(), all.end(), path) != all.end();
}

// Shift (canvas) or Ctrl/Shift (layers) + click: in or out of the selection. The board never joins a group
void UiPanel::toggleSelected(const ui::SourcePath& path) {
    if (path.empty()) return;
    std::vector<ui::SourcePath> all = selectedPaths();
    all.erase(std::remove_if(all.begin(), all.end(), [](const ui::SourcePath& p) { return p.empty(); }), all.end());
    const auto it = std::find(all.begin(), all.end(), path);
    if (it != all.end()) {
        all.erase(it);
        if (all.empty()) {
            m_hasSelection = false;
            m_multi.clear();
            return;
        }
        m_selection = all.back();
    } else {
        all.push_back(path);
        m_selection = path;
    }
    m_hasSelection = true;
    m_multi = all.size() > 1 ? all : std::vector<ui::SourcePath>{};
}

void UiPanel::setSelection(const std::vector<ui::SourcePath>& paths) {
    m_multi.clear();
    m_hasSelection = !paths.empty();
    if (paths.empty()) return;
    m_selection = paths.back();
    if (paths.size() > 1) m_multi = paths;
}

// Ctrl+A while the workspace has the keys: the selected node's siblings (or the board's children)
void UiPanel::selectAll() {
    if (!editing()) return;
    ui::SourcePath parent;
    if (m_hasSelection && !m_selection.empty()) parent.assign(m_selection.begin(), m_selection.end() - 1);
    const ui::SourceNode* p = ui::findNode(m_source, parent);
    if (!p || p->children.empty()) return;
    std::vector<ui::SourcePath> all;
    for (u32 c = 0; c < p->children.size(); ++c) {
        ui::SourcePath path = parent;
        path.push_back(c);
        if (!editorLocked(path)) all.push_back(path);
    }
    setSelection(all);
}

// Selected nodes without the ones inside another selected node (they go with it), and never the board
std::vector<ui::SourcePath> UiPanel::topSelected() const {
    std::vector<ui::SourcePath> all = selectedPaths(), out;
    for (const ui::SourcePath& p : all) {
        if (p.empty()) continue;
        bool inside = false;
        for (const ui::SourcePath& q : all) inside = inside || (q != p && !q.empty() && under(p, q));
        if (!inside) out.push_back(p);
    }
    return out;
}

std::vector<std::string> UiPanel::namesOf(const std::vector<ui::SourcePath>& paths) const {
    std::vector<std::string> out;
    for (const ui::SourcePath& p : paths)
        if (const ui::SourceNode* n = ui::findNode(m_source, p)) out.push_back(n->name);
    return out;
}

void UiPanel::selectNames(const std::vector<std::string>& names) {
    std::vector<ui::SourcePath> paths;
    for (const std::string& n : names) {
        ui::SourcePath p;
        if (pathOfName(n, p)) paths.push_back(p);
    }
    setSelection(paths);
}

// ---------------------------------------------------------------------------------------------------- group edits

bool UiPanel::deleteMany() {
    std::vector<ui::SourcePath> paths = topSelected();
    if (paths.empty()) return false;
    std::sort(paths.begin(), paths.end(), [](const ui::SourcePath& a, const ui::SourcePath& b) { return b < a; }); // later first: earlier paths stay valid
    for (const ui::SourcePath& p : paths) ui::removeNode(m_source, p);
    m_hasSelection = false;
    m_multi.clear();
    syncSource();
    return true;
}

bool UiPanel::duplicateMany() {
    std::vector<ui::SourcePath> paths = topSelected();
    if (paths.empty()) return false;
    std::sort(paths.begin(), paths.end(), [](const ui::SourcePath& a, const ui::SourcePath& b) { return b < a; });
    std::vector<std::string> copies;
    for (const ui::SourcePath& p : paths) {
        const ui::SourceNode* n = ui::findNode(m_source, p);
        if (!n) continue;
        ui::SourceNode copy = *n;
        copy.name = ui::uniqueName(m_source, copy.name);
        copies.push_back(copy.name);
        ui::addNode(m_source, ui::SourcePath(p.begin(), p.end() - 1), p.back() + 1, std::move(copy));
    }
    std::reverse(copies.begin(), copies.end());
    selectNames(copies);
    syncSource();
    return true;
}

std::string UiPanel::selectionText() const {
    std::string text;
    for (const ui::SourcePath& p : topSelected())
        if (const ui::SourceNode* n = ui::findNode(m_source, p)) text += ui::writeNodeText(*n);
    return text;
}

// Several nodes (clipboard text with more than one head line): after the selection, or inside a selected frame, in order
bool UiPanel::pasteMany(const std::string& text) {
    ui::Source s;
    if (!ui::parseSource("board clip 1 1\n" + text + (text.empty() || text.back() == '\n' ? "" : "\n") + "end\n", s) || s.root.children.empty()) return false;
    std::vector<ui::SourceNode> nodes = s.root.children[0].children;
    if (nodes.empty()) return false;
    ui::SourcePath parent;
    u32 at = 0xFFFFFFFFu;
    if (m_hasSelection) {
        const ui::SourceNode* n = ui::findNode(m_source, m_selection);
        if (n && (n->kind == "frame" || n->kind == "board") && !multiSelected()) parent = m_selection;
        else if (!m_selection.empty()) {
            parent.assign(m_selection.begin(), m_selection.end() - 1);
            at = m_selection.back() + 1;
        }
    }
    std::vector<std::string> names;
    for (ui::SourceNode& n : nodes) {
        n.name = ui::uniqueName(m_source, n.name);
        names.push_back(n.name);
        const ui::SourcePath added = ui::addNode(m_source, parent, at, std::move(n));
        if (!added.empty() && at != 0xFFFFFFFFu) at = added.back() + 1;
    }
    selectNames(names);
    syncSource();
    return true;
}

// Group wrap: siblings only. Free parent: the frame takes the selection's bounds and the nodes keep where they are drawn. Layout
// parent: the frame takes the first node's place, fits its content and lines the nodes up like the parent did
bool UiPanel::wrapMany() {
    std::vector<ui::SourcePath> paths = topSelected();
    if (paths.size() < 2) return false;
    const ui::SourcePath parent(paths[0].begin(), paths[0].end() - 1);
    for (const ui::SourcePath& p : paths)
        if (ui::SourcePath(p.begin(), p.end() - 1) != parent) {
            notice(tr("Wrap in frame: pick nodes with the same parent"));
            return false;
        }
    std::sort(paths.begin(), paths.end());
    const ui::SourceNode* pn = ui::findNode(m_source, parent);
    if (!pn) return false;
    std::string layout, gap;
    const bool inLayout = ui::getProperty(*pn, "layout", layout);
    ui::SourceNode frame;
    frame.kind = "frame";
    frame.name = ui::uniqueName(m_source, "group");
    std::vector<std::pair<std::string, std::vector<f32>>> moves; // node name, new pos (free parent)
    if (inLayout) {
        // Across the parent's direction the frame stretches like a fill child would, so fill children keep their width (or height)
        frame.items.push_back({layout.rfind("column", 0) == 0 ? "size fill hug" : layout.rfind("grid", 0) == 0 ? "size hug hug" : "size hug fill", -1});
        frame.items.push_back({"layout " + layout, -1});
        if (ui::getProperty(*pn, "gap", gap)) frame.items.push_back({"gap " + gap, -1});
    } else {
        // Bounds of the drawn boxes, then each node's pos relative to the frame (pos lines are in the parent's units)
        f32 x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        ui::NodeBox parentBox{};
        const bool hasParentBox = boxOf(parent, parentBox);
        for (const ui::SourcePath& p : paths) {
            ui::NodeBox b;
            if (!boxOf(p, b)) return false;
            x0 = std::min(x0, b.x);
            y0 = std::min(y0, b.y);
            x1 = std::max(x1, b.x + b.w);
            y1 = std::max(y1, b.y + b.h);
        }
        const f32 ox = hasParentBox ? parentBox.x : 0.0f, oy = hasParentBox ? parentBox.y : 0.0f;
        frame.items.push_back({"pos " + whole(x0 - ox) + " " + whole(y0 - oy), -1});
        frame.items.push_back({"size " + whole(x1 - x0) + " " + whole(y1 - y0), -1});
        for (const ui::SourcePath& p : paths) {
            ui::NodeBox b;
            boxOf(p, b);
            if (const ui::SourceNode* n = ui::findNode(m_source, p)) moves.push_back({n->name, {b.x - x0, b.y - y0}});
        }
    }
    const std::vector<std::string> names = namesOf(paths);
    const std::string frameName = frame.name;
    ui::addNode(m_source, parent, paths[0].back(), std::move(frame));
    for (const std::string& name : names) {
        ui::SourcePath from, into;
        if (!pathOfName(name, from) || !pathOfName(frameName, into)) continue;
        const ui::SourceNode* f = ui::findNode(m_source, into);
        ui::moveNode(m_source, from, into, f ? static_cast<u32>(f->children.size()) : 0u);
    }
    for (const auto& [name, pos] : moves) {
        ui::SourcePath p;
        if (pathOfName(name, p))
            if (ui::SourceNode* n = ui::findNode(m_source, p)) {
                std::string v;
                if (ui::getProperty(*n, "pos", v) || pos[0] != 0.0f || pos[1] != 0.0f) ui::setProperty(*n, "pos", whole(pos[0]) + " " + whole(pos[1]));
            }
    }
    selectNames({frameName});
    syncSource();
    return true;
}

// Free nodes with a numeric pos among the selection (a layout places the others; a $token pos isn't moved)
std::vector<ui::SourcePath> UiPanel::movableSelected() const {
    std::vector<ui::SourcePath> out;
    for (const ui::SourcePath& p : topSelected()) {
        if (!layoutOf(p).empty()) continue;
        const ui::SourceNode* n = ui::findNode(m_source, p);
        std::string v;
        if (n && (!ui::getProperty(*n, "pos", v) || numbersOf(v).size() == 2)) out.push_back(p);
    }
    return out;
}

void UiPanel::offsetPos(const ui::SourcePath& path, f32 dx, f32 dy) {
    ui::SourceNode* n = ui::findNode(m_source, path);
    if (!n) return;
    std::string v;
    std::vector<f32> pos = ui::getProperty(*n, "pos", v) ? numbersOf(v) : std::vector<f32>{0.0f, 0.0f};
    if (pos.size() != 2) return;
    ui::setProperty(*n, "pos", whole(pos[0] + dx) + " " + whole(pos[1] + dy));
}

void UiPanel::nudgeMany(f32 dx, f32 dy) {
    for (const ui::SourcePath& p : movableSelected()) offsetPos(p, dx, dy);
    syncSource();
}

// how: 0 left, 1 center, 2 right, 3 top, 4 middle, 5 bottom. Several nodes line up on their joint bounds; one node in its parent
void UiPanel::alignSelected(int how) {
    if (!editing()) return;
    const std::vector<ui::SourcePath> paths = movableSelected();
    const size_t skipped = topSelected().size() - paths.size();
    if (paths.empty()) {
        notice(tr("Align: the selection is placed by a layout (nothing to move)"));
        return;
    }
    f32 x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    if (paths.size() == 1) {
        ui::NodeBox p;
        if (!boxOf(ui::SourcePath(paths[0].begin(), paths[0].end() - 1), p)) return;
        x0 = p.x, y0 = p.y, x1 = p.x + p.w, y1 = p.y + p.h;
    } else {
        for (const ui::SourcePath& path : paths) {
            ui::NodeBox b;
            if (!boxOf(path, b)) continue;
            x0 = std::min(x0, b.x), y0 = std::min(y0, b.y), x1 = std::max(x1, b.x + b.w), y1 = std::max(y1, b.y + b.h);
        }
    }
    for (const ui::SourcePath& path : paths) {
        ui::NodeBox b;
        if (!boxOf(path, b)) continue;
        f32 dx = 0.0f, dy = 0.0f;
        if (how == 0) dx = x0 - b.x;
        else if (how == 1) dx = (x0 + x1) * 0.5f - (b.x + b.w * 0.5f);
        else if (how == 2) dx = x1 - (b.x + b.w);
        else if (how == 3) dy = y0 - b.y;
        else if (how == 4) dy = (y0 + y1) * 0.5f - (b.y + b.h * 0.5f);
        else dy = y1 - (b.y + b.h);
        offsetPos(path, dx, dy);
    }
    syncSource();
    if (skipped > 0) notice(trf("Aligned; %zu node(s) placed by a layout were left as they are", skipped));
}

// Equal gaps between three or more free nodes, the outer two staying where they are
void UiPanel::distributeSelected(bool vertical) {
    if (!editing()) return;
    std::vector<std::pair<ui::SourcePath, ui::NodeBox>> items;
    for (const ui::SourcePath& p : movableSelected()) {
        ui::NodeBox b;
        if (boxOf(p, b)) items.push_back({p, b});
    }
    if (items.size() < 3) {
        notice(tr("Distribute: select three or more free nodes"));
        return;
    }
    std::sort(items.begin(), items.end(), [vertical](const auto& a, const auto& b) { return vertical ? a.second.y < b.second.y : a.second.x < b.second.x; });
    const ui::NodeBox& first = items.front().second;
    const ui::NodeBox& last = items.back().second;
    f32 sizes = 0.0f;
    for (const auto& it : items) sizes += vertical ? it.second.h : it.second.w;
    const f32 span = vertical ? last.y + last.h - first.y : last.x + last.w - first.x;
    const f32 gap = (span - sizes) / static_cast<f32>(items.size() - 1);
    f32 at = vertical ? first.y + first.h + gap : first.x + first.w + gap;
    for (size_t i = 1; i + 1 < items.size(); ++i) {
        const ui::NodeBox& b = items[i].second;
        offsetPos(items[i].first, vertical ? 0.0f : at - b.x, vertical ? at - b.y : 0.0f);
        at += (vertical ? b.h : b.w) + gap;
    }
    syncSource();
}

// Align buttons in one row: left, center, right | top, middle, bottom (and distribute when several are selected)
void UiPanel::drawAlignRow(bool several) {
    const char* tips[] = {tr("Align left edges"), tr("Align horizontal centers"), tr("Align right edges"), tr("Align top edges"), tr("Align vertical centers"),
                          tr("Align bottom edges")};
    const char* text[] = {tr("Left"), tr("Center"), tr("Right"), tr("Top"), tr("Middle"), tr("Bottom")};
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(several ? tr("Align") : tr("Align in parent"));
    ImGui::SetItemTooltip("%s", several ? tr("Lines the selected free nodes up on their joint bounds") : tr("Moves the node to an edge or the center of its parent"));
    const f32 w = (ImGui::GetContentRegionAvail().x - 5.0f * 2.0f) / 6.0f;
    for (int i = 0; i < 6; ++i) {
        if (i > 0) ImGui::SameLine(0.0f, 2.0f);
        ImGui::PushID(i);
        if (ImGui::Button(text[i], ImVec2(w, 0.0f))) defer([this, i] { alignSelected(i); });
        ImGui::SetItemTooltip("%s", tips[i]);
        ImGui::PopID();
    }
    if (!several) return;
    const f32 w2 = (ImGui::GetContentRegionAvail().x - 2.0f) * 0.5f;
    if (ImGui::Button(tr("Distribute horizontally"), ImVec2(w2, 0.0f))) defer([this] { distributeSelected(false); });
    ImGui::SetItemTooltip("%s", tr("Equal gaps from left to right (three or more nodes; the outer two stay)"));
    ImGui::SameLine(0.0f, 2.0f);
    if (ImGui::Button(tr("Distribute vertically"), ImVec2(w2, 0.0f))) defer([this] { distributeSelected(true); });
    ImGui::SetItemTooltip("%s", tr("Equal gaps from top to bottom (three or more nodes; the outer two stay)"));
}

// The right column while several nodes are selected: what they are and what can be done to all of them
void UiPanel::drawMultiInspector() {
    const std::vector<ui::SourcePath> paths = selectedPaths();
    ImGui::Spacing();
    ImGui::TextUnformatted(trf("%zu nodes selected", paths.size()).c_str());
    std::string names;
    for (const std::string& n : namesOf(paths)) names += (names.empty() ? "" : ", ") + n;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", names.c_str());
    ImGui::TextDisabled("%s", tr("Shift+click on the canvas (Ctrl+click in the layers) adds or removes a node. Drag on empty space to select with a box."));
    ImGui::PopTextWrapPos();
    ImGui::SeparatorText(tr("Arrange"));
    drawAlignRow(true);
    const size_t placed = topSelected().size() - movableSelected().size();
    if (placed > 0) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", trf("%zu of them are placed by a layout: align and distribute leave them.", placed).c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::SeparatorText(tr("All of them"));
    if (ImGui::Button(tr("Wrap in frame"))) defer([this] { wrapMany(); });
    ImGui::SameLine();
    if (ImGui::Button(tr("Duplicate"))) defer([this] { duplicateMany(); });
    ImGui::SameLine();
    if (ImGui::Button(tr("Delete"))) defer([this] { deleteMany(); });
}

// ---------------------------------------------------------------------------------------------------- marquee

void UiPanel::finishMarquee() {
    const f32 x0 = std::min(m_marquee[0], m_marquee[2]), x1 = std::max(m_marquee[0], m_marquee[2]);
    const f32 y0 = std::min(m_marquee[1], m_marquee[3]), y1 = std::max(m_marquee[1], m_marquee[3]);
    // Boxes fully inside, then only the outermost of them (a panel and not also its children)
    std::vector<ui::SourcePath> inside;
    for (const ui::NodeBox& b : m_boxes) {
        if (!b.visible || b.node >= m_map.size()) continue;
        const ui::SourcePath& p = m_map[b.node];
        if (p.empty() || editorLocked(p) || editorHidden(p)) continue;
        if (b.x >= x0 && b.y >= y0 && b.x + b.w <= x1 && b.y + b.h <= y1 && std::find(inside.begin(), inside.end(), p) == inside.end()) inside.push_back(p);
    }
    std::vector<ui::SourcePath> out;
    for (const ui::SourcePath& p : inside) {
        bool nested = false;
        for (const ui::SourcePath& q : inside) nested = nested || (q != p && under(p, q));
        if (!nested) out.push_back(p);
    }
    if (ImGui::GetIO().KeyShift)
        for (const ui::SourcePath& p : selectedPaths())
            if (!p.empty() && std::find(out.begin(), out.end(), p) == out.end()) out.insert(out.begin(), p);
    if (!out.empty()) setSelection(out);
}

} // namespace mealkit::editor
