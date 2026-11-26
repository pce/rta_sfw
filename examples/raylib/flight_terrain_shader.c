#include "rta_sfw_abi.h"
#include "rta_raylib_ui.h"
#include "raylib.h"
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *kTopoVertShader =
    "#version 330\n"
    "in vec3 vertexPosition;\n"
    "in vec2 vertexTexCoord;\n"
    "in vec4 vertexColor;\n"
    "uniform mat4 mvp;\n"
    "uniform float uSub;\n"
    "uniform float uBody;\n"
    "uniform float uKick;\n"
    "uniform float uFlight;\n"
    "out vec2 vUv;\n"
    "out float vHeight;\n"
    "void main() {\n"
    "    vUv = vertexTexCoord;\n"
    "    vec3 p = vertexPosition;\n"
    "    float ridge = sin(p.x * 0.25 + uBody * 3.0) * cos(p.z * 0.25) * (0.6 + uBody * 2.5);\n"
    "    float bedrock = sin(p.z * 0.12) * (0.4 + uSub * 3.0 + uKick * 1.5);\n"
    "    p.y += (ridge + bedrock) * (0.35 + 0.65 * uFlight);\n"
    "    vHeight = p.y;\n"
    "    gl_Position = mvp * vec4(p, 1.0);\n"
    "}\n";

static const char *kTopoFragShader =
    "#version 330\n"
    "in vec2 vUv;\n"
    "in float vHeight;\n"
    "uniform float uSub;\n"
    "uniform float uBody;\n"
    "uniform float uAir;\n"
    "uniform float uKick;\n"
    "uniform float uFlight;\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "    float gridX = abs(fract(vUv.x * 24.0) - 0.5);\n"
    "    float gridZ = abs(fract(vUv.y * 24.0) - 0.5);\n"
    "    float line = smoothstep(0.46, 0.50, max(gridX, gridZ));\n"
    "    vec3 frogCol = vec3(0.95, 0.45, 0.15) * (0.45 + uSub + uKick);\n"
    "    vec3 flightCol = vec3(0.20, 0.75, 0.95) * (0.45 + uAir + 0.4 * uFlight);\n"
    "    vec3 baseCol = mix(frogCol, flightCol, clamp(uFlight, 0.0, 1.0));\n"
    "    vec3 col = baseCol * (0.55 + 0.18 * vHeight) + vec3(0.85, 0.92, 1.0) * line * (0.25 + 0.5 * uBody);\n"
    "    finalColor = vec4(col, 1.0);\n"
    "}\n";

typedef struct {
    RtaContext *rta;
    float *file_stereo;
    size_t file_frames;
    size_t file_cursor;
    bool has_file;

    double synth_phase;
    double synth_beat;

    atomic_int preset_id;
    atomic_int flight_permille;
    atomic_int paused;
    atomic_int sub_milli;
    atomic_int body_milli;
    atomic_int air_milli;
    atomic_int kick_milli;
} FlightAudioCtx;

static FlightAudioCtx g_ctx = {0};

static bool load_audio_file_44100_stereo(const char *path, float **out_buf, size_t *out_frames) {
    if (!path || !FileExists(path)) return false;

    Wave wave = LoadWave(path);
    if (wave.data == NULL || wave.frameCount == 0) {
        return false;
    }

    if (wave.sampleRate != 44100 || wave.sampleSize != 16 || wave.channels != 2) {
        WaveFormat(&wave, 44100, 16, 2);
    }

    size_t frames = (size_t)wave.frameCount;
    const short *pcm = (const short *)wave.data;
    float *stereo = (float *)malloc(frames * 2 * sizeof(float));
    if (!stereo) {
        UnloadWave(wave);
        return false;
    }

    const float scale = 1.0f / 32768.0f;
    for (size_t i = 0; i < frames * 2; ++i) {
        stereo[i] = (float)pcm[i] * scale;
    }

    UnloadWave(wave);
    *out_buf = stereo;
    *out_frames = frames;
    return true;
}

