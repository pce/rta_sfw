#include "rta_sfw.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>

#define WAVE_RING_SIZE 512

typedef struct {
    RtaTopographicGraph graph;
    float sample_rate;

    /* Shared parameters (updated from main thread, read in audio loop) */
    atomic_int preset_id;
    atomic_int flight_permille;       /* 0..1000 -> 0.0..1.0 */
    atomic_int sub_to_body_permille;  /* 0..1000 -> 0.0..1.0 */
    atomic_int kick_to_body_permille; /* 0..1000 -> 0.0..1.0 */
    atomic_int air_width_permille;    /* 0..2000 -> 0.0..2.0 */
    atomic_int monitor_enabled;       /* 0 or 1 */

    /* Live Significators (0..1000) */
    atomic_int sig_sub_milli;
    atomic_int sig_body_milli;
    atomic_int sig_air_milli;
    atomic_int sig_kick_milli;

    /* Lock-light ring buffer of processed samples for visualization */
    float wave_ring[WAVE_RING_SIZE];
    atomic_size_t wave_write_idx;
} LiveAnalyzerCtx;

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void push_waveform_samples(LiveAnalyzerCtx *ctx, const float *stereo_lr, size_t frames) {
    size_t w = atomic_load(&ctx->wave_write_idx);
    for (size_t i = 0; i < frames; ++i) {
        float mono = 0.5f * (stereo_lr[2 * i] + stereo_lr[2 * i + 1]);
        ctx->wave_ring[w] = clampf(mono, -1.0f, 1.0f);
        w = (w + 1) % WAVE_RING_SIZE;
    }
    atomic_store(&ctx->wave_write_idx, w);
}

static void sync_params_to_graph(LiveAnalyzerCtx *ctx) {
    int preset = atomic_load(&ctx->preset_id);
    float flight = (float)atomic_load(&ctx->flight_permille) / 1000.0f;
    float sub2body = (float)atomic_load(&ctx->sub_to_body_permille) / 1000.0f;
    float kick2body = (float)atomic_load(&ctx->kick_to_body_permille) / 1000.0f;
    float air_w = (float)atomic_load(&ctx->air_width_permille) / 1000.0f;

    rta_graph_apply_preset(&ctx->graph, preset);
    ctx->graph.sub_to_body_duck = sub2body;
    ctx->graph.kick_to_body_duck = kick2body;
    rta_graph_set_flight(&ctx->graph, flight);
    rta_ms_width_set_target(&ctx->graph.air_width, air_w);
}

static void draw_meter(SDL_Renderer *r, float x, float y, float w, float h,
                       float norm_val, Uint8 cr, Uint8 cg, Uint8 cb) {
    norm_val = clampf(norm_val, 0.0f, 1.0f);
    SDL_FRect bg = {x, y, w, h};
    SDL_SetRenderDrawColor(r, 28, 32, 40, 255);
    SDL_RenderFillRect(r, &bg);

    SDL_FRect fg = {x, y, w * norm_val, h};
    SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
    SDL_RenderFillRect(r, &fg);

    SDL_SetRenderDrawColor(r, 70, 78, 92, 255);
    SDL_RenderRect(r, &bg);
}

