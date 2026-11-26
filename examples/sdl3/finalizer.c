#include "rta_sfw_abi.h"
#include "wav_stream_io.h"
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STREAM_BLOCK_FRAMES 1024

typedef struct {
    int preset;
    bool has_flight;
    float flight;
    bool has_air_width;
    float air_width;
    bool has_sub_body;
    float sub_body;
    bool has_kick_sub;
    float kick_sub;
    bool has_kick_body;
    float kick_body;
    bool has_body_air;
    float body_air;
    bool has_kick_air;
    float kick_air;
} DspVariantConfig;

typedef struct {
    const char *in_path;
    const char *out_path;
    const char *out_a_path;
    const char *out_b_path;
    bool ab_compare;
    int sample_rate;

    /* Time window & slice segmentation */
    bool has_window;
    double window_start_sec;
    double window_end_sec;
    bool crop_window;

    bool has_skip_window;
    double skip_start_sec;
    double skip_end_sec;

    double slice_sec;
    int skip_every;     /* 0 = disabled; N > 0 skips every N-th slice (1-indexed) */
    bool drop_skipped;  /* true = omit skipped/outside frames; false = pass dry */

    DspVariantConfig cfg_a;
    DspVariantConfig cfg_b;
} FinalizerCli;

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int parse_preset(const char *s) {
    if (!s) return 0;
    if (strcmp(s, "1") == 0 || strcmp(s, "techno") == 0 || strcmp(s, "techno_pumper") == 0) {
        return 1;
    }
    return 0;
}

static bool parse_range_sec(const char *s, double *out_start, double *out_end) {
    if (!s || !out_start || !out_end) return false;
    char *colon = strchr(s, ':');
    if (!colon) return false;
    *out_start = strtod(s, NULL);
    *out_end = strtod(colon + 1, NULL);
    return (*out_end > *out_start && *out_start >= 0.0);
}

static void make_ab_paths(const char *base_out, char *out_a, size_t cap_a,
                          char *out_b, size_t cap_b) {
    const char *dot = strrchr(base_out, '.');
    const char *slash = strrchr(base_out, '/');
    if (dot && (!slash || dot > slash)) {
        size_t stem_len = (size_t)(dot - base_out);
        snprintf(out_a, cap_a, "%.*s_a%s", (int)stem_len, base_out, dot);
        snprintf(out_b, cap_b, "%.*s_b%s", (int)stem_len, base_out, dot);
    } else {
        snprintf(out_a, cap_a, "%s_a.wav", base_out);
        snprintf(out_b, cap_b, "%s_b.wav", base_out);
    }
}

static void print_help(const char *prog) {
    fprintf(stderr,
        "Usage: %s --in <input.wav> [options]\n"
        "I/O & A/B Compare:\n"
        "  -i, --in <path>            Input WAV file (any sample rate/channels -> converted to F32 stereo)\n"
        "  -o, --out <path>           Output WAV path (default: finalized.wav)\n"
        "  --ab                       Render A & B comparison files (<stem>_a.wav, <stem>_b.wav)\n"
        "  --out-a <path>             Explicit path for variant A\n"
        "  --out-b <path>             Explicit path for variant B\n"
        "  --rate <hz>                Processing/output sample rate (default: 48000)\n"
        "\n"
        "Segmentation & Time Windows:\n"
        "  --window <start:end>       Active time window in seconds (e.g. 10.0:45.0)\n"
        "  --crop-window              Write only frames inside --window (instead of passing outside dry)\n"
        "  --skip-window <start:end>  Bypass DSP in time window [start, end) seconds\n"
        "  --slice-sec <sec>          Segment timeline into slices of <sec> seconds\n"
        "  --skip-every <N>           Skip every N-th slice (e.g. --slice-sec 4 --skip-every 2)\n"
        "  --drop-skipped             Omit skipped/outside-window frames instead of passing dry\n"
        "\n"
        "DSP Parameters (A = default/primary, B = A/B comparison target):\n"
        "  --preset <0|1|s54|tech>    Preset for A (default: 0 / studio54)\n"
        "  --flight <0.0..1.0>        Flight perspective tilt for A\n"
        "  --air-width <0.0..2.0>     Air band stereo width for A\n"
        "  --sub-body <-1.0..1.0>     Sub -> Body modulation weight for A\n"
        "  --kick-sub <-1.0..1.0>     Kick -> Sub modulation weight for A\n"
        "  --kick-body <-1.0..1.0>    Kick -> Body modulation weight for A\n"
        "  --body-air <-1.0..1.0>     Body -> Air width weight for A\n"
        "  --kick-air <-1.0..1.0>     Kick -> Air width weight for A\n"
        "  --preset-b <0|1|s54|tech>  Preset for B (default: 1 / techno)\n"
        "  --flight-b <0.0..1.0>      Flight perspective tilt for B\n"
        "  --air-width-b <0.0..2.0>   Air band stereo width for B\n"
        "  --sub-body-b <-1.0..1.0>   Sub -> Body modulation weight for B\n"
        "  --kick-sub-b <-1.0..1.0>   Kick -> Sub modulation weight for B\n"
        "  --kick-body-b <-1.0..1.0>  Kick -> Body modulation weight for B\n",
        prog);
}

