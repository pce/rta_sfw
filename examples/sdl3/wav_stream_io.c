/* examples/sdl3/wav_stream_io.c */
#include "wav_stream_io.h"
#include <math.h>
#include <string.h>

#define RTA_RAW_CHUNK_BYTES 6144 /* Divisible by 2, 3, and 4 bytes */

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_u16_be(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static uint32_t read_u32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* Decode 80-bit IEEE 754 extended float (used by AIFF COMM sampleRate) */
static double read_ieee80_be(const uint8_t *b) {
    int exp = ((b[0] & 0x7F) << 8) | b[1];
    uint64_t mant = 0;
    for (int i = 2; i < 10; ++i) {
        mant = (mant << 8) | b[i];
    }
    if (exp == 0 && mant == 0) return 0.0;
    double sign = (b[0] & 0x80) ? -1.0 : 1.0;
    return sign * ldexp((double)mant, exp - 16383 - 63);
}

static bool parse_wav_header(RtaWavReader *r) {
    uint8_t hdr[12];
    if (SDL_ReadIO(r->io, hdr, 12) != 12) return false;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return false;

    bool got_fmt = false;
    bool got_data = false;

    while (!got_data) {
        uint8_t ch_hdr[8];
        if (SDL_ReadIO(r->io, ch_hdr, 8) != 8) break;
        uint32_t chunk_sz = read_u32_le(ch_hdr + 4);

        if (memcmp(ch_hdr, "fmt ", 4) == 0) {
            uint8_t fmt_buf[18] = {0};
            size_t to_read = chunk_sz < sizeof(fmt_buf) ? chunk_sz : sizeof(fmt_buf);
            if (SDL_ReadIO(r->io, fmt_buf, to_read) != to_read) return false;
            r->src_audio_format = (int)read_u16_le(fmt_buf + 0);
            r->src_channels     = (int)read_u16_le(fmt_buf + 2);
            r->sample_rate      = (int)read_u32_le(fmt_buf + 4);
            r->src_bits         = (int)read_u16_le(fmt_buf + 14);
            if (chunk_sz > to_read) {
                SDL_SeekIO(r->io, (Sint64)(chunk_sz - to_read), SDL_IO_SEEK_CUR);
            }
            got_fmt = true;
        } else if (memcmp(ch_hdr, "data", 4) == 0) {
            r->data_bytes_remaining = (Uint64)chunk_sz;
            got_data = true;
        } else {
            SDL_SeekIO(r->io, (Sint64)chunk_sz, SDL_IO_SEEK_CUR);
        }

        if (!got_data && (chunk_sz & 1U)) {
            SDL_SeekIO(r->io, 1, SDL_IO_SEEK_CUR);
        }
    }

    if (!got_fmt || !got_data || r->src_channels <= 0 || r->sample_rate <= 0) return false;
    r->container = RTA_CONTAINER_WAV;
    return true;
}

static bool parse_aiff_header(RtaWavReader *r) {
    uint8_t hdr[12];
    if (SDL_ReadIO(r->io, hdr, 12) != 12) return false;
    if (memcmp(hdr, "FORM", 4) != 0 || memcmp(hdr + 8, "AIFF", 4) != 0) return false;

    bool got_comm = false;
    bool got_ssnd = false;

    while (!got_ssnd) {
        uint8_t ch_hdr[8];
        if (SDL_ReadIO(r->io, ch_hdr, 8) != 8) break;
        uint32_t chunk_sz = read_u32_be(ch_hdr + 4);

        if (memcmp(ch_hdr, "COMM", 4) == 0) {
            uint8_t comm[18];
            if (chunk_sz < 18 || SDL_ReadIO(r->io, comm, 18) != 18) return false;
            r->src_channels = (int)read_u16_be(comm + 0);
            r->src_bits     = (int)read_u16_be(comm + 6);
            r->sample_rate  = (int)llround(read_ieee80_be(comm + 8));
            r->src_audio_format = 1;
            if (chunk_sz > 18) {
                SDL_SeekIO(r->io, (Sint64)(chunk_sz - 18), SDL_IO_SEEK_CUR);
            }
            got_comm = true;
        } else if (memcmp(ch_hdr, "SSND", 4) == 0) {
            if (chunk_sz < 8) return false;
            uint8_t ssnd_hdr[8];
            if (SDL_ReadIO(r->io, ssnd_hdr, 8) != 8) return false;
            uint32_t offset = read_u32_be(ssnd_hdr + 0);
            if (offset > 0 && chunk_sz >= 8 + offset) {
                SDL_SeekIO(r->io, (Sint64)offset, SDL_IO_SEEK_CUR);
                r->data_bytes_remaining = (Uint64)(chunk_sz - 8U - offset);
            } else {
                r->data_bytes_remaining = (Uint64)(chunk_sz - 8U);
            }
            got_ssnd = true;
        } else {
            SDL_SeekIO(r->io, (Sint64)chunk_sz, SDL_IO_SEEK_CUR);
        }

        if (!got_ssnd && (chunk_sz & 1U)) {
            SDL_SeekIO(r->io, 1, SDL_IO_SEEK_CUR);
        }
    }

    if (!got_comm || !got_ssnd || r->src_channels <= 0 || r->sample_rate <= 0) return false;
    r->container = RTA_CONTAINER_AIFF;
    return true;
}

bool rta_wav_reader_open(RtaWavReader *r, const char *path, int target_sr, int target_channels) {
    if (!r || !path || target_sr <= 0 || target_channels <= 0) return false;
    *r = (RtaWavReader){0};

    r->io = SDL_IOFromFile(path, "rb");
    if (!r->io) return false;

    uint8_t magic[4] = {0};
    if (SDL_ReadIO(r->io, magic, 4) != 4 || SDL_SeekIO(r->io, 0, SDL_IO_SEEK_SET) < 0) {
        SDL_CloseIO(r->io);
        r->io = NULL;
        return false;
    }

    bool parsed = false;
    if (memcmp(magic, "RIFF", 4) == 0) {
        parsed = parse_wav_header(r);
    } else if (memcmp(magic, "FORM", 4) == 0) {
        parsed = parse_aiff_header(r);
    }

    if (!parsed) {
        SDL_CloseIO(r->io);
        r->io = NULL;
        return false;
    }

    SDL_AudioFormat src_fmt = SDL_AUDIO_F32LE;
    if (r->container == RTA_CONTAINER_WAV) {
        if (r->src_audio_format == 3 && r->src_bits == 32) {
            src_fmt = SDL_AUDIO_F32LE;
        } else if (r->src_audio_format == 1 && r->src_bits == 16) {
            src_fmt = SDL_AUDIO_S16LE;
        } else if (r->src_audio_format == 1 && r->src_bits == 32) {
            src_fmt = SDL_AUDIO_S32LE;
        } else if (r->src_audio_format == 1 && r->src_bits == 8) {
            src_fmt = SDL_AUDIO_U8;
        } else if (r->src_audio_format == 1 && r->src_bits == 24) {
            /* Unpacked manually to F32LE before feeding SDL_AudioStream */
            src_fmt = SDL_AUDIO_F32LE;
        } else {
            SDL_CloseIO(r->io);
            r->io = NULL;
            return false;
        }
    } else {
        /* AIFF PCM is unpacked manually to F32LE before feeding SDL_AudioStream */
        if (r->src_bits != 16 && r->src_bits != 24 && r->src_bits != 8) {
            SDL_CloseIO(r->io);
            r->io = NULL;
            return false;
        }
        src_fmt = SDL_AUDIO_F32LE;
    }

    SDL_AudioSpec src_spec = {
        .format = src_fmt,
        .channels = r->src_channels,
        .freq = r->sample_rate
    };
    SDL_AudioSpec dst_spec = {
        .format = SDL_AUDIO_F32LE,
        .channels = target_channels,
        .freq = target_sr
    };

    r->stream = SDL_CreateAudioStream(&src_spec, &dst_spec);
    if (!r->stream) {
        SDL_CloseIO(r->io);
        r->io = NULL;
        return false;
    }

    r->target_rate = target_sr;
    r->target_channels = target_channels;
    r->eof = false;
    return true;
}

size_t rta_wav_reader_read_f32(RtaWavReader *r, float *dst, size_t max_frames) {
    if (!r || !r->stream || !dst || max_frames == 0) return 0;

    const int frame_bytes = r->target_channels * (int)sizeof(float);
    const int desired_bytes = (int)max_frames * frame_bytes;
    uint8_t raw_buf[RTA_RAW_CHUNK_BYTES];
    float f32_buf[RTA_RAW_CHUNK_BYTES / 2];

    while (SDL_GetAudioStreamAvailable(r->stream) < desired_bytes && !r->eof) {
        if (r->data_bytes_remaining == 0) {
            SDL_FlushAudioStream(r->stream);
            r->eof = true;
            break;
        }

        size_t to_read = sizeof(raw_buf);
        if ((Uint64)to_read > r->data_bytes_remaining) {
            to_read = (size_t)r->data_bytes_remaining;
        }

        size_t got = SDL_ReadIO(r->io, raw_buf, to_read);
        if (got == 0) {
            SDL_FlushAudioStream(r->stream);
            r->eof = true;
            break;
        }
        r->data_bytes_remaining -= (Uint64)got;

        if (r->container == RTA_CONTAINER_WAV && r->src_audio_format == 1 && r->src_bits == 24) {
            size_t n_samp = got / 3;
            const float inv = 1.0f / 8388608.0f;
            for (size_t i = 0; i < n_samp; ++i) {
                int32_t v = (int32_t)((uint32_t)raw_buf[3 * i] |
                                      ((uint32_t)raw_buf[3 * i + 1] << 8) |
                                      ((uint32_t)raw_buf[3 * i + 2] << 16));
                if (v & 0x00800000) v |= (int32_t)0xFF000000U;
                f32_buf[i] = (float)v * inv;
            }
            SDL_PutAudioStreamData(r->stream, f32_buf, (int)(n_samp * sizeof(float)));
        } else if (r->container == RTA_CONTAINER_AIFF && r->src_bits == 24) {
            size_t n_samp = got / 3;
            const float inv = 1.0f / 8388608.0f;
            for (size_t i = 0; i < n_samp; ++i) {
                int32_t v = (int32_t)(((uint32_t)raw_buf[3 * i] << 16) |
                                      ((uint32_t)raw_buf[3 * i + 1] << 8) |
                                      (uint32_t)raw_buf[3 * i + 2]);
                if (v & 0x00800000) v |= (int32_t)0xFF000000U;
                f32_buf[i] = (float)v * inv;
            }
            SDL_PutAudioStreamData(r->stream, f32_buf, (int)(n_samp * sizeof(float)));
        } else if (r->container == RTA_CONTAINER_AIFF && r->src_bits == 16) {
            size_t n_samp = got / 2;
            const float inv = 1.0f / 32768.0f;
            for (size_t i = 0; i < n_samp; ++i) {
                int16_t v = (int16_t)read_u16_be(raw_buf + 2 * i);
                f32_buf[i] = (float)v * inv;
            }
            SDL_PutAudioStreamData(r->stream, f32_buf, (int)(n_samp * sizeof(float)));
        } else if (r->container == RTA_CONTAINER_AIFF && r->src_bits == 8) {
            const float inv = 1.0f / 128.0f;
            for (size_t i = 0; i < got; ++i) {
                int8_t v = (int8_t)raw_buf[i];
                f32_buf[i] = (float)v * inv;
            }
            SDL_PutAudioStreamData(r->stream, f32_buf, (int)(got * sizeof(float)));
        } else {
            SDL_PutAudioStreamData(r->stream, raw_buf, (int)got);
        }
    }

    int avail = SDL_GetAudioStreamAvailable(r->stream);
    if (avail <= 0) return 0;

    int pull_bytes = avail > desired_bytes ? desired_bytes : avail;
    pull_bytes = (pull_bytes / frame_bytes) * frame_bytes;
    if (pull_bytes <= 0) return 0;

    int read_bytes = SDL_GetAudioStreamData(r->stream, dst, pull_bytes);
    if (read_bytes <= 0) return 0;

    return (size_t)read_bytes / (size_t)frame_bytes;
}

void rta_wav_reader_close(RtaWavReader *r) {
    if (!r) return;
    if (r->stream) SDL_DestroyAudioStream(r->stream);
    if (r->io) SDL_CloseIO(r->io);
    *r = (RtaWavReader){0};
}

static inline float next_tpdf(uint32_t *state) {
    *state = (*state) * 1664525U + 1013904223U;
    float r1 = (float)((*state) >> 8) / 16777216.0f;
    *state = (*state) * 1664525U + 1013904223U;
    float r2 = (float)((*state) >> 8) / 16777216.0f;
    return (r1 - r2) / 32768.0f;
}

bool rta_wav_writer_open(RtaWavWriter *w, const char *path, int sample_rate, int channels) {
    if (!w || !path || sample_rate <= 0 || channels <= 0) return false;
    *w = (RtaWavWriter){0};

    w->io = SDL_IOFromFile(path, "wb");
    if (!w->io) return false;

    w->sample_rate = sample_rate;
    w->channels = channels;
    w->bits_per_sample = 16; /* CD-quality default */
    w->rng_state = 0x54454348U;

    uint8_t fmt[16];
    uint16_t audio_format = 1; /* Uncompressed PCM */
    uint16_t ch = (uint16_t)channels;
    uint32_t sr = (uint32_t)sample_rate;
    uint16_t bits = 16;
    uint32_t byte_rate = sr * (uint32_t)ch * (uint32_t)(bits / 8U);
    uint16_t block_align = (uint16_t)(ch * (bits / 8U));

    memcpy(fmt + 0,  &audio_format, 2);
    memcpy(fmt + 2,  &ch, 2);
    memcpy(fmt + 4,  &sr, 4);
    memcpy(fmt + 8,  &byte_rate, 4);
    memcpy(fmt + 12, &block_align, 2);
    memcpy(fmt + 14, &bits, 2);

    uint32_t zero32 = 0;
    uint32_t fmt_sz = 16;

    SDL_WriteIO(w->io, "RIFF", 4);
    SDL_WriteIO(w->io, &zero32, 4);
    SDL_WriteIO(w->io, "WAVEfmt ", 8);
    SDL_WriteIO(w->io, &fmt_sz, 4);
    SDL_WriteIO(w->io, fmt, 16);
    SDL_WriteIO(w->io, "data", 4);
    SDL_WriteIO(w->io, &zero32, 4);

    return true;
}

bool rta_wav_writer_write_f32(RtaWavWriter *w, const float *src, size_t frames) {
    if (!w || !w->io || !src) return false;
    if (frames == 0) return true;

    int16_t pcm16[2048];
    size_t max_pcm_samples = sizeof(pcm16) / sizeof(pcm16[0]);
    size_t total_samples = frames * (size_t)w->channels;
    size_t offset = 0;

    while (offset < total_samples) {
        size_t chunk = total_samples - offset;
        if (chunk > max_pcm_samples) chunk = max_pcm_samples;

        for (size_t i = 0; i < chunk; ++i) {
            float s = src[offset + i];
            if (!(s >= -1.0f && s <= 1.0f)) {
                s = (s > 1.0f) ? 1.0f : ((s < -1.0f) ? -1.0f : 0.0f);
            }
            float dithered = s + next_tpdf(&w->rng_state);
            if (dithered > 0.9999695f) dithered = 0.9999695f;
            if (dithered < -1.0f) dithered = -1.0f;
            pcm16[i] = (int16_t)lrintf(dithered * 32767.0f);
        }

        size_t bytes = chunk * sizeof(int16_t);
        if (SDL_WriteIO(w->io, pcm16, bytes) != bytes) {
            return false;
        }
        offset += chunk;
    }

    w->frames_written += (uint64_t)frames;
    return true;
}

bool rta_wav_writer_close(RtaWavWriter *w) {
    if (!w) return false;
    bool ok = true;

    if (w->io) {
        uint32_t data_bytes = (uint32_t)(w->frames_written * (uint64_t)w->channels * 2ULL);
        uint32_t riff_bytes = 36U + data_bytes;

        if (SDL_SeekIO(w->io, 4, SDL_IO_SEEK_SET) < 0 ||
            SDL_WriteIO(w->io, &riff_bytes, 4) != 4) {
            ok = false;
        }
        if (SDL_SeekIO(w->io, 40, SDL_IO_SEEK_SET) < 0 ||
            SDL_WriteIO(w->io, &data_bytes, 4) != 4) {
            ok = false;
        }
        if (!SDL_CloseIO(w->io)) {
            ok = false;
        }
        w->io = NULL;
    }

    w->frames_written = 0;
    return ok;
}