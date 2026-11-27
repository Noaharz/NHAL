/* audio.output: Gerät öffnen, kurz Stille ausgeben, Frames zählen. Ohne Gerät: Grund melden. */
#include "nhal/features/audio/output.h"
#include "check.h"

#if NHAL_HAS_TIME_MONOTONIC
#include "nhal/features/time/monotonic.h"
#endif

static int g_calls = 0;
static uint32_t g_channels = 0;

static void render(void* user, float* samples, uint32_t frames, uint32_t channels) {
    (void)user;
    ++g_calls;
    g_channels = channels;
    for (uint32_t i = 0; i < frames * channels; ++i) samples[i] = 0.0f; /* Stille */
}

int main(void) {
    const nhal_request req[] = {
        { NHAL_FEATURE_AUDIO_OUTPUT, 1u, NHAL_REQUIRED },
        { NHAL_FEATURE_TIME_MONOTONIC, 1u, NHAL_OPTIONAL },
    };
    const nhal_manifest m = { req, 2 };
    const nhal_init_info info = { sizeof info, &m, NULL };
    nhal_context* ctx = NULL;
    CHECK_OK(nhal_init(&info, &ctx));
    const nhal_audio_output_v1* A = nhal_get_audio_output_v1(ctx);
    if (!A) return check_report("audio_test");

    nhal_audio_stream* s = NULL;
    CHECK(A->open(NULL, &s, NULL).code == NHAL_ERR_INVALID_ARGUMENT);
    const nhal_audio_config bad = { 0, 64, 0, render, NULL };
    CHECK(A->open(&bad, &s, NULL).code == NHAL_ERR_INVALID_ARGUMENT);

    const nhal_audio_config cfg = { 0, 2, 512, render, NULL };
    nhal_audio_format fmt = { 0, 0, 0 };
    const nhal_status st = A->open(&cfg, &s, &fmt);
    if (st.code == NHAL_ERR_UNAVAILABLE) {
        printf("audio_test: kein Gerät – %s\n", st.reason);
        nhal_shutdown(ctx);
        return check_report("audio_test (ohne Gerät)");
    }
    CHECK_OK(st);
    if (!s) return check_report("audio_test");
    printf("audio_test: %u Hz, %u Kanäle, %u Frames je Puffer\n", fmt.sample_rate, fmt.channels, fmt.frames_per_buffer);
    CHECK(fmt.sample_rate >= 8000 && fmt.channels == 2);
    CHECK(A->state(s) == NHAL_AUDIO_STOPPED);
    CHECK_OK(A->start(s));
    CHECK(A->state(s) != NHAL_AUDIO_STOPPED);
#if NHAL_HAS_TIME_MONOTONIC && !defined(__EMSCRIPTEN__)
    const nhal_time_monotonic_v1* T = nhal_get_time_monotonic_v1(ctx);
    if (T) {
        const uint64_t until = T->now_ns() + 400000000ull; /* 0,4 s */
        while (T->now_ns() < until && A->frames_rendered(s) < fmt.sample_rate / 10) {}
        CHECK(A->frames_rendered(s) > 0);
        CHECK(g_calls > 0 && g_channels == 2);
    }
#endif
    CHECK_OK(A->stop(s));
    CHECK(A->state(s) == NHAL_AUDIO_STOPPED);
    A->close(s);
    nhal_shutdown(ctx);
    return check_report("audio_test");
}
