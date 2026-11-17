#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MVDeepFilter MVDeepFilter;
// Thread-affine: create, process, reset, inspect and destroy on one worker.
// The upstream runtime must not be transferred between threads.
MVDeepFilter *mv_df_create(char *error, size_t capacity);
void mv_df_destroy(MVDeepFilter *state);
int mv_df_reset(MVDeepFilter *state);
uint32_t mv_df_delay_frames(const MVDeepFilter *state);
// 480 mono normalized samples at 48 kHz, separate input/output buffers.
// Runs on the processing worker, not a hardware callback. Returns 0 or -1.
int mv_df_process(MVDeepFilter *state, const float *input, float *output);
float mv_df_last_snr(const MVDeepFilter *state);
// Upstream DSP with an accelerated predictor. One mono 10 ms neural step:
// ERB [1,1,1,32], complex features [1,2,1,96], gains[32], coefs[96,5,2], SNR[1].
typedef int (*MVDFPredict)(void *, const float *, const float *, float *, float *, float *);
typedef struct MVDFStream MVDFStream;
MVDFStream *mv_df_stream_create(MVDFPredict predict, void *context);
void mv_df_stream_destroy(MVDFStream *state);
int mv_df_stream_reset(MVDFStream *state);
int mv_df_stream_process(MVDFStream *state, const float *input, float *output);
#ifdef __cplusplus
}
#endif
