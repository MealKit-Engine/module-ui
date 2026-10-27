#include "mealkit/ui/Ui.h"

#include "mealkit/core/Log.h"
#include "mealkit/input/Actions.h"
#include "mealkit/loc/Strings.h"
#include "mealkit/platform/Input.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/render/Renderer.h"
#include "mealkit/ui/Atlas.h"
#include "mealkit/ui/Document.h"
#include "mealkit/ui/Text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace mealkit::ui {
namespace {

constexpr f64 kReloadSeconds = 0.25;
constexpr f32 kScrollStep = 60.0f;   // 휠 한 칸 (논리 단위)
constexpr f32 kDegToRad = 0.01745329252f;

struct Rect {
    f32 x = 0, y = 0, w = 0, h = 0;
    bool contains(f32 px, f32 py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

// ---------------------------------------------------------------- 색

u32 lerpColor(u32 a, u32 b, f32 t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    u32 out = 0;
    for (int i = 0; i < 4; ++i) {
        const f32 ca = static_cast<f32>((a >> (i * 8)) & 0xFF), cb = static_cast<f32>((b >> (i * 8)) & 0xFF);
        out |= static_cast<u32>(std::lround(ca + (cb - ca) * t)) << (i * 8);
    }
    return out;
}

u32 withAlpha(u32 c, f32 opacity) {
    const f32 a = static_cast<f32>(c >> 24) * std::clamp(opacity, 0.0f, 1.0f);
    return (c & 0x00FFFFFFu) | (static_cast<u32>(std::lround(a)) << 24);
}

// ---------------------------------------------------------------- 화면

struct RNode {
    u32 src = 0;
    i32 parent = -1;
    u32 end = 0;
    i32 listIndex = -1;
    reflect::View item; // 목록 항목 (바인딩이 먼저 여기서 찾는다)
    Rect rect;          // 논리 단위 (보드 기준)
    bool visible = true;
    bool disabled = false, checked = false;
    f32 mix[static_cast<int>(State::Count)] = {};
    std::string text; // 바인딩을 푼 글자
    bool placeholder = false; // 글자 입력이 비어 자리 표시 글자를 보인다
    Shaped shaped;
    f32 shapedWidth = -2.0f; // 이 너비로 줄을 나눴다
    std::string shapedText;
    f32 scroll = 0.0f, contentExtent = 0.0f;
};

struct Screen {
    ScreenId id = kNoScreen;
    i32 layer = 0;
    bool worldTemplate = false;
    bool fresh = true; // 연 뒤 아직 update를 거치지 않았다: ui.* 액션은 연 프레임의 것이라 받지 않는다 (Esc로 열고 같은 Esc로 닫히지 않게)
    std::string path;
    unsigned long long fileTime = 0;
    Document doc;
    std::vector<std::array<Visual, static_cast<size_t>(State::Count)>> stateVisuals; // 상태가 있는 노드: 상태마다 그리기 값
    std::vector<i32> stateIndex; // doc 노드 → stateVisuals 번호 (-1 없음)
    reflect::View vm;
    std::vector<RNode> nodes;
    std::vector<u32> listCounts; // 목록 개수가 바뀌면 다시 펼친다
    bool rebuild = true;
    std::vector<std::string> bindingErrors;
    i32 focus = -1, hover = -1, pressed = -1;
    i32 editing = -1; // 글자를 치고 있는 글자 입력 노드
    f32 rootW = 1920, rootH = 1080, scale = 1, originX = 0, originY = 0;
};

struct WorldDraw {
    ScreenId screen;
    Vec3 position;
    reflect::View vm;
    bool clamp;
};

bool truthy(const reflect::ValueInfo& v, const void* data) {
    switch (v.kind) {
    case reflect::Kind::Bool: return *static_cast<const bool*>(data);
    case reflect::Kind::Int:
    case reflect::Kind::UInt:
    case reflect::Kind::Ticks:
    case reflect::Kind::Color: {
        u64 value = 0;
        std::memcpy(&value, data, std::min<u32>(v.size, 8));
        return value != 0;
    }
    case reflect::Kind::Float: return *static_cast<const f32*>(data) != 0.0f;
    case reflect::Kind::String: return !static_cast<const std::string*>(data)->empty();
    default: return false;
    }
}

f64 numberOf(const reflect::ValueInfo& v, const void* data) {
    switch (v.kind) {
    case reflect::Kind::Bool: return *static_cast<const bool*>(data) ? 1.0 : 0.0;
    case reflect::Kind::Float: return *static_cast<const f32*>(data);
    case reflect::Kind::Ticks: return static_cast<f64>(*static_cast<const u32*>(data)) / static_cast<f64>(reflect::tickRate());
    case reflect::Kind::Int: {
        i64 value = 0;
        if (v.size == 1) value = *static_cast<const i8*>(data);
        else if (v.size == 2) value = *static_cast<const i16*>(data);
        else if (v.size == 4) value = *static_cast<const i32*>(data);
        else value = *static_cast<const i64*>(data);
        return static_cast<f64>(value);
    }
    case reflect::Kind::UInt: {
        u64 value = 0;
        std::memcpy(&value, data, std::min<u32>(v.size, 8));
        return static_cast<f64>(value);
    }
    default: return 0.0;
    }
}

void writeNumber(const reflect::ValueInfo& v, void* data, f64 value) {
    switch (v.kind) {
    case reflect::Kind::Bool: *static_cast<bool*>(data) = value != 0.0; break;
    case reflect::Kind::Float: *static_cast<f32*>(data) = static_cast<f32>(value); break;
    case reflect::Kind::Int: {
        const i64 x = static_cast<i64>(std::llround(value));
        if (v.size == 1) *static_cast<i8*>(data) = static_cast<i8>(x);
        else if (v.size == 2) *static_cast<i16*>(data) = static_cast<i16>(x);
        else if (v.size == 4) *static_cast<i32*>(data) = static_cast<i32>(x);
        else *static_cast<i64*>(data) = x;
        break;
    }
    case reflect::Kind::UInt: {
        const u64 x = static_cast<u64>(std::max<i64>(0, std::llround(value)));
        if (v.size == 1) *static_cast<u8*>(data) = static_cast<u8>(x);
        else if (v.size == 2) *static_cast<u16*>(data) = static_cast<u16>(x);
        else if (v.size == 4) *static_cast<u32*>(data) = static_cast<u32>(x);
        else *static_cast<u64*>(data) = x;
        break;
    }
    default: break;
    }
}

std::string thousands(i64 v) {
    std::string digits = std::to_string(v < 0 ? -v : v), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return v < 0 ? "-" + out : out;
}

// 서식: 없음, n(천 단위), n1·f1(소수 자리), time(분:초), pct(0..1 → %)
std::string formatValue(const reflect::ValueInfo& v, const void* data, const std::string& spec) {
    if (v.kind == reflect::Kind::String) return *static_cast<const std::string*>(data);
    if (!v.enumNames.empty()) {
        const u64 index = static_cast<u64>(numberOf(v, data));
        return index < v.enumNames.size() ? v.enumNames[index] : std::to_string(index);
    }
    if (v.kind == reflect::Kind::Bool || v.kind == reflect::Kind::Int || v.kind == reflect::Kind::UInt ||
        v.kind == reflect::Kind::Float || v.kind == reflect::Kind::Ticks) {
        const f64 x = numberOf(v, data);
        char buf[64];
        if (spec == "time") {
            const i64 total = static_cast<i64>(std::floor(std::max(0.0, x)));
            std::snprintf(buf, sizeof(buf), "%lld:%02lld", static_cast<long long>(total / 60), static_cast<long long>(total % 60));
            return buf;
        }
        if (spec == "pct") {
            std::snprintf(buf, sizeof(buf), "%.0f%%", x * 100.0);
            return buf;
        }
        int decimals = -1;
        bool group = false;
        if (!spec.empty() && (spec[0] == 'n' || spec[0] == 'f')) {
            group = spec[0] == 'n';
            decimals = spec.size() > 1 ? std::atoi(spec.c_str() + 1) : 0;
        }
        if (decimals < 0) {
            if (v.kind == reflect::Kind::Float || v.kind == reflect::Kind::Ticks) {
                std::snprintf(buf, sizeof(buf), "%g", x);
                return buf;
            }
            return std::to_string(static_cast<long long>(x));
        }
        if (group && decimals == 0) return thousands(static_cast<i64>(std::llround(x)));
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, x);
        if (group) {
            std::string s = buf;
            const size_t dot = s.find('.');
            return thousands(static_cast<i64>(std::trunc(x))) + (dot != std::string::npos ? s.substr(dot) : "");
        }
        return buf;
    }
    return reflect::formatScalar(v, data);
}

void lerpVisual(Visual& a, const Visual& b, f32 t) {
    if (t <= 0.0f) return;
    if (b.fill.kind != PaintKind::None || a.fill.kind != PaintKind::None) {
        if (a.fill.kind == b.fill.kind) {
            a.fill.c0.rgba = lerpColor(a.fill.c0.rgba, b.fill.c0.rgba, t);
            a.fill.c1.rgba = lerpColor(a.fill.c1.rgba, b.fill.c1.rgba, t);
        } else if (t >= 0.5f) {
            a.fill = b.fill;
        }
    }
    a.strokeWidth += (b.strokeWidth - a.strokeWidth) * t;
    a.strokeColor.rgba = lerpColor(a.strokeColor.rgba, b.strokeColor.rgba, t);
    for (int i = 0; i < 4; ++i) a.radii[i] += (b.radii[i] - a.radii[i]) * t;
    a.opacity += (b.opacity - a.opacity) * t;
    a.textColor.rgba = lerpColor(a.textColor.rgba, b.textColor.rgba, t);
    a.tint.rgba = lerpColor(a.tint.rgba, b.tint.rgba, t);
    if (t >= 0.5f) a.shadows = b.shadows;
}

// 테마 토큰을 실제 색으로 (상태 섞기 전에)
void resolveVisual(Visual& v, const Theme& theme) {
    v.fill.c0.rgba = theme.color(v.fill.c0, v.fill.c0.rgba);
    v.fill.c1.rgba = theme.color(v.fill.c1, v.fill.c1.rgba);
    v.strokeColor.rgba = theme.color(v.strokeColor, v.strokeColor.rgba);
    v.textColor.rgba = v.hasTextColor ? theme.color(v.textColor, v.textColor.rgba) : 0xFFFFFFFFu;
    v.tint.rgba = v.hasTint ? theme.color(v.tint, v.tint.rgba) : 0xFFFFFFFFu;
    for (Shadow& s : v.shadows) s.color.rgba = theme.color(s.color, s.color.rgba);
}

} // namespace

struct System::Impl {
    render::Renderer* renderer = nullptr;
    const loc::Strings* strings = nullptr;
    Atlas atlas;
    Theme theme;
    unsigned long long themeTime = 0;
    bool themeLoaded = false;
    std::vector<Screen> screens;
    ScreenId nextId = 1;
    f32 uiScale = 1.0f;
    std::vector<Action> actions;
    bool wantsMouse = false, wantsKeyboard = false;
    bool focusVisible = false, focusAlways = false;
    f64 time = 0.0, nextReload = 0.0;
    f32 viewX = 0, viewY = 0, viewW = 1, viewH = 1;
    Mat4 viewProj;
    std::vector<WorldDraw> world;
    std::vector<render::UiQuad> quads;
    u32 lastQuadCount = 0;

