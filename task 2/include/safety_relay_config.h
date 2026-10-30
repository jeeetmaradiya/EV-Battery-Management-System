/**
 * @file safety_relay_config.h
 * @brief Compile-Time Configuration Parameters for the Non-Blocking Safety System.
 * 
 * Standards Target: IEC 60255 (Protection Relays), IEC 61508 (Functional Safety SIL-3).
 * Architecture: Deterministic non-blocking cooperative tick execution.
 */

#ifndef SAFETY_RELAY_CONFIG_H
#define SAFETY_RELAY_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* ========================================================================== */
/* TIMING & EXECUTION ENGINE CONFIGURATION                                    */
/* ========================================================================== */

/** @brief Nominal cooperative loop tick interval in milliseconds */
#define RELAY_TICK_INTERVAL_MS              (10U)

/** @brief Size of the statistical moving window (must be power of 2 for fast modulus) */
#define RELAY_WINDOW_SIZE                   (16U)

/** @brief Capacity of the deterministic transition event ring buffer */
#define RELAY_EVENT_LOG_CAPACITY            (64U)


/* ========================================================================== */
/* PROTECTION THRESHOLDS & HYSTERESIS (ANTI-CHATTER)                          */
/* ========================================================================== */

/** @brief Overcurrent Trip Threshold (Amperes) */
#define RELAY_CURRENT_OVERCURRENT_TRIP_A    (50.0f)

/** @brief Overcurrent Reset / Clear Threshold with Hysteresis (Amperes) */
#define RELAY_CURRENT_OVERCURRENT_RESET_A   (42.0f)   /* 8.0A Hysteresis band */

/** @brief Trip Debounce Duration (Fault must persist this long to trip) */
#define RELAY_TRIP_DEBOUNCE_MS              (120U)    /* 12 consecutive ticks @ 10ms */

/** @brief Clear Debounce Duration (Condition must remain healthy to clear) */
#define RELAY_CLEAR_DEBOUNCE_MS             (300U)    /* 30 consecutive ticks @ 10ms */

/** @brief Overvoltage Trip Threshold (Volts) */
#define RELAY_VOLTAGE_OVERVOLT_TRIP_V       (260.0f)

/** @brief Overvoltage Reset Threshold (Volts) */
#define RELAY_VOLTAGE_OVERVOLT_RESET_V      (245.0f)  /* 15.0V Hysteresis band */

/** @brief Undervoltage Trip Threshold (Volts) */
#define RELAY_VOLTAGE_UNDERVOLT_TRIP_V      (185.0f)

/** @brief Undervoltage Reset Threshold (Volts) */
#define RELAY_VOLTAGE_UNDERVOLT_RESET_V     (200.0f)  /* 15.0V Hysteresis band */


/* ========================================================================== */
/* SENSOR ANOMALY DETECTION LIMITS                                            */
/* ========================================================================== */

/** @brief Physical absolute minimum valid voltage (below indicates wire break) */
#define RELAY_SENSOR_V_MIN_VALID            (10.0f)

/** @brief Physical absolute maximum valid voltage (above indicates short/saturation) */
#define RELAY_SENSOR_V_MAX_VALID            (320.0f)

/** @brief Physical absolute minimum valid current (Amperes) */
#define RELAY_SENSOR_I_MIN_VALID            (-5.0f)

/** @brief Physical absolute maximum valid current (Amperes) */
#define RELAY_SENSOR_I_MAX_VALID            (120.0f)

/** @brief Maximum realistic current slew rate |dI/dt| (Amperes per second) */
#define RELAY_MAX_PHYSICAL_SLEW_RATE_A_S    (150.0f)

/** @brief Sensor Frozen Reading Detection Window (consecutive samples) */
#define RELAY_FREEZE_DETECTION_SAMPLES      (16U)

/** @brief Sensor Frozen Threshold: minimum expected variation span */
#define RELAY_FREEZE_MIN_SPAN_A             (0.015f)

/** @brief Sensor Noise Floor (Nominal Standard Deviation in healthy sensor) */
#define RELAY_NOMINAL_NOISE_STD_DEV_A       (0.12f)

/** @brief Noise vs Step Persistence Confirmation Count (samples) */
#define RELAY_STEP_PERSISTENCE_MIN_SAMPLES  (3U)

/** @brief Noise rejection threshold multiplier (sigma multiple for spike detection) */
#define RELAY_NOISE_SPIKE_SIGMA_FACTOR      (4.0f)


/* ========================================================================== */
/* TIMED RECOVERY SEQUENCE (POST-FAULT)                                       */
/* ========================================================================== */

/** @brief Post-fault thermal cooldown dwell time (milliseconds) */
#define RELAY_RECOVERY_COOLDOWN_MS          (3000U)

/** @brief Signal stabilization and soak pre-check duration (milliseconds) */
#define RELAY_RECOVERY_SOAK_PRECHECK_MS     (2000U)

/** @brief Relay coil pre-strike and contact re-arming duration (milliseconds) */
#define RELAY_RECOVERY_REARMING_MS          (500U)

/** @brief Maximum re-fault count before latched lockout */
#define RELAY_MAX_RE_FAULTS_BEFORE_LOCKOUT  (3U)

/** @brief Lockout evaluation window in milliseconds */
#define RELAY_LOCKOUT_WINDOW_MS             (30000U)

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_RELAY_CONFIG_H */
