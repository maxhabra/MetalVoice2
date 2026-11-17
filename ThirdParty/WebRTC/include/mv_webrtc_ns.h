#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MVNoiseSuppressor MVNoiseSuppressor;
// Mono, 48 kHz, exactly 480 normalized Float32 samples per call.
// Levels 0..3 are upstream k6dB/k12dB/k18dB/k21dB. Construction allocates;
// process does not allocate. Use a single owner/processing thread.
MVNoiseSuppressor *mv_ns_create(int level);
void mv_ns_destroy(MVNoiseSuppressor *ns);
void mv_ns_process(MVNoiseSuppressor *ns, float *samples);
uint32_t mv_ns_delay_frames(void);
#ifdef __cplusplus
}
#endif