    Screen* find(ScreenId id) {
        for (Screen& s : screens)
            if (s.id == id) return &s;
        return nullptr;
    }

    void ensureTheme() {
        if (themeLoaded) return;
        themeLoaded = true;
        std::vector<std::string> errors;
        loadTheme(theme, &errors);
        for (const std::string& e : errors) MEALKIT_WARN("UI: %s", e.c_str());
        const std::string themePath = findAsset(std::string("assets/") + kThemeFile);
        themeTime = themePath.empty() ? 0 : fileWriteTime(themePath);
    }

    void loadScreen(Screen& s) {
        ensureTheme(); // 숫자 토큰($radius)은 읽을 때 푼다
        loadDocument(s.path, s.doc, &theme);
        const std::string file = findAsset("assets/" + s.path);
        s.fileTime = file.empty() ? 0 : fileWriteTime(file);
        for (const std::string& e : s.doc.errors) MEALKIT_WARN("UI: %s", e.c_str());
        // 상태별 그리기 값을 미리 만든다
        s.stateVisuals.clear();
        s.stateIndex.assign(s.doc.nodes.size(), -1);
        for (size_t i = 0; i < s.doc.nodes.size(); ++i) {
            Node& d = s.doc.nodes[i];
            resolveVisual(d.visual, theme);
            if (d.states.empty()) continue;
            std::array<Visual, static_cast<size_t>(State::Count)> visuals;
            for (int st = 0; st < static_cast<int>(State::Count); ++st) {
                Node copy = d;
                for (const StateOverride& o : d.states)
                    if (static_cast<int>(o.state) == st)
                        for (const std::string& line : o.lines) applyProperty(copy, line, &theme);
                resolveVisual(copy.visual, theme);
                visuals[static_cast<size_t>(st)] = copy.visual;
            }
            s.stateIndex[i] = static_cast<i32>(s.stateVisuals.size());
            s.stateVisuals.push_back(std::move(visuals));
        }
        s.rebuild = true;
        s.focus = s.hover = s.pressed = -1;
    }

    // ---------------------------------------------------------------- 바인딩

    bool resolve(Screen& s, const RNode& n, const std::string& path, const reflect::ValueInfo*& v, void*& data) {
        if (n.item.type && reflect::resolvePath(*n.item.type, const_cast<void*>(n.item.data), path, v, data)) return true;
        if (s.vm.type && reflect::resolvePath(*s.vm.type, const_cast<void*>(s.vm.data), path, v, data)) return true;
        return false;
    }

    void bindingError(Screen& s, const std::string& message) {
        for (const std::string& e : s.bindingErrors)
            if (e == message) return;
        s.bindingErrors.push_back(message);
        MEALKIT_WARN("UI %s: %s", s.path.c_str(), message.c_str());
    }

    bool flag(Screen& s, const RNode& n, const std::string& field) {
        if (field.empty()) return false;
        const bool negate = field[0] == '!';
        const std::string path = negate ? field.substr(1) : field;
        const reflect::ValueInfo* v = nullptr;
        void* data = nullptr;
        if (!resolve(s, n, path, v, data)) {
            if (s.vm.type) bindingError(s, "no field '" + path + "' in " + s.vm.type->name);
            return negate;
        }
        return truthy(*v, data) != negate;
    }

    std::string optionLabel(Screen& s, i32 index) { // 가장 가까운 선택 상자의 지금 선택
        for (i32 a = index; a >= 0; a = s.nodes[static_cast<size_t>(a)].parent) {
            const Node& d = s.doc.nodes[s.nodes[static_cast<size_t>(a)].src];
            if (d.widget != WidgetKind::Select || d.options.empty()) continue;
            const reflect::ValueInfo* v = nullptr;
            void* data = nullptr;
            u32 choice = 0;
            if (resolve(s, s.nodes[static_cast<size_t>(a)], d.field, v, data))
                choice = static_cast<u32>(std::clamp<f64>(numberOf(*v, data), 0.0, static_cast<f64>(d.options.size() - 1)));
            const std::string& key = d.options[choice];
            return strings ? strings->get(key) : key;
        }
        return {};
    }

    std::string resolveText(Screen& s, i32 index) {
        RNode& n = s.nodes[static_cast<size_t>(index)];
        const Node& d = s.doc.nodes[n.src];
        if (d.widget == WidgetKind::TextInput) { // 필드 값 그대로 (비어 있으면 자리 표시 글자)
            n.placeholder = false;
            const reflect::ValueInfo* v = nullptr;
            void* data = nullptr;
            if (resolve(s, n, d.field, v, data) && v->kind == reflect::Kind::String) {
                const std::string& value = *static_cast<const std::string*>(data);
                if (!value.empty() || s.editing == index) return value;
            } else if (s.vm.type) {
                bindingError(s, "text input '" + d.field + "' is not a string field in " + s.vm.type->name);
            }
            n.placeholder = true;
            return d.placeholderKey.empty() ? std::string() : (strings ? strings->get(d.placeholderKey) : d.placeholderKey);
        }
        const std::string& source = !d.textKey.empty() ? (strings ? strings->get(d.textKey) : d.textKey) : d.textContent;
        std::string out;
        for (size_t i = 0; i < source.size(); ++i) {
            if (source[i] == '{') {
                const size_t close = source.find('}', i + 1);
                if (close != std::string::npos) {
                    std::string name = source.substr(i + 1, close - i - 1), spec;
                    const size_t colon = name.find(':');
                    if (colon != std::string::npos) {
                        spec = name.substr(colon + 1);
                        name = name.substr(0, colon);
                    }
                    if (name == "option") {
                        out += optionLabel(s, index);
                        i = close;
                        continue;
                    }
                    const reflect::ValueInfo* v = nullptr;
                    void* data = nullptr;
                    if (resolve(s, n, name, v, data)) {
                        out += formatValue(*v, data, spec);
                        i = close;
                        continue;
                    }
                    if (s.vm.type) bindingError(s, "no field '" + name + "' in " + s.vm.type->name + " (text " + d.name + ")");
                }
            }
            out += source[i];
        }
        return out;
    }

    // ---------------------------------------------------------------- 펼치기 (목록 항목)

    u32 listCount(Screen& s, const RNode& n, const Node& d, const reflect::ValueInfo** element, u8** base, u32* capacity) {
        const reflect::ValueInfo* v = nullptr;
        void* data = nullptr;
        *element = nullptr;
        if (!resolve(s, n, d.field, v, data) || v->kind != reflect::Kind::Array) {
            if (s.vm.type) bindingError(s, "list '" + d.field + "' is not an array (MEALKIT_LIST) in " + s.vm.type->name);
            return 0;
        }
        *element = v->element;
        *base = static_cast<u8*>(data);
        *capacity = v->count;
        u32 count = v->count;
        const reflect::ValueInfo* cv = nullptr;
        void* cdata = nullptr;
        if (resolve(s, n, d.field + "Count", cv, cdata)) count = std::min(count, static_cast<u32>(numberOf(*cv, cdata)));
        return count;
    }

    void buildSubtree(Screen& s, u32 docIndex, i32 parent, reflect::View item, i32 listIndex) {
        const u32 index = static_cast<u32>(s.nodes.size());
        RNode r;
        r.src = docIndex;
        r.parent = parent;
        r.item = item;
        r.listIndex = listIndex;
        s.nodes.push_back(r);
        const Node& d = s.doc.nodes[docIndex];
        if (d.widget == WidgetKind::List) {
            const reflect::ValueInfo* element = nullptr;
            u8* base = nullptr;
            u32 capacity = 0;
            const u32 count = listCount(s, s.nodes[index], d, &element, &base, &capacity);
            s.listCounts.push_back(count);
            const u32 templ = docIndex + 1;
            if (templ < d.end && element && element->kind == reflect::Kind::Struct)
                for (u32 k = 0; k < count; ++k)
                    buildSubtree(s, templ, static_cast<i32>(index), reflect::View{element->type, base + static_cast<size_t>(k) * element->size},
                                 static_cast<i32>(k));
        } else {
            for (u32 c = docIndex + 1; c < d.end; c = s.doc.nodes[c].end) buildSubtree(s, c, static_cast<i32>(index), item, listIndex);
        }
        s.nodes[index].end = static_cast<u32>(s.nodes.size());
    }

    void rebuild(Screen& s) {
        const ScreenId keepFocus = s.focus >= 0 ? s.nodes[static_cast<size_t>(s.focus)].src : 0xFFFFFFFFu;
        const i32 keepList = s.focus >= 0 ? s.nodes[static_cast<size_t>(s.focus)].listIndex : -1;
        const bool keepEditing = s.editing >= 0 && s.editing == s.focus;
        std::vector<RNode> old = std::move(s.nodes);
        s.nodes.clear();
        s.listCounts.clear();
        if (!s.doc.nodes.empty()) buildSubtree(s, 0, -1, {}, -1);
        s.focus = s.hover = s.pressed = -1;
        for (size_t i = 0; i < s.nodes.size(); ++i) { // 포커스·스크롤은 같은 노드로 이어 간다
            if (s.nodes[i].src == keepFocus && s.nodes[i].listIndex == keepList) s.focus = static_cast<i32>(i);
            for (const RNode& o : old)
                if (o.src == s.nodes[i].src && o.listIndex == s.nodes[i].listIndex) {
                    s.nodes[i].scroll = o.scroll;
                    std::memcpy(s.nodes[i].mix, o.mix, sizeof(o.mix));
                    break;
                }
        }
        s.editing = keepEditing ? s.focus : -1;
        s.rebuild = false;
    }

    bool listsChanged(Screen& s) {
        size_t k = 0;
        for (size_t i = 0; i < s.nodes.size(); ++i) {
            const Node& d = s.doc.nodes[s.nodes[i].src];
            if (d.widget != WidgetKind::List) continue;
            const reflect::ValueInfo* element = nullptr;
            u8* base = nullptr;
            u32 capacity = 0;
            const u32 count = listCount(s, s.nodes[i], d, &element, &base, &capacity);
            if (k >= s.listCounts.size() || s.listCounts[k] != count) return true;
            ++k;
        }
        return false;
    }

    // ---------------------------------------------------------------- 배치

    void shapeText(Screen& s, i32 index, f32 wrapWidth, f32 maxHeight) {
        RNode& n = s.nodes[static_cast<size_t>(index)];
        if (n.shapedWidth == wrapWidth && n.shapedText == n.text) return;
        const Node& d = s.doc.nodes[n.src];
        shapeGlyphs(d, n.text, atlas, n.shaped);
        breakLines(d, n.shaped, wrapWidth, maxHeight, atlas);
        n.shapedWidth = wrapWidth;
        n.shapedText = n.text;
    }

