#include "selection_sync.hpp"

#include <cstdio>

using namespace waydisplay;

namespace {
int failures = 0;
#define CHECK(condition)                                                                          \
    do                                                                                            \
    {                                                                                             \
        if (!(condition))                                                                         \
        {                                                                                         \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures++;                                                                           \
        }                                                                                         \
    } while (false)

void test_outbound_dedupe_and_force() {
    ClientSelectionSyncState state;
    CHECK(client_selection_sync_should_send(state, ClientSelectionKind::Clipboard,
                                            "one", false));
    client_selection_sync_note_sent(state, ClientSelectionKind::Clipboard, "one");
    CHECK(!client_selection_sync_should_send(state, ClientSelectionKind::Clipboard,
                                             "one", false));
    CHECK(client_selection_sync_should_send(state, ClientSelectionKind::Clipboard,
                                            "one", true));
    CHECK(client_selection_sync_should_send(state, ClientSelectionKind::Clipboard,
                                            "two", false));

    CHECK(client_selection_sync_should_send(state, ClientSelectionKind::Primary,
                                            "one", false));
}

void test_remote_apply_suppresses_echo() {
    ClientSelectionSyncState state;
    CHECK(client_selection_sync_should_apply(state, ClientSelectionKind::Clipboard,
                                             "remote"));
    client_selection_sync_note_applied(state, ClientSelectionKind::Clipboard,
                                       "remote");
    CHECK(!client_selection_sync_should_apply(state, ClientSelectionKind::Clipboard,
                                              "remote"));
    CHECK(!client_selection_sync_should_send(state, ClientSelectionKind::Clipboard,
                                             "remote", false));
    CHECK(client_selection_sync_should_send(state, ClientSelectionKind::Clipboard,
                                            "remote", true));
    CHECK(client_selection_sync_should_apply(state, ClientSelectionKind::Clipboard,
                                             "changed"));
}

void test_empty_and_reset() {
    ClientSelectionSyncState state;
    CHECK(client_selection_sync_should_send(state, ClientSelectionKind::Clipboard,
                                            "", false));
    client_selection_sync_note_sent(state, ClientSelectionKind::Clipboard, "");
    CHECK(!client_selection_sync_should_send(state, ClientSelectionKind::Clipboard,
                                             "", false));
    client_selection_sync_reset(state);
    CHECK(client_selection_sync_should_send(state, ClientSelectionKind::Clipboard,
                                            "", false));
}
}

int main() {
    test_outbound_dedupe_and_force();
    test_remote_apply_suppresses_echo();
    test_empty_and_reset();
    return failures == 0 ? 0 : 1;
}
