#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_encode_completion_queue {
    uint16_t* storage;
    uint16_t  capacity;
    uint16_t  read_index;
    uint16_t  write_index;
    uint16_t  count;
};

void wd_encode_completion_queue_init(struct wd_encode_completion_queue* queue, uint16_t* storage, uint16_t capacity);
bool wd_encode_completion_queue_push(struct wd_encode_completion_queue* queue, uint16_t job_index);
bool wd_encode_completion_queue_pop(struct wd_encode_completion_queue* queue, uint16_t* out_job_index);

#ifdef __cplusplus
}
#endif