    // 내용에 맞춘 크기 (hug). availW: 글자 줄바꿈에 쓸 너비 (<0 없음)
    void intrinsic(Screen& s, i32 index, f32 availW, f32& outW, f32& outH) {
        RNode& n = s.nodes[static_cast<size_t>(index)];
        const Node& d = s.doc.nodes[n.src];
        if (d.kind == NodeKind::Text) {
            const f32 wrap = d.wMode == SizeMode::Hug ? -1.0f : (d.wMode == SizeMode::Fixed ? d.w : availW);
            shapeText(s, index, wrap, -1.0f);
            outW = d.wMode == SizeMode::Fixed ? d.w : n.shaped.width;
            outH = d.hMode == SizeMode::Fixed ? d.h : n.shaped.height;
            return;
        }
        if (d.layout == LayoutKind::Grid) {
            std::vector<i32> flow;
            for (i32 c = index + 1; c < static_cast<i32>(n.end); c = static_cast<i32>(s.nodes[static_cast<size_t>(c)].end))
                if (s.nodes[static_cast<size_t>(c)].visible && !s.doc.nodes[s.nodes[static_cast<size_t>(c)].src].absolute) flow.push_back(c);
            const f32 innerW = d.wMode == SizeMode::Fixed ? d.w - d.padding[1] - d.padding[3] : -1.0f;
            const f32 innerH = d.hMode == SizeMode::Fixed ? d.h - d.padding[0] - d.padding[2] : -1.0f;
            std::vector<f32> cols, rows;
            gridSolve(s, d, gridItems(s, d, flow), innerW, innerH, cols, rows);
            outW = d.wMode == SizeMode::Fixed ? d.w : trackSum(cols, d.columnGap) + d.padding[1] + d.padding[3];
            outH = d.hMode == SizeMode::Fixed ? d.h : trackSum(rows, d.rowGap) + d.padding[0] + d.padding[2];
            return;
        }
        if (d.layout != LayoutKind::Flex) {
            outW = d.w;
            outH = d.h;
            return;
        }
        f32 main = 0.0f, cross = 0.0f;
        u32 count = 0;
        const f32 padMain = d.column ? d.padding[0] + d.padding[2] : d.padding[1] + d.padding[3];
        const f32 padCross = d.column ? d.padding[1] + d.padding[3] : d.padding[0] + d.padding[2];
        const f32 innerW = d.wMode == SizeMode::Fixed ? d.w - d.padding[1] - d.padding[3] : (availW >= 0 ? availW - d.padding[1] - d.padding[3] : -1.0f);
        for (i32 c = index + 1; c < static_cast<i32>(n.end); c = static_cast<i32>(s.nodes[static_cast<size_t>(c)].end)) {
            RNode& cn = s.nodes[static_cast<size_t>(c)];
            const Node& cd = s.doc.nodes[cn.src];
            if (!cn.visible || cd.absolute) continue;
            f32 cw = 0, ch = 0;
            childSize(s, c, d.column ? innerW : -1.0f, cw, ch);
            const f32 along = d.column ? ch + cd.margin[0] + cd.margin[2] : cw + cd.margin[1] + cd.margin[3];
            const f32 across = d.column ? cw + cd.margin[1] + cd.margin[3] : ch + cd.margin[0] + cd.margin[2];
            main += along;
            cross = std::max(cross, across);
            ++count;
        }
        if (count > 1) main += (d.column ? d.rowGap : d.columnGap) * static_cast<f32>(count - 1);
        const f32 w = d.column ? cross + padCross : main + padMain;
        const f32 h = d.column ? main + padMain : cross + padCross;
        outW = d.wMode == SizeMode::Fixed ? d.w : w;
        outH = d.hMode == SizeMode::Fixed ? d.h : h;
    }

    // 자식이 원하는 크기 (fill은 최소 크기로 센다)
    void childSize(Screen& s, i32 index, f32 availW, f32& w, f32& h) {
        const Node& d = s.doc.nodes[s.nodes[static_cast<size_t>(index)].src];
        f32 iw = d.w, ih = d.h;
        if (d.wMode == SizeMode::Hug || d.hMode == SizeMode::Hug || d.kind == NodeKind::Text) intrinsic(s, index, availW, iw, ih);
        w = d.wMode == SizeMode::Fixed ? d.w : d.wMode == SizeMode::Hug ? iw : d.minW;
        h = d.hMode == SizeMode::Fixed ? d.h : d.hMode == SizeMode::Hug ? ih : d.minH;
        w = std::clamp(w, d.minW, d.maxW);
        h = std::clamp(h, d.minH, d.maxH);
    }

    void place(Screen& s, i32 index, Rect r) {
        RNode& n = s.nodes[static_cast<size_t>(index)];
        const Node& d = s.doc.nodes[n.src];
        if (!d.widthRatioField.empty()) { // 너비 비율 바인딩 (체력 막대)
            const reflect::ValueInfo* v = nullptr;
            void* data = nullptr;
            if (resolve(s, n, d.widthRatioField, v, data)) r.w *= static_cast<f32>(std::clamp(numberOf(*v, data), 0.0, 1.0));
            else if (s.vm.type) bindingError(s, "no field '" + d.widthRatioField + "' in " + s.vm.type->name);
        }
        n.rect = r;
        if (d.kind == NodeKind::Text) {
            const f32 wrap = d.wMode == SizeMode::Hug ? -1.0f : r.w;
            shapeText(s, index, wrap, d.hMode == SizeMode::Fixed || d.hMode == SizeMode::Fill ? r.h : -1.0f);
        }
        layoutChildren(s, index);
    }

    void placeConstrained(Screen& s, i32 c, const Rect& parentRect, f32 designW, f32 designH, f32 scrollX, f32 scrollY) {
        const Node& cd = s.doc.nodes[s.nodes[static_cast<size_t>(c)].src];
        f32 w = 0, h = 0;
        childSize(s, c, cd.wMode == SizeMode::Fill ? parentRect.w - cd.x : -1.0f, w, h);
        if (cd.wMode == SizeMode::Fill) w = std::clamp(parentRect.w - cd.x, cd.minW, cd.maxW);
        if (cd.hMode == SizeMode::Fill) h = std::clamp(parentRect.h - cd.y, cd.minH, cd.maxH);
        auto axis = [](Constraint k, f32 pos, f32 size, f32 design, f32 actual, f32& outPos, f32& outSize) {
            outSize = size;
            switch (k) {
            case Constraint::Start: outPos = pos; break;
            case Constraint::End: outPos = actual - (design - pos - size) - size; break;
            case Constraint::Both:
                outPos = pos;
                outSize = std::max(0.0f, actual - pos - (design - pos - size));
                break;
            case Constraint::Center: outPos = actual * 0.5f + (pos + size * 0.5f - design * 0.5f) - size * 0.5f; break;
            case Constraint::Scale: {
                const f32 k2 = design > 0 ? actual / design : 1.0f;
                outPos = pos * k2;
                outSize = size * k2;
                break;
            }
            }
        };
        f32 x = 0, y = 0, outW = w, outH = h;
        axis(cd.ch, cd.x, w, designW, parentRect.w, x, outW);
        axis(cd.cv, cd.y, h, designH, parentRect.h, y, outH);
        place(s, c, {parentRect.x + x - scrollX, parentRect.y + y - scrollY, outW, outH});
    }

    void layoutChildren(Screen& s, i32 index) {
        RNode& n = s.nodes[static_cast<size_t>(index)];
        const Node& d = s.doc.nodes[n.src];
        const bool scroll = d.widget == WidgetKind::Scroll;
        const f32 sx = scroll && d.scrollHorizontal ? n.scroll : 0.0f, sy = scroll && !d.scrollHorizontal ? n.scroll : 0.0f;
        const Rect r = n.rect;
        const f32 designW = d.w > 0 ? d.w : r.w, designH = d.h > 0 ? d.h : r.h;
        std::vector<i32> flow;
        for (i32 c = index + 1; c < static_cast<i32>(n.end); c = static_cast<i32>(s.nodes[static_cast<size_t>(c)].end)) {
            RNode& cn = s.nodes[static_cast<size_t>(c)];
            const Node& cd = s.doc.nodes[cn.src];
            cn.visible = !cd.hidden && (cd.visibleField.empty() || flag(s, cn, cd.visibleField));
            if (!cn.visible) continue;
            if (d.layout == LayoutKind::None || cd.absolute) placeConstrained(s, c, r, designW, designH, sx, sy);
            else flow.push_back(c);
        }
        if (d.layout == LayoutKind::Flex && !flow.empty()) flex(s, index, flow, sx, sy);
        if (d.layout == LayoutKind::Grid && !flow.empty()) grid(s, index, flow, sx, sy);
        // 스크롤 영역: 내용 끝
        if (scroll) {
            f32 extent = 0.0f;
            for (i32 c = index + 1; c < static_cast<i32>(n.end); c = static_cast<i32>(s.nodes[static_cast<size_t>(c)].end)) {
                const RNode& cn = s.nodes[static_cast<size_t>(c)];
                if (!cn.visible) continue;
                extent = std::max(extent, d.scrollHorizontal ? cn.rect.x + cn.rect.w + sx - r.x : cn.rect.y + cn.rect.h + sy - r.y);
            }
            s.nodes[static_cast<size_t>(index)].contentExtent = extent + (d.scrollHorizontal ? d.padding[1] : d.padding[2]);
        }
    }

