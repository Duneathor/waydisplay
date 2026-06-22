#include "wd_audio_routing.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

void test_builds_process_scoped_fail_closed_routing() {
    wd_audio_routing_env env{};
    require(wd_audio_routing_env_build(&env,
                                       "waydisplay.audio.sink.1234",
                                       "waydisplay.audio.sink.1234",
                                       1234),
            "valid private sink routing should build");
    require(env.enabled, "private sink routing should be enabled");
    require(std::strcmp(env.pulse_sink, "waydisplay.audio.sink.1234") == 0,
            "Pulse target should use the private sink name");
    require(std::strcmp(env.pipewire_target,
                        "waydisplay.audio.sink.1234") == 0,
            "native PipeWire target should use the private sink name");
    require(std::strcmp(env.scope, "waydisplay.1234") == 0,
            "routing scope should identify the owning server");

    const std::string pulse_props = env.pulse_props;
    require(pulse_props.find("node.dont-fallback=true") != std::string::npos,
            "Pulse streams should fail closed");
    require(pulse_props.find("waydisplay.audio.scope=waydisplay.1234") !=
                std::string::npos,
            "Pulse streams should carry the server scope");

    const std::string pipewire_props = env.pipewire_props;
    require(pipewire_props.find(
                "target.object = \"waydisplay.audio.sink.1234\"") !=
                std::string::npos,
            "native streams should explicitly target the private sink");
    require(pipewire_props.find("node.dont-fallback = true") !=
                std::string::npos,
            "native streams should not fall back to local playback");
    require(pipewire_props.find("node.dont-reconnect = true") !=
                std::string::npos,
            "native streams should not reconnect to another sink");
    require(pipewire_props.find("node.dont-move = true") !=
                std::string::npos,
            "native streams should not be moved by restored metadata");
    require(pipewire_props.find(
                "waydisplay.audio.scope = \"waydisplay.1234\"") !=
                std::string::npos,
            "native streams should carry the server scope");
}

void test_leaves_unavailable_audio_unrouted() {
    wd_audio_routing_env env{};
    require(wd_audio_routing_env_build(&env, nullptr, nullptr, 55),
            "missing backend should produce a valid disabled environment");
    require(!env.enabled, "missing backend must not install partial routing");
    require(env.pulse_sink[0] == '\0' && env.pipewire_target[0] == '\0' &&
                env.pulse_props[0] == '\0' && env.pipewire_props[0] == '\0',
            "disabled routing should not leak inherited values");
}

void test_rejects_unquoted_property_injection() {
    wd_audio_routing_env env{};
    require(!wd_audio_routing_env_build(&env,
                                        "safe.sink",
                                        "safe.sink\" node.dont-fallback=false",
                                        77),
            "unsafe target characters should be rejected");
    require(!wd_audio_routing_env_build(&env, "unsafe sink", "safe.sink", 77),
            "unsafe sink characters should be rejected");
    require(!wd_audio_routing_env_build(&env, "safe.sink", "safe.sink", 0),
            "invalid owner PID should be rejected");
}

void test_rejects_truncated_identifiers() {
    wd_audio_routing_env env{};
    const std::string oversized(WD_AUDIO_ROUTING_TARGET_MAX, 'a');
    require(!wd_audio_routing_env_build(&env,
                                        "waydisplay.audio.sink.1",
                                        oversized.c_str(),
                                        1),
            "routing identifiers must never be silently truncated");
}
}

int main() {
    test_builds_process_scoped_fail_closed_routing();
    test_leaves_unavailable_audio_unrouted();
    test_rejects_unquoted_property_injection();
    test_rejects_truncated_identifiers();
    return 0;
}
