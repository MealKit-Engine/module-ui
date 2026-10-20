#pragma once

#include <string>

// Penpot 배치 비교 (ROADMAP "게임 화면 UI 2부" 검증): Penpot에서 내보낸 화면(assets/ui/*.ui, 첫 줄 "# MealKit UI exported from Penpot")마다
// 플러그인이 같이 내보낸 상자(art/ui/boxes/<화면>.boxes, Penpot이 계산한 노드별 위치·크기)와 엔진 ui::System의 배치를
// 보드 크기·배율 1에서 id로 맞춰 본다. 허용 오차는 0.5 논리 픽셀, 글자 노드는 글꼴 래스터 차이로 kTextTolerance.
// 바인딩이 있는 글자({필드}), 목록 안, 엔진에서 보이지 않는 노드(visible 바인딩)는 건너뛴다.
//   MealKitEditor.exe --project <폴더> --ui-check   → 화면마다 "UI-CHECK ..." 줄, 다르면 종료 코드 2
namespace mealkit::editor {

inline constexpr float kBoxTolerance = 0.5f;
inline constexpr float kTextTolerance = 2.0f;

// 비교한 화면이 하나도 없으면 0 ("UI-CHECK none")
int runUiCheck(const std::string& projectDir, const std::string& language);

} // namespace mealkit::editor