    void flex(Screen& s, i32 index, const std::vector<i32>& flow, f32 sx, f32 sy) {
        const RNode& n = s.nodes[static_cast<size_t>(index)];
        const Node& d = s.doc.nodes[n.src];
        const bool col = d.column;
        const Rect r = n.rect;
        const f32 innerX = r.x + d.padding[3] - sx, innerY = r.y + d.padding[0] - sy;
        const f32 innerW = std::max(0.0f, r.w - d.padding[1] - d.padding[3]);
        const f32 innerH = std::max(0.0f, r.h - d.padding[0] - d.padding[2]);
        const f32 mainSize = col ? innerH : innerW, crossSize = col ? innerW : innerH;
        const f32 mainGap = col ? d.rowGap : d.columnGap, crossGap = col ? d.columnGap : d.rowGap;

        struct Item {
            i32 node;
            f32 main, cross;        // 크기 (여백 제외)
            f32 m0, m1, c0, c1;     // 여백: 앞·뒤 (주축), 앞·뒤 (교차축)
            bool grow, stretch;
            f32 minMain, maxMain;
        };
        std::vector<Item> items;
        for (i32 c : flow) {
            const Node& cd = s.doc.nodes[s.nodes[static_cast<size_t>(c)].src];
            f32 w = 0, h = 0;
            childSize(s, c, col ? innerW - cd.margin[1] - cd.margin[3] : -1.0f, w, h);
            Item it;
            it.node = c;
            it.main = col ? h : w;
            it.cross = col ? w : h;
            it.m0 = col ? cd.margin[0] : cd.margin[3];
            it.m1 = col ? cd.margin[2] : cd.margin[1];
            it.c0 = col ? cd.margin[3] : cd.margin[0];
            it.c1 = col ? cd.margin[1] : cd.margin[2];
            const SizeMode mainMode = col ? cd.hMode : cd.wMode, crossMode = col ? cd.wMode : cd.hMode;
            it.grow = mainMode == SizeMode::Fill;
            const Align self = cd.alignSelf != Align::Auto ? cd.alignSelf : d.alignItems;
            it.stretch = crossMode == SizeMode::Fill || (self == Align::Stretch && crossMode != SizeMode::Fixed);
            it.minMain = col ? cd.minH : cd.minW;
            it.maxMain = col ? cd.maxH : cd.maxW;
            items.push_back(it);
        }
        if (d.reverse) std::reverse(items.begin(), items.end());

        // 줄 나누기 (wrap)
        struct Line {
            size_t begin, end;
            f32 cross = 0;
        };
        std::vector<Line> lines;
        {
            size_t begin = 0;
            f32 used = 0.0f;
            for (size_t i = 0; i < items.size(); ++i) {
                const f32 size = items[i].main + items[i].m0 + items[i].m1;
                if (d.wrap && i > begin && used + mainGap + size > mainSize + 0.01f) {
                    lines.push_back({begin, i});
                    begin = i;
                    used = 0.0f;
                }
                used += (i > begin ? mainGap : 0.0f) + size;
            }
            lines.push_back({begin, items.size()});
        }
        // 채우기 자식에 남는 자리 나누기, 줄 높이
        for (Line& line : lines) {
            f32 used = 0.0f;
            u32 grow = 0;
            for (size_t i = line.begin; i < line.end; ++i) {
                used += items[i].main + items[i].m0 + items[i].m1;
                if (items[i].grow) ++grow;
            }
            used += mainGap * static_cast<f32>(line.end - line.begin - 1);
            const f32 free = mainSize - used;
            if (grow > 0 && free > 0.0f)
                for (size_t i = line.begin; i < line.end; ++i)
                    if (items[i].grow)
                        items[i].main = std::clamp(items[i].main + free / static_cast<f32>(grow), items[i].minMain, items[i].maxMain);
            for (size_t i = line.begin; i < line.end; ++i) line.cross = std::max(line.cross, items[i].cross + items[i].c0 + items[i].c1);
        }
        if (lines.size() == 1 && !d.wrap) lines[0].cross = crossSize; // 한 줄이면 교차축 전체
        // 여러 줄 배분
        f32 linesCross = 0.0f;
        for (const Line& l : lines) linesCross += l.cross;
        linesCross += crossGap * static_cast<f32>(lines.size() - 1);
        f32 crossPos = 0.0f, crossBetween = crossGap;
        if (lines.size() > 1) {
            const f32 freeCross = crossSize - linesCross;
            if (d.alignContent == Align::Stretch && freeCross > 0) {
                for (Line& l : lines) l.cross += freeCross / static_cast<f32>(lines.size());
            } else if (d.alignContent == Align::Center) {
                crossPos = freeCross * 0.5f;
            } else if (d.alignContent == Align::End) {
                crossPos = freeCross;
            } else if (d.alignContentJustify == Justify::SpaceBetween && lines.size() > 1) {
                crossBetween += std::max(0.0f, freeCross) / static_cast<f32>(lines.size() - 1);
            } else if (d.alignContentJustify == Justify::SpaceAround) {
                crossBetween += std::max(0.0f, freeCross) / static_cast<f32>(lines.size());
                crossPos = std::max(0.0f, freeCross) / static_cast<f32>(lines.size()) * 0.5f;
            } else if (d.alignContentJustify == Justify::SpaceEvenly) {
                crossBetween += std::max(0.0f, freeCross) / static_cast<f32>(lines.size() + 1);
                crossPos = std::max(0.0f, freeCross) / static_cast<f32>(lines.size() + 1);
            }
        }
        for (const Line& line : lines) {
            f32 used = 0.0f;
            for (size_t i = line.begin; i < line.end; ++i) used += items[i].main + items[i].m0 + items[i].m1;
            const size_t count = line.end - line.begin;
            f32 free = mainSize - used - mainGap * static_cast<f32>(count - 1);
            f32 pos = 0.0f, between = mainGap;
            switch (d.justify) {
            case Justify::Start: break;
            case Justify::Center: pos = free * 0.5f; break;
            case Justify::End: pos = free; break;
            case Justify::SpaceBetween:
                if (count > 1 && free > 0) between += free / static_cast<f32>(count - 1);
                break;
            case Justify::SpaceAround:
                if (free > 0) {
                    between += free / static_cast<f32>(count);
                    pos = free / static_cast<f32>(count) * 0.5f;
                }
                break;
            case Justify::SpaceEvenly:
                if (free > 0) {
                    between += free / static_cast<f32>(count + 1);
                    pos = free / static_cast<f32>(count + 1);
                }
                break;
            }
            for (size_t i = line.begin; i < line.end; ++i) {
                Item& it = items[i];
                const Node& cd = s.doc.nodes[s.nodes[static_cast<size_t>(it.node)].src];
                const Align self = cd.alignSelf != Align::Auto ? cd.alignSelf : d.alignItems;
                const f32 lineCross = line.cross - it.c0 - it.c1;
                const f32 cross = it.stretch ? lineCross : it.cross;
                f32 offset = 0.0f;
                if (!it.stretch) {
                    if (self == Align::Center) offset = (lineCross - cross) * 0.5f;
                    else if (self == Align::End) offset = lineCross - cross;
                }
                pos += it.m0;
                const f32 mx = pos, cx = crossPos + it.c0 + offset;
                Rect cr = col ? Rect{innerX + cx, innerY + mx, cross, it.main} : Rect{innerX + mx, innerY + cx, it.main, cross};
                place(s, it.node, cr);
                pos += it.main + it.m1 + between;
            }
            crossPos += line.cross + crossBetween;
        }
    }

    // ---------------------------------------------------------------- Grid

    struct GridItem {
        i32 node;
        u32 row, col, rowSpan, colSpan; // 0부터
    };

    // 자식마다 칸을 정한다: cell이 있으면 그 칸, 없으면 빈 칸을 행(또는 열) 순서로. 트랙 밖이면 마지막 트랙으로 자른다
    std::vector<GridItem> gridItems(Screen& s, const Node& d, const std::vector<i32>& flow) {
        const u32 rows = static_cast<u32>(std::max<size_t>(1, d.rows.size())), cols = static_cast<u32>(std::max<size_t>(1, d.columns.size()));
        std::vector<u8> used(static_cast<size_t>(rows) * cols, 0);
        auto mark = [&](const GridItem& it) {
            for (u32 r = it.row; r < it.row + it.rowSpan; ++r)
                for (u32 c = it.col; c < it.col + it.colSpan; ++c) used[static_cast<size_t>(r) * cols + c] = 1;
        };
        std::vector<GridItem> items;
        for (i32 c : flow) { // 정한 칸 먼저
            const Node& cd = s.doc.nodes[s.nodes[static_cast<size_t>(c)].src];
            GridItem it{c, 0, 0, 1, 1};
            if (cd.cellRow > 0 && cd.cellColumn > 0) {
                it.row = std::min<u32>(cd.cellRow - 1u, rows - 1);
                it.col = std::min<u32>(cd.cellColumn - 1u, cols - 1);
                it.rowSpan = std::clamp<u32>(cd.rowSpan, 1, rows - it.row);
                it.colSpan = std::clamp<u32>(cd.columnSpan, 1, cols - it.col);
                mark(it);
            } else {
                it.row = it.col = ~0u;
            }
            items.push_back(it);
        }
        u32 cursor = 0;
        for (GridItem& it : items) {
            if (it.row != ~0u) continue;
            while (cursor < rows * cols) {
                const u32 r = d.gridColumnFlow ? cursor % rows : cursor / cols;
                const u32 c = d.gridColumnFlow ? cursor / rows : cursor % cols;
                ++cursor;
                if (!used[static_cast<size_t>(r) * cols + c]) {
                    it.row = r;
                    it.col = c;
                    break;
                }
            }
            if (it.row == ~0u) it.row = rows - 1, it.col = cols - 1; // 칸이 모자라면 마지막 칸에 겹친다
            mark(it);
        }
        return items;
    }

    // 트랙 크기: 고정·%는 그대로, auto는 한 칸짜리 자식의 가장 큰 크기, fr은 남는 자리를 비율로(최소는 내용).
    // inner < 0이면 내용에 맞춘 크기(%와 fr은 auto처럼). fr이 없고 stretch면 남는 자리를 auto 트랙이 나눈다
    static void gridTracks(const std::vector<Track>& tracks, f32 inner, f32 gap, const std::vector<f32>& content, bool stretch,
                           std::vector<f32>& out) {
        const size_t n = std::max<size_t>(1, tracks.size());
        out.assign(n, 0.0f);
        f32 used = gap * static_cast<f32>(n - 1), fr = 0.0f;
        u32 autos = 0;
        for (size_t i = 0; i < n; ++i) {
            const Track t = i < tracks.size() ? tracks[i] : Track{};
            const f32 c = i < content.size() ? content[i] : 0.0f;
            switch (t.kind) {
            case TrackKind::Fixed: out[i] = t.value; break;
            case TrackKind::Percent: out[i] = inner >= 0.0f ? inner * t.value * 0.01f : c; break;
            case TrackKind::Auto: out[i] = c, ++autos; break;
            case TrackKind::Flex: out[i] = c, fr += t.value; break;
            }
            if (t.kind != TrackKind::Flex) used += out[i];
        }
        if (inner < 0.0f) return;
        if (fr > 0.0f) {
            // 내용보다 작아지는 fr 트랙은 내용 크기로 고정하고 나머지로 다시 나눈다
            std::vector<u8> fixed(n, 0);
            for (int pass = 0; pass < 4; ++pass) {
                f32 left = inner - used, frLeft = 0.0f;
                for (size_t i = 0; i < n; ++i)
                    if (i < tracks.size() && tracks[i].kind == TrackKind::Flex) {
                        if (fixed[i]) left -= out[i];
                        else frLeft += tracks[i].value;
                    }
                bool again = false;
                for (size_t i = 0; i < n; ++i) {
                    if (i >= tracks.size() || tracks[i].kind != TrackKind::Flex || fixed[i]) continue;
                    const f32 c = i < content.size() ? content[i] : 0.0f;
                    const f32 share = frLeft > 0.0f ? std::max(0.0f, left) * tracks[i].value / frLeft : 0.0f;
                    if (share < c) fixed[i] = 1, out[i] = c, again = true;
                    else out[i] = share;
                }
                if (!again) break;
            }
        } else if (stretch && autos > 0 && inner > used) {
            const f32 extra = (inner - used) / static_cast<f32>(autos);
            for (size_t i = 0; i < n; ++i)
                if (i >= tracks.size() || tracks[i].kind == TrackKind::Auto) out[i] += extra;
        }
    }