static void draw_waveform_visual(SDL_Renderer *r, const LiveAnalyzerCtx *ctx,
                                 float x, float y, float w, float h, bool viz_only) {
    SDL_FRect box = {x, y, w, h};
    if (!viz_only) {
        SDL_SetRenderDrawColor(r, 18, 20, 26, 255);
        SDL_RenderFillRect(r, &box);
        SDL_SetRenderDrawColor(r, 55, 62, 76, 255);
        SDL_RenderRect(r, &box);
    }

    float sub  = clampf((float)atomic_load(&ctx->sig_sub_milli)  / 450.0f, 0.0f, 1.0f);
    float body = clampf((float)atomic_load(&ctx->sig_body_milli) / 450.0f, 0.0f, 1.0f);
    float air  = clampf((float)atomic_load(&ctx->sig_air_milli)  / 450.0f, 0.0f, 1.0f);
    float kick = clampf((float)atomic_load(&ctx->sig_kick_milli) / 450.0f, 0.0f, 1.0f);

    /* Topographic horizon / warp curve driven by Flight + Sub/Body energy */
    float flight = (float)atomic_load(&ctx->flight_permille) / 1000.0f;
    float mid_y = y + h * (0.52f - 0.14f * flight);

    /* Center reference line */
    SDL_SetRenderDrawColor(r, 45, 52, 64, 255);
    SDL_RenderLine(r, x + 4.0f, mid_y, x + w - 4.0f, mid_y);

    /* Rolling waveform */
    size_t write_idx = atomic_load(&ctx->wave_write_idx);
    float amp_scale = h * (0.36f + 0.12f * kick);
    float prev_px = x;
    float prev_py = mid_y;

    for (size_t i = 0; i < WAVE_RING_SIZE; ++i) {
        size_t idx = (write_idx + i) % WAVE_RING_SIZE;
        float s = ctx->wave_ring[idx];
        float px = x + ((float)i / (float)(WAVE_RING_SIZE - 1)) * w;
        float py = mid_y - s * amp_scale;

        Uint8 r_col = (Uint8)clampf(80.0f + 175.0f * sub + 40.0f * kick, 0.0f, 255.0f);
        Uint8 g_col = (Uint8)clampf(150.0f + 90.0f * body, 0.0f, 255.0f);
        Uint8 b_col = (Uint8)clampf(180.0f + 75.0f * air, 0.0f, 255.0f);
        SDL_SetRenderDrawColor(r, r_col, g_col, b_col, 255);

        if (i > 0) {
            SDL_RenderLine(r, prev_px, prev_py, px, py);
        }
        prev_px = px;
        prev_py = py;
    }
}

static void print_settings_status(const LiveAnalyzerCtx *ctx, bool viz_only) {
    printf("\r[VIZ_ONLY=%s] Preset=%s | Flight=%.2f | Sub->Body=%.2f | Kick->Body=%.2f | AirW=%.2f | Mon=%s | Sub=%.2f Body=%.2f Air=%.2f Kick=%.2f   ",
           viz_only ? "ON " : "OFF",
           atomic_load(&ctx->preset_id) == 0 ? "Studio54" : "Techno  ",
           (float)atomic_load(&ctx->flight_permille) / 1000.0f,
           (float)atomic_load(&ctx->sub_to_body_permille) / 1000.0f,
           (float)atomic_load(&ctx->kick_to_body_permille) / 1000.0f,
           (float)atomic_load(&ctx->air_width_permille) / 1000.0f,
           atomic_load(&ctx->monitor_enabled) ? "ON " : "OFF",
           (float)atomic_load(&ctx->sig_sub_milli) / 1000.0f,
           (float)atomic_load(&ctx->sig_body_milli) / 1000.0f,
           (float)atomic_load(&ctx->sig_air_milli) / 1000.0f,
           (float)atomic_load(&ctx->sig_kick_milli) / 1000.0f);
    fflush(stdout);
}

