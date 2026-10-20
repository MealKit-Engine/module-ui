#pragma once

#include "mealkit/core/Math.h"
#include "mealkit/core/Reflect.h"
#include "mealkit/core/Types.h"

#include <string>
#include <vector>

namespace mealkit {
class Input;
namespace render {
class Renderer;
struct Viewport;
} // namespace render
namespace loc {
class Strings;
}
namespace input {
class Actions;
}
} // namespace mealkit

// 게임 화면 UI 런타임 (ROADMAP 1단계 "게임 화면 UI", docs/CODING_MODEL.md "게임 UI를 만드는 방법"·"게임 코드와 UI 연결").
//   화면 = assets/ui/*.ui 한 개 (mealkit/ui/Document.h). 엔진이 유지 모드로 배치하고 3D 다음에 2D 패스 하나로 그린다.
//   게임 코드와 연결은 단방향 뷰 모델 + 액션 하나뿐이다:
//     MEALKIT_VIEWMODEL(HudVm) { MEALKIT_FIELD(u32, score, 0, "점수"); };
//     m_hud = ctx.ui.open("ui/hud.ui");  ctx.ui.bind(m_hud, m_hudVm);   // 한 번
//     fillHud(world, m_hudVm);                                          // 매 프레임: 상태 → 뷰 모델
//     for (const ui::Action& a : ctx.ui.actions()) onUiAction(a);       // 버튼·입력 위젯이 낸 액션
//   UI는 시뮬레이션 상태를 고치지 않는다. 입력 위젯(토글·슬라이더·선택)만 자기 뷰 모델 필드에 값을 쓰고 액션을 낸다.
//   좌표는 보드(기준 1920×1080)의 논리 단위, 배율 = min(가로/보드 가로, 세로/보드 세로) × UI 배율.
//   파일(.ui, 테마, 글꼴, 이미지)이 바뀌면 다시 읽는다. 조작은 마우스, 그리고 입력 액션 ui.up/down/left/right/confirm/back(키보드·게임패드).
//   글자 입력(textInput)은 키보드로 친 글자(Input::text)를 문자열 필드에 쓰고, Enter면 액션을 낸다 (치는 동안 게임은 키보드를 받지 않는다).
namespace mealkit::ui {

using ScreenId = u32;
inline constexpr ScreenId kNoScreen = 0;

struct Action {
    std::string name;   // .ui의 버튼 액션 (입력 위젯은 지정한 액션 또는 "changed")
    ScreenId screen = kNoScreen;
    std::string node;   // 낸 노드 이름
    std::string field;  // 입력 위젯: 바뀐 뷰 모델 필드
    i32 listIndex = -1; // 목록 안이면 몇 번째 항목인가
};

struct FrameInput {
    const Input* input = nullptr;          // 마우스 (없으면 마우스 없음)
    const input::Actions* actions = nullptr; // ui.* 액션 (없으면 키보드·게임패드 조작 없음)
    bool mouseAllowed = true, keyboardAllowed = true;
};

// 미리보기 패널과 검사에 쓰는 정보
struct ScreenInfo {
    std::string path;
    u32 nodes = 0;         // 펼친 노드 수 (목록 항목 포함, 예산 500)
    u32 visibleNodes = 0;
    std::vector<std::string> errors; // 파일 오류, 바인딩 오류(없는 필드), 없는 글꼴·이미지
    bool exported = false;
};

struct NodeBox {
    std::string name;
    std::string id; // Penpot 도형 id (내보낸 파일만)
    f32 x, y, w, h; // 논리 단위 (보드 왼쪽 위 기준)
    u32 depth;
    bool visible;
    bool text; // 글자 노드 (배치 비교는 글꼴 차이만큼 느슨하게 본다)
};

class System {
public:
    System();
    ~System();
    System(const System&) = delete;
    System& operator=(const System&) = delete;