    // 열을 먼저 정하고(글자 줄바꿈 없이), 그 너비로 행 높이를 정한다. innerW·innerH < 0이면 내용에 맞춤
    void gridSolve(Screen& s, const Node& d, const std::vector<GridItem>& items, f32 innerW, f32 innerH,
                   std::vector<f32>& colSizes, std::vector<f32>& rowSizes) {
        std::vector<f32> colContent(std::max<size_t>(1, d.columns.size()), 0.0f), rowContent(std::max<size_t>(1, d.rows.size()), 0.0f);
        for (const GridItem& it : items) {
            if (it.colSpan != 1) continue;
            const Node& cd = s.doc.nodes[s.nodes[static_cast<size_t>(it.node)].src];
            f32 w = 0, h = 0;
            childSize(s, it.node, -1.0f, w, h);
            colContent[it.col] = std::max(colContent[it.col], w + cd.margin[1] + cd.margin[3]);
        }
        gridTracks(d.columns, innerW, d.columnGap, colContent, d.justifyStretch, colSizes);
        for (const GridItem& it : items) {
            if (it.rowSpan != 1) continue;
            const Node& cd = s.doc.nodes[s.nodes[static_cast<size_t>(it.node)].src];
            f32 cellW = d.columnGap * static_cast<f32>(it.colSpan - 1);
            for (u32 c = it.col; c < it.col + it.colSpan; ++c) cellW += colSizes[c];
            f32 w = 0, h = 0;
            childSize(s, it.node, cellW - cd.margin[1] - cd.margin[3], w, h);
            rowContent[it.row] = std::max(rowContent[it.row], h + cd.margin[0] + cd.margin[2]);
        }
        gridTracks(d.rows, innerH, d.rowGap, rowContent, d.alignContent == Align::Stretch, rowSizes);
    }

    static f32 trackSum(const std::vector<f32>& sizes, f32 gap) {
        f32 sum = gap * static_cast<f32>(sizes.size() - 1);
        for (f32 v : sizes) sum += v;
        return sum;
    }

    // 남는 자리 배분 (justify: 열, alignContent: 행) → 트랙 시작 위치
    static void trackStarts(const std::vector<f32>& sizes, f32 gap, f32 inner, Justify j, Align a, std::vector<f32>& starts) {
        const size_t n = sizes.size();
        const f32 free = std::max(0.0f, inner - trackSum(sizes, gap));
        f32 pos = 0.0f, between = gap;
        if (a == Align::Center) pos = free * 0.5f;
        else if (a == Align::End) pos = free;
        else if (j == Justify::Center) pos = free * 0.5f;
        else if (j == Justify::End) pos = free;
        else if (j == Justify::SpaceBetween && n > 1) between += free / static_cast<f32>(n - 1);
        else if (j == Justify::SpaceAround) between += free / static_cast<f32>(n), pos = free / static_cast<f32>(n) * 0.5f;
        else if (j == Justify::SpaceEvenly) between += free / static_cast<f32>(n + 1), pos = free / static_cast<f32>(n + 1);
        starts.resize(n);
        for (size_t i = 0; i < n; ++i) {
            starts[i] = pos;
            pos += sizes[i] + between;
        }
    }

    void grid(Screen& s, i32 index, const std::vector<i32>& flow, f32 sx, f32 sy) {
        const RNode& n = s.nodes[static_cast<size_t>(index)];
        const Node& d = s.doc.nodes[n.src];
        const Rect r = n.rect;
        const f32 innerX = r.x + d.padding[3] - sx, innerY = r.y + d.padding[0] - sy;
        const f32 innerW = std::max(0.0f, r.w - d.padding[1] - d.padding[3]);
        const f32 innerH = std::max(0.0f, r.h - d.padding[0] - d.padding[2]);
        const std::vector<GridItem> items = gridItems(s, d, flow);
        std::vector<f32> cols, rows, colStart, rowStart;
        gridSolve(s, d, items, innerW, innerH, cols, rows);
        // 열 배분은 justify, 행 배분은 alignContent (Align이 start·center·end면 그것, 아니면 space-*)
        trackStarts(cols, d.columnGap, innerW, d.justify, Align::Start, colStart);
        trackStarts(rows, d.rowGap, innerH, d.alignContentJustify, d.alignContent, rowStart);
        for (const GridItem& it : items) {
            const Node& cd = s.doc.nodes[s.nodes[static_cast<size_t>(it.node)].src];
            const u32 c1 = it.col + it.colSpan - 1, r1 = it.row + it.rowSpan - 1;
            const f32 cellX = colStart[it.col] + cd.margin[3], cellY = rowStart[it.row] + cd.margin[0];
            const f32 cellW = std::max(0.0f, colStart[c1] + cols[c1] - colStart[it.col] - cd.margin[1] - cd.margin[3]);
            const f32 cellH = std::max(0.0f, rowStart[r1] + rows[r1] - rowStart[it.row] - cd.margin[0] - cd.margin[2]);
            f32 w = 0, h = 0;
            childSize(s, it.node, cellW, w, h);
            const Align hAlign = cd.justifySelf != Align::Auto ? cd.justifySelf : d.justifyItems;
            const Align vAlign = cd.alignSelf != Align::Auto ? cd.alignSelf : d.alignItems;
            if (cd.wMode == SizeMode::Fill || (hAlign == Align::Stretch && cd.wMode != SizeMode::Fixed)) w = std::clamp(cellW, cd.minW, cd.maxW);
            if (cd.hMode == SizeMode::Fill || (vAlign == Align::Stretch && cd.hMode != SizeMode::Fixed)) h = std::clamp(cellH, cd.minH, cd.maxH);
            const f32 ox = hAlign == Align::Center ? (cellW - w) * 0.5f : hAlign == Align::End ? cellW - w : 0.0f;
            const f32 oy = vAlign == Align::Center ? (cellH - h) * 0.5f : vAlign == Align::End ? cellH - h : 0.0f;
            place(s, it.node, {innerX + cellX + ox, innerY + cellY + oy, w, h});
        }
    }

    void layout(Screen& s, f32 rootW, f32 rootH) {
        if (s.nodes.empty()) return;
        // 글자 바인딩을 먼저 푼다 (바뀐 글자만 다시 나눈다)
        for (size_t i = 0; i < s.nodes.size(); ++i) {
            RNode& n = s.nodes[i];
            const Node& d = s.doc.nodes[n.src];
            if (d.kind == NodeKind::Text) n.text = resolveText(s, static_cast<i32>(i));
            n.disabled = !d.disabledField.empty() && flag(s, n, d.disabledField);
            n.checked = false;
            if (d.widget == WidgetKind::Toggle) {
                const reflect::ValueInfo* v = nullptr;
                void* data = nullptr;
                if (resolve(s, n, d.field, v, data)) n.checked = truthy(*v, data);
                else if (s.vm.type) bindingError(s, "no field '" + d.field + "' in " + s.vm.type->name);
            }
        }
        RNode& root = s.nodes[0];
        root.visible = !s.doc.nodes[0].hidden;
        place(s, 0, {0.0f, 0.0f, rootW, rootH});
        // 부모가 숨으면 자손도 숨는다
        for (size_t i = 1; i < s.nodes.size(); ++i)
            if (!s.nodes[static_cast<size_t>(s.nodes[i].parent)].visible) s.nodes[i].visible = false;
    }

    // ---------------------------------------------------------------- 그리기

    void emitText(Screen& s, const RNode& n, const Node& d, const Visual& v, f32 opacity, const f32 clip[4], bool caret) {
        const Shaped& sh = n.shaped;
        const f32 k = s.scale;
        if (n.placeholder) opacity *= 0.45f;
        f32 y = n.rect.y;
        f32 caretX = d.textAlign == TextAlign::Center ? n.rect.x + n.rect.w * 0.5f
                     : d.textAlign == TextAlign::Right ? n.rect.x + n.rect.w
                                                        : n.rect.x;
        f32 caretY = y, caretH = d.text.size * d.lineHeight;
        if (d.hMode != SizeMode::Hug) {
            if (d.verticalAlign == VerticalAlign::Center) y += (n.rect.h - sh.height) * 0.5f;
            else if (d.verticalAlign == VerticalAlign::Bottom) y += n.rect.h - sh.height;
        }
        const f32 sdfUnits = atlas.sdfUnitsPerPixel();
        for (const ShapedLine& line : sh.lines) {
            f32 x = n.rect.x;
            if (d.textAlign == TextAlign::Center) x += (n.rect.w - line.width) * 0.5f;
            else if (d.textAlign == TextAlign::Right) x += n.rect.w - line.width;
            const f32 baseline = y + (line.height - (line.ascent + line.descent)) * 0.5f + line.ascent;
            caretX = x + line.width;
            caretY = y;
            caretH = line.height;
            for (u32 i = line.begin; i < line.end; ++i) {
                const ShapedGlyph& g = sh.glyphs[i];
                const TextRun& run = sh.runs[g.run];
                u32 color = v.textColor.rgba;
                if (run.span >= 0) color = theme.color(d.spans[static_cast<size_t>(run.span)].color, color);
                color = withAlpha(color, opacity);
                if (g.glyph.u1 > g.glyph.u0) {
                    render::UiQuad q;
                    q.rect[0] = s.originX + (x + g.glyph.x0 * run.size) * k;
                    q.rect[1] = s.originY + (baseline + g.glyph.y0 * run.size) * k;
                    q.rect[2] = (g.glyph.x1 - g.glyph.x0) * run.size * k;
                    q.rect[3] = (g.glyph.y1 - g.glyph.y0) * run.size * k;
                    std::memcpy(q.clip, clip, sizeof(q.clip));
                    q.uv[0] = g.glyph.u0;
                    q.uv[1] = g.glyph.v0;
                    q.uv[2] = g.glyph.u1;
                    q.uv[3] = g.glyph.v1;
                    q.color0 = color;
                    q.flags = render::kUiGlyph;
                    q.sdfScale = sdfUnits * run.size * k / kBakePixels;
                    quads.push_back(q);
                }
                if ((run.underline || run.strike) && g.cp != '\n') {
                    const FontMetrics& fm = atlas.metrics(g.font);
                    const f32 thick = std::max(1.0f / k, run.size * 0.06f);
                    auto deco = [&](f32 dy) {
                        render::UiQuad q;
                        q.rect[0] = s.originX + x * k;
                        q.rect[1] = s.originY + (baseline + dy) * k;
                        q.rect[2] = g.advance * k;
                        q.rect[3] = thick * k;
                        std::memcpy(q.clip, clip, sizeof(q.clip));
                        q.color0 = color;
                        q.flags = render::kUiShape;
                        quads.push_back(q);
                    };
                    if (run.underline) deco(run.size * 0.12f);
                    if (run.strike) deco(-fm.ascent * run.size * 0.32f);
                }
                x += g.advance;
            }
            y += line.height;
        }
        if (caret && !n.placeholder) { // 글자 입력 중: 끝에 깜박이는 세로 막대
            render::UiQuad q;
            q.rect[0] = s.originX + caretX * k;
            q.rect[1] = s.originY + (caretY + caretH * 0.1f) * k;
            q.rect[2] = std::max(1.0f, 2.0f * k);
            q.rect[3] = caretH * 0.8f * k;
            std::memcpy(q.clip, clip, sizeof(q.clip));
            q.color0 = withAlpha(v.textColor.rgba, opacity);
            q.flags = render::kUiShape;
            quads.push_back(q);
        }
    }

