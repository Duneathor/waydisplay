#pragma once

#include <stdbool.h>
#include <stdint.h>

struct wd_frame_pacing_state {
    uint64_t next_deadline_ns;
    uint16_t capture_fps;
};

uint16_t wd_frame_rate_normalize_client_request(uint16_t requested_fps);
uint32_t wd_frame_service_interval_ms(uint16_t capture_fps, uint32_t minimum_ms, uint32_t maximum_ms);
void     wd_frame_pacing_reset(struct wd_frame_pacing_state* state);
bool     wd_frame_pacing_due(struct wd_frame_pacing_state* state, uint64_t now_ns, uint16_t capture_fps);
