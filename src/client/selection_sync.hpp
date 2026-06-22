#pragma once

#include <string>

namespace waydisplay {

enum class ClientSelectionKind {
    Clipboard,
    Primary,
};

struct ClientSelectionSyncState {
    std::string last_sent_clipboard;
    std::string last_sent_primary;
    std::string last_applied_clipboard;
    std::string last_applied_primary;
    bool        last_sent_clipboard_valid    = false;
    bool        last_sent_primary_valid      = false;
    bool        last_applied_clipboard_valid = false;
    bool        last_applied_primary_valid   = false;
};

void client_selection_sync_reset(ClientSelectionSyncState& state);

bool client_selection_sync_should_send(const ClientSelectionSyncState& state,
                                       ClientSelectionKind kind,
                                       const std::string& text,
                                       bool force);

void client_selection_sync_note_sent(ClientSelectionSyncState& state,
                                     ClientSelectionKind kind,
                                     const std::string& text);

bool client_selection_sync_should_apply(const ClientSelectionSyncState& state,
                                        ClientSelectionKind kind,
                                        const std::string& text);

/* A successful remote apply also becomes the most recently sent value. This
 * suppresses echoes on platforms which report an application-owned clipboard
 * update as external. Forced paste sends still bypass this suppression. */
void client_selection_sync_note_applied(ClientSelectionSyncState& state,
                                        ClientSelectionKind kind,
                                        const std::string& text);

} // namespace waydisplay
