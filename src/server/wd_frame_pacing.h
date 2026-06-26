#pragma once

#include <stdbool.h>
#include <stdint.h>

uint32_t wd_frame_service_interval_ms(uint16_t capture_fps, uint32_t minimum_ms, uint32_t maximum_ms);
bool     wd_frame_pacing_due(uint64_t last_frame_ns, uint64_t now_ns, uint16_t capture_fps, uint32_t service_interval_ms);
