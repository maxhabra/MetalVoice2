#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MVAppleDF MVAppleDF;
// A single worker owns each instance. Null path finds the bundled model.
// Units: 0 automatic, 1 CPU, 2 CPU + GPU, 3 CPU + Neural Engine.
MVAppleDF *mv_apple_df_create(const char *path, int units, char *error, size_t capacity);
void mv_apple_df_destroy(MVAppleDF *state);
int mv_apple_df_reset(MVAppleDF *state);
int mv_apple_df_predict(void *state, const float *erb, const float *complex,
                        float *gains, float *coefficients, float *lsnr);
// Preferred devices from Apple's compute plan: CPU=1, GPU=2, ANE=4; 0 unknown.
uint32_t mv_apple_df_devices(const MVAppleDF *state);
#ifdef __cplusplus
}
#endif
