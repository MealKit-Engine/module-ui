#pragma once

#include "mealkit/core/Types.h"

#include <string>
#include <vector>

// .ui file (assets/ui/*.ui): one game screen. The same format the Penpot plugin will export later; for now written by hand (ROADMAP stage 1 "Game screen UI").
// Files exported by the plugin start with "# MealKit UI exported from Penpot ..." and aren't edited by hand.
//
//   board hud 1920 1080          ← root board (logical units of the reference resolution). Board lines: navigation (keyboard/gamepad focus), firstFocus, back
//     frame topBar
//       layout row               ← Flex: row|column [reverse] [wrap] / Grid: layout grid [row|column] + columns·rows
//       columns 200 1fr auto 25%  ← Grid tracks: fixed / fr (fraction of remaining space) / auto (content) / % (of inner size). Children use cell <row> <column> [row span column span]
//       size fill hug            ← number / hug (fit content) / fill
//       padding 24 32
//       gap 16
//       align center
//       fill #10141CCC
//       radius 12
//       text score
//         key hud.score          ← string table key (or content <text>), {field} is a view model value
//         font Pretendard-Medium
//         fontSize 36
//         color #FFFFFF
//         span 1 color $accent   ← style of numbered tag <1>…</1>
//       end
//     end
//   end
//
// One line = one property ("name value..."). Nodes open with board / frame / text / image / use (another .ui as a component) and close with end.
// Colors are #RRGGBB[AA] or theme tokens $name (assets/ui/theme.ui). The property list is parseProperty in Document.cpp.
namespace mealkit::ui {

enum class NodeKind : u8 { Frame, Text, Image };
enum class SizeMode : u8 { Fixed, Hug, Fill };
enum class LayoutKind : u8 { None, Flex, Grid };
enum class TrackKind : u8 { Fixed, Flex, Percent, Auto };
enum class Justify : u8 { Start, Center, End, SpaceBetween, SpaceAround, SpaceEvenly };
enum class Align : u8 { Auto, Start, Center, End, Stretch };
enum class Constraint : u8 { Start, End, Both, Center, Scale }; // horizontal: left/right/leftright/center/scale
enum class PaintKind : u8 { None, Solid, Linear, Radial };
enum class WidgetKind : u8 { None, Button, Toggle, Slider, Select, Scroll, List, TextInput };
enum class TextAlign : u8 { Left, Center, Right };
enum class VerticalAlign : u8 { Top, Center, Bottom };
enum class WrapMode : u8 { Word, Syllable, None };
enum class TextTransform : u8 { None, Upper, Lower };
enum class State : u8 { Normal, Hover, Pressed, Focus, Disabled, Checked, Count };

// Color: sRGB RGBA8 (render::rgba) or a theme token name
struct ColorValue {
    u32 rgba = 0;
    std::string token; // if not empty, looked up in the theme
};

struct Paint {
    PaintKind kind = PaintKind::None;
    ColorValue c0, c1;
    f32 p[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // linear: start xy, end xy / radial: center xy, radius xy (box 0..1)
};

struct Shadow {
    f32 dx = 0, dy = 0, blur = 0, spread = 0;
    ColorValue color;
};

// Drawing values that can change per state (hover, pressed ...)
struct Visual {
    Paint fill;
    f32 strokeWidth = 0.0f;
    ColorValue strokeColor;
    f32 radii[4] = {};
    std::vector<Shadow> shadows;
    f32 opacity = 1.0f;
    ColorValue textColor; // text nodes: default color
    ColorValue tint;      // image nodes: color to multiply (default white)
    bool hasTextColor = false, hasTint = false;
};

struct StateOverride {
    State state = State::Hover;
    std::vector<std::string> lines; // property lines to override in this state (fill, stroke, radius, shadow, opacity, color, tint)
};

struct TextStyle {
    std::string font;      // file name in assets/fonts (first font if empty)
    f32 size = 24.0f;      // em size (logical units)
    ColorValue color;      // Visual::textColor if empty
    bool hasColor = false;
    f32 letterSpacing = 0.0f;
    bool underline = false, strike = false;
    TextTransform transform = TextTransform::None;
};

// One Grid track (same as Penpot's Track)
struct Track {
    TrackKind kind = TrackKind::Auto;
    f32 value = 0.0f; // fixed: logical units, Flex: fr, Percent: %
};

struct SpanStyle {
    u32 tag = 0; // <tag>…</tag>
    // Overrides only what's set
    std::string font;
    f32 size = 0.0f;
    ColorValue color;
    bool hasColor = false;
    i32 underline = -1, strike = -1;
    f32 letterSpacing = -1e9f;
};

struct Node {
    std::string name;
    std::string id; // Penpot shape id (exported files; layout comparison finds nodes by it)
    NodeKind kind = NodeKind::Frame;
    i32 parent = -1;
    u32 depth = 0;
    u32 end = 0; // index after this node's last descendant (subtree = [i+1, end) in the depth-first array)

