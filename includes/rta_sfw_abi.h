#ifndef RTA_SFW_ABI_H
#define RTA_SFW_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
  #if defined(RTA_SFW_EXPORTS)
    #define RTA_API __declspec(dllexport)
  #elif defined(RTA_SFW_SHARED)
    #define RTA_API __declspec(dllimport)
  #else
    #define RTA_API
  #endif
#elif defined(__GNUC__) || defined(__clang__)
  #define RTA_API __attribute__((visibility("default")))
#else
  #define RTA_API
#endif

typedef struct RtaContext RtaContext;

typedef enum {
    RTA_PRESET_STUDIO_54 = 0,
    RTA_PRESET_TECHNO_PUMPER = 1
} RtaPresetId;

typedef enum {
    RTA_SRC_SUB = 0,
    RTA_SRC_BODY = 1,
    RTA_SRC_KICK = 2
} RtaMatrixSource;

typedef enum {
    RTA_TGT_SUB_GAIN = 0,
    RTA_TGT_BODY_GAIN = 1,
    RTA_TGT_AIR_WIDTH = 2
} RtaMatrixTarget;

typedef struct {
    float sub_rms;
    float body_rms;
    float air_rms;
    float kick_env;
    float sub_gain;
    float body_gain;
    float air_gain;
} RtaTelemetry;

/* Lifecycle (allocates once outside audio thread) */
RTA_API RtaContext *rta_create(float sample_rate);
RTA_API void rta_destroy(RtaContext *ctx);
RTA_API void rta_reset(RtaContext *ctx, float sample_rate);

/* High-level controls */
RTA_API void rta_set_preset(RtaContext *ctx, RtaPresetId preset);
RTA_API void rta_set_flight(RtaContext *ctx, float flight_0_to_1);
RTA_API void rta_set_air_width(RtaContext *ctx, float width_0_to_2);
RTA_API void rta_set_matrix_weight(RtaContext *ctx, RtaMatrixSource src,
                                   RtaMatrixTarget tgt, float weight_neg1_to_pos1);
RTA_API float rta_get_matrix_weight(const RtaContext *ctx, RtaMatrixSource src,
                                    RtaMatrixTarget tgt);

/* Real-time audio thread (zero allocations, deterministic) */
RTA_API void rta_process_interleaved_stereo(RtaContext *ctx, float *io_lr, size_t frames);
RTA_API void rta_process_split_stereo(RtaContext *ctx, const float *in_l, const float *in_r,
                                      float *out_l, float *out_r, size_t frames);

/* UI / meter thread snapshot */
RTA_API void rta_get_telemetry(const RtaContext *ctx, RtaTelemetry *out_telemetry);

#ifdef __cplusplus
}
#endif

#endif /* RTA_SFW_ABI_H */