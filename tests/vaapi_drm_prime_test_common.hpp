#pragma once

#include <gbm.h>
#include <drm_fourcc.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace waydisplay::test::vaapi_drm {

constexpr uint32_t kWidth = 256;
constexpr uint32_t kHeight = 256;

struct Node {
    std::string path;
    int fd = -1;
    gbm_device* gbm = nullptr;
    VADisplay display = nullptr;
    int va_major = 0;
    int va_minor = 0;

    Node() = default;
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    ~Node() {
        if (display)
            vaTerminate(display);
        if (gbm)
            gbm_device_destroy(gbm);
        if (fd >= 0)
            close(fd);
    }
};

struct Buffer {
    gbm_bo* bo = nullptr;
    int fd = -1;
    uint32_t stride = 0;
    uint64_t modifier = DRM_FORMAT_MOD_INVALID;
    uint32_t object_size = 0;
    uint32_t usage = 0;

    Buffer() = default;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    ~Buffer() {
        if (fd >= 0)
            close(fd);
        if (bo)
            gbm_bo_destroy(bo);
    }
};

inline bool open_node(const std::string& path, Node& node) {
    node.path = path;
    node.fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (node.fd < 0)
        return false;

    node.gbm = gbm_create_device(node.fd);
    if (!node.gbm)
        return false;

    node.display = vaGetDisplayDRM(node.fd);
    if (!node.display)
        return false;

    const VAStatus status = vaInitialize(node.display, &node.va_major, &node.va_minor);
    if (status != VA_STATUS_SUCCESS)
    {
        node.display = nullptr;
        return false;
    }
    return true;
}

inline uint32_t object_size_for(int fd, uint32_t stride) {
    struct stat st{};
    if (fstat(fd, &st) == 0 && st.st_size > 0 && static_cast<uint64_t>(st.st_size) <= UINT32_MAX)
        return static_cast<uint32_t>(st.st_size);
    const uint64_t fallback = static_cast<uint64_t>(stride) * kHeight;
    return fallback <= UINT32_MAX ? static_cast<uint32_t>(fallback) : 0;
}

inline bool create_buffer(Node& node, uint32_t usage, Buffer& buffer) {
    buffer.usage = usage;
    buffer.bo = gbm_bo_create(node.gbm, kWidth, kHeight, GBM_FORMAT_XRGB8888, usage);
    if (!buffer.bo)
        return false;

    buffer.stride = gbm_bo_get_stride(buffer.bo);
    buffer.modifier = gbm_bo_get_modifier(buffer.bo);
    buffer.fd = gbm_bo_get_fd(buffer.bo);
    if (buffer.fd < 0)
        return false;
    buffer.object_size = object_size_for(buffer.fd, buffer.stride);
    return buffer.stride >= kWidth * sizeof(uint32_t) && buffer.object_size != 0;
}

inline const char* usage_name(uint32_t usage) {
    return (usage & GBM_BO_USE_LINEAR) != 0 ? "rendering+linear" : "rendering";
}

inline void print_buffer(const Node& node, const Buffer& buffer) {
    std::fprintf(stderr,
                 "DRM PRIME diagnostic: node=%s vendor=%s va=%d.%d usage=%s drm_fourcc=0x%08x va_fourcc=0x%08x stride=%u modifier=0x%016llx object_size=%u\n",
                 node.path.c_str(), vaQueryVendorString(node.display) ? vaQueryVendorString(node.display) : "(unknown)", node.va_major, node.va_minor,
                 usage_name(buffer.usage), DRM_FORMAT_XRGB8888, VA_FOURCC_BGRX,
                 buffer.stride, static_cast<unsigned long long>(buffer.modifier), buffer.object_size);
}

