#ifndef LP32_AUDIO_BRIDGE_H
#define LP32_AUDIO_BRIDGE_H

#include <stdint.h>

int audio_bridge32_dispatch(const char *import_name, const uint32_t *arguments,
                            uint64_t *result);

/* Pre-opened graph pool counters for frame statistics. */
void audio_bridge32_pool_statistics(uint64_t *hits, uint64_t *misses);

/* Deferred-worker queue latency since the previous call (values reset). */
struct audio_bridge32_worker_stats {
    uint64_t starts;
    double start_wait_avg_ms;
    double start_wait_max_ms;
    double op_run_max_ms;
    uint32_t queue_depth_max;
    /* Render-callback health: guest renders that came back flagged silent
       (a starved stream), discontinuities in the host sample-time sequence
       (IO cycles the HAL skipped) with the frames they covered, and the
       longest host->guest->host round trip. */
    uint64_t silent_renders;
    uint64_t timestamp_gaps;
    uint64_t timestamp_gap_frames;
    double callback_max_ms;
    /* Engine stream ring: deepest refill backlog seen and renders that ran
       with the worker a full ring behind (stale chunk playback). */
    uint64_t stream_pending_max;
    uint64_t stream_behind_renders;
    /* Audio hold: frames that overran the engine's 100 ms delta clamp, the
       simulation time they discarded (which the hold silenced instead of
       letting audio run ahead), and guest renders skipped while holding. */
    uint64_t hold_episodes;
    double hold_ms;
    uint64_t hold_renders;
};
void audio_bridge32_worker_statistics(struct audio_bridge32_worker_stats *stats);

/* Presentation clock for the audio hold; call once per presented frame from
   the render thread. */
void audio_bridge32_note_frame_presented(void);

#endif
