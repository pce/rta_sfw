#include "rta_sfw.h"

static inline float smooth_coeff_from_ms(float ms, float sample_rate) {
    if (ms <= 0.001f || sample_rate <= 0.0f) return 1.0f;
    float samples = (ms * 0.001f) * sample_rate;
    return 1.0f - expf(-1.0f / samples);
}

static inline float fast_softclip_tanh(float x, float drive) {
    float u = x * drive;
    /* Smooth rational approximation to tanh: fast on small cores/DSPs */
    float u2 = u * u;
    return (u * (27.0f + u2)) / (27.0f + 9.0f * u2);
}

/* --- Biquad --- */
void rta_biquad_init(RtaBiquadState *st) {
    st->z1_l = st->z2_l = 0.0f;
    st->z1_r = st->z2_r = 0.0f;
    st->coeffs.b0 = 1.0f; st->coeffs.b1 = 0.0f; st->coeffs.b2 = 0.0f;
    st->coeffs.a1 = 0.0f; st->coeffs.a2 = 0.0f;
}

void rta_biquad_set(RtaBiquadState *st, RtaBiquadType type,
                    float freq_hz, float q, float gain_db, float sample_rate) {
    float w0 = 2.0f * (float)M_PI * freq_hz / sample_rate;
    float cos_w0 = cosf(w0);
    float sin_w0 = sinf(w0);
    float alpha = sin_w0 / (2.0f * (q > 0.01f ? q : 0.7071f));

    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;

    if (type == RTA_BIQUAD_LPF) {
        b0 = (1.0f - cos_w0) * 0.5f;
        b1 = 1.0f - cos_w0;
        b2 = (1.0f - cos_w0) * 0.5f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cos_w0;
        a2 = 1.0f - alpha;
    } else if (type == RTA_BIQUAD_HPF) {
        b0 = (1.0f + cos_w0) * 0.5f;
        b1 = -(1.0f + cos_w0);
        b2 = (1.0f + cos_w0) * 0.5f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cos_w0;
        a2 = 1.0f - alpha;
    } else if (type == RTA_BIQUAD_HIGHSHELF) {
        float A = powf(10.0f, gain_db / 40.0f);
        float sqA = sqrtf(A);
        b0 = A * ((A + 1.0f) + (A - 1.0f) * cos_w0 + 2.0f * sqA * alpha);
        b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cos_w0);
        b2 = A * ((A + 1.0f) + (A - 1.0f) * cos_w0 - 2.0f * sqA * alpha);
        a0 = (A + 1.0f) - (A - 1.0f) * cos_w0 + 2.0f * sqA * alpha;
        a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cos_w0);
        a2 = (A + 1.0f) - (A - 1.0f) * cos_w0 - 2.0f * sqA * alpha;
    }

    st->coeffs.b0 = b0 / a0;
    st->coeffs.b1 = b1 / a0;
    st->coeffs.b2 = b2 / a0;
    st->coeffs.a1 = a1 / a0;
    st->coeffs.a2 = a2 / a0;
}

void rta_biquad_process_stereo(RtaBiquadState *st, float *io_lr, size_t frames) {
    const float b0 = st->coeffs.b0, b1 = st->coeffs.b1, b2 = st->coeffs.b2;
    const float a1 = st->coeffs.a1, a2 = st->coeffs.a2;
    float z1_l = st->z1_l, z2_l = st->z2_l;
    float z1_r = st->z1_r, z2_r = st->z2_r;

    for (size_t i = 0; i < frames; ++i) {
        float xl = io_lr[2 * i];
        float yl = b0 * xl + z1_l;
        z1_l = b1 * xl - a1 * yl + z2_l;
        z2_l = b2 * xl - a2 * yl;
        io_lr[2 * i] = yl;

        float xr = io_lr[2 * i + 1];
        float yr = b0 * xr + z1_r;
        z1_r = b1 * xr - a1 * yr + z2_r;
        z2_r = b2 * xr - a2 * yr;
        io_lr[2 * i + 1] = yr;
    }

    st->z1_l = z1_l; st->z2_l = z2_l;
    st->z1_r = z1_r; st->z2_r = z2_r;
}

/* --- Mid/Side Width --- */
void rta_ms_width_init(RtaMsWidthState *st, float width, float smooth_ms, float sample_rate) {
    st->width = width;
    st->cur_width = width;
    st->smooth_coeff = smooth_coeff_from_ms(smooth_ms, sample_rate);
}

void rta_ms_width_set_target(RtaMsWidthState *st, float width) {
    st->width = width < 0.0f ? 0.0f : (width > 2.0f ? 2.0f : width);
}