static bool parse_cli(int argc, char **argv, FinalizerCli *cli) {
    memset(cli, 0, sizeof(*cli));
    cli->out_path = "finalized.wav";
    cli->sample_rate = 48000;
    cli->cfg_a.preset = 0;
    cli->cfg_b.preset = 1;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_help(argv[0]);
            return false;
        } else if ((strcmp(a, "-i") == 0 || strcmp(a, "--in") == 0) && i + 1 < argc) {
            cli->in_path = argv[++i];
        } else if ((strcmp(a, "-o") == 0 || strcmp(a, "--out") == 0) && i + 1 < argc) {
            cli->out_path = argv[++i];
        } else if (strcmp(a, "--out-a") == 0 && i + 1 < argc) {
            cli->out_a_path = argv[++i];
            cli->ab_compare = true;
        } else if (strcmp(a, "--out-b") == 0 && i + 1 < argc) {
            cli->out_b_path = argv[++i];
            cli->ab_compare = true;
        } else if (strcmp(a, "--ab") == 0) {
            cli->ab_compare = true;
        } else if (strcmp(a, "--rate") == 0 && i + 1 < argc) {
            int r = atoi(argv[++i]);
            if (r >= 8000 && r <= 192000) cli->sample_rate = r;
        } else if (strcmp(a, "--window") == 0 && i + 1 < argc) {
            if (!parse_range_sec(argv[++i], &cli->window_start_sec, &cli->window_end_sec)) {
                fprintf(stderr, "Invalid --window format; use start:end in seconds.\n");
                return false;
            }
            cli->has_window = true;
        } else if (strcmp(a, "--crop-window") == 0) {
            cli->crop_window = true;
        } else if (strcmp(a, "--skip-window") == 0 && i + 1 < argc) {
            if (!parse_range_sec(argv[++i], &cli->skip_start_sec, &cli->skip_end_sec)) {
                fprintf(stderr, "Invalid --skip-window format; use start:end in seconds.\n");
                return false;
            }
            cli->has_skip_window = true;
        } else if (strcmp(a, "--slice-sec") == 0 && i + 1 < argc) {
            cli->slice_sec = strtod(argv[++i], NULL);
            if (cli->slice_sec <= 0.0) {
                fprintf(stderr, "--slice-sec must be > 0.\n");
                return false;
            }
        } else if (strcmp(a, "--skip-every") == 0 && i + 1 < argc) {
            cli->skip_every = atoi(argv[++i]);
            if (cli->skip_every < 0) cli->skip_every = 0;
        } else if (strcmp(a, "--drop-skipped") == 0) {
            cli->drop_skipped = true;
        } else if (strcmp(a, "--preset") == 0 && i + 1 < argc) {
            cli->cfg_a.preset = parse_preset(argv[++i]);
        } else if (strcmp(a, "--flight") == 0 && i + 1 < argc) {
            cli->cfg_a.has_flight = true;
            cli->cfg_a.flight = clampf(strtof(argv[++i], NULL), 0.0f, 1.0f);
        } else if (strcmp(a, "--air-width") == 0 && i + 1 < argc) {
            cli->cfg_a.has_air_width = true;
            cli->cfg_a.air_width = clampf(strtof(argv[++i], NULL), 0.0f, 2.0f);
        } else if (strcmp(a, "--sub-body") == 0 && i + 1 < argc) {
            cli->cfg_a.has_sub_body = true;
            cli->cfg_a.sub_body = clampf(strtof(argv[++i], NULL), -1.0f, 1.0f);
        } else if (strcmp(a, "--kick-sub") == 0 && i + 1 < argc) {
            cli->cfg_a.has_kick_sub = true;
            cli->cfg_a.kick_sub = clampf(strtof(argv[++i], NULL), -1.0f, 1.0f);
        } else if (strcmp(a, "--kick-body") == 0 && i + 1 < argc) {
            cli->cfg_a.has_kick_body = true;
            cli->cfg_a.kick_body = clampf(strtof(argv[++i], NULL), -1.0f, 1.0f);
        } else if (strcmp(a, "--body-air") == 0 && i + 1 < argc) {
            cli->cfg_a.has_body_air = true;
            cli->cfg_a.body_air = clampf(strtof(argv[++i], NULL), -1.0f, 1.0f);
        } else if (strcmp(a, "--kick-air") == 0 && i + 1 < argc) {
            cli->cfg_a.has_kick_air = true;
            cli->cfg_a.kick_air = clampf(strtof(argv[++i], NULL), -1.0f, 1.0f);
        } else if (strcmp(a, "--preset-b") == 0 && i + 1 < argc) {
            cli->cfg_b.preset = parse_preset(argv[++i]);
            cli->ab_compare = true;
        } else if (strcmp(a, "--flight-b") == 0 && i + 1 < argc) {
            cli->cfg_b.has_flight = true;
            cli->cfg_b.flight = clampf(strtof(argv[++i], NULL), 0.0f, 1.0f);
            cli->ab_compare = true;
        } else if (strcmp(a, "--air-width-b") == 0 && i + 1 < argc) {
            cli->cfg_b.has_air_width = true;
            cli->cfg_b.air_width = clampf(strtof(argv[++i], NULL), 0.0f, 2.0f);
            cli->ab_compare = true;
        } else if (strcmp(a, "--sub-body-b") == 0 && i + 1 < argc) {
            cli->cfg_b.has_sub_body = true;
            cli->cfg_b.sub_body = clampf(strtof(argv[++i], NULL), -1.0f, 1.0f);
            cli->ab_compare = true;
        } else if (strcmp(a, "--kick-sub-b") == 0 && i + 1 < argc) {
            cli->cfg_b.has_kick_sub = true;
            cli->cfg_b.kick_sub = clampf(strtof(argv[++i], NULL), -1.0f, 1.0f);
            cli->ab_compare = true;
        } else if (strcmp(a, "--kick-body-b") == 0 && i + 1 < argc) {
            cli->cfg_b.has_kick_body = true;
            cli->cfg_b.kick_body = clampf(strtof(argv[++i], NULL), -1.0f, 1.0f);
            cli->ab_compare = true;
        } else {
            fprintf(stderr, "Unknown or incomplete argument: %s\n", a);
            print_help(argv[0]);
            return false;
        }
    }

    if (!cli->in_path || cli->in_path[0] == '\0') {
        fprintf(stderr, "Error: --in <input.wav> is required.\n");
        print_help(argv[0]);
        return false;
    }

    return true;
}

