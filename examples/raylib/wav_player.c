#include "rta_sfw.h"
#include "raylib.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    RtaTopographicGraph graph;
    float *stereo_samples;   /* Interleaved L, R float samples */
    size_t total_frames;
    atomic_size_t play_cursor;
    atomic_int playing;
    atomic_int preset_id;
    atomic_int flight_permille;
    atomic_int sub_milli;
    atomic_int body_milli;
    atomic_int kick_milli;
} WavAudioCtx;

static WavAudioCtx g_wav = {0};

static void wav_stream_callback(void *buffer_data, unsigned int frames) {
    float *out = (float *)buffer_data;

    int preset = atomic_load(&g_wav.preset_id);
    float flight = (float)atomic_load(&g_wav.flight_permille) / 1000.0f;
    rta_graph_apply_preset(&g_wav.graph, preset);
    rta_graph_set_flight(&g_wav.graph, flight);

    size_t cursor = atomic_load(&g_wav.play_cursor);
    int playing = atomic_load(&g_wav.playing);

    for (unsigned int i = 0; i < frames; ++i) {
        if (!playing || g_wav.total_frames == 0) {
            out[2 * i]     = 0.0f;
            out[2 * i + 1] = 0.0f;
            continue;
        }

        out[2 * i]     = g_wav.stereo_samples[2 * cursor];
        out[2 * i + 1] = g_wav.stereo_samples[2 * cursor + 1];

        cursor++;
        if (cursor >= g_wav.total_frames) {
            cursor = 0; /* Loop WAV */
        }
    }

    atomic_store(&g_wav.play_cursor, cursor);

    rta_graph_process_block(&g_wav.graph, out, frames);

    atomic_store(&g_wav.sub_milli,  (int)(g_wav.graph.sig.sub_rms  * 1000.0f));
    atomic_store(&g_wav.body_milli, (int)(g_wav.graph.sig.body_rms * 1000.0f));
    atomic_store(&g_wav.kick_milli, (int)(g_wav.graph.sig.kick_env * 1000.0f));
}

static bool load_wav_16bit_44100_to_stereo_float(const char *path,
                                                 float **out_stereo,
                                                 size_t *out_frames) {
    Wave wave = LoadWave(path);
    if (wave.data == NULL || wave.frameCount == 0) {
        return false;
    }

    /* Convert to 16-bit 44100 Hz if needed */
    if (wave.sampleRate != 44100) {
        WaveFormat(&wave, 44100, 16, wave.channels);
    } else if (wave.sampleSize != 16) {
        WaveFormat(&wave, wave.sampleRate, 16, wave.channels);
    }

    const size_t frames = (size_t)wave.frameCount;
    const int channels = wave.channels > 0 ? wave.channels : 1;
    const short *pcm16 = (const short *)wave.data;

    float *stereo = (float *)calloc(frames * 2, sizeof(float));
    if (!stereo) {
        UnloadWave(wave);
        return false;
    }

    const float inv_32768 = 1.0f / 32768.0f;
    for (size_t f = 0; f < frames; ++f) {
        if (channels == 1) {
            float s = (float)pcm16[f] * inv_32768;
            stereo[2 * f]     = s;
            stereo[2 * f + 1] = s;
        } else {
            float l = (float)pcm16[f * (size_t)channels] * inv_32768;
            float r = (float)pcm16[f * (size_t)channels + 1] * inv_32768;
            stereo[2 * f]     = l;
            stereo[2 * f + 1] = r;
        }
    }

    UnloadWave(wave);
    *out_stereo = stereo;
    *out_frames = frames;
    return true;
}

static void trim_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ')) {
        s[--len] = '\0';
    }
}

