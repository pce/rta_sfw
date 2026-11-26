#include "rta_sfw.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    RtaTopographicGraph graph;
    float sample_rate;
    double phase;
    double pulse_phase;
    atomic_int sub_milli;
    atomic_int body_milli;
    atomic_int kick_milli;
} SdlAudioCtx;

static void SDLCALL sdl_audio_cb(void *userdata, SDL_AudioStream *stream,
                                 int additional_amount, int total_amount) {
    (void)total_amount;
    if (additional_amount <= 0) return;

    SdlAudioCtx *ctx = (SdlAudioCtx *)userdata;
    int samples_needed = additional_amount / (int)sizeof(float);
    int frames = samples_needed / 2;
    if (frames <= 0) return;
    if (frames > 2048) frames = 2048;

    float scratch[4096];
    const double sr = (double)ctx->sample_rate;

    for (int i = 0; i < frames; ++i) {
        double env = 0.5 + 0.5 * sin(ctx->pulse_phase * 2.0 * M_PI);
        float s = (float)(0.45 * sin(ctx->phase * 2.0 * M_PI) * env);

        scratch[2 * i]     = s;
        scratch[2 * i + 1] = s * 0.9f;

        ctx->phase += 110.0 / sr;
        if (ctx->phase >= 1.0) ctx->phase -= 1.0;

        ctx->pulse_phase += 2.0 / sr; /* 120 BPM quarter-note pulse */
        if (ctx->pulse_phase >= 1.0) ctx->pulse_phase -= 1.0;
    }

    rta_graph_process_block(&ctx->graph, scratch, (size_t)frames);

    atomic_store(&ctx->sub_milli,  (int)(ctx->graph.sig.sub_rms  * 1000.0f));
    atomic_store(&ctx->body_milli, (int)(ctx->graph.sig.body_rms * 1000.0f));
    atomic_store(&ctx->kick_milli, (int)(ctx->graph.sig.kick_env * 1000.0f));

    SDL_PutAudioStreamData(stream, scratch, frames * 2 * (int)sizeof(float));
}

int main(void) {
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SdlAudioCtx ctx = {0};
    ctx.sample_rate = 48000.0f;
    atomic_init(&ctx.sub_milli, 0);
    atomic_init(&ctx.body_milli, 0);
    atomic_init(&ctx.kick_milli, 0);

    rta_graph_init(&ctx.graph, ctx.sample_rate);
    rta_graph_apply_preset(&ctx.graph, 1); /* Techno Pumper */
    rta_graph_set_flight(&ctx.graph, 0.35f);

    SDL_AudioSpec spec = {
        .format = SDL_AUDIO_F32,
        .channels = 2,
        .freq = (int)ctx.sample_rate
    };

    SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                                        &spec, sdl_audio_cb, &ctx);
    if (!stream) {
        SDL_Log("SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_ResumeAudioStreamDevice(stream);

    for (int step = 0; step < 50; ++step) {
        SDL_Delay(100);
        printf("sub=%.3f body=%.3f kick=%.3f\n",
               (float)atomic_load(&ctx.sub_milli) / 1000.0f,
               (float)atomic_load(&ctx.body_milli) / 1000.0f,
               (float)atomic_load(&ctx.kick_milli) / 1000.0f);
        fflush(stdout);
    }

    SDL_DestroyAudioStream(stream);
    SDL_Quit();
    return 0;
}