#pragma once

#include "mealkit/core/Types.h"

#include <string>
#include <vector>

// .ui 파일 (assets/ui/*.ui): 게임 화면 하나. Penpot 플러그인이 나중에 내보낼 형식 그대로이고, 지금은 손으로 쓴다(ROADMAP 1단계 "게임 화면 UI").
// 플러그인이 내보낸 파일은 첫 줄이 "# MealKit UI exported from Penpot ..."이고 손으로 고치지 않는다.
//
//   board hud 1920 1080          ← 뿌리 보드 (기준 해상도의 논리 단위). 보드 줄: navigation(키보드·게임패드 포커스), firstFocus, back
//     frame topBar
//       layout row               ← Flex: row|column [reverse] [wrap] / Grid: layout grid [row|column] + columns·rows
//       columns 200 1fr auto 25%  ← Grid 트랙: 고정 / fr(남는 자리 비율) / auto(내용) / %(안쪽 크기 기준). 자식은 cell <행> <열> [행 수 열 수]
//       size fill hug            ← 숫자 / hug(내용에 맞춤) / fill(채우기)
//       padding 24 32
//       gap 16
//       align center
//       fill #10141CCC
//       radius 12
//       text score
//         key hud.score          ← 문자열 표 키 (또는 content <글자>), {필드}는 뷰 모델 값
//         font Pretendard-Medium
//         fontSize 36
//         color #FFFFFF
//         span 1 color $accent   ← 번호 태그 <1>…</1>의 스타일
//       end
//     end
//   end
//
// 한 줄 = 속성 하나 ("이름 값..."). 노드는 board / frame / text / image / use(다른 .ui를 컴포넌트로)로 열고 end로 닫는다.
// 색은 #RRGGBB[AA] 또는 테마 토큰 $이름 (assets/ui/theme.ui). 속성 목록은 Document.cpp의 parseProperty.
namespace mealkit::ui {

enum class NodeKind : u8 { Frame, Text, Image };
enum class SizeMode : u8 { Fixed, Hug, Fill };
enum class LayoutKind : u8 { None, Flex, Grid };
enum class TrackKind : u8 { Fixed, Flex, Percent, Auto };
enum class Justify : u8 { Start, Center, End, SpaceBetween, SpaceAround, SpaceEvenly };
enum class Align : u8 { Auto, Start, Center, End, Stretch };
enum class Constraint : u8 { Start, End, Both, Center, Scale }; // 가로: left/right/leftright/center/scale
enum class PaintKind : u8 { None, Solid, Linear, Radial };
enum class WidgetKind : u8 { None, Button, Toggle, Slider, Select, Scroll, List, TextInput };
enum class TextAlign : u8 { Left, Center, Right };
enum class VerticalAlign : u8 { Top, Center, Bottom };
enum class WrapMode : u8 { Word, Syllable, None };
enum class TextTransform : u8 { None, Upper, Lower };
enum class State : u8 { Normal, Hover, Pressed, Focus, Disabled, Checked, Count };

// 색: sRGB RGBA8 (render::rgba) 또는 테마 토큰 이름
struct ColorValue {
    u32 rgba = 0;
    std::string token; // 비어 있지 않으면 테마에서 찾는다
};

struct Paint {
    PaintKind kind = PaintKind::None;
    ColorValue c0, c1;
    f32 p[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // 선형: 시작 xy, 끝 xy / 원형: 중심 xy, 반지름 xy (상자 0..1)
};

struct Shadow {
    f32 dx = 0, dy = 0, blur = 0, spread = 0;
    ColorValue color;
};

// 상태(마우스 올림·눌림 ...)마다 바꿀 수 있는 그리기 값
struct Visual {
    Paint fill;
    f32 strokeWidth = 0.0f;
    ColorValue strokeColor;
    f32 radii[4] = {};
    std::vector<Shadow> shadows;
    f32 opacity = 1.0f;
    ColorValue textColor; // 글자 노드: 기본 색
    ColorValue tint;      // 이미지 노드: 곱할 색 (기본 흰색)
    bool hasTextColor = false, hasTint = false;
};

struct StateOverride {
    State state = State::Hover;
    std::vector<std::string> lines; // 상태일 때 덮어쓸 속성 줄 (fill, stroke, radius, shadow, opacity, color, tint)
};

struct TextStyle {
    std::string font;      // assets/fonts 파일 이름 (비어 있으면 첫 글꼴)
    f32 size = 24.0f;      // em 크기 (논리 단위)
    ColorValue color;      // 비어 있으면 Visual::textColor
    bool hasColor = false;
    f32 letterSpacing = 0.0f;
    bool underline = false, strike = false;
    TextTransform transform = TextTransform::None;
};

// Grid 트랙 하나 (Penpot의 Track과 같다)
struct Track {
    TrackKind kind = TrackKind::Auto;
    f32 value = 0.0f; // 고정: 논리 단위, Flex: fr, Percent: %
};

struct SpanStyle {
    u32 tag = 0; // <tag>…</tag>
    // 설정된 것만 덮어쓴다
    std::string font;
    f32 size = 0.0f;
    ColorValue color;
    bool hasColor = false;
    i32 underline = -1, strike = -1;
    f32 letterSpacing = -1e9f;
};

struct Node {
    std::string name;
    std::string id; // Penpot 도형 id (내보낸 파일, 배치 비교가 노드를 찾는다)
    NodeKind kind = NodeKind::Frame;
    i32 parent = -1;
    u32 depth = 0;
    u32 end = 0; // 이 노드의 마지막 자손 다음 번호 (깊이 우선 배열에서 하위 트리 = [i+1, end))