int main(int argc, char **argv) {
    char chosen_path[512] = {0};

    if (argc > 1 && argv[1][0] != '\0') {
        snprintf(chosen_path, sizeof(chosen_path), "%s", argv[1]);
    } else if (FileExists("assets/input.wav")) {
        snprintf(chosen_path, sizeof(chosen_path), "assets/input.wav");
    } else {
        printf("No CLI path or assets/input.wav found.\n");
        printf("Enter path to a 16-bit 44.1kHz .wav file: ");
        fflush(stdout);
        if (fgets(chosen_path, sizeof(chosen_path), stdin) != NULL) {
            trim_newline(chosen_path);
        }
    }

    InitWindow(760, 440, "RTA-SFW WAV Finalizer (Raylib)");
    InitAudioDevice();
    SetAudioStreamBufferSizeDefault(512);

    rta_graph_init(&g_wav.graph, 44100.0f);
    atomic_init(&g_wav.play_cursor, 0);
    atomic_init(&g_wav.playing, 0);
    atomic_init(&g_wav.preset_id, 0);
    atomic_init(&g_wav.flight_permille, 0);
    atomic_init(&g_wav.sub_milli, 0);
    atomic_init(&g_wav.body_milli, 0);
    atomic_init(&g_wav.kick_milli, 0);

    bool loaded = false;
    if (chosen_path[0] != '\0' && FileExists(chosen_path)) {
        loaded = load_wav_16bit_44100_to_stereo_float(chosen_path,
                                                      &g_wav.stereo_samples,
                                                      &g_wav.total_frames);
    }
    if (loaded) {
        atomic_store(&g_wav.playing, 1);
    }

    AudioStream stream = LoadAudioStream(44100, 32, 2);
    SetAudioStreamCallback(stream, wav_stream_callback);
    PlayAudioStream(stream);

    SetTargetFPS(60);

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_SPACE)) {
            int cur = atomic_load(&g_wav.playing);
            atomic_store(&g_wav.playing, cur ? 0 : 1);
        }
        if (IsKeyPressed(KEY_ONE)) atomic_store(&g_wav.preset_id, 0);
        if (IsKeyPressed(KEY_TWO)) atomic_store(&g_wav.preset_id, 1);

        int flight = atomic_load(&g_wav.flight_permille);
        if (IsKeyDown(KEY_UP))   flight = flight + 15 > 1000 ? 1000 : flight + 15;
        if (IsKeyDown(KEY_DOWN)) flight = flight - 15 < 0    ? 0    : flight - 15;
        atomic_store(&g_wav.flight_permille, flight);

        float sub  = fminf(1.0f, (float)atomic_load(&g_wav.sub_milli)  / 500.0f);
        float body = fminf(1.0f, (float)atomic_load(&g_wav.body_milli) / 500.0f);
        float kick = fminf(1.0f, (float)atomic_load(&g_wav.kick_milli) / 500.0f);
        int preset = atomic_load(&g_wav.preset_id);
        int playing = atomic_load(&g_wav.playing);

        BeginDrawing();
        ClearBackground(BLACK);

        DrawText("RTA-SFW WAV File Finalizer", 24, 20, 22, RAYWHITE);
        DrawText(loaded ? TextFormat("File: %s (%s)", chosen_path, playing ? "PLAYING" : "PAUSED")
                        : "No valid WAV loaded (place 16-bit 44.1kHz WAV at assets/input.wav or pass path)",
                 24, 52, 16, loaded ? LIGHTGRAY : ORANGE);
        DrawText(preset == 0 ? "Preset: [1] Studio 54 Glide" : "Preset: [2] Techno Pumper", 24, 78, 18, GREEN);
        DrawText(TextFormat("Flight [UP/DOWN]: %.2f  |  [SPACE] Play/Pause", (float)flight / 1000.0f),
                 24, 104, 18, SKYBLUE);

        DrawText("Sub Significator", 24, 155, 16, LIGHTGRAY);
        DrawRectangle(24, 178, (int)(sub * 440.0f), 22, ORANGE);

        DrawText("Body Significator", 24, 220, 16, LIGHTGRAY);
        DrawRectangle(24, 243, (int)(body * 440.0f), 22, GOLD);

        DrawText("Kick Envelope", 24, 285, 16, LIGHTGRAY);
        DrawRectangle(24, 308, (int)(kick * 440.0f), 22, RED);

        EndDrawing();
    }

    StopAudioStream(stream);
    UnloadAudioStream(stream);
    free(g_wav.stereo_samples);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}