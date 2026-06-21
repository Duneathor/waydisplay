#include "waydisplay/wd_zstd.h"

#include <zstd.h>

#include <stdlib.h>

struct wd_zstd_compressor {
    ZSTD_CCtx* context;
};


struct wd_zstd_compressor* wd_zstd_compressor_create(void) {
    struct wd_zstd_compressor* compressor = calloc(1, sizeof(*compressor));
    if (!compressor)
    {
        return NULL;
    }
    compressor->context = ZSTD_createCCtx();
    if (!compressor->context)
    {
        free(compressor);
        return NULL;
    }
    return compressor;
}

void wd_zstd_compressor_destroy(struct wd_zstd_compressor* compressor) {
    if (!compressor)
    {
        return;
    }
    ZSTD_freeCCtx(compressor->context);
    free(compressor);
}

bool wd_zstd_compress_with_context(struct wd_zstd_compressor* compressor, const void* src, size_t src_size,
                                   void* dst, size_t dst_capacity, int level,
                                   uint32_t* out_compressed_size) {
    if (out_compressed_size)
    {
        *out_compressed_size = 0;
    }
    if (!compressor || !compressor->context || !src || src_size == 0 || !dst || dst_capacity == 0 ||
        !out_compressed_size)
    {
        return false;
    }
    const size_t result = ZSTD_compressCCtx(compressor->context, dst, dst_capacity, src, src_size, level);
    if (ZSTD_isError(result) || result > UINT32_MAX)
    {
        return false;
    }
    *out_compressed_size = (uint32_t)result;
    return true;
}

size_t wd_zstd_compress_bound(size_t src_size) {
    return ZSTD_compressBound(src_size);
}

bool wd_zstd_compress(const void* src, size_t src_size, void* dst, size_t dst_capacity, int level, uint32_t* out_compressed_size) {
    if (out_compressed_size)
    {
        *out_compressed_size = 0;
    }

    if (!src || src_size == 0 || !dst || dst_capacity == 0 || !out_compressed_size)
    {
        return false;
    }

    size_t result = ZSTD_compress(dst, dst_capacity, src, src_size, level);

    if (ZSTD_isError(result))
    {
        return false;
    }

    if (result > UINT32_MAX)
    {
        return false;
    }

    *out_compressed_size = (uint32_t)result;

    return true;
}

bool wd_zstd_decompress(const void* src, size_t src_size, void* dst, size_t dst_capacity, uint32_t expected_decompressed_size) {
    if (!src || src_size == 0 || !dst || dst_capacity == 0)
    {
        return false;
    }

    size_t result = ZSTD_decompress(dst, dst_capacity, src, src_size);

    if (ZSTD_isError(result))
    {
        return false;
    }

    return result == expected_decompressed_size;
}

const char* wd_zstd_error_name(size_t zstd_result) {
    return ZSTD_getErrorName(zstd_result);
}