    // 렌더러(아틀라스를 올릴 곳, 없으면 배치만), 문자열 표(글자 키)
    void init(render::Renderer* renderer, const loc::Strings* strings);

    // 화면 열기·닫기. layer가 큰 화면이 위에 그려지고 먼저 입력을 받는다
    ScreenId open(const std::string& uiPath, i32 layer = 0);
    // 월드 위 UI의 템플릿 (이름표 하나): 화면으로 그리지 않고 drawAt으로만, 입력은 받지 않는다
    ScreenId openWorld(const std::string& uiPath);
    void close(ScreenId screen);
    bool isOpen(ScreenId screen) const;
    void closeAll();
    // 뷰 모델 묶기 (MEALKIT_VIEWMODEL 값, 화면이 열려 있는 동안 살아 있어야 한다). 입력 위젯이 여기에 값을 쓴다
    void bind(ScreenId screen, reflect::View viewModel);
    template <class Vm>
    void bind(ScreenId screen, Vm& viewModel) {
        bind(screen, reflect::View{&reflect::typeOf<Vm>(), &viewModel});
    }
    void setScale(f32 uiScale); // 설정의 UI 배율 (기본 1)
    f32 scale() const;

    // 엔진이 매 프레임: 입력 → 상태·액션 (게임 update 전). viewport는 UI를 그릴 화면 영역(픽셀)
    void update(const FrameInput& in, const render::Viewport& viewport, f64 dt);
    // 이번 프레임에 난 액션 (다음 update까지)
    const std::vector<Action>& actions() const;
    // 마우스가 UI 위에 있다 / 포커스된 위젯이 있다 (게임은 그 입력을 받지 않는다)
    bool wantsMouse() const;
    bool wantsKeyboard() const;

    // 월드 위 UI (이름표·데미지 숫자): 이번 프레임에 template 화면을 3D 위치에 하나 그린다 (게임이 buildFrame에서).
    // 엔진이 buildFrame 다음에 그 프레임의 카메라를 setCamera로 넘기고, render가 화면 위치로 옮긴다
    void setCamera(const Mat4& viewProj);
    void drawAt(ScreenId templateScreen, Vec3 worldPosition, reflect::View viewModel, bool clampToEdges = false);
    template <class Vm>
    void drawAt(ScreenId templateScreen, Vec3 worldPosition, const Vm& viewModel, bool clampToEdges = false) {
        drawAt(templateScreen, worldPosition, reflect::View{&reflect::typeOf<Vm>(), &viewModel}, clampToEdges);
    }

    // 배치하고 그린다 (3D 다음, 디버그 UI 전). 렌더러 프레임 안에서
    void render(render::Renderer& renderer);

    // 그리지 않고 배치만 한다 (시험, 배치 비교 검사). render도 같은 배치를 쓴다
    void layout(const render::Viewport& viewport);

    // 진단 (에디터 [UI] 패널, 배치 비교 검사)
    ScreenInfo info(ScreenId screen) const;
    std::vector<NodeBox> boxes(ScreenId screen) const;
    void setFocusVisible(bool always); // 미리보기: 포커스 표시를 항상 보인다
    u32 quadCount() const;             // 지난 render의 사각형 수
    // 위젯 조작 시험용: 노드 이름으로 누른 것처럼 (자동 시험·미리보기)
    bool press(ScreenId screen, const std::string& nodeName);
    // 열린 화면 중 위에서부터 이 이름의 위젯을 찾아 누른다 (자동 시험 --ui-press)
    bool pressAny(const std::string& nodeName);
    // 글자 입력 칸에서 글자 치기를 시작한다 (채팅 키 등). 그 노드가 글자 입력이 아니면 false
    bool focusText(ScreenId screen, const std::string& nodeName);
    // 어느 화면에서든 글자를 치는 중이다 (게임은 키보드를 받지 않는다)
    bool editingText() const;

private:
    struct Impl;
    Impl* m;
};

} // namespace mealkit::ui
