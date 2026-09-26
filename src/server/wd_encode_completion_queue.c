#include "wd_encode_completion_queue.h"

void wd_encode_completion_queue_init(struct wd_encode_completion_queue* queue, uint16_t* storage, uint16_t capacity) {
    if (!queue)
    {
        return;
    }

    queue->storage     = storage;
    queue->capacity    = capacity;
    queue->read_index  = 0;
    queue->write_index = 0;
    queue->count       = 0;
}

bool wd_encode_completion_queue_push(struct wd_encode_completion_queue* queue, uint16_t job_index) {
    if (!queue || !queue->storage || queue->capacity == 0 || queue->count >= queue->capacity)
    {
        return false;
    }

    queue->storage[queue->write_index] = job_index;
    queue->write_index                 = (uint16_t)((queue->write_index + 1u) % queue->capacity);
    queue->count++;
    return true;
}

bool wd_encode_completion_queue_pop(struct wd_encode_completion_queue* queue, uint16_t* out_job_index) {
    if (!queue || !out_job_index || !queue->storage || queue->capacity == 0 || queue->count == 0)
    {
        return false;
    }

    *out_job_index   = queue->storage[queue->read_index];
    queue->read_index = (uint16_t)((queue->read_index + 1u) % queue->capacity);
    queue->count--;
    return true;
}
