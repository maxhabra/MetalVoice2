#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct MVEngine MVEngine;
typedef struct {
    uint32_t id, input_channels, output_channels, buffer_frames;
    double sample_rate;
    int is_virtual, is_default_input, alive;
    char name[256], uid[256];
} MVDevice;

typedef struct {
    int running, latency_valid, device_changed;
    int32_t last_error;
    double input_rate, output_rate, queue_ms, target_queue_ms;
    double latency_ms, latency_p50_ms, latency_p95_ms, latency_max_ms;
    double drift_ppm, input_peak, output_peak;
    double input_device_latency_ms, output_device_latency_ms;
    uint32_t input_buffer_frames, output_buffer_frames;
    uint64_t input_frames, output_frames, underruns, overruns, resyncs, latency_samples;
    int noise_suppression_level; // -1 off; 0..3 Low / Moderate / High / Very high
    int enhanced_suppression; // 0 off; 1 RNNoise; 2 DeepFilterNet3; 3 DPDFNet-2; 4 LocalVQE v1.3
    double suppression_delay_ms, suppression_mean_ms, suppression_max_ms, suppression_p99_ms;
    double webrtc_delay_ms, rnnoise_delay_ms, deepfilter_delay_ms;
    uint64_t suppression_blocks;
    int deepfilter_backend; // 0 inactive, 1 native, 2 Core ML, 3 native fallback
    uint32_t deepfilter_compute_devices; // Core ML preferred CPU=1, GPU=2, ANE=4
    double dpdf_delay_ms;
    uint32_t dpdf_compute_devices;
    int room_dereverb; // 0 off, 1 conservative single-channel online WPE
    double room_dereverb_delay_ms;
    int echo_cancellation, echo_reference_state; // 0 off, 1 waiting, 2 synchronized, 3 failed
    double echo_delay_ms, echo_reference_headroom_ms, echo_reference_peak;
    double echo_erle_db, echo_estimated_path_delay_ms, echo_reference_age_ms;
    uint64_t echo_reference_frames, echo_reference_drops;
    char echo_reference_device[256];
    double localvqe_delay_ms;
    uint32_t localvqe_threads; // GGML CPU worker count; zero while inactive
} MVStats;

enum { MVErrorEchoReferenceUnavailable = -100001, MVErrorEchoProcessing = -100002 };

// Control-thread API. Enumeration and lifecycle functions are never callback-safe.
int mv_devices(MVDevice *devices, int capacity);
MVEngine *mv_create(void);
void mv_destroy(MVEngine *engine);
int mv_start(MVEngine *engine, uint32_t input, uint32_t output, uint32_t channel,
             char *error, size_t error_capacity);
void mv_stop(MVEngine *engine);
void mv_set_muted(MVEngine *engine, int muted);
// Configure only while stopped. Returns -1 if running or level is outside -1..3.
int mv_set_noise_suppression(MVEngine *engine, int level);
// Configure only while stopped. mode: 0 off, 1 RNNoise, 2 DeepFilterNet3,
// 3 DPDFNet-2, 4 LocalVQE. Mode 4 suspends Echo and WPE regardless of setter
// order; their saved settings resume on another mode. Stats report active stages.
int mv_set_enhanced_suppression(MVEngine *engine, int mode);
// Configure only while stopped. Uses the physical macOS default output as reference.
int mv_set_echo_cancellation(MVEngine *engine, int enabled);
// Configure only while stopped. Reduces room reverberation, not speaker echo.
int mv_set_room_dereverb(MVEngine *engine, int enabled);
// Default is 1: Core ML automatic, with native fallback if loading fails.
int mv_set_deepfilter_acceleration(MVEngine *engine, int enabled);
void mv_stats(MVEngine *engine, MVStats *stats);

#ifdef __cplusplus
}
#endif
