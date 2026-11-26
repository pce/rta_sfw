/* examples/sdl3/wav_stream_io.h */
#ifndef RTA_WAV_STREAM_IO_H
#define RTA_WAV_STREAM_IO_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum RtaContainerKind {
    RTA_CONTAINER_UNKNOWN = 0,
    RTA_CONTAINER_WAV,
    RTA_CONTAINER_AIFF
} RtaContainerKind;

typedef struct RtaWavReader {
    SDL_IOStream *io;
    SDL_AudioStream *stream;
    RtaContainerKind container;
    int sample_rate;
    int src_channels;
    int src_bits;
    int src_audio_format; /* WAV: 1=PCM, 3=IEEE float; AIFF: 1=PCM */
    int target_rate;
    int target_channels;
    Uint64 data_bytes_remaining;
    bool eof;
} RtaWavReader;

typedef struct RtaWavWriter {
    SDL_IOStream *io;
    int sample_rate;
    int channels;
    int bits_per_sample; /* Default 16 for CD-quality stereo WAV */
    uint64_t frames_written;
    uint32_t rng_state;
} RtaWavWriter;

bool rta_wav_reader_open(RtaWavReader *r, const char *path, int target_sr, int target_channels);
size_t rta_wav_reader_read_f32(RtaWavReader *r, float *dst, size_t max_frames);
void rta_wav_reader_close(RtaWavReader *r);

bool rta_wav_writer_open(RtaWavWriter *w, const char *path, int sample_rate, int channels);
bool rta_wav_writer_write_f32(RtaWavWriter *w, const float *src, size_t frames);
bool rta_wav_writer_close(RtaWavWriter *w);

#endif /* RTA_WAV_STREAM_IO_H */