    // Layout (logical units, reference board)
    f32 x = 0, y = 0, w = 100, h = 100; // design position/size (parent-relative)
    SizeMode wMode = SizeMode::Fixed, hMode = SizeMode::Fixed;
    f32 minW = 0, minH = 0, maxW = 1e9f, maxH = 1e9f;
    f32 margin[4] = {}; // top, right, bottom, left
    bool absolute = false;
    Constraint ch = Constraint::Start, cv = Constraint::Start;
    f32 rotation = 0.0f; // degrees
    bool hidden = false, clip = false;

    LayoutKind layout = LayoutKind::None;
    bool column = false, reverse = false, wrap = false;
    f32 rowGap = 0, columnGap = 0;
    f32 padding[4] = {};
    Justify justify = Justify::Start;
    Align alignItems = Align::Start, alignContent = Align::Start, alignSelf = Align::Auto;
    Justify alignContentJustify = Justify::Start; // multi-line distribution (space-*)
    // Grid (layout grid): tracks, horizontal alignment inside cells, distribution of remaining space (justify: horizontal, alignContent: vertical; with stretch auto tracks grow)
    std::vector<Track> rows, columns;
    bool gridColumnFlow = false; // auto placement direction: false = fill rows first
    Align justifyItems = Align::Start, justifySelf = Align::Auto;
    bool justifyStretch = false;
    u16 cellRow = 0, cellColumn = 0, rowSpan = 1, columnSpan = 1; // cell (from 1, 0 = auto placement)

    Visual visual;
    std::vector<StateOverride> states;
    f32 transition = 0.1f; // blend time when the state changes (seconds)

    // Image
    std::string image; // PNG relative to assets
    f32 slice[4] = {};  // 9-slice margins (source pixels: left, top, right, bottom)

    // Text
    std::string textKey, textContent; // string table key or literal text
    TextStyle text;
    std::vector<SpanStyle> spans;
    TextAlign textAlign = TextAlign::Left;
    VerticalAlign verticalAlign = VerticalAlign::Top;
    f32 lineHeight = 1.2f; // multiple of the font size
    WrapMode wrapMode = WrapMode::Syllable;
    bool ellipsis = false;
    u32 maxLines = 0;

    // Bindings (view model field paths, ! in front inverts)
    std::string visibleField, disabledField, widthRatioField, colorField;

    // Widgets
    WidgetKind widget = WidgetKind::None;
    std::string action; // buttons: action to fire, input widgets: action fired on change ("changed" if empty)
    std::string field;  // toggles/sliders/selects/lists: view model field
    f32 sliderMin = 0, sliderMax = 1, sliderStep = 0;
    std::vector<std::string> options; // select boxes: string table keys
    std::string placeholderKey;       // text input: string table key shown dimmed when empty
    u32 maxLength = 64;               // text input: max character count
    bool scrollHorizontal = false;
    std::string focusNext[4]; // node names to go to for up, down, left, right (nearest if empty)
};

struct Document {
    std::string path;     // relative to assets
    bool exported = false; // exported by the Penpot plugin (not edited by hand)
    f32 boardW = 1920, boardH = 1080;
    std::string backAction; // action fired by back (ui.back) input
    std::string firstFocus; // name of the node to focus first
    bool navigation = false; // keyboard/gamepad move between widgets (menus). Off for HUDs (so they don't take movement keys)
    std::vector<Node> nodes; // depth first, [0] = board
    std::vector<std::string> errors;
};

// Theme (assets/ui/theme.ui): "color name #RRGGBBAA", "number name value". Color tokens are resolved when drawing, number tokens ($name) when reading
struct Theme {
    std::vector<std::pair<std::string, u32>> colors;
    std::vector<std::pair<std::string, f32>> numbers;
    u32 color(const ColorValue& c, u32 fallback = 0xFFFFFFFFu) const;
    bool number(const std::string& name, f32& out) const;
};
inline constexpr const char* kThemeFile = "ui/theme.ui"; // relative to assets
bool loadTheme(Theme& out, std::vector<std::string>* errors = nullptr);

// Reads a .ui at a path relative to assets (use rereads that file and inserts it as a subtree). Even on failure uses what was read and puts line numbers in errors.
bool loadDocument(const std::string& assetPath, Document& out, const Theme* theme = nullptr);
bool parseDocument(const std::string& text, const std::string& assetPath, Document& out, const Theme* theme = nullptr);

// Applies one property line to a node (state overrides use this function too). false for unknown properties
bool applyProperty(Node& node, const std::string& line, const Theme* theme, std::string* error = nullptr);

} // namespace mealkit::ui
