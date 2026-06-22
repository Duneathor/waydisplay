#ifndef WAYDISPLAY_SERVER_SELECTION_DELIVERY_H
#define WAYDISPLAY_SERVER_SELECTION_DELIVERY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_selection_delivery {
    uint8_t* text;
    uint32_t size;
    bool     known;
    bool     pending;
};

void wd_selection_delivery_init(struct wd_selection_delivery* delivery);
void wd_selection_delivery_destroy(struct wd_selection_delivery* delivery);
void wd_selection_delivery_mark_unknown(struct wd_selection_delivery* delivery);
bool wd_selection_delivery_set_owned(struct wd_selection_delivery* delivery,
                                     uint8_t* text, uint32_t size);
void wd_selection_delivery_request(struct wd_selection_delivery* delivery);
bool wd_selection_delivery_pending(const struct wd_selection_delivery* delivery,
                                   const uint8_t** text, uint32_t* size);
void wd_selection_delivery_mark_queued(struct wd_selection_delivery* delivery);

#ifdef __cplusplus
}
#endif

#endif
