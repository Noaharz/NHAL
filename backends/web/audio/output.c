/*
 * audio.output – Web (Web Audio, ScriptProcessorNode).
 *
 * Jeder Stream hat einen eigenen AudioContext. Der Browser ruft
 * onaudioprocess im Hauptthread; NHAL ruft dort render() der App in einen
 * Puffer im Wasm-Speicher und verteilt die Samples auf die Kanäle.
 * Ohne Web Audio (z. B. node) liefert open() NHAL_ERR_UNAVAILABLE.
 */
#include "nhal/features/audio/output.h"
#include "nhal/nhal_backend.h"

#include <emscripten.h>
#include <string.h>

struct nhal_audio_stream {
    int id; /* Eintrag in der JS-Tabelle */
    nhal_audio_render_fn render;
    void* user;
    uint32_t channels;
    uint32_t frames_per_buffer;
    float* buffer;
    size_t buffer_bytes;
    uint64_t frames;
    int running;
};

static const nhal_allocator* g_alloc = NULL;

/* Vom Browser gerufen: Puffer füllen. */
EMSCRIPTEN_KEEPALIVE void nhal_web_audio_render(nhal_audio_stream* s, uint32_t frames) {
    memset(s->buffer, 0, (size_t)frames * s->channels * sizeof(float));
    s->render(s->user, s->buffer, frames, s->channels);
    s->frames += frames;
}

/* clang-format off */
EM_JS(int, nhal_js_audio_open, (int stream, int rate, int channels, int frames, int buffer), {
    const AC = globalThis.AudioContext || globalThis.webkitAudioContext;
    if (!AC) return -1;
    let ctx;
    try { ctx = rate > 0 ? new AC({ sampleRate: rate }) : new AC(); } catch (e) { return -2; }
    const node = ctx.createScriptProcessor(frames, 0, channels);
    node.onaudioprocess = (ev) => {
        const out = ev.outputBuffer;
        const n = out.length;
        _nhal_web_audio_render(stream, n);
        const base = buffer >> 2;
        for (let c = 0; c < channels; ++c) {
            const dst = out.getChannelData(c);
            for (let i = 0; i < n; ++i) dst[i] = HEAPF32[base + i * channels + c];
        }
    };
    const t = (globalThis.nhalAudio = globalThis.nhalAudio || { next: 1, streams: {} });
    const id = t.next++;
    t.streams[id] = { ctx: ctx, node: node, started: false };
    if (typeof document !== "undefined") {
        const resume = () => { const s = t.streams[id]; if (s && s.started) s.ctx.resume(); };
        document.addEventListener("pointerdown", resume, { passive: true });
        document.addEventListener("keydown", resume, { passive: true });
    }
    return id;
})

EM_JS(int, nhal_js_audio_rate, (int id), { return globalThis.nhalAudio.streams[id].ctx.sampleRate | 0; })

EM_JS(void, nhal_js_audio_start, (int id), {
    const s = globalThis.nhalAudio.streams[id];
    s.started = true;
    s.node.connect(s.ctx.destination);
    s.ctx.resume();
})

EM_JS(void, nhal_js_audio_stop, (int id), {
    const s = globalThis.nhalAudio.streams[id];
    s.started = false;
    try { s.node.disconnect(); } catch (e) {}
    s.ctx.suspend();
})

EM_JS(int, nhal_js_audio_suspended, (int id), {
    return globalThis.nhalAudio.streams[id].ctx.state !== "running" ? 1 : 0;
})

EM_JS(void, nhal_js_audio_close, (int id), {
    const s = globalThis.nhalAudio.streams[id];
    s.started = false;
    s.node.onaudioprocess = null;
    try { s.node.disconnect(); } catch (e) {}
    s.ctx.close();
    delete globalThis.nhalAudio.streams[id];
})
/* clang-format on */

static uint32_t round_frames(uint32_t want) { /* ScriptProcessor: Zweierpotenz 256..16384 */
    uint32_t f = 256;
    while (f < want && f < 16384) f <<= 1;
    return f;
}

static nhal_status audio_open(const nhal_audio_config* cfg, nhal_audio_stream** out, nhal_audio_format* actual) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out ist NULL");
    *out = NULL;
    if (!cfg || !cfg->render) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "config.render fehlt");
    const uint32_t channels = cfg->channels ? cfg->channels : 2u;
    if (channels > 32) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Zu viele Kanäle");
    const uint32_t frames = round_frames(cfg->frames_per_buffer ? cfg->frames_per_buffer : 1024u);

    nhal_audio_stream* s = (nhal_audio_stream*)g_alloc->alloc(g_alloc->user, sizeof *s, _Alignof(nhal_audio_stream));
    if (!s) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für den Audio-Stream");
    memset(s, 0, sizeof *s);
    s->render = cfg->render;
    s->user = cfg->user;
    s->channels = channels;
    s->frames_per_buffer = frames;
    s->buffer_bytes = (size_t)frames * channels * sizeof(float);
    s->buffer = (float*)g_alloc->alloc(g_alloc->user, s->buffer_bytes, 16);
    if (!s->buffer) {
        g_alloc->free(g_alloc->user, s, sizeof *s);
        return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für den Audio-Puffer");
    }
    s->id = nhal_js_audio_open((int)(intptr_t)s, (int)cfg->sample_rate, (int)channels, (int)frames,
                               (int)(intptr_t)s->buffer);
    if (s->id <= 0) {
        const char* why = s->id == -1 ? "Dieser Browser bzw. diese Umgebung hat kein Web Audio"
                                      : "AudioContext ließ sich mit diesem Format nicht anlegen";
        g_alloc->free(g_alloc->user, s->buffer, s->buffer_bytes);
        g_alloc->free(g_alloc->user, s, sizeof *s);
        return nhal_status_error(NHAL_ERR_UNAVAILABLE, why);
    }
    if (actual) {
        actual->sample_rate = (uint32_t)nhal_js_audio_rate(s->id);
        actual->channels = channels;
        actual->frames_per_buffer = frames;
    }
    *out = s;
    return nhal_status_ok();
}

static nhal_status audio_start(nhal_audio_stream* s) {
    nhal_js_audio_start(s->id);
    s->running = 1;
    return nhal_status_ok();
}

static nhal_status audio_stop(nhal_audio_stream* s) {
    nhal_js_audio_stop(s->id);
    s->running = 0;
    return nhal_status_ok();
}

static void audio_close(nhal_audio_stream* s) {
    if (!s) return;
    nhal_js_audio_close(s->id);
    g_alloc->free(g_alloc->user, s->buffer, s->buffer_bytes);
    g_alloc->free(g_alloc->user, s, sizeof *s);
}

static nhal_audio_state audio_state(const nhal_audio_stream* s) {
    if (!s->running) return NHAL_AUDIO_STOPPED;
    return nhal_js_audio_suspended(s->id) ? NHAL_AUDIO_SUSPENDED : NHAL_AUDIO_RUNNING;
}

static uint64_t audio_frames(const nhal_audio_stream* s) { return s->frames; }

static nhal_status feature_init(const nhal_allocator* allocator) {
    g_alloc = allocator;
    return nhal_status_ok();
}

static void feature_shutdown(void) { g_alloc = NULL; }

static const nhal_audio_output_v1 table = {
    sizeof(nhal_audio_output_v1), audio_open, audio_close, audio_start, audio_stop, audio_state, audio_frames,
};

const nhal_feature_impl nhal_impl_audio_output = { 1u, feature_init, feature_shutdown, &table };