void rta_ms_width_process_stereo(RtaMsWidthState *st, float *io_lr, size_t frames) {
    float w = st->cur_width;
    const float k = st->smooth_coeff;
    const float target = st->width;

    for (size_t i = 0; i < frames; ++i) {
        w += k * (target - w);
        float l = io_lr[2 * i];
        float r = io_lr[2 * i + 1];
        float mid = 0.5f * (l + r);
        float side = 0.5f * (l - r) * w;
        io_lr[2 * i]     = mid + side;
        io_lr[2 * i + 1] = mid - side;
    }
    st->cur_width = w;
}

/* --- Sweet Spot Protector --- */
void rta_sweetspot_init(RtaSweetSpotState *st, float ceiling_db, float drive,
                        float attack_ms, float release_ms, float sample_rate) {
    st->ceiling_lin = powf(10.0f, ceiling_db / 20.0f);
    st->drive = drive < 0.1f ? 1.0f : drive;
    st->env = 1.0f;
    st->attack_coeff = smooth_coeff_from_ms(attack_ms, sample_rate);
    st->release_coeff = smooth_coeff_from_ms(release_ms, sample_rate);
}

void rta_sweetspot_process_stereo(RtaSweetSpotState *st, float *io_lr, size_t frames) {
    float env = st->env;
    const float ceil = st->ceiling_lin;
    const float atk = st->attack_coeff;
    const float rel = st->release_coeff;
    const float drv = st->drive;

    for (size_t i = 0; i < frames; ++i) {
        float l = io_lr[2 * i];
        float r = io_lr[2 * i + 1];
        float peak = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);

        float desired_gain = (peak > ceil && peak > 1e-7f) ? (ceil / peak) : 1.0f;
        float coeff = (desired_gain < env) ? atk : rel;
        env += coeff * (desired_gain - env);

        float out_l = fast_softclip_tanh(l * env, drv);
        float out_r = fast_softclip_tanh(r * env, drv);

        /* Hard clamp guarantee against NaN or extreme values */
        if (!(out_l >= -ceil && out_l <= ceil)) out_l = 0.0f;
        if (!(out_r >= -ceil && out_r <= ceil)) out_r = 0.0f;

        io_lr[2 * i] = out_l;
        io_lr[2 * i + 1] = out_r;
    }
    st->env = env;
}

/* --- Topographic Graph --- */
void rta_graph_init(RtaTopographicGraph *g, float sample_rate) {
    g->sample_rate = sample_rate;
    rta_biquad_init(&g->sub_lpf);
    rta_biquad_init(&g->body_hpf);
    rta_biquad_init(&g->body_lpf);
    rta_biquad_init(&g->air_hpf);

    rta_biquad_set(&g->sub_lpf, RTA_BIQUAD_LPF, 90.0f, 0.7071f, 0.0f, sample_rate);
    rta_biquad_set(&g->body_hpf, RTA_BIQUAD_HPF, 90.0f, 0.7071f, 0.0f, sample_rate);
    rta_biquad_set(&g->body_lpf, RTA_BIQUAD_LPF, 2500.0f, 0.7071f, 0.0f, sample_rate);
    rta_biquad_set(&g->air_hpf, RTA_BIQUAD_HPF, 2500.0f, 0.7071f, 0.0f, sample_rate);

    rta_ms_width_init(&g->air_width, 1.0f, 25.0f, sample_rate);
    rta_sweetspot_init(&g->protector, -1.0f, 1.0f, 0.5f, 80.0f, sample_rate);

    g->sub_gain = 1.0f;
    g->body_gain = 1.0f;
    g->air_gain = 1.0f;
    g->flight_tilt = 0.0f;
    g->sig.sub_rms = 0.0f;
    g->sig.body_rms = 0.0f;
    g->sig.air_rms = 0.0f;
    g->sig.kick_env = 0.0f;

    rta_graph_apply_preset(g, 0);
}

void rta_graph_apply_preset(RtaTopographicGraph *g, int preset_id) {
    if (preset_id == 1) {
        /* Techno Pumper: tight transient ducking */
        g->sub_to_body_duck = 0.25f;
        g->kick_to_sub_duck  = 0.35f;
        g->kick_to_body_duck = 0.45f;
        rta_ms_width_set_target(&g->air_width, 1.25f);
    } else {
        /* Studio 54 Glide: smooth sub-to-body breathing */
        g->sub_to_body_duck = 0.20f;
        g->kick_to_sub_duck  = 0.10f;
        g->kick_to_body_duck = 0.15f;
        rta_ms_width_set_target(&g->air_width, 1.10f);
    }
}

