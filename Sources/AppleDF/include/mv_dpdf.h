#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MVDPDF MVDPDF;
// All lifecycle/process calls belong to the serial audio worker, never AUHAL.
// Null paths find bundled resources. Units: qualified CPU default=0, CPU=1,
// experimental CPU/GPU=2, CPU/ANE=3, automatic=4.
MVDPDF *mv_dpdf_create(const char *model_path, const char *state_path, int units,
                       char *error, size_t capacity);
void mv_dpdf_destroy(MVDPDF *state);
int mv_dpdf_reset(MVDPDF *state);
// Exactly 480 normalized samples; input and output may alias.
int mv_dpdf_process(MVDPDF *state, const float *input, float *output);
uint32_t mv_dpdf_devices(const MVDPDF *state);
// Spectral interface for independent official-ONNX parity tests.
int mv_dpdf_predict(MVDPDF *state, const float *spectrum, float *output);
#ifdef __cplusplus
}
#endif
