#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_zstd_compressor;

size_t                     wd_zstd_compress_bound(size_t src_size);
struct wd_zstd_compressor* wd_zstd_compressor_create(void);
void                       wd_zstd_compressor_destroy(struct wd_zstd_compressor* compressor);
bool wd_zstd_compress_with_context(struct wd_zstd_compressor* compressor, const void* src, size_t src_size, void* dst, size_t dst_capacity,
                                   int level, uint32_t* out_compressed_size);

bool wd_zstd_compress(const void* src, size_t src_size, void* dst, size_t dst_capacity, int level, uint32_t* out_compressed_size);

bool wd_zstd_decompress(const void* src, size_t src_size, void* dst, size_t dst_capacity, uint32_t expected_decompressed_size);

const char* wd_zstd_error_name(size_t zstd_result);

#ifdef __cplusplus
}
#endif
