// [UI] panel editing with the mouse on the preview (UiPanel.h): select, move and resize free nodes with snapping,
// reorder nodes a flex/grid layout places, and the overlays that show it (handles, guides, insert line, padding)
#include "mealkit/editor/UiPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <imgui.h>

namespace mealkit::editor {
namespace {

constexpr f32 kHandle = 5.0f;      // handle half size (screen pixels)
constexpr f32 kDragStart = 3.0f;   // pixels the mouse moves before a press becomes a drag
constexpr f32 kSnap = 6.0f;        // snapping distance (screen pixels)
constexpr u32 kLeft = 1, kRight = 2, kTop = 4, kBottom = 8;

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    size_t a = 0;
    while ((a = s.find_first_not_of(' ', a)) != std::string::npos) {
        const size_t b = s.find(' ', a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        a = b;
    }
    return out;
}

bool toNumber(const std::string& s, f32& out) {
    char* end = nullptr;
    const f32 v = std::strtof(s.c_str(), &end);
    if (end == s.c_str() || *end) return false;
    out = v;
    return true;
}

std::string whole(f32 v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(v)));
    return buf;
}

ui::SourcePath parentOf(const ui::SourcePath& path) { return ui::SourcePath(path.begin(), path.end() - 1); }

} // namespace

bool UiPanel::boxOf(const ui::SourcePath& path, ui::NodeBox& out) const {
    for (const ui::NodeBox& b : m_boxes)
        if (b.visible && b.node < m_map.size() && m_map[b.node] == path) {
            out = b;
            return true;
        }
    return false;
}

std::string UiPanel::layoutOf(const ui::SourcePath& path) const {
    if (path.empty()) return {};
    const ui::SourceNode* n = ui::findNode(m_source, path);
    const ui::SourceNode* parent = ui::findNode(m_source, parentOf(path));
    std::string layout, unused;
    if (!n || !parent || !ui::getProperty(*parent, "layout", layout) || ui::getProperty(*n, "absolute", unused)) return {};
    return layout.empty() ? std::string("row") : layout;
}

u32 UiPanel::handleAt(f32 x, f32 y) const {
    ui::NodeBox b;
    if (!m_hasSelection || m_selection.empty() || !layoutOf(m_selection).empty() || !boxOf(m_selection, b)) return 0;
    const f32 x0 = m_originX + b.x * m_scaleK, y0 = m_originY + b.y * m_scaleK;
    const f32 x1 = x0 + b.w * m_scaleK, y1 = y0 + b.h * m_scaleK;
    const f32 r = kHandle + 2.0f;
    auto near = [r](f32 a, f32 b) { return std::fabs(a - b) <= r; };
    if (x < x0 - r || x > x1 + r || y < y0 - r || y > y1 + r) return 0;
    u32 h = 0;
    if (near(x, x0)) h |= kLeft;
    else if (near(x, x1)) h |= kRight;
    if (near(y, y0)) h |= kTop;
    else if (near(y, y1)) h |= kBottom;
    // Edges count only in their middle third, corners anywhere near
    const bool corner = (h & (kLeft | kRight)) && (h & (kTop | kBottom));
    if (!corner && (h & (kLeft | kRight)) && std::fabs(y - (y0 + y1) * 0.5f) > (y1 - y0) / 6.0f + r) return 0;
    if (!corner && (h & (kTop | kBottom)) && std::fabs(x - (x0 + x1) * 0.5f) > (x1 - x0) / 6.0f + r) return 0;
    return h;
}

