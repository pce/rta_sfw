#include "rta_sfw_abi.h"
#include "rta_sfw.h"
#include <stdlib.h>
#include <string.h>

#define RTA_ABI_MAX_BLOCK 1024

struct RtaContext {
    RtaTopographicGraph graph;
    float matrix[3][3];
    float base_air_width;
};

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void sync_matrix_to_graph(RtaContext *ctx) {
    ctx->graph.sub_to_body_duck  = clampf(-ctx->matrix[RTA_SRC_SUB][RTA_TGT_BODY_GAIN], 0.0f, 1.0f);
    ctx->graph.kick_to_sub_duck  = clampf(-ctx->matrix[RTA_SRC_KICK][RTA_TGT_SUB_GAIN], 0.0f, 1.0f);
    ctx->graph.kick_to_body_duck = clampf(-ctx->matrix[RTA_SRC_KICK][RTA_TGT_BODY_GAIN], 0.0f, 1.0f);

    float air_mod = ctx->matrix[RTA_SRC_BODY][RTA_TGT_AIR_WIDTH]
                  + ctx->matrix[RTA_SRC_KICK][RTA_TGT_AIR_WIDTH];
    float target_w = clampf(ctx->base_air_width + air_mod * ctx->graph.sig.body_rms, 0.0f, 2.0f);
    rta_ms_width_set_target(&ctx->graph.air_width, target_w);
}

RtaContext *rta_create(float sample_rate) {
    RtaContext *ctx = (RtaContext *)calloc(1, sizeof(RtaContext));
    if (!ctx) return NULL;
    rta_reset(ctx, sample_rate);
    return ctx;
}

void rta_destroy(RtaContext *ctx) {
    free(ctx);
}

void rta_reset(RtaContext *ctx, float sample_rate) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    rta_graph_init(&ctx->graph, sample_rate > 1000.0f ? sample_rate : 48000.0f);
    rta_set_preset(ctx, RTA_PRESET_STUDIO_54);
}

void rta_set_preset(RtaContext *ctx, RtaPresetId preset) {
    if (!ctx) return;
    memset(ctx->matrix, 0, sizeof(ctx->matrix));
    rta_graph_apply_preset(&ctx->graph, (int)preset);

    if (preset == RTA_PRESET_TECHNO_PUMPER) {
        ctx->matrix[RTA_SRC_KICK][RTA_TGT_SUB_GAIN]  = -0.35f;
        ctx->matrix[RTA_SRC_KICK][RTA_TGT_BODY_GAIN] = -0.45f;
        ctx->matrix[RTA_SRC_SUB][RTA_TGT_BODY_GAIN]  = -0.25f;
        ctx->matrix[RTA_SRC_KICK][RTA_TGT_AIR_WIDTH] =  0.20f;
        ctx->base_air_width = 1.25f;
    } else {
        ctx->matrix[RTA_SRC_SUB][RTA_TGT_BODY_GAIN]  = -0.20f;
        ctx->matrix[RTA_SRC_KICK][RTA_TGT_SUB_GAIN]  = -0.10f;
        ctx->matrix[RTA_SRC_KICK][RTA_TGT_BODY_GAIN] = -0.15f;
        ctx->matrix[RTA_SRC_BODY][RTA_TGT_AIR_WIDTH] =  0.15f;
        ctx->base_air_width = 1.10f;
    }
    sync_matrix_to_graph(ctx);
}

void rta_set_flight(RtaContext *ctx, float flight_0_to_1) {
    if (!ctx) return;
    rta_graph_set_flight(&ctx->graph, flight_0_to_1);
}

void rta_set_air_width(RtaContext *ctx, float width_0_to_2) {
    if (!ctx) return;
    ctx->base_air_width = clampf(width_0_to_2, 0.0f, 2.0f);
    sync_matrix_to_graph(ctx);
}

void rta_set_matrix_weight(RtaContext *ctx, RtaMatrixSource src,
                           RtaMatrixTarget tgt, float weight_neg1_to_pos1) {
    if (!ctx || src < 0 || src >= 3 || tgt < 0 || tgt >= 3) return;
    ctx->matrix[src][tgt] = clampf(weight_neg1_to_pos1, -1.0f, 1.0f);
    sync_matrix_to_graph(ctx);
}

float rta_get_matrix_weight(const RtaContext *ctx, RtaMatrixSource src,
                            RtaMatrixTarget tgt) {
    if (!ctx || src < 0 || src >= 3 || tgt < 0 || tgt >= 3) return 0.0f;
    return ctx->matrix[src][tgt];
}

void rta_process_interleaved_stereo(RtaContext *ctx, float *io_lr, size_t frames) {
    if (!ctx || !io_lr || frames == 0) return;
    sync_matrix_to_graph(ctx);

    size_t offset = 0;
    while (offset < frames) {
        size_t chunk = frames - offset;
        if (chunk > RTA_ABI_MAX_BLOCK) chunk = RTA_ABI_MAX_BLOCK;
        rta_graph_process_block(&ctx->graph, &io_lr[offset * 2], chunk);
        offset += chunk;
    }
}

void rta_process_split_stereo(RtaContext *ctx, const float *in_l, const float *in_r,
                              float *out_l, float *out_r, size_t frames) {
    if (!ctx || !in_l || !in_r || !out_l || !out_r || frames == 0) return;
    sync_matrix_to_graph(ctx);

    float inter[RTA_ABI_MAX_BLOCK * 2];
    size_t offset = 0;
    while (offset < frames) {
        size_t chunk = frames - offset;
        if (chunk > RTA_ABI_MAX_BLOCK) chunk = RTA_ABI_MAX_BLOCK;

        for (size_t i = 0; i < chunk; ++i) {
            inter[2 * i]     = in_l[offset + i];
            inter[2 * i + 1] = in_r[offset + i];
        }

        rta_graph_process_block(&ctx->graph, inter, chunk);

        for (size_t i = 0; i < chunk; ++i) {
            out_l[offset + i] = inter[2 * i];
            out_r[offset + i] = inter[2 * i + 1];
        }
        offset += chunk;
    }
}

void rta_get_telemetry(const RtaContext *ctx, RtaTelemetry *out_telemetry) {
    if (!ctx || !out_telemetry) return;
    out_telemetry->sub_rms    = ctx->graph.sig.sub_rms;
    out_telemetry->body_rms   = ctx->graph.sig.body_rms;
    out_telemetry->air_rms    = ctx->graph.sig.air_rms;
    out_telemetry->kick_env   = ctx->graph.sig.kick_env;
    out_telemetry->sub_gain   = ctx->graph.sub_gain;
    out_telemetry->body_gain  = ctx->graph.body_gain;
    out_telemetry->air_gain   = ctx->graph.air_gain;
}