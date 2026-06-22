#include "wd_scene_policy.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void test_popup_constraint_box() {
    wd_scene_policy_box box{};

    wd_scene_policy_popup_constraint_box(100, 50, 8, 24, 1920, 1080, &box);
    require(box.x == -92, "popup x constraint should include the parent offset");
    require(box.y == -26, "popup y constraint should include the parent offset");
    require(box.width == 1920, "popup constraint should retain output width");
    require(box.height == 1080, "popup constraint should retain output height");

    wd_scene_policy_popup_constraint_box(-20, -10, 0, 0, 1366, 768, &box);
    require(box.x == 20, "negative view x should move the constraint origin right");
    require(box.y == 10, "negative view y should move the constraint origin down");
    require(box.width == 1366, "second popup constraint should retain output width");
    require(box.height == 768, "second popup constraint should retain output height");

    wd_scene_policy_popup_constraint_box(0, 0, -5, -7, 1, 1, &box);
    require(box.x == -5, "negative surface offset should be preserved on x");
    require(box.y == -7, "negative surface offset should be preserved on y");
    require(box.width == 1, "minimum output width should be accepted");
    require(box.height == 1, "minimum output height should be accepted");

    wd_scene_policy_popup_constraint_box(1, 2, 3, 4, 5, 6, nullptr);
}

void test_focus_candidate() {
    require(wd_scene_policy_focus_candidate(true, false, false, false),
            "a mapped focusable view should be a focus candidate");
    require(!wd_scene_policy_focus_candidate(false, false, false, false),
            "an unmapped view should not be a focus candidate");
    require(!wd_scene_policy_focus_candidate(true, true, false, false),
            "an override-redirect view should not be a focus candidate");
    require(!wd_scene_policy_focus_candidate(true, false, true, false),
            "a minimized view should not be a focus candidate");
    require(!wd_scene_policy_focus_candidate(true, false, false, true),
            "a destroyed view should not be a focus candidate");
}

} // namespace

int main() {
    test_popup_constraint_box();
    test_focus_candidate();
    return EXIT_SUCCESS;
}