inline VAStatus import_prime2(const Node& node, const Buffer& buffer, VASurfaceID* surface) {
    if (buffer.modifier == DRM_FORMAT_MOD_INVALID)
        return VA_STATUS_ERROR_UNIMPLEMENTED;

    VADRMPRIMESurfaceDescriptor descriptor{};
    descriptor.fourcc = VA_FOURCC_BGRX;
    descriptor.width = kWidth;
    descriptor.height = kHeight;
    descriptor.num_objects = 1;
    descriptor.objects[0].fd = buffer.fd;
    descriptor.objects[0].size = buffer.object_size;
    descriptor.objects[0].drm_format_modifier = buffer.modifier;
    descriptor.num_layers = 1;
    descriptor.layers[0].drm_format = DRM_FORMAT_XRGB8888;
    descriptor.layers[0].num_planes = 1;
    descriptor.layers[0].object_index[0] = 0;
    descriptor.layers[0].offset[0] = 0;
    descriptor.layers[0].pitch[0] = buffer.stride;

    VASurfaceAttrib attrs[2]{};
    attrs[0].type = VASurfaceAttribMemoryType;
    attrs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[0].value.type = VAGenericValueTypeInteger;
    attrs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attrs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attrs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[1].value.type = VAGenericValueTypePointer;
    attrs[1].value.value.p = &descriptor;

    return vaCreateSurfaces(node.display, VA_RT_FORMAT_RGB32, kWidth, kHeight,
                            surface, 1, attrs, 2);
}

inline VAStatus import_legacy(const Node& node, const Buffer& buffer, VASurfaceID* surface) {
    unsigned long buffer_handle = static_cast<unsigned long>(buffer.fd);
    VASurfaceAttribExternalBuffers descriptor{};
    descriptor.pixel_format = VA_FOURCC_BGRX;
    descriptor.width = kWidth;
    descriptor.height = kHeight;
    descriptor.data_size = buffer.object_size;
    descriptor.buffers = &buffer_handle;
    descriptor.num_buffers = 1;
    descriptor.num_planes = 1;
    descriptor.pitches[0] = buffer.stride;
    descriptor.offsets[0] = 0;

    VASurfaceAttrib attrs[2]{};
    attrs[0].type = VASurfaceAttribMemoryType;
    attrs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[0].value.type = VAGenericValueTypeInteger;
    attrs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    attrs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attrs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[1].value.type = VAGenericValueTypePointer;
    attrs[1].value.value.p = &descriptor;

    return vaCreateSurfaces(node.display, VA_RT_FORMAT_RGB32, kWidth, kHeight,
                            surface, 1, attrs, 2);
}

struct ImportResult {
    VASurfaceID surface = VA_INVALID_SURFACE;
    const char* method = nullptr;
    VAStatus prime2_status = VA_STATUS_ERROR_UNIMPLEMENTED;
    VAStatus legacy_status = VA_STATUS_ERROR_UNIMPLEMENTED;
};

inline ImportResult import_any(const Node& node, const Buffer& buffer) {
    ImportResult result{};
    result.prime2_status = import_prime2(node, buffer, &result.surface);
    if (result.prime2_status == VA_STATUS_SUCCESS)
    {
        result.method = "prime2";
        return result;
    }

    result.surface = VA_INVALID_SURFACE;
    result.legacy_status = import_legacy(node, buffer, &result.surface);
    if (result.legacy_status == VA_STATUS_SUCCESS)
        result.method = "legacy";
    return result;
}

struct VppResult {
    VAStatus create_config = VA_STATUS_ERROR_UNIMPLEMENTED;
    VAStatus create_destination = VA_STATUS_ERROR_UNIMPLEMENTED;
    VAStatus create_context = VA_STATUS_ERROR_UNIMPLEMENTED;
    VAStatus create_buffer = VA_STATUS_ERROR_UNIMPLEMENTED;
    VAStatus begin = VA_STATUS_ERROR_UNIMPLEMENTED;
    VAStatus render = VA_STATUS_ERROR_UNIMPLEMENTED;
    VAStatus end = VA_STATUS_ERROR_UNIMPLEMENTED;
    VAStatus sync = VA_STATUS_ERROR_UNIMPLEMENTED;
};