int main(void) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    const int win_w = 880;
    const int win_h = 540;
    SDL_Window *window = SDL_CreateWindow("RTA-SFW Live Input & Visual Analyzer (SDL3)",
                                          win_w, win_h, SDL_WINDOW_RESIZABLE);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    LiveAnalyzerCtx ctx = {0};
    ctx.sample_rate = 48000.0f;
    atomic_init(&ctx.preset_id, 0);
    atomic_init(&ctx.flight_permille, 150);
    atomic_init(&ctx.sub_to_body_permille, 200);
    atomic_init(&ctx.kick_to_body_permille, 150);
    atomic_init(&ctx.air_width_permille, 1100);
    atomic_init(&ctx.monitor_enabled, 0); /* Safe default: start unmonitored to avoid feedback */
    atomic_init(&ctx.sig_sub_milli, 0);
    atomic_init(&ctx.sig_body_milli, 0);
    atomic_init(&ctx.sig_air_milli, 0);
    atomic_init(&ctx.sig_kick_milli, 0);
    atomic_init(&ctx.wave_write_idx, 0);

    rta_graph_init(&ctx.graph, ctx.sample_rate);
    sync_params_to_graph(&ctx);

    SDL_AudioSpec spec = {
        .format = SDL_AUDIO_F32,
        .channels = 2,
        .freq = (int)ctx.sample_rate
    };

    /* Open default recording stream (microphone / line-in) */
    SDL_AudioStream *rec_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING,
                                                          &spec, NULL, NULL);
    if (!rec_stream) {
        SDL_Log("SDL_OpenAudioDeviceStream (recording) failed: %s", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* Optional playback stream for monitoring processed signal */
    SDL_AudioStream *play_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                                           &spec, NULL, NULL);
    if (!play_stream) {
        SDL_Log("SDL_OpenAudioDeviceStream (playback) failed: %s", SDL_GetError());
        SDL_DestroyAudioStream(rec_stream);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_ResumeAudioStreamDevice(rec_stream);
    SDL_ResumeAudioStreamDevice(play_stream);

    bool running = true;
    bool viz_only = false;
    float block_buf[4096];

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                switch (ev.key.key) {
                    case SDLK_V:
                        viz_only = !viz_only;
                        break;
                    case SDLK_1:
                        atomic_store(&ctx.preset_id, 0);
                        atomic_store(&ctx.sub_to_body_permille, 200);
                        atomic_store(&ctx.kick_to_body_permille, 150);
                        atomic_store(&ctx.air_width_permille, 1100);
                        break;
                    case SDLK_2:
                        atomic_store(&ctx.preset_id, 1);
                        atomic_store(&ctx.sub_to_body_permille, 250);
                        atomic_store(&ctx.kick_to_body_permille, 450);
                        atomic_store(&ctx.air_width_permille, 1250);
                        break;
                    case SDLK_M: {
                        int m = atomic_load(&ctx.monitor_enabled);
                        atomic_store(&ctx.monitor_enabled, m ? 0 : 1);
                        break;
                    }
                    default:
                        break;
                }
            }
        }

        const bool *keys = SDL_GetKeyboardState(NULL);
        if (keys[SDL_SCANCODE_UP]) {
            int v = atomic_load(&ctx.flight_permille) + 15;
            atomic_store(&ctx.flight_permille, v > 1000 ? 1000 : v);
        }
        if (keys[SDL_SCANCODE_DOWN]) {
            int v = atomic_load(&ctx.flight_permille) - 15;
            atomic_store(&ctx.flight_permille, v < 0 ? 0 : v);
        }
        if (keys[SDL_SCANCODE_Q]) {
            int v = atomic_load(&ctx.sub_to_body_permille) + 12;
            atomic_store(&ctx.sub_to_body_permille, v > 1000 ? 1000 : v);
        }
        if (keys[SDL_SCANCODE_A]) {
            int v = atomic_load(&ctx.sub_to_body_permille) - 12;
            atomic_store(&ctx.sub_to_body_permille, v < 0 ? 0 : v);
        }
        if (keys[SDL_SCANCODE_W]) {
            int v = atomic_load(&ctx.kick_to_body_permille) + 12;
            atomic_store(&ctx.kick_to_body_permille, v > 1000 ? 1000 : v);
        }
        if (keys[SDL_SCANCODE_S]) {
            int v = atomic_load(&ctx.kick_to_body_permille) - 12;
            atomic_store(&ctx.kick_to_body_permille, v < 0 ? 0 : v);
        }
        if (keys[SDL_SCANCODE_E]) {
            int v = atomic_load(&ctx.air_width_permille) + 20;
            atomic_store(&ctx.air_width_permille, v > 2000 ? 2000 : v);
        }
        if (keys[SDL_SCANCODE_D]) {
            int v = atomic_load(&ctx.air_width_permille) - 20;
            atomic_store(&ctx.air_width_permille, v < 0 ? 0 : v);
        }

        /* Pull available audio from recording stream and process */
        sync_params_to_graph(&ctx);
        int avail_bytes = SDL_GetAudioStreamAvailable(rec_stream);
        const int frame_bytes = (int)(2 * sizeof(float));
        if (avail_bytes >= frame_bytes) {
            int max_bytes = (int)sizeof(block_buf);
            int to_read = avail_bytes > max_bytes ? max_bytes : avail_bytes;
            to_read = (to_read / frame_bytes) * frame_bytes;
            if (to_read > 0) {
                int got = SDL_GetAudioStreamData(rec_stream, block_buf, to_read);
                if (got >= frame_bytes) {
                    size_t frames = (size_t)(got / frame_bytes);
                    rta_graph_process_block(&ctx.graph, block_buf, frames);

                    atomic_store(&ctx.sig_sub_milli,  (int)(ctx.graph.sig.sub_rms  * 1000.0f));
                    atomic_store(&ctx.sig_body_milli, (int)(ctx.graph.sig.body_rms * 1000.0f));
                    atomic_store(&ctx.sig_air_milli,  (int)(ctx.graph.sig.air_rms  * 1000.0f));
                    atomic_store(&ctx.sig_kick_milli, (int)(ctx.graph.sig.kick_env * 1000.0f));
                    push_waveform_samples(&ctx, block_buf, frames);

                    if (atomic_load(&ctx.monitor_enabled)) {
                        SDL_PutAudioStreamData(play_stream, block_buf, got);
                    }
                }
            }
        }

        print_settings_status(&ctx, viz_only);

        int cw = win_w, ch = win_h;
        SDL_GetCurrentRenderOutputSize(renderer, &cw, &ch);
        float fw = (float)cw;
        float fh = (float)ch;

        SDL_SetRenderDrawColor(renderer, 10, 12, 16, 255);
        SDL_RenderClear(renderer);

        if (viz_only) {
            /* Visualization-only mode: full-canvas waveform + bottom Significator bars */
            float bar_h = 22.0f;
            float viz_h = fh - (bar_h + 24.0f);
            if (viz_h < 80.0f) viz_h = 80.0f;

            draw_waveform_visual(renderer, &ctx, 12.0f, 12.0f, fw - 24.0f, viz_h, true);

            float sub  = (float)atomic_load(&ctx.sig_sub_milli)  / 450.0f;
            float body = (float)atomic_load(&ctx.sig_body_milli) / 450.0f;
            float air  = (float)atomic_load(&ctx.sig_air_milli)  / 450.0f;
            float kick = (float)atomic_load(&ctx.sig_kick_milli) / 450.0f;

            float seg_w = (fw - 60.0f) / 4.0f;
            float by = fh - bar_h - 8.0f;
            draw_meter(renderer, 12.0f,                  by, seg_w, bar_h, sub,  255, 130, 50);
            draw_meter(renderer, 24.0f + seg_w,          by, seg_w, bar_h, body, 235, 195, 60);
            draw_meter(renderer, 36.0f + seg_w * 2.0f,   by, seg_w, bar_h, air,  90,  200, 255);
            draw_meter(renderer, 48.0f + seg_w * 3.0f,   by, seg_w, bar_h, kick, 235, 70,  90);
        } else {
            /* Analyzer + Parameter Tweaking & Settings view */
            draw_waveform_visual(renderer, &ctx, 20.0f, 20.0f, fw - 40.0f, fh * 0.45f, false);

            float sub  = (float)atomic_load(&ctx.sig_sub_milli)  / 450.0f;
            float body = (float)atomic_load(&ctx.sig_body_milli) / 450.0f;
            float air  = (float)atomic_load(&ctx.sig_air_milli)  / 450.0f;
            float kick = (float)atomic_load(&ctx.sig_kick_milli) / 450.0f;

            float meter_y = 20.0f + fh * 0.45f + 16.0f;
            float mw = (fw - 80.0f) / 4.0f;
            draw_meter(renderer, 20.0f,             meter_y, mw, 24.0f, sub,  255, 130, 50);
            draw_meter(renderer, 40.0f + mw,        meter_y, mw, 24.0f, body, 235, 195, 60);
            draw_meter(renderer, 60.0f + mw * 2.0f, meter_y, mw, 24.0f, air,  90,  200, 255);
            draw_meter(renderer, 80.0f + mw * 3.0f, meter_y, mw, 24.0f, kick, 235, 70,  90);

            /* Parameter bars (Flight, Sub->Body, Kick->Body, Air Width) */
            float py = meter_y + 44.0f;
            float flight = (float)atomic_load(&ctx.flight_permille) / 1000.0f;
            float s2b    = (float)atomic_load(&ctx.sub_to_body_permille) / 1000.0f;
            float k2b    = (float)atomic_load(&ctx.kick_to_body_permille) / 1000.0f;
            float air_w  = (float)atomic_load(&ctx.air_width_permille) / 2000.0f;

            draw_meter(renderer, 20.0f, py,          fw - 40.0f, 16.0f, flight, 110, 180, 255);
            draw_meter(renderer, 20.0f, py + 24.0f,  fw - 40.0f, 16.0f, s2b,    255, 160, 80);
            draw_meter(renderer, 20.0f, py + 48.0f,  fw - 40.0f, 16.0f, k2b,    255, 95,  110);
            draw_meter(renderer, 20.0f, py + 72.0f,  fw - 40.0f, 16.0f, air_w,  120, 230, 170);
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    printf("\n");
    SDL_DestroyAudioStream(play_stream);
    SDL_DestroyAudioStream(rec_stream);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}