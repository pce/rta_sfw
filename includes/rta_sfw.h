#ifndef RTA_SFW_H
#define RTA_SFW_H

#include <stddef.h>
#include <stdint.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* --- 1. Significators (Perceptual Band Energy) --- */
typedef struct {
    float sub_rms;        /* 20 - 90 Hz energy */
    float body_rms;       /* 90 - 2500 Hz energy */
    float air_rms;        /* 2500+ Hz energy */
    float kick_env;       /* Fast transient envelope for 4/4 pulse */
} RtaSignificators;

/* --- 2. Biquad Filter (Crossover & Spectral Tilt) --- */
typedef enum {
    RTA_BIQUAD_LPF = 0,
    RTA_BIQUAD_HPF = 1,
    RTA_BIQUAD_HIGHSHELF = 2
} RtaBiquadType;

typedef struct {
    float b0, b1, b2, a1, a2; /* Normalized coefficients (a0 = 1.0) */
} RtaBiquadCoeffs;

typedef struct {
    float z1_l, z2_l;         /* Transposed Direct Form II state (Left) */
    float z1_r, z2_r;         /* Transposed Direct Form II state (Right) */
    RtaBiquadCoeffs coeffs;
} RtaBiquadState;

void rta_biquad_init(RtaBiquadState *st);
void rta_biquad_set(RtaBiquadState *st, RtaBiquadType type,
                    float freq_hz, float q, float gain_db, float sample_rate);
void rta_biquad_process_stereo(RtaBiquadState *st, float *io_lr, size_t frames);

/* --- 3. Mid/Side Width Operator (Air / Topographic Spread) --- */
typedef struct {
    float width;          /* 0.0 = mono, 1.0 = standard stereo, >1.0 = wider */
    float smooth_coeff;
    float cur_width;
} RtaMsWidthState;

void rta_ms_width_init(RtaMsWidthState *st, float width, float smooth_ms, float sample_rate);
void rta_ms_width_set_target(RtaMsWidthState *st, float width);
void rta_ms_width_process_stereo(RtaMsWidthState *st, float *io_lr, size_t frames);

/* --- 4. Sweet Spot Protector (Brickwall Limiter + Soft Clipper) --- */
typedef struct {
    float ceiling_lin;    /* e.g. 0.891f (-1.0 dBFS) */
    float drive;          /* Soft clip curve amount (1.0f = gentle) */
    float env;            /* Peak/gain-reduction envelope */
    float attack_coeff;
    float release_coeff;
} RtaSweetSpotState;

void rta_sweetspot_init(RtaSweetSpotState *st, float ceiling_db, float drive,
                        float attack_ms, float release_ms, float sample_rate);
void rta_sweetspot_process_stereo(RtaSweetSpotState *st, float *io_lr, size_t frames);

/* --- 5. Full 3-Band Topographic Finalizer Graph --- */
typedef struct {
    float sample_rate;

    /* Crossover stages */
    RtaBiquadState sub_lpf;
    RtaBiquadState body_hpf, body_lpf;
    RtaBiquadState air_hpf;

    /* Band operators */
    RtaMsWidthState air_width;

    /* Live band gains (updated from matrix + altitude/perspective) */
    float sub_gain;
    float body_gain;
    float air_gain;

    /* Patch matrix weights: [source][target] */
    float sub_to_body_duck;   /* Sub energy ducks Body */
    float kick_to_sub_duck;   /* Kick transient ducks Sub */
    float kick_to_body_duck;  /* Kick transient ducks Body */
    float flight_tilt;        /* 0.0 (Frog) -> 1.0 (Flight perspective) */

    /* Significators & Safety */
    RtaSignificators sig;
    RtaSweetSpotState protector;
} RtaTopographicGraph;

void rta_graph_init(RtaTopographicGraph *g, float sample_rate);
void rta_graph_apply_preset(RtaTopographicGraph *g, int preset_id); /* 0=Studio54, 1=Techno */
void rta_graph_set_flight(RtaTopographicGraph *g, float flight_0_to_1);
void rta_graph_process_block(RtaTopographicGraph *g, float *io_lr, size_t frames);

#endif /* RTA_SFW_H */