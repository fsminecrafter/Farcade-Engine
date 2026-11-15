// Internal: the decoders of the audio system behind one small C interface
// (decode.c wraps dr_wav, dr_mp3 and stb_vorbis). Decoding is from memory;
// the data must outlive the decoder. Output is float, interleaved, 1 or 2
// channels (files with more channels keep their first two).
#ifndef FE_AUDIO_DECODE_H
#define FE_AUDIO_DECODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FeDecoder FeDecoder;

// Picks the decoder from the first bytes (RIFF = WAV, OggS = Ogg Vorbis,
// anything else = MP3). NULL when the data is not decodable. `frames` is the
// length in frames, 0 when unknown (MP3).
FeDecoder* fe_decoder_open(const void* data, size_t size, int* rate, int* channels, uint64_t* frames);
// Up to `frames` frames; returns how many (0 at the end).
uint64_t fe_decoder_read(FeDecoder* d, float* out, uint64_t frames);
// Back to the start. 1 on success.
int fe_decoder_rewind(FeDecoder* d);
void fe_decoder_close(FeDecoder* d);

#ifdef __cplusplus
}
#endif
#endif
