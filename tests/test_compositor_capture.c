#include "wd_compositor_capture.h"

#include <assert.h>
#include <string.h>

int main(void) {
    struct wd_compositor_capture_stats s = {0};
    wd_compositor_capture_x11_surface_commit(&s, 1422, 773);
    wd_compositor_capture_x11_surface_commit(&s, 0, 50);
    ++s.x11_maps;
    ++s.x11_unmaps;
    ++s.x11_configures;
    s.x11_decoration_layout_updates = 2;
    s.x11_decoration_layout_reused = 127;
    wd_compositor_capture_scene_build(&s, 5000000);
    wd_compositor_capture_texture_read(&s, 1422, 773, 3000000, true);
    wd_compositor_capture_texture_read(&s, 16, 16, 1000000, false);
    assert(s.x11_surface_commits == 2 && s.x11_commit_bounds_pixels == 1422ull * 773ull);
    assert(s.x11_maps == 1 && s.x11_unmaps == 1 && s.x11_configures == 1);
    assert(s.x11_decoration_layout_updates == 2 && s.x11_decoration_layout_reused == 127);
    assert(s.scene_build_calls == 1 && s.scene_build_ns == 5000000);
    assert(s.texture_read_calls == 2 && s.texture_read_pixels == 1422ull * 773ull + 256);
    assert(s.texture_read_ns == 4000000 && s.texture_read_failures == 1);
    ++s.buffer_data_fallbacks;
    assert(s.buffer_data_fallbacks == 1 && s.texture_read_calls == 2);
    memset(&s, 0, sizeof(s));
    assert(s.x11_surface_commits == 0 && s.texture_read_calls == 0 && s.buffer_data_fallbacks == 0);
    return 0;
}
