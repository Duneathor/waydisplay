#include "wd_selection_delivery.h"

#include <stdlib.h>
#include <string.h>

void wd_selection_delivery_init(struct wd_selection_delivery* delivery) {
    if (delivery)
    {
        memset(delivery, 0, sizeof(*delivery));
    }
}

void wd_selection_delivery_destroy(struct wd_selection_delivery* delivery) {
    if (!delivery)
    {
        return;
    }

    free(delivery->text);
    memset(delivery, 0, sizeof(*delivery));
}

void wd_selection_delivery_mark_unknown(struct wd_selection_delivery* delivery) {
    if (!delivery)
    {
        return;
    }

    free(delivery->text);
    delivery->text    = NULL;
    delivery->size    = 0;
    delivery->known   = false;
    delivery->pending = false;
}

bool wd_selection_delivery_set_owned(struct wd_selection_delivery* delivery,
                                     uint8_t* text, uint32_t size) {
    if (!delivery || (!text && size != 0))
    {
        free(text);
        return false;
    }

    free(delivery->text);
    delivery->text    = text;
    delivery->size    = size;
    delivery->known   = true;
    delivery->pending = true;
    return true;
}

void wd_selection_delivery_request(struct wd_selection_delivery* delivery) {
    if (delivery && delivery->known)
    {
        delivery->pending = true;
    }
}

bool wd_selection_delivery_pending(const struct wd_selection_delivery* delivery,
                                   const uint8_t** text, uint32_t* size) {
    if (text)
    {
        *text = NULL;
    }
    if (size)
    {
        *size = 0;
    }

    if (!delivery || !text || !size || !delivery->known || !delivery->pending)
    {
        return false;
    }

    *text = delivery->text;
    *size = delivery->size;
    return true;
}

void wd_selection_delivery_mark_queued(struct wd_selection_delivery* delivery) {
    if (delivery)
    {
        delivery->pending = false;
    }
}
