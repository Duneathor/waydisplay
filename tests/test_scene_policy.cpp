#include "wd_scene_policy.h"

#include <cassert>

static void test_popup_constraint_box() {
    wd_scene_policy_box box{};

    wd_scene_policy_popup_constraint_box(100, 50, 8, 24, 1920, 1080, &box);
    assert(box.x == -92);
    assert(box.y == -26);
    assert(box.width == 1920);
    assert(box.height == 1080);

    wd_scene_policy_popup_constraint_box(-20, -10, 0, 0, 1366, 768, &box);
    assert(box.x == 20);
    assert(box.y == 10);
    assert(box.width == 1366);
    assert(box.height == 768);

    wd_scene_policy_popup_constraint_box(0, 0, -5, -7, 1, 1, &box);
    assert(box.x == -5);
    assert(box.y == -7);
    assert(box.width == 1);
    assert(box.height == 1);

    wd_scene_policy_popup_constraint_box(1, 2, 3, 4, 5, 6, nullptr);
}

static void test_focus_candidate() {
    assert(wd_scene_policy_focus_candidate(true, false, false, false));
    assert(!wd_scene_policy_focus_candidate(false, false, false, false));
    assert(!wd_scene_policy_focus_candidate(true, true, false, false));
    assert(!wd_scene_policy_focus_candidate(true, false, true, false));
    assert(!wd_scene_policy_focus_candidate(true, false, false, true));
}

int main() {
    test_popup_constraint_box();
    test_focus_candidate();
    return 0;
}