bool UiPanel::pointer(f32 x, f32 y, bool pressed, bool down, bool ctrl, bool overUi) {
    if (!editing() || !m_screen) {
        m_drag = Drag::None;
        return false;
    }
    const ui::ScreenInfo info = m_ui.info(m_screen);
    if (info.scale <= 0.0f) return false;
    m_originX = info.originX;
    m_originY = info.originY;
    m_scaleK = info.scale;
    m_boxes = m_ui.boxes(m_screen);
    const f32 lx = (x - m_originX) / m_scaleK, ly = (y - m_originY) / m_scaleK;
    if (m_interact) { // the screen takes the mouse itself (render passes it the input)
        m_drag = Drag::None;
        m_hasHover = false;
        return false;
    }
    m_hoverHandle = m_drag == Drag::None && !overUi ? handleAt(x, y) : m_dragHandle;
    // Hover outline: the topmost box under the mouse (a hovered layer row sets it instead)
    if (!m_treeHovered) {
        m_hasHover = false;
        u32 depth = 0;
        if (!overUi && m_drag == Drag::None)
            for (const ui::NodeBox& b : m_boxes)
                if (b.visible && b.node < m_map.size() && !m_map[b.node].empty() && !editorLocked(m_map[b.node]) && lx >= b.x && ly >= b.y && lx < b.x + b.w && ly < b.y + b.h && (!m_hasHover || b.depth >= depth)) {
                    m_hover = m_map[b.node];
                    m_hasHover = true;
                    depth = b.depth;
                }
    }

    if (pressed && !overUi) {
        if (const u32 h = handleAt(x, y)) {
            startDrag(Drag::Resize, x, y);
            m_dragHandle = h;
            return true;
        }
        // Select what's under the mouse; a press on the selected node may become a move (or a reorder in a layout)
        std::vector<std::pair<u32, ui::SourcePath>> hits;
        for (const ui::NodeBox& b : m_boxes) {
            if (!b.visible || lx < b.x || ly < b.y || lx >= b.x + b.w || ly >= b.y + b.h || b.node >= m_map.size()) continue;
            const ui::SourcePath& path = m_map[b.node];
            if (editorLocked(path)) continue; // locked in the editor: clicks go to what's beneath
            bool seen = false;
            for (const auto& hit : hits) seen = seen || hit.second == path;
            if (!seen) hits.push_back({b.depth, path});
        }
        if (hits.empty()) return false;
        std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        size_t pick = 0;
        if (ctrl && m_hasSelection) // Ctrl: the box beneath the selected one
            for (size_t i = 0; i < hits.size(); ++i)
                if (hits[i].second == m_selection) pick = (i + 1) % hits.size();
        // Without Ctrl, pressing inside the selected node keeps it (so its children don't steal the drag)
        bool keep = false;
        if (!ctrl && m_hasSelection)
            for (const auto& hit : hits) keep = keep || hit.second == m_selection;
        if (!keep) m_selection = hits[pick].second;
        m_hasSelection = true;
        if (!m_selection.empty()) startDrag(layoutOf(m_selection).empty() ? Drag::Move : Drag::Reorder, x, y);
        m_dragArmed = false; // becomes a drag after kDragStart pixels
        return true;
    }
    if (m_drag == Drag::None) return false;
    if (!down) {
        if (m_drag == Drag::Reorder && m_dragArmed) finishReorder();
        m_drag = Drag::None;
        m_guides.clear();
        m_insert = {};
        syncSource(); // commits the whole drag as one history step
        return true;
    }
    if (!m_dragArmed) {
        if (std::fabs(x - m_dragStart[0]) < kDragStart && std::fabs(y - m_dragStart[1]) < kDragStart) return true;
        m_dragArmed = true;
    }
    const f32 dx = (x - m_dragStart[0]) / m_scaleK, dy = (y - m_dragStart[1]) / m_scaleK;
    if (m_drag == Drag::Move) dragMove(dx, dy, !ctrl);
    else if (m_drag == Drag::Resize) dragResize(dx, dy);
    else updateReorder(lx, ly);
    return true;
}

void UiPanel::startDrag(Drag kind, f32 x, f32 y) {
    m_drag = kind;
    m_dragStart[0] = x;
    m_dragStart[1] = y;
    m_dragArmed = kind == Drag::Resize;
    m_dragHandle = 0;
    m_guides.clear();
    m_insert = {};
    ui::NodeBox b{};
    m_dragOk = boxOf(m_selection, b);
    m_startBox[0] = b.x;
    m_startBox[1] = b.y;
    m_startBox[2] = b.w;
    m_startBox[3] = b.h;
    const ui::SourceNode* n = ui::findNode(m_source, m_selection);
    std::string value;
    m_startPos[0] = m_startPos[1] = 0.0f;
    if (n && ui::getProperty(*n, "pos", value)) {
        const std::vector<std::string> w = words(value);
        m_dragOk = m_dragOk && w.size() == 2 && toNumber(w[0], m_startPos[0]) && toNumber(w[1], m_startPos[1]); // $tokens aren't dragged
    }
    // Size: a fixed number keeps its value, hug/fill/$token start from the drawn size (resizing makes that axis fixed)
    m_startSize[0] = b.w;
    m_startSize[1] = b.h;
    if (n && ui::getProperty(*n, "size", value)) {
        const std::vector<std::string> w = words(value);
        for (size_t i = 0; i < 2 && i < w.size(); ++i) toNumber(w[i], m_startSize[i]);
    }
}

