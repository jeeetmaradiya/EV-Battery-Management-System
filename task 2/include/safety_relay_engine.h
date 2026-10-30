/**
 * @file safety_relay_engine.h
 * @brief Public API Specification for the Non-Blocking Protection Relay and Safety System.
 * 
 * Target: IEC 60255 Protection Relays, IEC 61508 Functional Safety.
 * Execution Model: Deterministic non-blocking cooperative tick execution.
 */

#ifndef SAFETY_RELAY_ENGINE_H
#define SAFETY_RELAY_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "safety_relay_types.h"

/**
 * @brief Primary Control & Operational Context for Safety Relay Engine.
 * 
 * Entirely self-contained: Supports multiple independent relay instances.
 * Zero dynamic memory allocation. Deterministic static footprint.
 */
typedef struct {
    /* Primary State Machine */
    RelayState_t current_state;
    RelayContactState_t contacts;
    uint32_t state_entry_time_ms;
    uint32_t last_tick_time_ms;
    
    /* Timers & Debounce */
    uint32_t trip_debounce_counter_ms;
    uint32_t clear_debounce_counter_ms;
    uint32_t last_fault_time_ms;
    uint32_t re_fault_count;
    
    /* Active Sensor & Protection Flags */
    uint16_t active_flags;
    RelayTransitionReason_t pending_trip_reason;
    
    /* Statistical Processing Windows */
    WindowStats_t current_window;
    WindowStats_t voltage_window;
    float filtered_current_a;
    float filtered_voltage_v;
    
    /* Ring Buffer for State Transitions */
    RelayTransitionEvent_t event_ring[RELAY_EVENT_LOG_CAPACITY];
    uint32_t ring_head;
    uint32_t ring_total_events;
    
    /* Diagnostic Counters */
    uint32_t total_ticks_processed;
    uint32_t trip_count;
    uint32_t noise_spikes_rejected_count;
    uint32_t sensor_anomalies_count;
    
    /* Internal initialization flag */
    bool is_initialized;
} RelayContext_t;

/**
 * @brief Initialize the Safety Relay Context.
 * 
 * Resets all internal states, zero-initializes the ring buffer, clears anomaly flags,
 * and places the system in `RELAY_STATE_BOOT` awaiting the first processing tick.
 * 
 * @param ctx Pointer to allocated RelayContext_t instance.
 * @param start_time_ms System timestamp at boot in milliseconds.
 * @return true if initialized successfully, false if null pointer.
 */
bool SafetyRelay_Init(RelayContext_t *ctx, uint32_t start_time_ms);

/**
 * @brief Primary Non-Blocking Cooperative Step Function.
 * 
 * MUST be invoked periodically (e.g. every RELAY_TICK_INTERVAL_MS = 10ms).
 * Strictly non-blocking: guaranteed O(1) execution time with zero sleep/delay loops.
 * 
 * Performs:
 * 1. Physical range & transducer health verification.
 * 2. Window-based statistical anomaly detection (frozen reading, slew jump, noise spikes).
 * 3. Statistical discrimination of genuine fast load step vs sensor EMI burst.
 * 4. Dual-threshold anti-chatter hysteresis calculation.
 * 5. Non-blocking trip/clear debounce timing.
 * 6. Timed recovery sequence advancement (Cooldown -> Soak -> Re-arm -> Normal).
 * 7. Transition logging with millisecond timestamped forensic records.
 * 
 * @param ctx Pointer to RelayContext_t instance.
 * @param current_time_ms Current monotonic millisecond clock.
 * @param raw_current_a Raw physical current sample from transducer (Amperes).
 * @param raw_voltage_v Raw physical voltage sample from transducer (Volts).
 */
void SafetyRelay_ProcessTick(RelayContext_t *ctx, uint32_t current_time_ms, float raw_current_a, float raw_voltage_v);

/**
 * @brief Manual Operator / Supervisory Reset.
 * 
 * Clears latching lockout after human inspection and returns system to recovery sequence.
 * 
 * @param ctx Pointer to RelayContext_t instance.
 * @param current_time_ms Current monotonic millisecond clock.
 * @return true if reset accepted, false if blocked by active persistent fault.
 */
bool SafetyRelay_ManualReset(RelayContext_t *ctx, uint32_t current_time_ms);

/**
 * @brief Retrieve instantaneous diagnostic and operational telemetry.
 * 
 * @param ctx Pointer to RelayContext_t instance.
 * @param out_telem Pointer to destination telemetry struct.
 */
void SafetyRelay_GetTelemetry(const RelayContext_t *ctx, RelayTelemetry_t *out_telem);

/**
 * @brief Get total number of transition events recorded since initialization.
 * 
 * @param ctx Pointer to RelayContext_t instance.
 * @return Total transition count.
 */
uint32_t SafetyRelay_GetEventCount(const RelayContext_t *ctx);

/**
 * @brief Retrieve a specific historical transition event by chronological index.
 * 
 * @param ctx Pointer to RelayContext_t instance.
 * @param index Chronological event index (0 to min(total, CAPACITY-1)).
 * @param out_event Destination event pointer.
 * @return true if index exists, false otherwise.
 */
bool SafetyRelay_GetEventAt(const RelayContext_t *ctx, uint32_t index, RelayTransitionEvent_t *out_event);

/**
 * @brief Helper: Convert RelayState enum to human-readable string.
 */
const char* SafetyRelay_StateToString(RelayState_t state);

/**
 * @brief Helper: Convert TransitionReason enum to human-readable string.
 */
const char* SafetyRelay_ReasonToString(RelayTransitionReason_t reason);

/**
 * @brief Helper: Convert SignalClass enum to human-readable string.
 */
const char* SafetyRelay_SignalClassToString(SignalClass_t sclass);

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_RELAY_ENGINE_H */
