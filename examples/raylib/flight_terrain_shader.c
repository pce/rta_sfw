#include "rta_sfw_abi.h"
#include "raylib.h"
#include <math.h>
#include <stdatomic.h>

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
    "    vec3 frogCol = vec3(0.95, 0.45, 0.15) * (0.4 + uSub + uKick);\n"
    "    vec3 flightCol = vec3(0.20, 0.75, 0.95) * (0.4 + uAir + 0.4 * uFlight);\n"
    "    vec3 baseCol = mix(frogCol, flightCol, clamp(uFlight, 0.0, 1.0));\n"
    "    vec3 col = baseCol * (0.55 + 0.18 * vHeight) + vec3(0.85, 0.92, 1.0) * line * (0.25 + 0.5 * uBody);\n"
    "    finalColor = vec4(col, 1.0);\n"
    "}\n";

typedef struct {
    RtaContext *rta;
    double phase;
    double beat;
    atomic_int sub_milli;
    atomic_int body_milli;
    atomic_int air_milli;
    atomic_int kick_milli;
} FlightAudioState;

static FlightAudioState g_state = {0};

static void audio_cb(void *buffer_data, unsigned int frames) {
    float *out = (float *)buffer_data;
    const double sr = 44100.0;
    const double bpm = 126.0;
    const double bps = bpm / 60.0;

    for (unsigned int i = 0; i < frames; ++i) {
        double env = pow(fmax(0.0, 1.0 - g_state.beat), 4.0);
        float kick = (float)(sin(g_state.phase * 2.0 * M_PI * (0.5 + 1.2 * env)) * env * 0.55);
        float chord = (float)(0.20 * sin(g_state.phase * 4.0 * M_PI) * (0.4 + 0.6 * (1.0 - env)));
        out[2 * i]     = kick + chord;
        out[2 * i + 1] = kick * 0.95f - chord * 0.85f;

        g_state.beat += bps / sr;
        if (g_state.beat >= 1.0) g_state.beat -= 1.0;

        g_state.phase += 55.0 / sr;
        if (g_state.phase >= 1.0) g_state.phase -= 1.0;
    }

    if (g_state.rta) {
        rta_process_interleaved_stereo(g_state.rta, out, (size_t)frames);
        RtaTelemetry tel = {0};
        rta_get_telemetry(g_state.rta, &tel);
        atomic_store(&g_state.sub_milli,  (int)(tel.sub_rms  * 1000.0f));
        atomic_store(&g_state.body_milli, (int)(tel.body_rms * 1000.0f));
        atomic_store(&g_state.air_milli,  (int)(tel.air_rms  * 1000.0f));
        atomic_store(&g_state.kick_milli, (int)(tel.kick_env * 1000.0f));
    }
}

int main(void) {
    InitWindow(960, 600, "RTA-SFW 3D Topographic Flight Shader");
    InitAudioDevice();
    SetAudioStreamBufferSizeDefault(512);

    g_state.rta = rta_create(44100.0f);
    rta_set_preset(g_state.rta, RTA_PRESET_STUDIO_54);

    Mesh plane = GenMeshPlane(40.0f, 40.0f, 64, 64);
    Model terrain = LoadModelFromMesh(plane);
    Shader topo_shader = LoadShaderFromMemory(kTopoVertShader, kTopoFragShader);
    terrain.materials[0].shader = topo_shader;

    int loc_sub    = GetShaderLocation(topo_shader, "uSub");
    int loc_body   = GetShaderLocation(topo_shader, "uBody");
    int loc_air    = GetShaderLocation(topo_shader, "uAir");
    int loc_kick   = GetShaderLocation(topo_shader, "uKick");
    int loc_flight = GetShaderLocation(topo_shader, "uFlight");

    float flight = 0.25f;
    Camera3D cam = {
        .position = {0.0f, 5.0f, 18.0f},
        .target   = {0.0f, 0.0f, -4.0f},
        .up       = {0.0f, 1.0f, 0.0f},
        .fovy     = 50.0f,
        .projection = CAMERA_PERSPECTIVE
    };

    AudioStream stream = LoadAudioStream(44100, 32, 2);
    SetAudioStreamCallback(stream, audio_cb);
    PlayAudioStream(stream);
    SetTargetFPS(60);

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_ONE)) rta_set_preset(g_state.rta, RTA_PRESET_STUDIO_54);
        if (IsKeyPressed(KEY_TWO)) rta_set_preset(g_state.rta, RTA_PRESET_TECHNO_PUMPER);

        if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W)) {
            flight += 0.015f;
            if (flight > 1.0f) flight = 1.0f;
        }
        if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S)) {
            flight -= 0.015f;
            if (flight < 0.0f) flight = 0.0f;
        }

        rta_set_flight(g_state.rta, flight);
        cam.position.y = 2.0f + flight * 14.0f;
        cam.position.z = 18.0f - flight * 6.0f;
        cam.target.y   = -1.5f + flight * 1.5f;

        float u_sub    = (float)atomic_load(&g_state.sub_milli)  / 400.0f;
        float u_body   = (float)atomic_load(&g_state.body_milli) / 400.0f;
        float u_air    = (float)atomic_load(&g_state.air_milli)  / 400.0f;
        float u_kick   = (float)atomic_load(&g_state.kick_milli) / 400.0f;
        float angle_deg = flight * 30.0f;

        SetShaderValue(topo_shader, loc_sub,    &u_sub,    SHADER_UNIFORM_FLOAT);
        SetShaderValue(topo_shader, loc_body,   &u_body,   SHADER_UNIFORM_FLOAT);
        SetShaderValue(topo_shader, loc_air,    &u_air,    SHADER_UNIFORM_FLOAT);
        SetShaderValue(topo_shader, loc_kick,   &u_kick,   SHADER_UNIFORM_FLOAT);
        SetShaderValue(topo_shader, loc_flight, &flight,   SHADER_UNIFORM_FLOAT);

        BeginDrawing();
        ClearBackground(BLACK);

        BeginMode3D(cam);
        DrawModel(terrain, (Vector3){0.0f, 0.0f, 0.0f}, 1.0f, WHITE);
        EndMode3D();

        DrawText(TextFormat("Perspective: %.1f deg (0=Frog, 30=Flight) [W/S or UP/DOWN]", angle_deg),
                 20, 20, 18, RAYWHITE);
        DrawText("[1] Studio 54   [2] Techno Pumper", 20, 46, 18, GREEN);

        EndDrawing();
    }

    StopAudioStream(stream);
    UnloadAudioStream(stream);
    UnloadShader(topo_shader);
    UnloadModel(terrain);
    rta_destroy(g_state.rta);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}

