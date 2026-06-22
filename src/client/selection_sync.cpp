#include "selection_sync.hpp"

namespace waydisplay {
namespace {

std::string& sent_text(ClientSelectionSyncState& state, ClientSelectionKind kind) {
    return kind == ClientSelectionKind::Primary ? state.last_sent_primary
                                                : state.last_sent_clipboard;
}

const std::string& sent_text(const ClientSelectionSyncState& state,
                             ClientSelectionKind kind) {
    return kind == ClientSelectionKind::Primary ? state.last_sent_primary
                                                : state.last_sent_clipboard;
}

bool& sent_valid(ClientSelectionSyncState& state, ClientSelectionKind kind) {
    return kind == ClientSelectionKind::Primary ? state.last_sent_primary_valid
                                                : state.last_sent_clipboard_valid;
}

bool sent_valid(const ClientSelectionSyncState& state, ClientSelectionKind kind) {
    return kind == ClientSelectionKind::Primary ? state.last_sent_primary_valid
                                                : state.last_sent_clipboard_valid;
}

std::string& applied_text(ClientSelectionSyncState& state,
                          ClientSelectionKind kind) {
    return kind == ClientSelectionKind::Primary ? state.last_applied_primary
                                                : state.last_applied_clipboard;
}

const std::string& applied_text(const ClientSelectionSyncState& state,
                                ClientSelectionKind kind) {
    return kind == ClientSelectionKind::Primary ? state.last_applied_primary
                                                : state.last_applied_clipboard;
}

bool& applied_valid(ClientSelectionSyncState& state, ClientSelectionKind kind) {
    return kind == ClientSelectionKind::Primary
               ? state.last_applied_primary_valid
               : state.last_applied_clipboard_valid;
}

bool applied_valid(const ClientSelectionSyncState& state,
                   ClientSelectionKind kind) {
    return kind == ClientSelectionKind::Primary
               ? state.last_applied_primary_valid
               : state.last_applied_clipboard_valid;
}

} // namespace

void client_selection_sync_reset(ClientSelectionSyncState& state) {
    state = ClientSelectionSyncState{};
}

bool client_selection_sync_should_send(const ClientSelectionSyncState& state,
                                       ClientSelectionKind kind,
                                       const std::string& text,
                                       bool force) {
    return force || !sent_valid(state, kind) || sent_text(state, kind) != text;
}

void client_selection_sync_note_sent(ClientSelectionSyncState& state,
                                     ClientSelectionKind kind,
                                     const std::string& text) {
    sent_text(state, kind) = text;
    sent_valid(state, kind) = true;
}

bool client_selection_sync_should_apply(const ClientSelectionSyncState& state,
                                        ClientSelectionKind kind,
                                        const std::string& text) {
    return !applied_valid(state, kind) || applied_text(state, kind) != text;
}

void client_selection_sync_note_applied(ClientSelectionSyncState& state,
                                        ClientSelectionKind kind,
                                        const std::string& text) {
    applied_text(state, kind) = text;
    applied_valid(state, kind) = true;
    client_selection_sync_note_sent(state, kind, text);
}

} // namespace waydisplay
