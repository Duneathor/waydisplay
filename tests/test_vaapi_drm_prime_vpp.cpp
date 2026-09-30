#include "vaapi_drm_prime_test_common.hpp"

#include <cstdio>
#include <string>

using namespace waydisplay::test::vaapi_drm;

int main() {
#if !defined(__linux__)
    return 77;
#else
    bool imported_any = false;
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

        for (uint32_t usage : usages)
        {
            Buffer buffer;
            if (!create_buffer(node, usage, buffer))
                continue;
            print_buffer(node, buffer);

            ImportResult imported = import_any(node, buffer);
            if (imported.surface == VA_INVALID_SURFACE)
            {
                print_import_failure(imported);
                continue;
            }
            imported_any = true;
            std::fprintf(stderr, "  import: method=%s success\n", imported.method);

            VppResult vpp{};
            const bool converted = run_vpp(node, imported.surface, vpp);
            (void)vaDestroySurfaces(node.display, &imported.surface, 1);
            if (converted)
            {
                std::fprintf(stderr, "PASS: imported RGB dma-buf -> NV12 VPP node=%s method=%s\n",
                             node.path.c_str(), imported.method);
                return 0;
            }
            print_vpp_failure(vpp);
        }
    }

    if (!imported_any)
    {
        std::fprintf(stderr, "FAIL: no dma-buf reached the VPP stage because VAAPI import failed\n");
        return 1;
    }
    std::fprintf(stderr, "FAIL: VAAPI dma-buf import succeeded, but RGB -> NV12 VPP failed on every candidate\n");
    return 1;
#endif
}
