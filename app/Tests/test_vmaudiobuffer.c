#include "VMAudioBuffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static vm_audio_buffer_t pcm;
static void start(void) {
    CHECK(vm_audio_buffer_init(&pcm));
    vm_audio_buffer_active(&pcm, true);
    atomic_store(&pcm.host_running, true);
    vm_audio_render(&pcm, NULL, NULL, 0);
    float x[2] = {0, 0};
    for (unsigned i = 0; i < VM_AUDIO_PREFILL; i++)
        vm_audio_guest_frame(&pcm, 0, 48000, x);
    vm_audio_render(&pcm, NULL, NULL, VM_AUDIO_PREFILL);
}
static void test_queue(void) {
    start();
    float x[2] = {0.25f, -0.5f}, l[64], r[64];
    for (unsigned wrap = 0; wrap < 1000; wrap++) {
        for (unsigned i = 0; i < 64; i++) vm_audio_guest_frame(&pcm, 0, 48000, x);
        vm_audio_render(&pcm, l, r, 64);
        for (unsigned i = 0; i < 64; i++) CHECK(l[i] == x[0] && r[i] == x[1]);
    }
    vm_audio_render(&pcm, l, r, 64);
    CHECK(l[0] > 0 && l[0] < x[0] && r[0] < 0); /* smooth tail */
    vm_audio_render(&pcm, l, r, 64);
    CHECK(l[63] == 0 && r[63] == 0);
    for (unsigned i = 0; i < VM_AUDIO_FRAMES + 10u; i++) vm_audio_guest_frame(&pcm, 0, 48000, x);
    CHECK(pcm.playback_dropped == 10);
    CHECK(atomic_load(&pcm.playback.write) - atomic_load(&pcm.playback.read) == VM_AUDIO_FRAMES);
    vm_audio_buffer_active(&pcm, false);
    vm_audio_render(&pcm, l, r, 64);
    CHECK(l[0] == 0 && r[63] == 0);
    vm_audio_buffer_active(&pcm, true);
    vm_audio_render(&pcm, l, r, 64);
    CHECK(l[0] == 0 && r[63] == 0); /* no old sound on resume */
    /* Index wrap is unsigned arithmetic, not a signed negative queue size. */
    atomic_store(&pcm.playback.read, UINT32_MAX - 4u);
    atomic_store(&pcm.playback.write, UINT32_MAX - 4u);
    for (unsigned i = 0; i < 64; i++) vm_audio_guest_frame(&pcm, 0, 48000, x);
    vm_audio_render(&pcm, NULL, NULL, VM_AUDIO_PREFILL - 1u);
    vm_audio_render(&pcm, l, r, 64);
    CHECK(l[63] > 0 && r[63] < 0); /* short clip survives prefill + index wrap */
}
static void test_resample_and_privacy(void) {
    start();
    float x[2] = {0.25f, -0.5f};
    for (unsigned i = 0; i <= 441; i++) vm_audio_guest_frame(&pcm, 0, 44100, x);
    CHECK(atomic_load(&pcm.playback.write) - atomic_load(&pcm.playback.read) == 481);
    float l[481], r[481]; vm_audio_render(&pcm, l, r, 481);
    CHECK(l[480] == 0.25f && r[480] == -0.5f);
    float mic[128]; for (unsigned i = 0; i < 128; i++) mic[i] = 0.375f;
    vm_audio_microphone(&pcm, mic, NULL, 128, 48000);
    CHECK(atomic_load(&pcm.microphone.write) == 0); /* off by default */
    atomic_store(&pcm.microphone_enabled, true);
    vm_audio_microphone(&pcm, mic, NULL, 128, 48000);
    vm_audio_guest_frame(&pcm, 1, 16000, x);
    CHECK(x[0] == 0.375f && x[1] == 0.375f);
    for (unsigned i = 0; i < 30; i++) vm_audio_guest_frame(&pcm, 1, 16000, x);
    CHECK(x[0] == 0.375f && x[1] == 0.375f);
    atomic_store(&pcm.microphone_enabled, false);
    vm_audio_guest_frame(&pcm, 1, 16000, x);
    CHECK(x[0] == 0 && x[1] == 0);
    CHECK(atomic_load(&pcm.microphone.read) == atomic_load(&pcm.microphone.write));
    atomic_store(&pcm.host_running, false);
    vm_audio_guest_frame(&pcm, 1, 16000, x);
    CHECK(x[0] == 0 && x[1] == 0);
}
/* The worker produces guest PCM in bursts; the host callback runs on its own
 * clock. Average production here is exactly 48 kHz, with only 8 ms of jitter.
 * Starting with no reserve must not turn that jitter into repeated gaps. */