static void configure_context(RtaContext *ctx, const DspVariantConfig *cfg) {
    rta_set_preset(ctx, cfg->preset == 1 ? RTA_PRESET_TECHNO_PUMPER : RTA_PRESET_STUDIO_54);
    if (cfg->has_flight)    rta_set_flight(ctx, cfg->flight);
    if (cfg->has_air_width) rta_set_air_width(ctx, cfg->air_width);
    if (cfg->has_sub_body)  rta_set_matrix_weight(ctx, RTA_SRC_SUB,  RTA_TGT_BODY_GAIN, cfg->sub_body);
    if (cfg->has_kick_sub)  rta_set_matrix_weight(ctx, RTA_SRC_KICK, RTA_TGT_SUB_GAIN,  cfg->kick_sub);
    if (cfg->has_kick_body) rta_set_matrix_weight(ctx, RTA_SRC_KICK, RTA_TGT_BODY_GAIN, cfg->kick_body);
    if (cfg->has_body_air)  rta_set_matrix_weight(ctx, RTA_SRC_BODY, RTA_TGT_AIR_WIDTH, cfg->body_air);
    if (cfg->has_kick_air)  rta_set_matrix_weight(ctx, RTA_SRC_KICK, RTA_TGT_AIR_WIDTH, cfg->kick_air);
}

static bool is_frame_active(const FinalizerCli *cli, uint64_t frame_idx, bool *out_drop) {
    *out_drop = false;
    double t_sec = (double)frame_idx / (double)cli->sample_rate;

    if (cli->has_window) {
        if (t_sec < cli->window_start_sec || t_sec >= cli->window_end_sec) {
            if (cli->crop_window || cli->drop_skipped) {
                *out_drop = true;
            }
            return false;
        }
    }

    if (cli->has_skip_window) {
        if (t_sec >= cli->skip_start_sec && t_sec < cli->skip_end_sec) {
            if (cli->drop_skipped) *out_drop = true;
            return false;
        }
    }

    if (cli->slice_sec > 0.0 && cli->skip_every > 0) {
        uint64_t slice_index_1based = (uint64_t)(t_sec / cli->slice_sec) + 1ULL;
        if ((slice_index_1based % (uint64_t)cli->skip_every) == 0ULL) {
            if (cli->drop_skipped) *out_drop = true;
            return false;
        }
    }

    return true;
}

