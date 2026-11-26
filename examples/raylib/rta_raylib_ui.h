#ifndef RTA_RAYLIB_UI_H
#define RTA_RAYLIB_UI_H

#include "raylib.h"
#include <math.h>
#include <stdbool.h>

typedef struct RtaRaylibHudState {
    int preset_id;        /* 0 = Studio 54 Glide, 1 = Techno Pumper */
    float flight_0_to_1;  /* 0.0f (Frog 0 deg) .. 1.0f (Flight 30 deg) */
    float sub_norm;       /* Normalized Sub energy */
    float body_norm;      /* Normalized Body energy */
    float air_norm;       /* Normalized Air energy */
    float kick_norm;      /* Normalized Kick transient energy */
    bool paused;
    const char *source_label;
} RtaRaylibHudState;

static inline float rta_ui_clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* Futuristic chamfered side rails + outer top/bottom brackets (center stays open) */
static inline void rta_ui_draw_futuristic_frame(int width, int height,
                                                float air_norm, float kick_norm,
                                                float flight_0_to_1) {
    float air = rta_ui_clamp01(air_norm);
    float kick = rta_ui_clamp01(kick_norm);
    float flight = rta_ui_clamp01(flight_0_to_1);

    float pad = 16.0f;
    float side_w = 18.0f + air * 14.0f;
    float corner_len = 90.0f + flight * 60.0f;
    float bevel = 14.0f;
    float fw = (float)width;
    float fh = (float)height;

    Color accent = (Color){
        (unsigned char)fminf(255.0f, 55.0f + kick * 180.0f),
        (unsigned char)fminf(255.0f, 190.0f + air * 65.0f),
        (unsigned char)fminf(255.0f, 225.0f + flight * 30.0f),
        220
    };
    Color rail_fill = (Color){16, 26, 38, 130};

    /* Left shaped rail */
    Vector2 l_pts[6] = {
        {pad, pad + bevel},
        {pad + bevel, pad},
        {pad + side_w, pad + bevel * 1.5f},
        {pad + side_w, fh - pad - bevel * 1.5f},
        {pad + bevel, fh - pad},
        {pad, fh - pad - bevel}
    };
    DrawTriangleFan(l_pts, 6, rail_fill);
    DrawLineEx(l_pts[0], l_pts[1], 2.5f, accent);
    DrawLineEx(l_pts[1], l_pts[2], 2.0f, accent);
    DrawLineEx(l_pts[2], l_pts[3], 2.0f, accent);
    DrawLineEx(l_pts[3], l_pts[4], 2.0f, accent);
    DrawLineEx(l_pts[4], l_pts[5], 2.5f, accent);

    /* Right shaped rail */
    Vector2 r_pts[6] = {
        {fw - pad, pad + bevel},
        {fw - pad - bevel, pad},
        {fw - pad - side_w, pad + bevel * 1.5f},
        {fw - pad - side_w, fh - pad - bevel * 1.5f},
        {fw - pad - bevel, fh - pad},
        {fw - pad, fh - pad - bevel}
    };
    DrawTriangleFan(r_pts, 6, rail_fill);
    DrawLineEx(r_pts[0], r_pts[1], 2.5f, accent);
    DrawLineEx(r_pts[1], r_pts[2], 2.0f, accent);
    DrawLineEx(r_pts[2], r_pts[3], 2.0f, accent);
    DrawLineEx(r_pts[3], r_pts[4], 2.0f, accent);
    DrawLineEx(r_pts[4], r_pts[5], 2.5f, accent);

    /* Top-left and top-right outer segments (center stays open) */
    DrawLineEx((Vector2){pad + bevel, pad}, (Vector2){pad + corner_len, pad}, 2.5f, accent);
    DrawLineEx((Vector2){pad + corner_len, pad}, (Vector2){pad + corner_len + bevel, pad + bevel * 0.6f}, 2.0f, accent);

    DrawLineEx((Vector2){fw - pad - bevel, pad}, (Vector2){fw - pad - corner_len, pad}, 2.5f, accent);
    DrawLineEx((Vector2){fw - pad - corner_len, pad}, (Vector2){fw - pad - corner_len - bevel, pad + bevel * 0.6f}, 2.0f, accent);

    /* Bottom-left and bottom-right outer segments (center stays open) */
    DrawLineEx((Vector2){pad + bevel, fh - pad}, (Vector2){pad + corner_len, fh - pad}, 2.5f, accent);
    DrawLineEx((Vector2){pad + corner_len, fh - pad}, (Vector2){pad + corner_len + bevel, fh - pad - bevel * 0.6f}, 2.0f, accent);

    DrawLineEx((Vector2){fw - pad - bevel, fh - pad}, (Vector2){fw - pad - corner_len, fh - pad}, 2.5f, accent);
    DrawLineEx((Vector2){fw - pad - corner_len, fh - pad}, (Vector2){fw - pad - corner_len - bevel, fh - pad - bevel * 0.6f}, 2.0f, accent);
}

/* Top-left preset & perspective readout + bottom-left audio source readout */
static inline void rta_ui_draw_hud(int width, int height, const RtaRaylibHudState *hud) {
    if (!hud) return;

    float flight = rta_ui_clamp01(hud->flight_0_to_1);
    float angle_deg = flight * 30.0f;
    const char *preset_txt = (hud->preset_id == 1)
        ? "PRESET [2]: TECHNO PUMPER"
        : "PRESET [1]: STUDIO 54 GLIDE";
    Color preset_col = (hud->preset_id == 1) ? ORANGE : SKYBLUE;

    rta_ui_draw_futuristic_frame(width, height, hud->air_norm, hud->kick_norm, flight);

    DrawText(preset_txt, 46, 28, 20, preset_col);
    DrawText(TextFormat("PERSPECTIVE: %.1f deg [W/S or UP/DOWN] | [SPACE] %s",
                        angle_deg, hud->paused ? "Resume" : "Pause"),
             46, 54, 16, RAYWHITE);

    const char *src_txt = hud->source_label ? hud->source_label : "SOURCE: Built-in Synth";
    DrawText(src_txt, 46, height - 46, 15, LIGHTGRAY);
}

#endif /* RTA_RAYLIB_UI_H */