static void flight_audio_callback(void *buffer_data, unsigned int frames) {
    float *out = (float *)buffer_data;
    const double sr = 44100.0;

    int preset = atomic_load(&g_ctx.preset_id);
    float flight = (float)atomic_load(&g_ctx.flight_permille) / 1000.0f;
    int paused = atomic_load(&g_ctx.paused);

    if (g_ctx.rta) {
        rta_set_preset(g_ctx.rta, preset == 1 ? RTA_PRESET_TECHNO_PUMPER : RTA_PRESET_STUDIO_54);
        rta_set_flight(g_ctx.rta, flight);
    }

    for (unsigned int i = 0; i < frames; ++i) {
        if (paused) {
            out[2 * i]     = 0.0f;
            out[2 * i + 1] = 0.0f;
            continue;
        }

        if (g_ctx.has_file && g_ctx.file_stereo && g_ctx.file_frames > 0) {
            out[2 * i]     = g_ctx.file_stereo[2 * g_ctx.file_cursor];
            out[2 * i + 1] = g_ctx.file_stereo[2 * g_ctx.file_cursor + 1];
            g_ctx.file_cursor++;
            if (g_ctx.file_cursor >= g_ctx.file_frames) {
                g_ctx.file_cursor = 0;
            }
        } else {
            double env = pow(fmax(0.0, 1.0 - g_ctx.synth_beat), 4.0);
            float kick = (float)(sin(g_ctx.synth_phase * 2.0 * M_PI * (0.5 + 1.2 * env)) * env * 0.55);
            float arp  = (float)(0.22 * sin(g_ctx.synth_phase * 4.0 * M_PI) * (0.35 + 0.65 * (1.0 - env)));
            out[2 * i]     = kick + arp;
            out[2 * i + 1] = kick * 0.95f - arp * 0.85f;

            g_ctx.synth_beat += (126.0 / 60.0) / sr;
            if (g_ctx.synth_beat >= 1.0) g_ctx.synth_beat -= 1.0;

            g_ctx.synth_phase += 55.0 / sr;
            if (g_ctx.synth_phase >= 1.0) g_ctx.synth_phase -= 1.0;
        }
    }

    if (g_ctx.rta) {
        rta_process_interleaved_stereo(g_ctx.rta, out, (size_t)frames);
        RtaTelemetry tel = {0};
        rta_get_telemetry(g_ctx.rta, &tel);
        atomic_store(&g_ctx.sub_milli,  (int)(tel.sub_rms  * 1000.0f));
        atomic_store(&g_ctx.body_milli, (int)(tel.body_rms * 1000.0f));
        atomic_store(&g_ctx.air_milli,  (int)(tel.air_rms  * 1000.0f));
        atomic_store(&g_ctx.kick_milli, (int)(tel.kick_env * 1000.0f));
    }
}