static void test_jittered_playback(void) {
    CHECK(vm_audio_buffer_init(&pcm));
    vm_audio_buffer_active(&pcm, true);
    atomic_store(&pcm.host_running, true);
    vm_audio_render(&pcm, NULL, NULL, 0);
    float l[480], r[480], x[2] = {0.25f, -0.5f};
    unsigned starved = 0;
    bool heard = false;
    for (unsigned block = 0; block < 200; block++) {
        unsigned produced = (block & 1u) ? 864u : 96u;
        for (unsigned i = 0; i < produced; i++)
            vm_audio_guest_frame(&pcm, 0, 48000, x);
        vm_audio_render(&pcm, l, r, 480);
        for (unsigned i = 0; i < 480; i++) {
            if (l[i] != 0) heard = true;
            else if (heard) starved++;
        }
    }
    printf("Jittered playback: %u silent frames after sound began\n", starved);
    CHECK(starved == 0);
    CHECK(pcm.playback_dropped == 0);
}

static void test_short_clip_and_stall_recovery(void) {
    CHECK(vm_audio_buffer_init(&pcm));
    vm_audio_buffer_active(&pcm, true);
    atomic_store(&pcm.host_running, true);
    vm_audio_render(&pcm, NULL, NULL, 0);
    float x[2] = {0.75f, -0.375f}, l[64], r[64];
    /* A one-sample click or a short keyboard sound must not wait forever for
     * the reserve. No consumer read occurs before the bounded wait expires. */
    vm_audio_guest_frame(&pcm, 0, 48000, x);
    vm_audio_render(&pcm, NULL, NULL, VM_AUDIO_PREFILL - 1u);
    CHECK(atomic_load(&pcm.playback.read) == 0);
    vm_audio_render(&pcm, l, r, 1);
    CHECK(l[0] > 0 && r[0] < 0);
    CHECK(atomic_load(&pcm.playback.read) == 1);
    vm_audio_render(&pcm, NULL, NULL, VM_AUDIO_FADE + 10u);
    CHECK(pcm.render_last[0] == 0 && pcm.render_last[1] == 0);

    for (unsigned i = 0; i < VM_AUDIO_PREFILL; i++)
        vm_audio_guest_frame(&pcm, 0, 48000, x);
    vm_audio_render(&pcm, NULL, NULL, VM_AUDIO_PREFILL);
    CHECK(pcm.render_last[0] == x[0] && pcm.render_last[1] == x[1]);
    uint64_t underruns = pcm.playback_underruns;
    float last = x[0];
    for (unsigned i = 0; i < VM_AUDIO_FADE + 64u; i++) {
        vm_audio_render(&pcm, l, r, 1);
        CHECK(l[0] <= last && l[0] >= 0);
        CHECK(fabsf(l[0] - last) <= 1.f / VM_AUDIO_FADE);
        CHECK(r[0] == -0.5f * l[0]);
        last = l[0];
    }
    CHECK(last == 0 && pcm.playback_underruns == underruns + 1u);
    for (unsigned i = 0; i < VM_AUDIO_PREFILL; i++)
        vm_audio_guest_frame(&pcm, 0, 48000, x);
    for (unsigned i = 0; i < VM_AUDIO_FADE; i++) {
        vm_audio_render(&pcm, l, r, 1);
        CHECK(l[0] >= last && l[0] <= x[0]);
        CHECK(fabsf(l[0] - last) <= 1.f / VM_AUDIO_FADE);
        CHECK(r[0] == -0.5f * l[0]);
        last = l[0];
    }
    CHECK(last == x[0]);
    /* A route epoch must discard the reserve/tail as well as queued PCM. */
    atomic_fetch_add(&pcm.epoch, 1);
    vm_audio_render(&pcm, l, r, 64);
    CHECK(l[0] == 0 && r[63] == 0 && !pcm.playback_started);
    CHECK(pcm.fade_in == 0 && pcm.fade_out == 0 && pcm.playback_wait == 0);
}

static void test_waveform_after_prefill(void) {
    static const uint32_t rates[] = {8000, 16000, 22050, 44100, 48000, 96000};
    for (unsigned rateIndex = 0; rateIndex < sizeof rates / sizeof rates[0]; rateIndex++) {
        CHECK(vm_audio_buffer_init(&pcm));
        vm_audio_buffer_active(&pcm, true);
        atomic_store(&pcm.host_running, true);
        vm_audio_render(&pcm, NULL, NULL, 0);
        uint32_t rate = rates[rateIndex];
        /* Exact integer-ratio period. Validate rate, phase and stereo balance,
         * not just nonzero output: prefill must not stretch or pitch-shift. */
        for (unsigned i = 0; i <= (rate + 19u) / 20u; i++) {
            float v = 0.5f * sinf(2.f * 3.14159265358979323846f * 400.f * i / rate);
            float x[2] = {v, -v};
            vm_audio_guest_frame(&pcm, 0, rate, x);
        }
        float l[2401], r[2401];
        vm_audio_render(&pcm, l, r, 2401);
        CHECK(pcm.playback_underruns == 0 && pcm.playback_dropped == 0);
        for (unsigned i = VM_AUDIO_FADE; i < 2401u; i++) {
            float expected = 0.5f * sinf(2.f * 3.14159265358979323846f * 400.f * i / 48000.f);
            CHECK(fabsf(l[i] - expected) < 0.007f);
            CHECK(r[i] == -l[i]);
        }
    }
}
int main(void) {
    test_queue(); test_resample_and_privacy();
    test_jittered_playback();
    test_short_clip_and_stall_recovery();
    test_waveform_after_prefill();
    printf("Audio handoff: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
