/*
 * audio.output – macOS (CoreAudio, Default-Output-Unit).
 *
 * Die Default-Output-Unit folgt dem Standardgerät des Systems und rechnet
 * Format und Rate selbst um. render() der App läuft im Render-Callback auf
 * dem Echtzeit-Thread von CoreAudio.
 */
#include "nhal/features/audio/output.h"
#include "nhal/nhal_backend.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <stdatomic.h>
#include <string.h>

struct nhal_audio_stream {
    AudioComponentInstance unit;
    nhal_audio_render_fn render;
    void* user;
    uint32_t channels;
    _Atomic uint64_t frames;
    atomic_int running;
};

static const nhal_allocator* g_alloc = NULL;

static OSStatus render_cb(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts,
                          UInt32 bus, UInt32 frames, AudioBufferList* io) {
    (void)ts;
    (void)bus;
    nhal_audio_stream* s = (nhal_audio_stream*)ref;
    for (UInt32 b = 0; b < io->mNumberBuffers; ++b) memset(io->mBuffers[b].mData, 0, io->mBuffers[b].mDataByteSize);
    if (io->mNumberBuffers > 0 && io->mBuffers[0].mData) {
        s->render(s->user, (float*)io->mBuffers[0].mData, frames, s->channels);
    } else if (flags) {
        *flags |= kAudioUnitRenderAction_OutputIsSilence;
    }
    atomic_fetch_add_explicit(&s->frames, frames, memory_order_relaxed);
    return noErr;
}

static nhal_status audio_open(const nhal_audio_config* cfg, nhal_audio_stream** out, nhal_audio_format* actual) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out ist NULL");
    *out = NULL;
    if (!cfg || !cfg->render) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "config.render fehlt");
    const uint32_t channels = cfg->channels ? cfg->channels : 2u;
    if (channels > 32) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Zu viele Kanäle");

    AudioComponentDescription desc;
    memset(&desc, 0, sizeof desc);
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Keine Default-Output-Unit (kein Audiogerät?)");

    nhal_audio_stream* s = (nhal_audio_stream*)g_alloc->alloc(g_alloc->user, sizeof *s, _Alignof(nhal_audio_stream));
    if (!s) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für den Audio-Stream");
    memset(s, 0, sizeof *s);
    s->render = cfg->render;
    s->user = cfg->user;
    s->channels = channels;
    atomic_init(&s->frames, 0);
    atomic_init(&s->running, 0);

    if (AudioComponentInstanceNew(comp, &s->unit) != noErr) {
        g_alloc->free(g_alloc->user, s, sizeof *s);
        return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Audio-Unit ließ sich nicht anlegen");
    }

    /* Rate des Geräts, falls die App keine wünscht. */
    AudioStreamBasicDescription device;
    UInt32 size = sizeof device;
    memset(&device, 0, sizeof device);
    AudioUnitGetProperty(s->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &device, &size);
    const double rate = cfg->sample_rate ? (double)cfg->sample_rate : (device.mSampleRate > 0 ? device.mSampleRate : 48000.0);

    AudioStreamBasicDescription fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.mSampleRate = rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = channels;
    fmt.mBitsPerChannel = 32;
    fmt.mBytesPerFrame = 4u * channels;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerPacket = fmt.mBytesPerFrame;
    OSStatus err = AudioUnitSetProperty(s->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof fmt);

    if (err == noErr && cfg->frames_per_buffer) {
        UInt32 frames = cfg->frames_per_buffer;
        AudioUnitSetProperty(s->unit, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &frames, sizeof frames);
    }
    AURenderCallbackStruct cb = { render_cb, s };
    if (err == noErr) {
        err = AudioUnitSetProperty(s->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    }
    if (err == noErr) err = AudioUnitInitialize(s->unit);
    if (err != noErr) {
        AudioComponentInstanceDispose(s->unit);
        g_alloc->free(g_alloc->user, s, sizeof *s);
        return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Audio-Gerät lehnt das Format ab oder ist nicht verfügbar");
    }

    if (actual) {
        UInt32 frames = 0;
        size = sizeof frames;
        if (AudioUnitGetProperty(s->unit, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &frames, &size) != noErr) {
            frames = 0;
        }
        actual->sample_rate = (uint32_t)(rate + 0.5);
        actual->channels = channels;
        actual->frames_per_buffer = frames;
    }
    *out = s;
    return nhal_status_ok();
}

static nhal_status audio_start(nhal_audio_stream* s) {
    if (AudioOutputUnitStart(s->unit) != noErr) {
        return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Audio-Ausgabe ließ sich nicht starten");
    }
    atomic_store(&s->running, 1);
    return nhal_status_ok();
}

static nhal_status audio_stop(nhal_audio_stream* s) {
    AudioOutputUnitStop(s->unit);
    atomic_store(&s->running, 0);
    return nhal_status_ok();
}

static void audio_close(nhal_audio_stream* s) {
    if (!s) return;
    AudioOutputUnitStop(s->unit);
    AudioUnitUninitialize(s->unit);
    AudioComponentInstanceDispose(s->unit);
    g_alloc->free(g_alloc->user, s, sizeof *s);
}

static nhal_audio_state audio_state(const nhal_audio_stream* s) {
    return atomic_load(&((nhal_audio_stream*)s)->running) ? NHAL_AUDIO_RUNNING : NHAL_AUDIO_STOPPED;
}

static uint64_t audio_frames(const nhal_audio_stream* s) {
    return atomic_load_explicit(&((nhal_audio_stream*)s)->frames, memory_order_relaxed);
}

static nhal_status feature_init(const nhal_allocator* allocator) {
    g_alloc = allocator;
    return nhal_status_ok();
}

static void feature_shutdown(void) { g_alloc = NULL; }

static const nhal_audio_output_v1 table = {
    sizeof(nhal_audio_output_v1), audio_open, audio_close, audio_start, audio_stop, audio_state, audio_frames,
};

const nhal_feature_impl nhal_impl_audio_output = { 1u, feature_init, feature_shutdown, &table };
