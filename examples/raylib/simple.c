#include "rta_sfw.h"
#include "raylib.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    RtaTopographicGraph graph;
    float sample_rate;
    double beat_phase;
    double bass_phase;
    atomic_int preset_id;
    atomic_int flight_permille; /* 0..1000 mapped to 0.0f..1.0f */
    atomic_int sig_sub_permille;
    atomic_int sig_body_permille;
    atomic_int sig_kick_permille;
} AppAudio;

static AppAudio g_app = {0};

static void audio_callback(void *buffer_data, unsigned int frames) {
    (void)buffer_data;
    float *out = (float *)buffer_data;

    int preset = atomic_load(&g_app.preset_id);
    float flight = (float)atomic_load(&g_app.flight_permille) / 1000.0f;
    rta_graph_apply_preset(&g_app.graph, preset);
    rta_graph_set_flight(&g_app.graph, flight);

    const double sr = (double)g_app.sample_rate;
    const double bpm = 124.0;
    const double beats_per_sec = bpm / 60.0;

    for (unsigned int i = 0; i < frames; ++i) {
        /* 4/4 kick envelope on each beat */
        double kick_env = pow(fmax(0.0, 1.0 - g_app.beat_phase), 4.0);
        double kick_freq = 52.0 + 95.0 * kick_env * kick_env;
        double kick = sin(g_app.bass_phase * 2.0 * M_PI * (kick_freq / 110.0)) * kick_env * 0.55;

        /* Warm bass / mid pulse on off-beat */
        double arp_env = 0.25 + 0.20 * sin(g_app.beat_phase * 4.0 * M_PI);
        double note_freq = (fmod(floor(g_app.beat_phase * 4.0), 2.0) < 0.5) ? 110.0 : 146.83;
        double body = 0.6 * sin(g_app.bass_phase * 2.0 * M_PI * (note_freq / 110.0))
                    + 0.4 * sin(g_app.bass_phase * 4.0 * M_PI * (note_freq / 110.0));
        float sample = (float)(kick + body * arp_env * 0.35);

        /* Slight stereo difference for Air/Width band testing */
        out[2 * i]     = sample;
        out[2 * i + 1] = sample * 0.92f;

        g_app.beat_phase += beats_per_sec / sr;
        if (g_app.beat_phase >= 1.0) g_app.beat_phase -= 1.0;

        g_app.bass_phase += 110.0 / sr;
        if (g_app.bass_phase >= 1.0) g_app.bass_phase -= 1.0;
    }

    rta_graph_process_block(&g_app.graph, out, frames);

    atomic_store(&g_app.sig_sub_permille,  (int)(g_app.graph.sig.sub_rms  * 1000.0f));
    atomic_store(&g_app.sig_body_permille, (int)(g_app.graph.sig.body_rms * 1000.0f));
    atomic_store(&g_app.sig_kick_permille, (int)(g_app.graph.sig.kick_env * 1000.0f));
}

int main(void) {
    const int width = 720;
    const int height = 420;
    const float sample_rate = 48000.0f;

    InitWindow(width, height, "RTA-SFW Raylib Simple");
    InitAudioDevice();
    SetAudioStreamBufferSizeDefault(512);

    g_app.sample_rate = sample_rate;
    atomic_init(&g_app.preset_id, 0);
    atomic_init(&g_app.flight_permille, 0);
    atomic_init(&g_app.sig_sub_permille, 0);
    atomic_init(&g_app.sig_body_permille, 0);
    atomic_init(&g_app.sig_kick_permille, 0);

    rta_graph_init(&g_app.graph, sample_rate);

    AudioStream stream = LoadAudioStream((unsigned int)sample_rate, 32, 2);
    SetAudioStreamCallback(stream, audio_callback);
    PlayAudioStream(stream);

    SetTargetFPS(60);

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_ONE)) atomic_store(&g_app.preset_id, 0);
        if (IsKeyPressed(KEY_TWO)) atomic_store(&g_app.preset_id, 1);

        int flight = atomic_load(&g_app.flight_permille);
        if (IsKeyDown(KEY_UP))   flight = flight + 15 > 1000 ? 1000 : flight + 15;
        if (IsKeyDown(KEY_DOWN)) flight = flight - 15 < 0    ? 0    : flight - 15;
        atomic_store(&g_app.flight_permille, flight);

        float sub  = fminf(1.0f, (float)atomic_load(&g_app.sig_sub_permille)  / 500.0f);
        float body = fminf(1.0f, (float)atomic_load(&g_app.sig_body_permille) / 500.0f);
        float kick = fminf(1.0f, (float)atomic_load(&g_app.sig_kick_permille) / 500.0f);
        int preset = atomic_load(&g_app.preset_id);

        BeginDrawing();
        ClearBackground(BLACK);

        DrawText("RTA-SFW Topographic Finalizer (Raylib)", 24, 20, 22, RAYWHITE);
        DrawText(preset == 0 ? "Preset: [1] Studio 54 Glide" : "Preset: [2] Techno Pumper", 24, 56, 18, GREEN);
        DrawText(TextFormat("Flight Perspective [UP/DOWN]: %.2f", (float)flight / 1000.0f), 24, 84, 18, SKYBLUE);

        DrawText("Sub Significator", 24, 140, 16, LIGHTGRAY);
        DrawRectangle(24, 164, (int)(sub * 420.0f), 24, ORANGE);

        DrawText("Body Significator", 24, 210, 16, LIGHTGRAY);
        DrawRectangle(24, 234, (int)(body * 420.0f), 24, GOLD);

        DrawText("Kick Envelope", 24, 280, 16, LIGHTGRAY);
        DrawRectangle(24, 304, (int)(kick * 420.0f), 24, RED);

        EndDrawing();
    }

    StopAudioStream(stream);
    UnloadAudioStream(stream);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}