inline bool run_vpp(const Node& node, VASurfaceID source, VppResult& result) {
    VAConfigID config = VA_INVALID_ID;
    VAContextID context = VA_INVALID_ID;
    VASurfaceID destination = VA_INVALID_SURFACE;
    VABufferID params_buffer = VA_INVALID_ID;
    bool began = false;
    bool ok = false;
    VASurfaceAttrib pixel_format{};
    VARectangle rect{0, 0, static_cast<uint16_t>(kWidth), static_cast<uint16_t>(kHeight)};
    VAProcPipelineParameterBuffer params{};

    result.create_config = vaCreateConfig(node.display, VAProfileNone, VAEntrypointVideoProc,
                                          nullptr, 0, &config);
    if (result.create_config != VA_STATUS_SUCCESS)
        goto done;

    pixel_format.type = VASurfaceAttribPixelFormat;
    pixel_format.flags = VA_SURFACE_ATTRIB_SETTABLE;
    pixel_format.value.type = VAGenericValueTypeInteger;
    pixel_format.value.value.i = VA_FOURCC_NV12;
    result.create_destination = vaCreateSurfaces(node.display, VA_RT_FORMAT_YUV420,
                                                  kWidth, kHeight, &destination, 1,
                                                  &pixel_format, 1);
    if (result.create_destination != VA_STATUS_SUCCESS)
        goto done;

    result.create_context = vaCreateContext(node.display, config, kWidth, kHeight,
                                            VA_PROGRESSIVE, nullptr, 0, &context);
    if (result.create_context != VA_STATUS_SUCCESS)
        goto done;

    params.surface = source;
    params.surface_region = &rect;
    params.output_region = &rect;

    result.create_buffer = vaCreateBuffer(node.display, context,
                                          VAProcPipelineParameterBufferType,
                                          sizeof(params), 1, &params, &params_buffer);
    if (result.create_buffer != VA_STATUS_SUCCESS)
        goto done;

    result.begin = vaBeginPicture(node.display, context, destination);
    if (result.begin != VA_STATUS_SUCCESS)
        goto done;
    began = true;

    result.render = vaRenderPicture(node.display, context, &params_buffer, 1);
    if (result.render != VA_STATUS_SUCCESS)
        goto done;

    result.end = vaEndPicture(node.display, context);
    began = false;
    if (result.end != VA_STATUS_SUCCESS)
        goto done;

    result.sync = vaSyncSurface(node.display, destination);
    ok = result.sync == VA_STATUS_SUCCESS;

done:
    if (began)
        (void)vaEndPicture(node.display, context);
    if (params_buffer != VA_INVALID_ID)
        (void)vaDestroyBuffer(node.display, params_buffer);
    if (context != VA_INVALID_ID)
        (void)vaDestroyContext(node.display, context);
    if (destination != VA_INVALID_SURFACE)
        (void)vaDestroySurfaces(node.display, &destination, 1);
    if (config != VA_INVALID_ID)
        (void)vaDestroyConfig(node.display, config);
    return ok;
}

inline void print_import_failure(const ImportResult& result) {
    std::fprintf(stderr, "  import: prime2=%s legacy=%s\n",
                 vaErrorStr(result.prime2_status), vaErrorStr(result.legacy_status));
}

inline void print_vpp_failure(const VppResult& result) {
    std::fprintf(stderr,
                 "  vpp: config=%s destination=%s context=%s buffer=%s begin=%s render=%s end=%s sync=%s\n",
                 vaErrorStr(result.create_config), vaErrorStr(result.create_destination),
                 vaErrorStr(result.create_context), vaErrorStr(result.create_buffer),
                 vaErrorStr(result.begin), vaErrorStr(result.render),
                 vaErrorStr(result.end), vaErrorStr(result.sync));
}

} // namespace waydisplay::test::vaapi_drm
