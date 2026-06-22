#pragma once

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
}

#include <glob.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace waydisplay::test {

inline bool is_render_node_name(const std::string& name) {
    constexpr const char prefix[] = "renderD";
    if (name.compare(0, sizeof(prefix) - 1, prefix) != 0 ||
        name.size() == sizeof(prefix) - 1)
    {
        return false;
    }

    return std::all_of(name.begin() + (sizeof(prefix) - 1), name.end(),
                       [](unsigned char character) {
                           return std::isdigit(character) != 0;
                       });
}

inline bool vaapi_device_opens(const char* path) {
    AVBufferRef* device = nullptr;
    const int rc = av_hwdevice_ctx_create(
        &device, AV_HWDEVICE_TYPE_VAAPI, path, nullptr, 0);
    av_buffer_unref(&device);
    return rc >= 0;
}

inline std::optional<std::string> find_vaapi_test_device() {
    const char* configured = std::getenv("WAYDISPLAY_VAAPI_DEVICE");
    if (configured && *configured)
    {
        if (vaapi_device_opens(configured))
        {
            return std::string(configured);
        }

        std::fprintf(stderr,
                     "configured WAYDISPLAY_VAAPI_DEVICE is unusable: %s\n",
                     configured);
        return std::nullopt;
    }

    glob_t matches{};
    const int glob_result = glob("/dev/dri/renderD*", GLOB_NOSORT, nullptr, &matches);
    std::vector<std::string> candidates;
    if (glob_result == 0)
    {
        for (size_t index = 0; index < matches.gl_pathc; ++index)
        {
            const std::string candidate(matches.gl_pathv[index]);
            const size_t separator = candidate.find_last_of('/');
            const std::string name = separator == std::string::npos
                                         ? candidate
                                         : candidate.substr(separator + 1);
            if (is_render_node_name(name))
            {
                candidates.push_back(candidate);
            }
        }
    }
    globfree(&matches);

    std::sort(candidates.begin(), candidates.end());
    for (const std::string& candidate : candidates)
    {
        if (vaapi_device_opens(candidate.c_str()))
        {
            return candidate;
        }
    }

    return std::nullopt;
}

} // namespace waydisplay::test
