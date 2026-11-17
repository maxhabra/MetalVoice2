#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MVLocalVQE MVLocalVQE;
// Worker-thread API; never call from an audio callback. Null path finds the
// bundled joint v1.3 model. threads=0 selects the qualified two-thread CPU mode.
MVLocalVQE *mv_localvqe_create(const char *model_path, int threads, char *error, size_t capacity);
void mv_localvqe_destroy(MVLocalVQE *state);
int mv_localvqe_reset(MVLocalVQE *state);
// 480 normalized mono samples at 48 kHz; input/output may alias.
// Noise + room cleanup only: the model's far-end input is always silent.
int mv_localvqe_process(MVLocalVQE *state, const float *input, float *output);
uint32_t mv_localvqe_delay_frames(void);
uint32_t mv_localvqe_threads(const MVLocalVQE *state);
#ifdef __cplusplus
}
#endif