void UiPanel::dragMove(f32 dx, f32 dy, bool snap) {
    ui::SourceNode* n = ui::findNode(m_source, m_selection);
    if (!n || !m_dragOk) return;
    m_guides.clear();
    if (snap && m_snapGrid) { // grid snapping: the box's top left on the grid (instead of the siblings' edges)
        const f32 g = std::max(m_gridSize, 1.0f);
        dx = std::round((m_startBox[0] + dx) / g) * g - m_startBox[0];
        dy = std::round((m_startBox[1] + dy) / g) * g - m_startBox[1];
    } else if (snap) {
        // Edges and centers of the moved box against the parent's and the siblings'
        std::vector<f32> xs, ys;
        ui::NodeBox p;
        const ui::SourcePath parent = parentOf(m_selection);
        if (boxOf(parent, p)) {
            xs.insert(xs.end(), {p.x, p.x + p.w * 0.5f, p.x + p.w});
            ys.insert(ys.end(), {p.y, p.y + p.h * 0.5f, p.y + p.h});
        }
        if (const ui::SourceNode* pn = ui::findNode(m_source, parent))
            for (u32 c = 0; c < pn->children.size(); ++c) {
                ui::SourcePath sibling = parent;
                sibling.push_back(c);
                ui::NodeBox s;
                if (sibling == m_selection || !boxOf(sibling, s)) continue;
                xs.insert(xs.end(), {s.x, s.x + s.w * 0.5f, s.x + s.w});
                ys.insert(ys.end(), {s.y, s.y + s.h * 0.5f, s.y + s.h});
            }
        const f32 limit = kSnap / m_scaleK;
        auto snapAxis = [&](f32& d, f32 start, f32 size, const std::vector<f32>& targets, bool vertical) {
            f32 best = limit, offset = 0.0f, at = 0.0f;
            for (f32 edge : {start + d, start + d + size * 0.5f, start + d + size})
                for (f32 t : targets)
                    if (std::fabs(t - edge) < best) {
                        best = std::fabs(t - edge);
                        offset = t - edge;
                        at = t;
                    }
            if (best < limit) {
                d += offset;
                m_guides.push_back({vertical, at});
            }
        };
        snapAxis(dx, m_startBox[0], m_startBox[2], xs, true);
        snapAxis(dy, m_startBox[1], m_startBox[3], ys, false);
    }
    ui::setProperty(*n, "pos", whole(m_startPos[0] + dx) + " " + whole(m_startPos[1] + dy));
    syncSource();
}

void UiPanel::dragResize(f32 dx, f32 dy) {
    ui::SourceNode* n = ui::findNode(m_source, m_selection);
    if (!n || !m_dragOk) return;
    f32 x = m_startPos[0], y = m_startPos[1], w = m_startSize[0], h = m_startSize[1];
    if (m_dragHandle & kRight) w += dx;
    if (m_dragHandle & kBottom) h += dy;
    if (m_dragHandle & kLeft) {
        w -= dx;
        x += dx;
    }
    if (m_dragHandle & kTop) {
        h -= dy;
        y += dy;
    }
    if (m_snapGrid && !ImGui::GetIO().KeyCtrl) { // the moved edges on the grid
        const f32 g = std::max(m_gridSize, 1.0f);
        if (m_dragHandle & kRight) w = std::round((x + w) / g) * g - x;
        if (m_dragHandle & kBottom) h = std::round((y + h) / g) * g - y;
        if (m_dragHandle & kLeft) {
            const f32 right = x + w;
            x = std::round(x / g) * g;
            w = right - x;
        }
        if (m_dragHandle & kTop) {
            const f32 bottom = y + h;
            y = std::round(y / g) * g;
            h = bottom - y;
        }
    }
    w = std::max(w, 1.0f);
    h = std::max(h, 1.0f);
    // Only the dragged axes change; the other keeps its word (hug, fill, $token)
    std::string value;
    std::vector<std::string> size = ui::getProperty(*n, "size", value) ? words(value) : std::vector<std::string>{};
    size.resize(2, "hug");
    if (m_dragHandle & (kLeft | kRight)) size[0] = whole(w);
    if (m_dragHandle & (kTop | kBottom)) size[1] = whole(h);
    ui::setProperty(*n, "size", size[0] + " " + size[1]);
    if (m_dragHandle & (kLeft | kTop)) ui::setProperty(*n, "pos", whole(x) + " " + whole(y));
    syncSource();
}