static bool render_pass(const FinalizerCli *cli, const DspVariantConfig *cfg,
                        const char *out_path) {
    RtaWavReader reader;
    if (!rta_wav_reader_open(&reader, cli->in_path, cli->sample_rate, 2)) {
        fprintf(stderr, "Failed to open input WAV '%s'.\n", cli->in_path);
        return false;
    }

    RtaWavWriter writer;
    if (!rta_wav_writer_open(&writer, out_path, cli->sample_rate, 2)) {
        fprintf(stderr, "Failed to open output WAV '%s'.\n", out_path);
        rta_wav_reader_close(&reader);
        return false;
    }

    RtaContext *ctx = rta_create((float)cli->sample_rate);
    if (!ctx) {
        fprintf(stderr, "Failed to create RtaContext.\n");
        rta_wav_writer_close(&writer);
        rta_wav_reader_close(&reader);
        return false;
    }
    configure_context(ctx, cfg);

    float in_buf[STREAM_BLOCK_FRAMES * 2];
    float keep_buf[STREAM_BLOCK_FRAMES * 2];
    uint64_t global_frame = 0;
    bool ok = true;

    for (;;) {
        size_t n = rta_wav_reader_read_f32(&reader, in_buf, STREAM_BLOCK_FRAMES);
        if (n == 0) break;

        size_t keep_count = 0;
        size_t run_start = 0;
        bool run_active = false;
        bool have_run = false;

        for (size_t i = 0; i < n; ++i) {
            bool drop = false;
            bool active = is_frame_active(cli, global_frame + (uint64_t)i, &drop);

            if (drop) {
                if (have_run && run_active) {
                    rta_process_interleaved_stereo(ctx, &in_buf[run_start * 2], i - run_start);
                }
                have_run = false;
                continue;
            }

            if (!have_run || active != run_active) {
                if (have_run && run_active) {
                    rta_process_interleaved_stereo(ctx, &in_buf[run_start * 2], i - run_start);
                }
                run_start = i;
                run_active = active;
                have_run = true;
            }

            keep_buf[keep_count * 2]     = in_buf[i * 2];
            keep_buf[keep_count * 2 + 1] = in_buf[i * 2 + 1];
            keep_count++;
        }

        if (have_run && run_active) {
            rta_process_interleaved_stereo(ctx, &in_buf[run_start * 2], n - run_start);
            /* Re-copy processed samples into keep_buf for the active tail */
            size_t tail_len = n - run_start;
            size_t dst_start = keep_count - tail_len;
            memcpy(&keep_buf[dst_start * 2], &in_buf[run_start * 2], tail_len * 2 * sizeof(float));
        }

        if (keep_count > 0) {
            if (!rta_wav_writer_write_f32(&writer, keep_buf, keep_count)) {
                fprintf(stderr, "Failed writing output samples to '%s'.\n", out_path);
                ok = false;
                break;
            }
        }

        global_frame += (uint64_t)n;
    }

    RtaTelemetry tel = {0};
    rta_get_telemetry(ctx, &tel);

    if (!rta_wav_writer_close(&writer)) {
        ok = false;
    }

    printf("Rendered -> %s (preset=%d, sub=%.3f, body=%.3f, air=%.3f, kick=%.3f)\n",
           out_path, cfg->preset, tel.sub_rms, tel.body_rms, tel.air_rms, tel.kick_env);

    rta_destroy(ctx);
    rta_wav_reader_close(&reader);
    return ok;
}

int main(int argc, char **argv) {
    FinalizerCli cli;
    if (!parse_cli(argc, argv, &cli)) {
        return 1;
    }

    if (!SDL_Init(SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    bool ok = true;
    if (cli.ab_compare) {
        char auto_a[512];
        char auto_b[512];
        make_ab_paths(cli.out_path, auto_a, sizeof(auto_a), auto_b, sizeof(auto_b));
        const char *path_a = cli.out_a_path ? cli.out_a_path : auto_a;
        const char *path_b = cli.out_b_path ? cli.out_b_path : auto_b;

        ok = render_pass(&cli, &cli.cfg_a, path_a) &&
             render_pass(&cli, &cli.cfg_b, path_b);
    } else {
        ok = render_pass(&cli, &cli.cfg_a, cli.out_path);
    }

    SDL_Quit();
    return ok ? 0 : 1;
}