    void emitNode(Screen& s, i32 index, f32 opacity, const f32 clip[4]) {
        const RNode& n = s.nodes[static_cast<size_t>(index)];
        const Node& d = s.doc.nodes[n.src];
        Visual v = d.visual;
        const i32 si = s.stateIndex[n.src];
        if (si >= 0)
            for (int st = 1; st < static_cast<int>(State::Count); ++st)
                lerpVisual(v, s.stateVisuals[static_cast<size_t>(si)][static_cast<size_t>(st)], n.mix[st]);
        if (!d.colorField.empty()) { // 색 바인딩 (Color 또는 u32 rgba)
            const reflect::ValueInfo* cv = nullptr;
            void* data = nullptr;
            if (resolve(s, n, d.colorField, cv, data) && cv->size == 4) {
                u32 c = 0;
                std::memcpy(&c, data, 4);
                if (d.kind == NodeKind::Text) v.textColor.rgba = c;
                else if (d.kind == NodeKind::Image) v.tint.rgba = c;
                else v.fill = {PaintKind::Solid, {c, {}}, {}, {0, 0, 0, 1}};
            }
        }
        opacity *= v.opacity;
        const f32 k = s.scale;
        const Rect r{s.originX + n.rect.x * k, s.originY + n.rect.y * k, n.rect.w * k, n.rect.h * k};
        const f32 rotation = d.rotation * kDegToRad;
        auto base = [&](render::UiQuad& q) {
            std::memcpy(q.clip, clip, sizeof(q.clip));
            q.rotation = rotation;
        };
        if (d.kind != NodeKind::Text) {
            for (const Shadow& sh : v.shadows) {
                render::UiQuad q;
                base(q);
                q.rect[0] = r.x + (sh.dx - sh.spread) * k;
                q.rect[1] = r.y + (sh.dy - sh.spread) * k;
                q.rect[2] = r.w + sh.spread * 2.0f * k;
                q.rect[3] = r.h + sh.spread * 2.0f * k;
                for (int i = 0; i < 4; ++i) q.radii[i] = std::max(0.0f, v.radii[i] + sh.spread) * k;
                q.color0 = withAlpha(sh.color.rgba, opacity);
                q.flags = render::kUiShadow;
                q.softness = sh.blur * 0.5f * k;
                quads.push_back(q);
            }
        }
        if (d.kind == NodeKind::Frame && (v.fill.kind != PaintKind::None || v.strokeWidth > 0.0f)) {
            render::UiQuad q;
            base(q);
            q.rect[0] = r.x;
            q.rect[1] = r.y;
            q.rect[2] = r.w;
            q.rect[3] = r.h;
            for (int i = 0; i < 4; ++i) q.radii[i] = v.radii[i] * k;
            q.color0 = v.fill.kind == PaintKind::None ? 0u : withAlpha(v.fill.c0.rgba, opacity);
            q.color1 = withAlpha(v.fill.c1.rgba, opacity);
            std::memcpy(q.gradient, v.fill.p, sizeof(q.gradient));
            q.flags = render::kUiShape | (v.fill.kind == PaintKind::Linear ? render::kUiLinear : v.fill.kind == PaintKind::Radial ? render::kUiRadial : 0u);
            q.strokeWidth = v.strokeWidth * k;
            q.strokeColor = withAlpha(v.strokeColor.rgba, opacity);
            quads.push_back(q);
        }
        if (d.kind == NodeKind::Image && !d.image.empty()) {
            const ImageInfo img = atlas.image(d.image);
            if (!img.valid) bindingError(s, "image assets/" + d.image + " not found");
            if (img.valid) {
                const u32 tint = withAlpha(v.tint.rgba, opacity);
                const bool sliced = d.slice[0] > 0 || d.slice[1] > 0 || d.slice[2] > 0 || d.slice[3] > 0;
                auto piece = [&](f32 x0, f32 y0, f32 x1, f32 y1, f32 u0, f32 v0, f32 u1, f32 v1) {
                    if (x1 <= x0 || y1 <= y0) return;
                    render::UiQuad q;
                    base(q);
                    q.rect[0] = x0;
                    q.rect[1] = y0;
                    q.rect[2] = x1 - x0;
                    q.rect[3] = y1 - y0;
                    q.uv[0] = u0;
                    q.uv[1] = v0;
                    q.uv[2] = u1;
                    q.uv[3] = v1;
                    q.color0 = tint;
                    q.flags = render::kUiImage;
                    if (!sliced)
                        for (int i = 0; i < 4; ++i) q.radii[i] = v.radii[i] * k;
                    quads.push_back(q);
                };
                if (!sliced) {
                    piece(r.x, r.y, r.x + r.w, r.y + r.h, img.u0, img.v0, img.u1, img.v1);
                } else { // 9-slice: 여백은 원본 픽셀 = 논리 단위
                    const f32 du = (img.u1 - img.u0) / static_cast<f32>(img.width), dv = (img.v1 - img.v0) / static_cast<f32>(img.height);
                    const f32 xs[4] = {r.x, r.x + d.slice[0] * k, r.x + r.w - d.slice[2] * k, r.x + r.w};
                    const f32 ys[4] = {r.y, r.y + d.slice[1] * k, r.y + r.h - d.slice[3] * k, r.y + r.h};
                    const f32 us[4] = {img.u0, img.u0 + d.slice[0] * du, img.u1 - d.slice[2] * du, img.u1};
                    const f32 vs[4] = {img.v0, img.v0 + d.slice[1] * dv, img.v1 - d.slice[3] * dv, img.v1};
                    for (int yi = 0; yi < 3; ++yi)
                        for (int xi = 0; xi < 3; ++xi) piece(xs[xi], ys[yi], xs[xi + 1], ys[yi + 1], us[xi], vs[yi], us[xi + 1], vs[yi + 1]);
                }
            }
        }
        if (d.kind == NodeKind::Text) emitText(s, n, d, v, opacity, clip, index == s.editing && std::fmod(time, 1.0) < 0.6);
        // 포커스 표시 (게임패드·키보드로 옮길 때)
        if (index == s.focus && (focusVisible || focusAlways) && d.widget != WidgetKind::None && d.widget != WidgetKind::List) {
            render::UiQuad q;
            base(q);
            const f32 pad = 3.0f * k;
            q.rect[0] = r.x - pad;
            q.rect[1] = r.y - pad;
            q.rect[2] = r.w + pad * 2;
            q.rect[3] = r.h + pad * 2;
            for (int i = 0; i < 4; ++i) q.radii[i] = v.radii[i] * k + pad;
            q.strokeWidth = 2.0f * k;
            q.strokeColor = theme.color({0, "focus"}, render::rgba(255, 210, 80, 255));
            q.flags = render::kUiShape;
            quads.push_back(q);
        }
        // 자식
        f32 childClip[4] = {clip[0], clip[1], clip[2], clip[3]};
        if (d.clip) {
            childClip[0] = std::max(clip[0], r.x);
            childClip[1] = std::max(clip[1], r.y);
            childClip[2] = std::min(clip[2], r.x + r.w);
            childClip[3] = std::min(clip[3], r.y + r.h);
        }
        for (i32 c = index + 1; c < static_cast<i32>(n.end); c = static_cast<i32>(s.nodes[static_cast<size_t>(c)].end))
            if (s.nodes[static_cast<size_t>(c)].visible) emitNode(s, c, opacity, childClip);
    }

    void prepare(Screen& s, f32 viewportW, f32 viewportH) {
        if (s.rebuild || listsChanged(s)) rebuild(s);
        const f32 bw = std::max(1.0f, s.doc.boardW), bh = std::max(1.0f, s.doc.boardH);
        s.scale = std::min(viewportW / bw, viewportH / bh) * uiScale;
        if (s.scale <= 0.0f) s.scale = 1.0f;
        s.rootW = viewportW / s.scale;
        s.rootH = viewportH / s.scale;
        layout(s, s.rootW, s.rootH);
    }

    // ---------------------------------------------------------------- 입력

    bool focusable(const Screen& s, i32 i) const {
        const RNode& n = s.nodes[static_cast<size_t>(i)];
        const WidgetKind w = s.doc.nodes[n.src].widget;
        return n.visible && !n.disabled && w != WidgetKind::None && w != WidgetKind::List && w != WidgetKind::Scroll;
    }

    i32 hit(Screen& s, f32 x, f32 y, bool& blocks) {
        blocks = false;
        for (i32 i = static_cast<i32>(s.nodes.size()) - 1; i >= 0; --i) {
            const RNode& n = s.nodes[static_cast<size_t>(i)];
            if (!n.visible || !n.rect.contains(x, y)) continue;
            bool clipped = false; // 잘린 부모 밖이면 무시
            for (i32 a = n.parent; a >= 0; a = s.nodes[static_cast<size_t>(a)].parent) {
                const RNode& an = s.nodes[static_cast<size_t>(a)];
                if (s.doc.nodes[an.src].clip && !an.rect.contains(x, y)) clipped = true;
            }
            if (clipped) continue;
            const Node& d = s.doc.nodes[n.src];
            if (d.widget != WidgetKind::None && d.widget != WidgetKind::List && !n.disabled) {
                blocks = true;
                return i;
            }
            if (i > 0 && d.kind == NodeKind::Frame && d.visual.fill.kind != PaintKind::None && (d.visual.fill.c0.rgba >> 24) > 0)
                blocks = true; // 채워진 판 위 (뒤의 게임은 클릭을 받지 않는다)
        }
        return -1;
    }

    void emit(Screen& s, i32 index, const std::string& name, const std::string& field = {}) {
        const RNode& n = s.nodes[static_cast<size_t>(index)];
        Action a;
        a.name = name;
        a.screen = s.id;
        a.node = s.doc.nodes[n.src].name;
        a.field = field;
        a.listIndex = n.listIndex;
        actions.push_back(std::move(a));
    }

