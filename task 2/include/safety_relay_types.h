/**
 * @file safety_relay_types.h
 * @brief Data Structures, Enumerations, and Telemetry Contracts for Safety Relay.
 * 
 * Standards Target: IEC 60255, IEC 61508.
 * Deterministic memory model with static allocations.
 */

#ifndef SAFETY_RELAY_TYPES_H
#define SAFETY_RELAY_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "safety_relay_config.h"

/**
 * @brief Primary Operational States of the Non-Blocking Safety Relay State Machine.
 */
typedef enum {
    RELAY_STATE_BOOT = 0,               /**< Power-up initialization and self-test */
    RELAY_STATE_NORMAL_MONITORING,      /**< Closed contacts, full protection active */
    RELAY_STATE_TRIP_DEBOUNCING,        /**< Fault detected, accumulating trip debounce time */
    RELAY_STATE_TRIPPED_FAULT,          /**< Open contacts, fault confirmed and isolated */
    RELAY_STATE_RECOVERY_COOLDOWN,      /**< Open contacts, waiting thermal cooldown dwell */
    RELAY_STATE_RECOVERY_SOAK,          /**< Open contacts, verifying signal stability & baseline */
    RELAY_STATE_RECOVERY_REARMING,      /**< Pre-charging coil, checking interlocks */
    RELAY_STATE_LOCKOUT_LATCHED         /**< Latched lockout due to excessive repeated faults */
} RelayState_t;

/**
 * @brief Physical Contactor Actuator State.
 */
typedef enum {
    RELAY_CONTACTS_OPEN = 0,            /**< Load disconnected (Safe de-energized state) */
    RELAY_CONTACTS_CLOSED = 1           /**< Load connected (Operational energized state) */
} RelayContactState_t;

/**
 * @brief Signal Classifier Classification Result.
 */
typedef enum {
    SIGNAL_CLASS_STEADY_NORMAL = 0,     /**< Normal operational signal within expected noise floor */
    SIGNAL_CLASS_GENUINE_LOAD_CHANGE,   /**< Legitimate fast load step: persistent post-step level */
    SIGNAL_CLASS_SENSOR_NOISE_SPIKE,    /**< Transient EMI impulse: isolated spike, non-persistent */
    SIGNAL_CLASS_ANOMALY_DETECTED       /**< Severe physical sensor anomaly (out-of-range/freeze/slew) */
} SignalClass_t;

/**
 * @brief Sensor Anomaly and Fault Bitmask Flags.
 */
typedef enum {
    ANOMALY_FLAG_NONE                    = 0x0000,
    ANOMALY_FLAG_OUT_OF_RANGE_LOW        = 0x0001,  /**< Below physical minimum (broken wire) */
    ANOMALY_FLAG_OUT_OF_RANGE_HIGH       = 0x0002,  /**< Above physical maximum (rail short) */
    ANOMALY_FLAG_UNREALISTIC_SLEW_JUMP   = 0x0004,  /**< |dI/dt| exceeds maximum physical slew rate */
    ANOMALY_FLAG_SENSOR_FROZEN           = 0x0008,  /**< Zero variance/span across window */
    ANOMALY_FLAG_NOISE_SPIKE_REJECTED    = 0x0010,  /**< High impulsive spike rejected by median filter */
    ANOMALY_FLAG_OVERCURRENT_TRIP        = 0x0020,  /**< Current exceeded trip threshold debounced */
    ANOMALY_FLAG_OVERVOLTAGE_TRIP        = 0x0040,  /**< Voltage exceeded trip threshold debounced */
    ANOMALY_FLAG_UNDERVOLTAGE_TRIP       = 0x0080,  /**< Voltage dropped below UV threshold debounced */
    ANOMALY_FLAG_HYSTERESIS_ACTIVE       = 0x0100   /**< Signal currently in hysteresis deadband */
} AnomalyFlag_t;

/**
 * @brief Formal Transition Reasons for Forensic Auditing.
 */
typedef enum {
    TR_REASON_NONE = 0,
    TR_REASON_BOOT_COMPLETE,
    TR_REASON_OVERCURRENT_PENDING,
    TR_REASON_OVERCURRENT_DEBOUNCED_TRIP,
    TR_REASON_OVERVOLTAGE_DEBOUNCED_TRIP,
    TR_REASON_UNDERVOLTAGE_DEBOUNCED_TRIP,
    TR_REASON_SENSOR_ANOMALY_TRIP,
    TR_REASON_TRANSIENT_CLEARED_BEFORE_DEBOUNCE,
    TR_REASON_FAULT_CLEARED_DEBOUNCED,
    TR_REASON_COOLDOWN_EXPIRED,
    TR_REASON_SOAK_PASSED,
    TR_REASON_REARM_COMPLETE,
    TR_REASON_RECOVERY_ABORT_REFAULT,
    TR_REASON_LOCKOUT_MAX_REFAULTS,
    TR_REASON_MANUAL_RESET
} RelayTransitionReason_t;

/**
 * @brief Forensic Audit Log Entry for Every State Transition.
 */
typedef struct {
    uint32_t timestamp_ms;
    RelayState_t from_state;
    RelayState_t to_state;
    RelayTransitionReason_t reason;
    float current_a;
    float voltage_v;
    uint16_t anomaly_flags;
    float rolling_variance;
    float slew_rate_a_s;
} RelayTransitionEvent_t;

/**
 * @brief Window-Based Statistical Metrics for Sensor Signal Analysis.
 */
typedef struct {
    float samples[RELAY_WINDOW_SIZE];
    uint32_t head_idx;
    uint32_t count;
    float sum;
    float mean;
    float variance;
    float std_dev;
    float min_val;
    float max_val;
    float span;
    float prev_sample;
    float instantaneous_slew_rate;
    SignalClass_t classification;
    uint32_t consecutive_step_samples;
    float candidate_step_level;
} WindowStats_t;

/**
 * @brief Diagnostic and Operational Telemetry Snapshot.
 */
typedef struct {
    uint32_t timestamp_ms;
    RelayState_t state;
    RelayContactState_t contacts;
    float current_a;
    float filtered_current_a;
    float voltage_v;
    uint16_t active_flags;
    SignalClass_t signal_class;
    
    /* Timers & Debounce counters */
    uint32_t state_dwell_time_ms;
    uint32_t trip_debounce_counter_ms;
    uint32_t clear_debounce_counter_ms;
    
    /* Statistical window diagnostics */
    float current_mean;
    float current_variance;
    float current_slew_rate_a_s;
    float current_window_span;
    
    /* Stability and Recovery counters */
    uint32_t total_transitions;
    uint32_t trip_count;
    uint32_t re_fault_count;
    uint32_t noise_spikes_rejected;
    uint32_t anomalies_detected;
} RelayTelemetry_t;

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_RELAY_TYPES_H */