void UiPanel::updateReorder(f32 lx, f32 ly) {
    // Insert position = how many siblings (by drawn center, in the layout's direction) come before the mouse
    const ui::SourcePath parent = parentOf(m_selection);
    const ui::SourceNode* pn = ui::findNode(m_source, parent);
    if (!pn) return;
    const std::string layout = layoutOf(m_selection);
    const bool column = layout.rfind("column", 0) == 0;
    const bool grid = layout.rfind("grid", 0) == 0;
    u32 at = 0;
    ui::NodeBox last{};
    bool any = false;
    m_insert = {};
    for (u32 c = 0; c < pn->children.size(); ++c) {
        ui::SourcePath sibling = parent;
        sibling.push_back(c);
        ui::NodeBox s;
        if (!boxOf(sibling, s)) continue;
        const f32 cx = s.x + s.w * 0.5f, cy = s.y + s.h * 0.5f;
        const bool before = grid ? (ly > s.y + s.h || (ly >= s.y && lx > cx)) : column ? ly > cy : lx > cx;
        if (before) {
            at = c + 1;
            last = s;
            any = true;
        } else if (!m_insert.valid) {
            // Line at the leading edge of the first sibling after the mouse
            m_insert = column ? InsertLine{true, s.x, s.y, s.w, 0.0f} : InsertLine{true, s.x, s.y, 0.0f, s.h};
        }
    }
    if (!m_insert.valid && any) m_insert = column ? InsertLine{true, last.x, last.y + last.h, last.w, 0.0f} : InsertLine{true, last.x + last.w, last.y, 0.0f, last.h};
    m_insertAt = at;
}

void UiPanel::finishReorder() {
    // at counts siblings before the mouse including the moved node itself; after removing it, one fewer comes before
    u32 at = m_insertAt;
    if (!m_selection.empty() && at > m_selection.back()) --at;
    if (!m_selection.empty() && at == m_selection.back()) return; // same place
    ui::SourcePath moved;
    if (ui::moveNode(m_source, m_selection, parentOf(m_selection), at, &moved)) m_selection = moved;
}

