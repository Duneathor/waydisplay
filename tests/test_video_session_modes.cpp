#include "waydisplay/wd_video_offer.h"
#include "wd_video_transition.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static void check_offer(uint8_t mode, uint8_t decoder, bool expect_video) {
    const auto offer = wd_client_video_offer_decide(mode, decoder,
                                                     WD_VIDEO_CODEC_H264 | WD_VIDEO_CODEC_H265,
                                                     WD_VIDEO_CODEC_H265);
    CHECK(((offer.capabilities & WD_CLIENT_CAP_VIDEO_STREAM) != 0) == expect_video);
    CHECK(((offer.capabilities & WD_CLIENT_CAP_VIDEO_FEEDBACK) != 0) == expect_video);
    CHECK((offer.codecs != 0) == expect_video);
    CHECK((offer.transport == WD_VIDEO_TRANSPORT_TCP) == expect_video);
    CHECK(wd_video_session_bootstrap_required(mode, offer.capabilities) == expect_video);
}

static void test_mode_matrix() {
    check_offer(WD_VIDEO_MODE_AUTO, WD_CLIENT_VIDEO_DECODER_AUTO, true);
    check_offer(WD_VIDEO_MODE_FORCE, WD_CLIENT_VIDEO_DECODER_AUTO, true);
    check_offer(WD_VIDEO_MODE_OFF, WD_CLIENT_VIDEO_DECODER_AUTO, false);
    check_offer(WD_VIDEO_MODE_AUTO, WD_CLIENT_VIDEO_DECODER_OFF, false);
    check_offer(WD_VIDEO_MODE_FORCE, WD_CLIENT_VIDEO_DECODER_OFF, false);
    check_offer(WD_VIDEO_MODE_AUTO, 255, false);

    const auto unsupported = wd_client_video_offer_decide(WD_VIDEO_MODE_AUTO,
                                                           WD_CLIENT_VIDEO_DECODER_AUTO,
                                                           WD_VIDEO_CODEC_H264,
                                                           WD_VIDEO_CODEC_H265);
    CHECK(unsupported.capabilities == 0);
    CHECK(unsupported.codecs == 0);
    CHECK(unsupported.transport == 0);
}

static void test_reconnect_mode_sequences() {
    /* Auto -> hard tiles -> auto must re-advertise video from scratch. */
    auto first = wd_client_video_offer_decide(WD_VIDEO_MODE_AUTO, WD_CLIENT_VIDEO_DECODER_AUTO,
                                               WD_VIDEO_CODEC_H265, WD_VIDEO_CODEC_H265);
    auto tiles = wd_client_video_offer_decide(WD_VIDEO_MODE_OFF, WD_CLIENT_VIDEO_DECODER_AUTO,
                                               WD_VIDEO_CODEC_H265, WD_VIDEO_CODEC_H265);
    auto again = wd_client_video_offer_decide(WD_VIDEO_MODE_AUTO, WD_CLIENT_VIDEO_DECODER_AUTO,
                                               WD_VIDEO_CODEC_H265, WD_VIDEO_CODEC_H265);
    CHECK(first.capabilities != 0);
    CHECK(tiles.capabilities == 0 && tiles.codecs == 0 && tiles.transport == 0);
    CHECK(again.capabilities == first.capabilities && again.codecs == first.codecs);

    /* Decoder-off is also a tiles-only session even when policy remains auto. */
    auto decoder_off = wd_client_video_offer_decide(WD_VIDEO_MODE_AUTO, WD_CLIENT_VIDEO_DECODER_OFF,
                                                     WD_VIDEO_CODEC_H265, WD_VIDEO_CODEC_H265);
    CHECK(decoder_off.capabilities == 0);
    CHECK(!wd_video_session_bootstrap_required(WD_VIDEO_MODE_AUTO, decoder_off.capabilities));
    CHECK(wd_video_session_bootstrap_required(WD_VIDEO_MODE_AUTO, again.capabilities));

    /* Force -> off -> force follows the same isolation rule. */
    auto forced = wd_client_video_offer_decide(WD_VIDEO_MODE_FORCE, WD_CLIENT_VIDEO_DECODER_AUTO,
                                                WD_VIDEO_CODEC_H265, WD_VIDEO_CODEC_H265);
    CHECK(wd_video_session_bootstrap_required(WD_VIDEO_MODE_FORCE, forced.capabilities));
    CHECK(!wd_video_session_bootstrap_required(WD_VIDEO_MODE_OFF, forced.capabilities));
}

int main() {
    test_mode_matrix();
    test_reconnect_mode_sequences();
    std::puts("video session mode transitions: PASS");
    return 0;
}
