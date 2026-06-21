#include "audio_video_sync.hpp"
#include <cassert>
using namespace waydisplay;
int main() {
    const uint64_t playhead = 48000;
    assert(client_video_audio_sync_decide(1000000, playhead) == ClientVideoAudioSyncDecision::Present);
    assert(client_video_audio_sync_decide(1050000, playhead) == ClientVideoAudioSyncDecision::Hold);
    assert(client_video_audio_sync_decide(900000, playhead) == ClientVideoAudioSyncDecision::Drop);
    assert(client_video_audio_sync_decide(1000000, playhead, 0) == ClientVideoAudioSyncDecision::Present);
    return 0;
}