    void activate(Screen& s, i32 index, i32 step = 1) {
        RNode& n = s.nodes[static_cast<size_t>(index)];
        const Node& d = s.doc.nodes[n.src];
        const reflect::ValueInfo* v = nullptr;
        void* data = nullptr;
        switch (d.widget) {
        case WidgetKind::Button: emit(s, index, d.action); break;
        case WidgetKind::Toggle:
            if (resolve(s, n, d.field, v, data)) {
                writeNumber(*v, data, truthy(*v, data) ? 0.0 : 1.0);
                emit(s, index, d.action.empty() ? "changed" : d.action, d.field);
            }
            break;
        case WidgetKind::Select:
            if (!d.options.empty() && resolve(s, n, d.field, v, data)) {
                const i32 count = static_cast<i32>(d.options.size());
                const i32 cur = static_cast<i32>(numberOf(*v, data));
                writeNumber(*v, data, static_cast<f64>(((cur + step) % count + count) % count));
                emit(s, index, d.action.empty() ? "changed" : d.action, d.field);
            }
            break;
        case WidgetKind::TextInput: s.editing = index; break; // 글자 치기 시작 (Enter: 액션, Esc: 그만)
        case WidgetKind::Slider:
            if (resolve(s, n, d.field, v, data)) {
                const f64 stepSize = d.sliderStep > 0 ? d.sliderStep : (d.sliderMax - d.sliderMin) / 20.0;
                setSlider(s, index, numberOf(*v, data) + stepSize * step);
            }
            break;
        default: break;
        }
    }

    bool setSlider(Screen& s, i32 index, f64 value) { // 값이 바뀌어 액션을 냈으면 true
        RNode& n = s.nodes[static_cast<size_t>(index)];
        const Node& d = s.doc.nodes[n.src];
        const reflect::ValueInfo* v = nullptr;
        void* data = nullptr;
        if (!resolve(s, n, d.field, v, data)) return false;
        value = std::clamp(value, static_cast<f64>(d.sliderMin), static_cast<f64>(d.sliderMax));
        if (d.sliderStep > 0) value = d.sliderMin + std::round((value - d.sliderMin) / d.sliderStep) * d.sliderStep;
        if (std::fabs(numberOf(*v, data) - value) < 1e-9) return false;
        writeNumber(*v, data, value);
        emit(s, index, d.action.empty() ? "changed" : d.action, d.field);
        return true;
    }

    i32 findByName(const Screen& s, const std::string& name) const {
        for (size_t i = 0; i < s.nodes.size(); ++i)
            if (s.doc.nodes[s.nodes[i].src].name == name && focusable(s, static_cast<i32>(i))) return static_cast<i32>(i);
        return -1;
    }

    // 방향으로 가장 가까운 위젯 (0 위, 1 아래, 2 왼쪽, 3 오른쪽)
    i32 neighbor(Screen& s, i32 from, int dir) {
        const Node& d = s.doc.nodes[s.nodes[static_cast<size_t>(from)].src];
        if (!d.focusNext[dir].empty()) {
            const i32 named = findByName(s, d.focusNext[dir]);
            if (named >= 0) return named;
        }
        const Rect a = s.nodes[static_cast<size_t>(from)].rect;
        const f32 ax = a.x + a.w * 0.5f, ay = a.y + a.h * 0.5f;
        i32 best = -1;
        f32 bestScore = 1e30f;
        for (size_t i = 0; i < s.nodes.size(); ++i) {
            if (static_cast<i32>(i) == from || !focusable(s, static_cast<i32>(i))) continue;
            const Rect b = s.nodes[i].rect;
            const f32 bx = b.x + b.w * 0.5f, by = b.y + b.h * 0.5f;
            const f32 dx = bx - ax, dy = by - ay;
            const f32 primary = dir == 0 ? -dy : dir == 1 ? dy : dir == 2 ? -dx : dx;
            const f32 secondary = dir <= 1 ? std::fabs(dx) : std::fabs(dy);
            if (primary <= 1.0f) continue;
            const f32 score = primary + secondary * 2.0f;
            if (score < bestScore) {
                bestScore = score;
                best = static_cast<i32>(i);
            }
        }
        return best;
    }

    void update(const FrameInput& in, f64 dt) {
        actions.clear();
        wantsMouse = wantsKeyboard = false;
        time += dt;
        if (time >= nextReload) {
            nextReload = time + kReloadSeconds;
            reloadChanged();
        }
        std::vector<Screen*> order;
        for (Screen& s : screens)
            if (!s.worldTemplate) order.push_back(&s);
        std::stable_sort(order.begin(), order.end(), [](const Screen* a, const Screen* b) { return a->layer > b->layer; });

        const Input* input = in.mouseAllowed ? in.input : nullptr;
        const f32 mx = input ? input->mouseX() : -1e9f, my = input ? input->mouseY() : -1e9f;
        const bool down = input && input->mouseDown(MouseButton::Left);
        const bool clicked = input && input->mousePressed(MouseButton::Left);
        if (input && input->mouseMoved()) focusVisible = false;

        bool mouseTaken = false;
        for (Screen* sp : order) {
            Screen& s = *sp;
            if (s.nodes.empty()) continue;
            const f32 lx = (mx - s.originX) / s.scale, ly = (my - s.originY) / s.scale;
            bool blocks = false;
            const i32 h = mouseTaken ? -1 : hit(s, lx, ly, blocks);
            if (blocks) mouseTaken = true;
            s.hover = h;
            if (clicked && s.editing >= 0 && h != s.editing) s.editing = -1; // 다른 곳을 누르면 글자 치기를 그만둔다
            if (clicked && h >= 0) {
                s.pressed = h;
                if (focusable(s, h)) s.focus = h;
            }
            if (s.pressed >= 0) {
                const Node& pd = s.doc.nodes[s.nodes[static_cast<size_t>(s.pressed)].src];
                if (pd.widget == WidgetKind::Slider && down) {
                    const Rect r = s.nodes[static_cast<size_t>(s.pressed)].rect;
                    const f64 t = std::clamp((lx - r.x) / std::max(r.w, 1.0f), 0.0f, 1.0f);
                    if (setSlider(s, s.pressed, pd.sliderMin + t * (pd.sliderMax - pd.sliderMin))) actions.back().dragging = true;
                }
                if (!down) {
                    if (pd.widget == WidgetKind::Slider) emit(s, s.pressed, pd.action.empty() ? "changed" : pd.action, pd.field); // 놓았다
                    else if (s.pressed == h) activate(s, h);
                    s.pressed = -1;
                }
            }
            if (input && input->wheelDelta() != 0.0f && !mouseTaken) {
                // 스크롤: 마우스 아래의 가장 안쪽 스크롤 영역
                for (i32 i = static_cast<i32>(s.nodes.size()) - 1; i >= 0; --i) {
                    RNode& n = s.nodes[static_cast<size_t>(i)];
                    const Node& d = s.doc.nodes[n.src];
                    if (d.widget != WidgetKind::Scroll || !n.visible || !n.rect.contains(lx, ly)) continue;
                    const f32 view = d.scrollHorizontal ? n.rect.w : n.rect.h;
                    n.scroll = std::clamp(n.scroll - input->wheelDelta() * kScrollStep, 0.0f, std::max(0.0f, n.contentExtent - view));
                    mouseTaken = true;
                    break;
                }
            }
            if (s.hover >= 0 || s.pressed >= 0) mouseTaken = true;
        }
        wantsMouse = mouseTaken;

        // 글자 입력 중이면 키보드는 그 칸만 받는다 (ui.* 이동·확인·뒤로도 쓰지 않는다)
        Screen* typing = nullptr;
        for (Screen* sp : order)
            if (sp->editing >= 0) {
                typing = sp;
                break;
            }
        if (typing) {
            if (in.keyboardAllowed && in.input) editText(*typing, *in.input);
            wantsKeyboard = true;
        }
        // 키보드·게임패드: 포커스할 위젯이 있는 가장 위 화면
        else if (in.keyboardAllowed && in.actions) {
            const input::Actions& a = *in.actions;
            for (Screen* sp : order) {
                Screen& s = *sp;
                if (s.fresh) break; // 맨 위가 방금 연 화면이면 이번에는 아래 화면에도 넘기지 않는다
                bool any = false;
                for (size_t i = 0; i < s.nodes.size() && !any && s.doc.navigation; ++i) any = focusable(s, static_cast<i32>(i));
                if (a.pressed("ui.back") && !s.doc.backAction.empty()) {
                    Action back;
                    back.name = s.doc.backAction;
                    back.screen = s.id;
                    actions.push_back(back);
                    break;
                }
                if (!any) continue;
                const char* dirs[4] = {"ui.up", "ui.down", "ui.left", "ui.right"};
                for (int dir = 0; dir < 4; ++dir) {
                    if (!a.pressed(dirs[dir])) continue;
                    focusVisible = true;
                    if (s.focus < 0 || !focusable(s, s.focus)) {
                        s.focus = s.doc.firstFocus.empty() ? -1 : findByName(s, s.doc.firstFocus);
                        for (size_t i = 0; i < s.nodes.size() && s.focus < 0; ++i)
                            if (focusable(s, static_cast<i32>(i))) s.focus = static_cast<i32>(i);
                        continue;
                    }
                    const WidgetKind w = s.doc.nodes[s.nodes[static_cast<size_t>(s.focus)].src].widget;
                    if (dir >= 2 && (w == WidgetKind::Slider || w == WidgetKind::Select)) {
                        activate(s, s.focus, dir == 2 ? -1 : 1);
                        continue;
                    }
                    const i32 next = neighbor(s, s.focus, dir);
                    if (next >= 0) s.focus = next;
                }
                if (a.pressed("ui.confirm") && s.focus >= 0 && focusable(s, s.focus)) {
                    focusVisible = true;
                    activate(s, s.focus);
                }
                wantsKeyboard = s.focus >= 0 && focusVisible;
                break;
            }
        }
        for (Screen& s : screens) s.fresh = false;

        // 상태 섞기 (마우스 올림·눌림·포커스·비활성·켜짐)
        for (Screen& s : screens)
            for (size_t i = 0; i < s.nodes.size(); ++i) {
                RNode& n = s.nodes[i];
                const Node& d = s.doc.nodes[n.src];
                if (s.stateIndex[n.src] < 0) continue;
                const bool target[static_cast<int>(State::Count)] = {
                    true, s.hover == static_cast<i32>(i), s.pressed == static_cast<i32>(i),
                    s.focus == static_cast<i32>(i) && (focusVisible || focusAlways), n.disabled, n.checked};
                const f32 rate = d.transition > 0 ? static_cast<f32>(dt) / d.transition : 1.0f;
                for (int st = 1; st < static_cast<int>(State::Count); ++st)
                    n.mix[st] = std::clamp(n.mix[st] + (target[st] ? rate : -rate), 0.0f, 1.0f);
            }
    }