    // 배치 (논리 단위, 기준 보드)
    f32 x = 0, y = 0, w = 100, h = 100; // 디자인 위치·크기 (부모 기준)
    SizeMode wMode = SizeMode::Fixed, hMode = SizeMode::Fixed;
    f32 minW = 0, minH = 0, maxW = 1e9f, maxH = 1e9f;
    f32 margin[4] = {}; // 위, 오른쪽, 아래, 왼쪽
    bool absolute = false;
    Constraint ch = Constraint::Start, cv = Constraint::Start;
    f32 rotation = 0.0f; // 도
    bool hidden = false, clip = false;

    LayoutKind layout = LayoutKind::None;
    bool column = false, reverse = false, wrap = false;
    f32 rowGap = 0, columnGap = 0;
    f32 padding[4] = {};
    Justify justify = Justify::Start;
    Align alignItems = Align::Start, alignContent = Align::Start, alignSelf = Align::Auto;
    Justify alignContentJustify = Justify::Start; // 여러 줄 배분 (space-*)
    // Grid (layout grid): 트랙, 칸 안 가로 정렬, 남는 자리 배분(justify: 가로, alignContent: 세로, stretch면 auto 트랙이 늘어난다)
    std::vector<Track> rows, columns;
    bool gridColumnFlow = false; // 자동 배치 방향: false = 행을 먼저 채운다
    Align justifyItems = Align::Start, justifySelf = Align::Auto;
    bool justifyStretch = false;
    u16 cellRow = 0, cellColumn = 0, rowSpan = 1, columnSpan = 1; // 칸 (1부터, 0 = 자동 배치)

    Visual visual;
    std::vector<StateOverride> states;
    f32 transition = 0.1f; // 상태 바뀔 때 섞는 시간 (초)

    // 이미지
    std::string image; // assets 기준 PNG
    f32 slice[4] = {};  // 9-slice 여백 (원본 픽셀: 왼쪽, 위, 오른쪽, 아래)

    // 글자
    std::string textKey, textContent; // 문자열 표 키 또는 글자 그대로
    TextStyle text;
    std::vector<SpanStyle> spans;
    TextAlign textAlign = TextAlign::Left;
    VerticalAlign verticalAlign = VerticalAlign::Top;
    f32 lineHeight = 1.2f; // 글자 크기의 배수
    WrapMode wrapMode = WrapMode::Syllable;
    bool ellipsis = false;
    u32 maxLines = 0;

    // 바인딩 (뷰 모델 필드 경로, 앞에 !면 반대로)
    std::string visibleField, disabledField, widthRatioField, colorField;

    // 위젯
    WidgetKind widget = WidgetKind::None;
    std::string action; // 버튼: 낼 액션, 입력 위젯: 바뀌면 낼 액션 (비면 "changed")
    std::string field;  // 토글·슬라이더·선택·목록: 뷰 모델 필드
    f32 sliderMin = 0, sliderMax = 1, sliderStep = 0;
    std::vector<std::string> options; // 선택 상자: 문자열 표 키들
    std::string placeholderKey;       // 글자 입력: 비어 있을 때 흐리게 보일 문자열 표 키
    u32 maxLength = 64;               // 글자 입력: 최대 글자 수
    bool scrollHorizontal = false;
    std::string focusNext[4]; // 위, 아래, 왼쪽, 오른쪽으로 갈 노드 이름 (비면 가장 가까운 것)
};

struct Document {
    std::string path;     // assets 기준
    bool exported = false; // Penpot 플러그인이 내보냄 (손으로 고치지 않는다)
    f32 boardW = 1920, boardH = 1080;
    std::string backAction; // 뒤로(ui.back) 입력이 낼 액션
    std::string firstFocus; // 처음 포커스를 줄 노드 이름
    bool navigation = false; // 키보드·게임패드로 위젯을 옮겨 다닌다 (메뉴). HUD는 끈다 (이동 키를 가져가지 않게)
    std::vector<Node> nodes; // 깊이 우선, [0] = board
    std::vector<std::string> errors;
};

// 테마 (assets/ui/theme.ui): "color 이름 #RRGGBBAA", "number 이름 값". 색 토큰은 그릴 때, 숫자 토큰($이름)은 읽을 때 푼다
struct Theme {
    std::vector<std::pair<std::string, u32>> colors;
    std::vector<std::pair<std::string, f32>> numbers;
    u32 color(const ColorValue& c, u32 fallback = 0xFFFFFFFFu) const;
    bool number(const std::string& name, f32& out) const;
};
inline constexpr const char* kThemeFile = "ui/theme.ui"; // assets 기준
bool loadTheme(Theme& out, std::vector<std::string>* errors = nullptr);

// assets 기준 경로의 .ui를 읽는다 (use는 그 파일을 다시 읽어 하위 트리로 넣는다). 실패해도 읽은 데까지 쓰고 errors에 줄 번호.
bool loadDocument(const std::string& assetPath, Document& out, const Theme* theme = nullptr);
bool parseDocument(const std::string& text, const std::string& assetPath, Document& out, const Theme* theme = nullptr);

// 한 속성 줄을 노드에 적용한다 (상태 덮어쓰기도 이 함수). 모르는 속성이면 false
bool applyProperty(Node& node, const std::string& line, const Theme* theme, std::string* error = nullptr);

} // namespace mealkit::ui
