#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MVEchoCanceller MVEchoCanceller;
// All calls belong to the processing worker. 48 kHz, 480 frames, mono capture,
// stereo render. Reference and microphone blocks use the same host-time span.
MVEchoCanceller *mv_aec_create(void);
void mv_aec_destroy(MVEchoCanceller *aec);
int mv_aec_reset(MVEchoCanceller *aec);
int mv_aec_process(MVEchoCanceller *aec, const float *left, const float *right, float *microphone);
uint32_t mv_aec_delay_frames(void);
void mv_aec_metrics(MVEchoCanceller *aec, double *erle_db, double *delay_ms);
#ifdef __cplusplus
}
#endif
