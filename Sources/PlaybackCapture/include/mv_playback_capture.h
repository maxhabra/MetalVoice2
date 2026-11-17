#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MVPlaybackCapture MVPlaybackCapture;
typedef struct {
    uint32_t device_id, buffer_frames;
    double sample_rate, peak, callback_age_ms;
    uint64_t frames, dropped;
    int32_t error;
    char device_name[256];
} MVPlaybackInfo;
// Control thread. Captures only the default physical playback device, not the
// virtual microphone. Creation may show macOS's system-audio permission dialog.
MVPlaybackCapture *mv_playback_create(char *error, size_t capacity);
int mv_playback_start(MVPlaybackCapture *capture, uint32_t wake_semaphore, char *error, size_t capacity);
void mv_playback_stop(MVPlaybackCapture *capture);
void mv_playback_destroy(MVPlaybackCapture *capture);
// Worker only: 1 ready, 0 wait for more reference, -1 timestamp gap/too old.
int mv_playback_read(MVPlaybackCapture *capture, double microphone_host_ns, float *left, float *right);
void mv_playback_discard(MVPlaybackCapture *capture);
void mv_playback_info(MVPlaybackCapture *capture, MVPlaybackInfo *info);
#ifdef __cplusplus
}
#endif