void UiPanel::drawOverlay() {
    if (!editing() || !m_screen || m_captureStep == 1) return; // a capture frame shows the board alone
    drawCanvasGuides();
    if (m_interact) return;
    ImDrawList* draw = ImGui::GetBackgroundDrawList(); // over the scene view, under the editor windows
    auto px = [this](f32 lx, f32 ly) { return ImVec2(m_originX + lx * m_scaleK, m_originY + ly * m_scaleK); };
    const ImU32 blue = IM_COL32(80, 170, 255, 255);
    // A small name tag at a box corner (above it when there is room, else inside)
    auto tag = [draw](ImVec2 at, const std::string& text, ImU32 bg, bool below) {
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        const ImVec2 pad(4.0f, 1.0f);
        const ImVec2 a(at.x, below ? at.y + 3.0f : at.y - size.y - pad.y * 2.0f - 3.0f);
        draw->AddRectFilled(a, ImVec2(a.x + size.x + pad.x * 2.0f, a.y + size.y + pad.y * 2.0f), bg, 3.0f);
        draw->AddText(ImVec2(a.x + pad.x, a.y + pad.y), IM_COL32(255, 255, 255, 255), text.c_str());
    };
    auto nameOf = [this](const ui::SourcePath& path) {
        const ui::SourceNode* n = ui::findNode(m_source, path);
        return !n ? std::string() : n->name.empty() ? n->kind : n->name;
    };
    ui::NodeBox b;
    // Hover: thin outline and the name, unless it is the selection or a drag is on
    if (m_hasHover && m_drag == Drag::None && (!m_hasSelection || m_hover != m_selection) && boxOf(m_hover, b)) {
        const ImVec2 a = px(b.x, b.y), c = px(b.x + b.w, b.y + b.h);
        draw->AddRect(a, c, IM_COL32(80, 170, 255, 170), 0.0f, 0, 1.0f);
        tag(a, nameOf(m_hover), IM_COL32(40, 90, 150, 220), a.y - m_canvas.y < ImGui::GetTextLineHeight() + 6.0f);
    }
    if (!m_hasSelection) return;
    if (boxOf(m_selection, b)) {
        const ImVec2 a = px(b.x, b.y), c = px(b.x + b.w, b.y + b.h);
        // Padding (numbers only; $tokens aren't resolved here): tinted band inside the box
        std::string value;
        const ui::SourceNode* n = ui::findNode(m_source, m_selection);
        if (n && ui::getProperty(*n, "padding", value)) {
            std::vector<f32> p;
            for (const std::string& w : words(value)) {
                f32 v;
                if (!toNumber(w, v)) {
                    p.clear();
                    break;
                }
                p.push_back(v);
            }
            if (p.size() == 1) p = {p[0], p[0], p[0], p[0]};
            else if (p.size() == 2) p = {p[0], p[1], p[0], p[1]};
            if (p.size() == 4) { // top, right, bottom, left
                const ImVec2 ia = px(b.x + p[3], b.y + p[0]), ic = px(b.x + b.w - p[1], b.y + b.h - p[2]);
                const ImU32 pad = IM_COL32(120, 200, 120, 60);
                draw->AddRectFilled(a, ImVec2(c.x, ia.y), pad);
                draw->AddRectFilled(ImVec2(a.x, ic.y), c, pad);
                draw->AddRectFilled(ImVec2(a.x, ia.y), ImVec2(ia.x, ic.y), pad);
                draw->AddRectFilled(ImVec2(ic.x, ia.y), ImVec2(c.x, ic.y), pad);
            }
        }
        draw->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(c.x + 1, c.y + 1), blue, 0.0f, 0, 2.0f);
        // Name and size under the box (inside its bottom when the canvas ends there)
        char size[48];
        std::snprintf(size, sizeof size, "  %.0f x %.0f", static_cast<double>(b.w), static_cast<double>(b.h));
        const bool room = m_canvas.y + m_canvas.height - c.y > ImGui::GetTextLineHeight() + kHandle + 8.0f;
        tag(ImVec2(a.x, room ? c.y + kHandle : c.y - ImGui::GetTextLineHeight() - 8.0f), nameOf(m_selection) + size, IM_COL32(40, 120, 220, 235), true);
        // Handles on free nodes (resize); nodes in a layout show none (they reorder)
        if (!m_selection.empty() && layoutOf(m_selection).empty())
            for (u32 h : {kLeft | kTop, kTop, kRight | kTop, kRight, kRight | kBottom, kBottom, kLeft | kBottom, kLeft}) {
                const f32 hx = (h & kLeft) ? a.x : (h & kRight) ? c.x : (a.x + c.x) * 0.5f;
                const f32 hy = (h & kTop) ? a.y : (h & kBottom) ? c.y : (a.y + c.y) * 0.5f;
                const bool hot = m_hoverHandle == h || (m_drag == Drag::Resize && m_dragHandle == h);
                draw->AddRectFilled(ImVec2(hx - kHandle, hy - kHandle), ImVec2(hx + kHandle, hy + kHandle), hot ? blue : IM_COL32(255, 255, 255, 255));
                draw->AddRect(ImVec2(hx - kHandle, hy - kHandle), ImVec2(hx + kHandle, hy + kHandle), blue);
            }
    }
    // Snap guides across the parent
    ui::NodeBox p;
    if (!m_guides.empty() && boxOf(parentOf(m_selection), p))
        for (const Guide& g : m_guides) {
            const ImVec2 a = g.vertical ? px(g.at, p.y) : px(p.x, g.at), c = g.vertical ? px(g.at, p.y + p.h) : px(p.x + p.w, g.at);
            draw->AddLine(a, c, IM_COL32(255, 80, 200, 220), 1.0f);
        }
    if (m_drag == Drag::Reorder && m_dragArmed && m_insert.valid) {
        const ImVec2 a = px(m_insert.x, m_insert.y), c = px(m_insert.x + m_insert.w, m_insert.y + m_insert.h);
        draw->AddLine(a, c, IM_COL32(255, 200, 60, 255), 3.0f);
    }
}

bool UiPanel::dragForTest(const std::string& node, f32 dx, f32 dy) {
    // Same path as the mouse (press inside, move, release) in logical units from the node's center
    if (!selectNode(node)) return false;
    const ui::ScreenInfo info = m_ui.info(m_screen);
    m_boxes = m_ui.boxes(m_screen);
    ui::NodeBox b;
    if (!boxOf(m_selection, b) || info.scale <= 0.0f) return false;
    const f32 x = info.originX + (b.x + b.w * 0.5f) * info.scale, y = info.originY + (b.y + b.h * 0.5f) * info.scale;
    pointer(x, y, true, true, false, false);
    pointer(x + dx * info.scale, y + dy * info.scale, false, true, true, false); // ctrl: no snapping, so the result is exact
    pointer(x + dx * info.scale, y + dy * info.scale, false, false, false, false);
    return true;
}

} // namespace mealkit::editor