int main(int argc, char **argv) {
    const char *input_path = NULL;
    if (argc > 1 && argv[1][0] != '\0') {
        input_path = argv[1];
    } else if (FileExists("assets/input.wav")) {
        input_path = "assets/input.wav";
    } else if (FileExists("assets/out_aif_cd.wav")) {
        input_path = "assets/out_aif_cd.wav";
    }

    InitWindow(1024, 640, "RTA-SFW 3D Topographic Flight Shader");
    InitAudioDevice();
    SetMasterVolume(1.0f);
    SetAudioStreamBufferSizeDefault(512);

    atomic_init(&g_ctx.preset_id, 0);
    atomic_init(&g_ctx.flight_permille, 250);
    atomic_init(&g_ctx.paused, 0);
    atomic_init(&g_ctx.sub_milli, 0);
    atomic_init(&g_ctx.body_milli, 0);
    atomic_init(&g_ctx.air_milli, 0);
    atomic_init(&g_ctx.kick_milli, 0);

    g_ctx.rta = rta_create(44100.0f);
    rta_set_preset(g_ctx.rta, RTA_PRESET_STUDIO_54);

    if (input_path && load_audio_file_44100_stereo(input_path, &g_ctx.file_stereo, &g_ctx.file_frames)) {
        g_ctx.has_file = true;
    }

    Mesh plane = GenMeshPlane(44.0f, 44.0f, 72, 72);
    Model terrain = LoadModelFromMesh(plane);
    Shader topo_shader = LoadShaderFromMemory(kTopoVertShader, kTopoFragShader);
    terrain.materials[0].shader = topo_shader;

    int loc_sub    = GetShaderLocation(topo_shader, "uSub");
    int loc_body   = GetShaderLocation(topo_shader, "uBody");
    int loc_air    = GetShaderLocation(topo_shader, "uAir");
    int loc_kick   = GetShaderLocation(topo_shader, "uKick");
    int loc_flight = GetShaderLocation(topo_shader, "uFlight");

    Camera3D cam = {
        .position   = {0.0f, 5.0f, 18.0f},
        .target     = {0.0f, 0.0f, -4.0f},
        .up         = {0.0f, 1.0f, 0.0f},
        .fovy       = 50.0f,
        .projection = CAMERA_PERSPECTIVE
    };

    AudioStream stream = LoadAudioStream(44100, 32, 2);
    SetAudioStreamVolume(stream, 1.0f);
    SetAudioStreamCallback(stream, flight_audio_callback);
    PlayAudioStream(stream);
    SetTargetFPS(60);

    char src_buf[640];
    if (g_ctx.has_file && input_path) {
        snprintf(src_buf, sizeof(src_buf), "SOURCE: %s", input_path);
    } else {
        snprintf(src_buf, sizeof(src_buf), "SOURCE: Built-in Synth (pass .wav or .aif as arg 1)");
    }

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_ONE) || IsKeyPressed(KEY_KP_1)) {
            atomic_store(&g_ctx.preset_id, 0);
        }
        if (IsKeyPressed(KEY_TWO) || IsKeyPressed(KEY_KP_2)) {
            atomic_store(&g_ctx.preset_id, 1);
        }
        if (IsKeyPressed(KEY_SPACE)) {
            int p = atomic_load(&g_ctx.paused);
            atomic_store(&g_ctx.paused, p ? 0 : 1);
        }

        int flight_pm = atomic_load(&g_ctx.flight_permille);
        if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W)) {
            flight_pm += 15;
            if (flight_pm > 1000) flight_pm = 1000;
        }
        if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S)) {
            flight_pm -= 15;
            if (flight_pm < 0) flight_pm = 0;
        }
        atomic_store(&g_ctx.flight_permille, flight_pm);

        float flight = (float)flight_pm / 1000.0f;
        cam.position.y = 2.0f + flight * 14.0f;
        cam.position.z = 18.0f - flight * 6.0f;
        cam.target.y   = -1.5f + flight * 1.5f;

        float u_sub  = (float)atomic_load(&g_ctx.sub_milli)  / 400.0f;
        float u_body = (float)atomic_load(&g_ctx.body_milli) / 400.0f;
        float u_air  = (float)atomic_load(&g_ctx.air_milli)  / 400.0f;
        float u_kick = (float)atomic_load(&g_ctx.kick_milli) / 400.0f;

        SetShaderValue(topo_shader, loc_sub,    &u_sub,    SHADER_UNIFORM_FLOAT);
        SetShaderValue(topo_shader, loc_body,   &u_body,   SHADER_UNIFORM_FLOAT);
        SetShaderValue(topo_shader, loc_air,    &u_air,    SHADER_UNIFORM_FLOAT);
        SetShaderValue(topo_shader, loc_kick,   &u_kick,   SHADER_UNIFORM_FLOAT);
        SetShaderValue(topo_shader, loc_flight, &flight,   SHADER_UNIFORM_FLOAT);

        RtaRaylibHudState hud = {
            .preset_id    = atomic_load(&g_ctx.preset_id),
            .flight_0_to_1 = flight,
            .sub_norm     = u_sub,
            .body_norm    = u_body,
            .air_norm     = u_air,
            .kick_norm    = u_kick,
            .paused       = atomic_load(&g_ctx.paused) != 0,
            .source_label = src_buf
        };

        BeginDrawing();
        ClearBackground((Color){8, 10, 14, 255});

        BeginMode3D(cam);
        DrawModel(terrain, (Vector3){0.0f, 0.0f, 0.0f}, 1.0f, WHITE);
        EndMode3D();

        rta_ui_draw_hud(GetScreenWidth(), GetScreenHeight(), &hud);

        EndDrawing();
    }

    StopAudioStream(stream);
    UnloadAudioStream(stream);
    UnloadShader(topo_shader);
    UnloadModel(terrain);
    free(g_ctx.file_stereo);
    rta_destroy(g_ctx.rta);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
