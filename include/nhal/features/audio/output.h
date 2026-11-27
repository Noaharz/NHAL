/*
 * audio.output, Version 1: Ausgabe auf das Standard-Audiogerät.
 *
 * Die App liefert die Samples selbst: render() wird immer dann gerufen, wenn
 * das Gerät neue Daten braucht, und füllt `frames` Frames mit `channels`
 * Kanälen als 32-Bit-Float, verschachtelt (L R L R …), Wertebereich -1..1.
 *
 * Semantik (gleich auf allen Plattformen):
 *   - sample_rate/channels/frames_per_buffer = 0 heißt Standard des Geräts.
 *     Was tatsächlich gilt, steht danach in `actual`. Wünscht die App eine
 *     andere Rate als das Gerät, rechnet das System um.
 *   - render() darf nicht blockieren, nicht allokieren und keine Sperren
 *     halten, auf die andere Threads lange warten. Was es nicht füllt, ist Stille.
 *   - Kein Gerät: open() liefert NHAL_ERR_UNAVAILABLE mit Grund. Es gibt
 *     keinen stillen Ersatz.
 *   - frames_rendered() zählt die gelieferten Frames (zum Synchronisieren).
 *
 * Plattformabhängig:
 *   - macOS: render() läuft auf einem Echtzeit-Thread von CoreAudio.
 *   - Web: render() läuft im Hauptthread des Browsers (ScriptProcessorNode).
 *     Browser starten Audio erst nach einer Nutzeraktion. Bis dahin ist state()
 *     NHAL_AUDIO_SUSPENDED, und NHAL setzt beim ersten Klick bzw. Tastendruck
 *     automatisch fort.
 */
#ifndef NHAL_FEATURES_AUDIO_OUTPUT_H
#define NHAL_FEATURES_AUDIO_OUTPUT_H

#include "nhal/nhal_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nhal_audio_render_fn)(void* user, float* samples, uint32_t frames, uint32_t channels);

typedef enum nhal_audio_state {
    NHAL_AUDIO_STOPPED = 0,
    NHAL_AUDIO_RUNNING = 1,
    NHAL_AUDIO_SUSPENDED = 2 /* gestartet, aber vom System angehalten (Web: Autoplay-Sperre) */
} nhal_audio_state;

typedef struct nhal_audio_config {
    uint32_t sample_rate;       /* 0 = Gerät */
    uint32_t channels;          /* 0 = 2 */
    uint32_t frames_per_buffer; /* 0 = Gerät; Wunsch, kein Versprechen */
    nhal_audio_render_fn render;
    void* user;
} nhal_audio_config;

typedef struct nhal_audio_format {
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t frames_per_buffer;
} nhal_audio_format;

typedef struct nhal_audio_stream nhal_audio_stream;

typedef struct nhal_audio_output_v1 {
    uint32_t struct_size;
    nhal_status (*open)(const nhal_audio_config* config, nhal_audio_stream** out, nhal_audio_format* actual);
    void (*close)(nhal_audio_stream* stream);
    nhal_status (*start)(nhal_audio_stream* stream);
    nhal_status (*stop)(nhal_audio_stream* stream);
    nhal_audio_state (*state)(const nhal_audio_stream* stream);
    uint64_t (*frames_rendered)(const nhal_audio_stream* stream);
} nhal_audio_output_v1;

static inline const nhal_audio_output_v1* nhal_get_audio_output_v1(const nhal_context* ctx) {
    return (const nhal_audio_output_v1*)nhal_get(ctx, NHAL_FEATURE_AUDIO_OUTPUT, 1u);
}

#ifdef __cplusplus
}
#endif

#endif /* NHAL_FEATURES_AUDIO_OUTPUT_H */
