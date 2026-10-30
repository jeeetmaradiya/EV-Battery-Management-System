/**
 * @file fault_state_machine_config.h
 * @brief Safety Thresholds, Debounce Windows, and Timing Limits for Fault FSM.
 * 
 * Safety Critical Embedded System Architecture (ISO 26262 ASIL-C / MISRA-C:2012)
 */

#ifndef FAULT_STATE_MACHINE_CONFIG_H
#define FAULT_STATE_MACHINE_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* SAMPLING & SYSTEM TIMING CONFIGURATION                                     */
/* ========================================================================== */

#define CONFIG_FSM_TICK_INTERVAL_MS             (10U)   /**< Main engine evaluation cycle (100 Hz) */
#define CONFIG_MAX_CELL_COUNT                   (12U)   /**< 12S lithium cell pack string */
#define CONFIG_MAX_TEMP_SENSORS                 (4U)    /**< 4 surface thermistors */
#define CONFIG_MAX_ADC_CHANNELS                 (4U)    /**< 4 analog monitoring channels */

/* ========================================================================== */
/* BATTERY CELL THRESHOLDS                                                    */
/* ========================================================================== */

#define CONFIG_CELL_OVP_CRITICAL_MV             (4250U) /**< Overvoltage protection trip (4.25 V) */
#define CONFIG_CELL_UVP_CRITICAL_MV             (2800U) /**< Undervoltage protection trip (2.80 V) */
#define CONFIG_CELL_IMBALANCE_WARNING_MV        (120U)  /**< Imbalance warning threshold (120 mV) */
#define CONFIG_CELL_IMBALANCE_CRITICAL_MV       (250U)  /**< Imbalance critical trip (250 mV) */
#define CONFIG_CELL_OTP_WARNING_C               (55)    /**< Cell overtemp warning (55 C) */
#define CONFIG_CELL_OTP_CRITICAL_C              (65)    /**< Cell overtemp critical (65 C) */

/* Recovery Cell Voltage Window */
#define CONFIG_RECOVERY_CELL_MIN_MV             (3000U) /**< Minimum voltage to allow recovery */
#define CONFIG_RECOVERY_CELL_MAX_MV             (4180U) /**< Maximum voltage to allow recovery */
#define CONFIG_RECOVERY_CELL_MAX_IMBALANCE_MV   (100U)  /**< Max delta for recovery qualification */

/* ========================================================================== */
/* RELAY SUBSYSTEM CONFIGURATION                                              */
/* ========================================================================== */

#define CONFIG_RELAY_DEBOUNCE_MS                (50U)   /**< Mechanical actuation allowance (50 ms) */
#define CONFIG_RELAY_MISMATCH_TRIP_COUNT        (3U)    /**< Ticks mismatch persists after debounce */

/* ========================================================================== */
/* ADC ACQUISITION & FROZEN DETECTOR CONFIGURATION                            */
/* ========================================================================== */

#define CONFIG_ADC_FROZEN_SAMPLE_DEPTH          (8U)    /**< Consecutive samples to evaluate invariance */
#define CONFIG_ADC_FROZEN_MAX_DELTA             (1U)    /**< Permissible noise variance threshold (<= 1 LSB) */
#define CONFIG_ADC_STUCK_LOW_THRESHOLD          (15U)   /**< Lower rail cutoff count (0-4095 range) */
#define CONFIG_ADC_STUCK_HIGH_THRESHOLD         (4080U) /**< Upper rail cutoff count (0-4095 range) */

/* ========================================================================== */
/* COMMUNICATION SUPERVISOR CONFIGURATION                                     */
/* ========================================================================== */

#define CONFIG_COMM_HEARTBEAT_TIMEOUT_MS        (500U)  /**< CAN/SPI bus loss timeout (500 ms) */
#define CONFIG_COMM_CRC_BURST_THRESHOLD         (5U)    /**< CRC errors within 1 second window */

/* ========================================================================== */
/* STRUCTURED RECOVERY TIMING & DEADLOCK PREVENTION                           */
/* ========================================================================== */

#define CONFIG_RECOVERY_QUARANTINE_MS           (500U)  /**< Stage 2: Clean quarantine dwell (500 ms) */
#define CONFIG_RECOVERY_SUBSYSTEM_QUALIFY_SAMPLES (20U) /**< Stage 3: Required healthy ADC/Comm ticks */
#define CONFIG_RECOVERY_PROVISIONAL_DEGRADED_MS (1000U) /**< Stage 4: Dwell time in DEGRADED before NORMAL */
#define CONFIG_RECOVERY_MAX_RETRIES             (3U)    /**< Deadlock prevention: Max attempts before SHUTDOWN */
#define CONFIG_RECOVERY_STAGE_TIMEOUT_MS        (3000U) /**< Watchdog timeout per individual recovery stage */

/* ========================================================================== */
/* STORAGE & CIRCULAR BUFFER CAPACITIES                                       */
/* ========================================================================== */

#define CONFIG_FAULT_REGISTRY_CAPACITY          (16U)   /**< Maximum concurrently tracked fault records */
#define CONFIG_TRANSITION_LOG_CAPACITY          (64U)   /**< Circular buffer for state transitions */

#ifdef __cplusplus
}
#endif

#endif /* FAULT_STATE_MACHINE_CONFIG_H */