    void editText(Screen& s, const Input& input) {
        RNode& n = s.nodes[static_cast<size_t>(s.editing)];
        const Node& d = s.doc.nodes[n.src];
        const reflect::ValueInfo* v = nullptr;
        void* data = nullptr;
        if (!n.visible || n.disabled || d.widget != WidgetKind::TextInput || !resolve(s, n, d.field, v, data) ||
            v->kind != reflect::Kind::String) {
            s.editing = -1;
            return;
        }
        std::string& value = *static_cast<std::string*>(data);
        for (u32 i = 0; i < input.backspaces() && !value.empty(); ++i) { // 마지막 글자 (UTF-8 이어지는 바이트까지)
            size_t end = value.size() - 1;
            while (end > 0 && (static_cast<u8>(value[end]) & 0xC0) == 0x80) --end;
            value.resize(end);
        }
        u32 count = static_cast<u32>(decodeUtf8(value).size());
        const std::string& text = input.text();
        for (size_t at = 0; at < text.size();) { // 최대 글자 수까지 UTF-8 그대로 붙인다
            size_t len = 1;
            while (at + len < text.size() && (static_cast<u8>(text[at + len]) & 0xC0) == 0x80) ++len;
            if (count < d.maxLength) {
                value.append(text, at, len);
                ++count;
            }
            at += len;
        }
        if (input.keyPressed(key::Enter)) {
            emit(s, s.editing, d.action.empty() ? "submit" : d.action, d.field);
            s.editing = -1;
        } else if (input.keyPressed(key::Escape)) {
            s.editing = -1;
        }
    }

    void reloadChanged() {
        const std::string themePath = findAsset(std::string("assets/") + kThemeFile);
        const unsigned long long tt = themePath.empty() ? 0 : fileWriteTime(themePath);
        const bool themeChanged = tt != themeTime;
        if (themeChanged) {
            themeTime = tt;
            std::vector<std::string> errors;
            loadTheme(theme, &errors);
            for (const std::string& e : errors) MEALKIT_WARN("UI: %s", e.c_str());
        }
        for (Screen& s : screens) {
            const std::string file = findAsset("assets/" + s.path);
            const unsigned long long t = file.empty() ? 0 : fileWriteTime(file);
            if (themeChanged || t != s.fileTime) {
                loadScreen(s);
                MEALKIT_INFO("UI: loaded %s (%zu nodes)", s.path.c_str(), s.doc.nodes.size());
            }
        }
        atlas.reloadChanged();
    }
};

System::System() : m(new Impl()) {}
System::~System() { delete m; }

void System::init(render::Renderer* renderer, const loc::Strings* strings) {
    m->renderer = renderer;
    m->strings = strings;
    m->atlas.setRenderer(renderer);
    m->themeLoaded = false;
    m->ensureTheme();
    for (Screen& s : m->screens) m->loadScreen(s); // init 전에 연 화면 (아틀라스·테마가 바뀌었다)
}

ScreenId System::openWorld(const std::string& uiPath) {
    const ScreenId id = open(uiPath, 0);
    m->find(id)->worldTemplate = true;
    return id;
}

ScreenId System::open(const std::string& uiPath, i32 layer) {
    Screen s;
    s.id = m->nextId++;
    s.layer = layer;
    s.path = uiPath;
    m->screens.push_back(std::move(s));
    m->loadScreen(m->screens.back());
    return m->screens.back().id;
}

void System::close(ScreenId screen) {
    m->screens.erase(std::remove_if(m->screens.begin(), m->screens.end(), [&](const Screen& s) { return s.id == screen; }),
                     m->screens.end());
}

bool System::isOpen(ScreenId screen) const { return m->find(screen) != nullptr; }
void System::closeAll() { m->screens.clear(); }

void System::bind(ScreenId screen, reflect::View viewModel) {
    if (Screen* s = m->find(screen)) {
        if (s->vm.data != viewModel.data || s->vm.type != viewModel.type) s->rebuild = true;
        s->vm = viewModel;
        s->bindingErrors.clear();
    }
}

void System::setScale(f32 uiScale) { m->uiScale = std::clamp(uiScale, 0.25f, 4.0f); }
f32 System::scale() const { return m->uiScale; }

void System::update(const FrameInput& in, const render::Viewport& viewport, f64 dt) {
    m->viewX = viewport.x;
    m->viewY = viewport.y;
    m->viewW = std::max(1.0f, viewport.width);
    m->viewH = std::max(1.0f, viewport.height);
    for (Screen& s : m->screens) {
        s.originX = m->viewX;
        s.originY = m->viewY;
    }
    m->update(in, dt);
    m->world.clear();
}

const std::vector<Action>& System::actions() const { return m->actions; }
bool System::wantsMouse() const { return m->wantsMouse; }
bool System::wantsKeyboard() const { return m->wantsKeyboard; }

void System::setCamera(const Mat4& viewProj) { m->viewProj = viewProj; }

void System::drawAt(ScreenId templateScreen, Vec3 p, reflect::View viewModel, bool clampToEdges) {
    m->world.push_back({templateScreen, p, viewModel, clampToEdges});
}

void System::render(render::Renderer& renderer) {
    m->quads.clear();
    std::vector<Screen*> order;
    for (Screen& s : m->screens) order.push_back(&s);
    std::stable_sort(order.begin(), order.end(), [](const Screen* a, const Screen* b) { return a->layer < b->layer; });
    const f32 noClip[4] = {-1e9f, -1e9f, 1e9f, 1e9f};
    // 월드 위 UI 먼저 (화면 UI 아래)
    for (const WorldDraw& w : m->world) {
        Screen* s = m->find(w.screen);
        if (!s || s->doc.nodes.empty()) continue;
        const Mat4& vp = m->viewProj;
        const f32 cx = w.position.x * vp.m[0][0] + w.position.y * vp.m[1][0] + w.position.z * vp.m[2][0] + vp.m[3][0];
        const f32 cy = w.position.x * vp.m[0][1] + w.position.y * vp.m[1][1] + w.position.z * vp.m[2][1] + vp.m[3][1];
        const f32 cw = w.position.x * vp.m[0][3] + w.position.y * vp.m[1][3] + w.position.z * vp.m[2][3] + vp.m[3][3];
        if (cw <= 1e-4f && !w.clamp) continue; // 카메라 뒤
        f32 nx = cx / std::max(cw, 1e-4f), ny = cy / std::max(cw, 1e-4f);
        if (cw <= 1e-4f) {
            nx = -nx;
            ny = -ny;
        }
        if (!w.clamp && (nx < -1.2f || nx > 1.2f || ny < -1.2f || ny > 1.2f)) continue;
        if (w.clamp) {
            nx = std::clamp(nx, -0.95f, 0.95f);
            ny = std::clamp(ny, -0.95f, 0.95f);
        }
        const f32 px = m->viewX + (nx * 0.5f + 0.5f) * m->viewW, py = m->viewY + (0.5f - ny * 0.5f) * m->viewH;
        const reflect::View keep = s->vm;
        if (s->vm.data != w.vm.data) {
            s->vm = w.vm;
            s->rebuild = true;
        }
        const f32 bw = std::max(1.0f, s->doc.boardW), bh = std::max(1.0f, s->doc.boardH);
        const f32 k = std::min(m->viewW / 1920.0f, m->viewH / 1080.0f) * m->uiScale; // 기준 화면에 대한 배율
        if (s->rebuild || m->listsChanged(*s)) m->rebuild(*s);
        s->scale = k;
        s->rootW = bw;
        s->rootH = bh;
        m->layout(*s, bw, bh);
        s->originX = px - bw * 0.5f * k;
        s->originY = py - bh * 0.5f * k;
        m->emitNode(*s, 0, 1.0f, noClip);
        s->vm = keep;
        s->rebuild = true;
    }
    for (Screen* s : order) {
        if (s->worldTemplate || s->doc.nodes.empty()) continue;
        s->originX = m->viewX;
        s->originY = m->viewY;
        m->prepare(*s, m->viewW, m->viewH);
        if (s->nodes[0].visible) m->emitNode(*s, 0, 1.0f, noClip);
    }
    m->lastQuadCount = static_cast<u32>(m->quads.size());
    renderer.drawUi(m->quads.data(), static_cast<u32>(m->quads.size()));
}

void System::layout(const render::Viewport& viewport) {
    m->viewX = viewport.x;
    m->viewY = viewport.y;
    m->viewW = std::max(1.0f, viewport.width);
    m->viewH = std::max(1.0f, viewport.height);
    for (Screen& s : m->screens) {
        if (s.worldTemplate || s.doc.nodes.empty()) continue;
        s.originX = m->viewX;
        s.originY = m->viewY;
        m->prepare(s, m->viewW, m->viewH);
    }
}

ScreenInfo System::info(ScreenId screen) const {
    ScreenInfo out;
    if (Screen* s = m->find(screen)) {
        out.path = s->path;
        out.nodes = static_cast<u32>(s->nodes.size());
        for (const RNode& n : s->nodes) out.visibleNodes += n.visible ? 1u : 0u;
        out.errors = s->doc.errors;
        out.errors.insert(out.errors.end(), s->bindingErrors.begin(), s->bindingErrors.end());
        out.exported = s->doc.exported;
    }
    return out;
}

std::vector<NodeBox> System::boxes(ScreenId screen) const {
    std::vector<NodeBox> out;
    if (Screen* s = m->find(screen))
        for (const RNode& n : s->nodes) {
            const Node& d = s->doc.nodes[n.src];
            out.push_back({d.name, d.id, n.rect.x, n.rect.y, n.rect.w, n.rect.h, d.depth, n.visible, d.kind == NodeKind::Text});
        }
    return out;
}

void System::setFocusVisible(bool always) { m->focusAlways = always; }
u32 System::quadCount() const { return m->lastQuadCount; }

bool System::press(ScreenId screen, const std::string& nodeName) {
    Screen* s = m->find(screen);
    if (!s) return false;
    if (s->rebuild) m->prepare(*s, m->viewW, m->viewH);
    for (size_t i = 0; i < s->nodes.size(); ++i)
        if (s->doc.nodes[s->nodes[i].src].name == nodeName && m->focusable(*s, static_cast<i32>(i))) {
            m->activate(*s, static_cast<i32>(i));
            return true;
        }
    return false;
}

bool System::pressAny(const std::string& nodeName) {
    std::vector<Screen*> order;
    for (Screen& s : m->screens)
        if (!s.worldTemplate) order.push_back(&s);
    std::stable_sort(order.begin(), order.end(), [](const Screen* a, const Screen* b) { return a->layer > b->layer; });
    for (Screen* s : order)
        if (press(s->id, nodeName)) return true;
    return false;
}

bool System::focusText(ScreenId screen, const std::string& nodeName) {
    Screen* s = m->find(screen);
    if (!s) return false;
    if (s->rebuild) m->prepare(*s, m->viewW, m->viewH);
    for (size_t i = 0; i < s->nodes.size(); ++i) {
        const Node& d = s->doc.nodes[s->nodes[i].src];
        if (d.name != nodeName || d.widget != WidgetKind::TextInput || !m->focusable(*s, static_cast<i32>(i))) continue;
        s->focus = s->editing = static_cast<i32>(i);
        return true;
    }
    return false;
}

bool System::editingText() const {
    for (const Screen& s : m->screens)
        if (s.editing >= 0) return true;
    return false;
}

} // namespace mealkit::ui