void rta_graph_set_flight(RtaTopographicGraph *g, float flight_0_to_1) {
    if (flight_0_to_1 < 0.0f) flight_0_to_1 = 0.0f;
    if (flight_0_to_1 > 1.0f) flight_0_to_1 = 1.0f;
    g->flight_tilt = flight_0_to_1;
    /* Taking off widens high-band panorama while keeping sub grounded */
    rta_ms_width_set_target(&g->air_width, 1.0f + 0.6f * flight_0_to_1);
}

void rta_graph_process_block(RtaTopographicGraph *g, float *io_lr, size_t frames) {
    /* Stack scratch buffers for small/medium blocks (e.g., <= 2048 frames) */
    float sub_buf[4096];
    float body_buf[4096];
    float air_buf[4096];
    size_t n = frames > 2048 ? 2048 : frames;

    float sub_sq = 0.0f, body_sq = 0.0f, air_sq = 0.0f;

    for (size_t i = 0; i < n; ++i) {
        float l = io_lr[2 * i];
        float r = io_lr[2 * i + 1];
        sub_buf[2 * i] = l;     sub_buf[2 * i + 1] = r;
        body_buf[2 * i] = l;    body_buf[2 * i + 1] = r;
        air_buf[2 * i] = l;     air_buf[2 * i + 1] = r;
    }

    /* 1. Isolate bands */
    rta_biquad_process_stereo(&g->sub_lpf, sub_buf, n);
    rta_biquad_process_stereo(&g->body_hpf, body_buf, n);
    rta_biquad_process_stereo(&g->body_lpf, body_buf, n);
    rta_biquad_process_stereo(&g->air_hpf, air_buf, n);

    /* 2. Measure Significators */
    for (size_t i = 0; i < n; ++i) {
        float sl = sub_buf[2 * i], sr = sub_buf[2 * i + 1];
        float bl = body_buf[2 * i], br = body_buf[2 * i + 1];
        float al = air_buf[2 * i], ar = air_buf[2 * i + 1];

        sub_sq  += 0.5f * (sl * sl + sr * sr);
        body_sq += 0.5f * (bl * bl + br * br);
        air_sq  += 0.5f * (al * al + ar * ar);
    }

    float inv_n = n > 0 ? (1.0f / (float)n) : 0.0f;
    g->sig.sub_rms  = sqrtf(sub_sq * inv_n);
    g->sig.body_rms = sqrtf(body_sq * inv_n);
    g->sig.air_rms  = sqrtf(air_sq * inv_n);

    /* Fast kick/transient detector from sub + body surge */
    float impulse = 0.7f * g->sig.sub_rms + 0.3f * g->sig.body_rms;
    if (impulse > g->sig.kick_env) {
        g->sig.kick_env += 0.4f * (impulse - g->sig.kick_env);
    } else {
        g->sig.kick_env *= 0.85f;
    }

    /* 3. Compute matrix gains */
    float sub_duck  = g->sig.kick_env * g->kick_to_sub_duck;
    float body_duck = g->sig.sub_rms * g->sub_to_body_duck + g->sig.kick_env * g->kick_to_body_duck;

    float target_sub_gain  = 1.0f - sub_duck;
    float target_body_gain = (1.0f - body_duck) * (1.0f - 0.15f * g->flight_tilt);
    float target_air_gain  = 1.0f + 0.25f * g->flight_tilt;

    if (target_sub_gain < 0.25f) target_sub_gain = 0.25f;
    if (target_body_gain < 0.25f) target_body_gain = 0.25f;

    g->sub_gain  += 0.2f * (target_sub_gain - g->sub_gain);
    g->body_gain += 0.2f * (target_body_gain - g->body_gain);
    g->air_gain  += 0.2f * (target_air_gain - g->air_gain);

    /* 4. Apply spatial width to Air band */
    rta_ms_width_process_stereo(&g->air_width, air_buf, n);

    /* 5. Sum bands */
    for (size_t i = 0; i < n; ++i) {
        io_lr[2 * i]     = sub_buf[2 * i]     * g->sub_gain
                         + body_buf[2 * i]    * g->body_gain
                         + air_buf[2 * i]     * g->air_gain;
        io_lr[2 * i + 1] = sub_buf[2 * i + 1] * g->sub_gain
                         + body_buf[2 * i + 1]* g->body_gain
                         + air_buf[2 * i + 1] * g->air_gain;
    }

    /* 6. Sweet Spot Protector */
    rta_sweetspot_process_stereo(&g->protector, io_lr, n);
}