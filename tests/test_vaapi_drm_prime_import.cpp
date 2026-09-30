#include "vaapi_drm_prime_test_common.hpp"

#include <cstdio>
#include <string>

using namespace waydisplay::test::vaapi_drm;

int main() {
#if !defined(__linux__)
    return 77;
#else
    bool opened_vaapi = false;
    bool created_bo = false;
    const uint32_t usages[] = {
        GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR,
        GBM_BO_USE_RENDERING,
    };

    for (unsigned index = 128; index < 192; ++index)
    {
        Node node;
        const std::string path = "/dev/dri/renderD" + std::to_string(index);
        if (!open_node(path, node))
            continue;
        opened_vaapi = true;

        for (uint32_t usage : usages)
        {
            Buffer buffer;
            if (!create_buffer(node, usage, buffer))
                continue;
            created_bo = true;
            print_buffer(node, buffer);

            ImportResult imported = import_any(node, buffer);
            if (imported.surface != VA_INVALID_SURFACE)
            {
                std::fprintf(stderr, "PASS: dma-buf import method=%s node=%s\n",
                             imported.method, node.path.c_str());
                (void)vaDestroySurfaces(node.display, &imported.surface, 1);
                return 0;
            }
            print_import_failure(imported);
        }
    }

    if (!opened_vaapi)
    {
        std::fprintf(stderr, "SKIP: no DRM render node initialized as a VAAPI display\n");
        return 77;
    }
    if (!created_bo)
    {
        std::fprintf(stderr, "SKIP: no single-plane GBM XRGB8888 dma-buf could be exported\n");
        return 77;
    }

    std::fprintf(stderr, "FAIL: GBM dma-bufs were exported but neither PRIME_2 nor legacy PRIME import succeeded\n");
    return 1;
#endif
}
