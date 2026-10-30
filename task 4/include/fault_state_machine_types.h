/**
 * @file fault_state_machine_types.h
 * @brief Formal Type Definitions, Enumerations, and Data Structures for
 *        Deterministic Fault State Machine with Structured Recovery.
 * 
 * Safety Critical Embedded System Architecture (ISO 26262 ASIL-C / MISRA-C:2012)
 * Operating States: NORMAL, DEGRADED, FAILSAFE, SHUTDOWN.
 */

#ifndef FAULT_STATE_MACHINE_TYPES_H
#define FAULT_STATE_MACHINE_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* OPERATING STATES                                                           */
/* ========================================================================== */

/**
 * @brief Deterministic System Operating States.
 */
typedef enum {
    FAULT_STATE_NORMAL    = 0U, /**< Subsystems nominal. Full performance & relay engagement. */
    FAULT_STATE_DEGRADED  = 1U, /**< Non-critical anomaly. Derated power/telemetry, safe continuity. */
    FAULT_STATE_FAILSAFE  = 2U, /**< Critical fault active. High-voltage isolation, relays opened. */
    FAULT_STATE_SHUTDOWN  = 3U, /**< Terminal latched state. Permanent lockout, requires external service. */
    FAULT_STATE_COUNT     = 4U
} FaultOperatingState_t;

/* ========================================================================== */
/* FAULT SEVERITY CLASSIFICATION                                              */
/* ========================================================================== */

/**
 * @brief Severity levels governing state transition decisions.
 */
typedef enum {
    FAULT_SEV_NONE        = 0U, /**< No active fault. */
    FAULT_SEV_WARNING     = 1U, /**< Derating required -> Prompts transition to DEGRADED. */
    FAULT_SEV_CRITICAL    = 2U, /**< Safety hazard -> Prompts immediate transition to FAILSAFE. */
    FAULT_SEV_FATAL       = 3U  /**< Catastrophic/Unrecoverable -> Prompts transition to SHUTDOWN. */
} FaultSeverity_t;

/* ========================================================================== */
/* FAULT SOURCE ISOLATION IDENTIFIERS                                         */
/* ========================================================================== */

/**
 * @brief Hardware and Subsystem Fault Domains.
 */
typedef enum {
    FAULT_SRC_NONE            = 0U,
    FAULT_SRC_BATTERY_CELL    = 1U, /**< Individual lithium cell voltage, imbalance, temperature. */
    FAULT_SRC_RELAY           = 2U, /**< Main contactor/precharge drive and auxiliary feedback. */
    FAULT_SRC_COMMUNICATION   = 3U, /**< Controller Area Network (CAN) / SPI telemetry bus. */
    FAULT_SRC_ADC             = 4U, /**< Analog-to-Digital Converter acquisition hardware. */
    FAULT_SRC_SYSTEM          = 5U  /**< Recovery watchdog, memory integrity, or supervisor logic. */
} FaultSource_t;

/* ========================================================================== */
/* SPECIFIC FAULT IDENTIFIERS                                                 */
/* ========================================================================== */

/**
 * @brief Standardized 16-bit Fault Identification Codes.
 * High nibble represents fault domain (0x1=Cell, 0x2=Relay, 0x3=Comm, 0x4=ADC, 0x5=System).
 */
typedef enum {
    FAULT_ID_NONE                     = 0x0000U,

    /* Battery Cell Fault Domain (0x1000 - 0x10FF) */
    FAULT_ID_CELL_OVERVOLTAGE         = 0x1001U,
    FAULT_ID_CELL_UNDERVOLTAGE        = 0x1002U,
    FAULT_ID_CELL_IMBALANCE           = 0x1003U,
    FAULT_ID_CELL_OVERTEMP            = 0x1004U,

    /* Relay Subsystem Fault Domain (0x2000 - 0x20FF) */
    FAULT_ID_RELAY_MISMATCH           = 0x2001U, /**< Commanded vs actual contact feedback mismatch */
    FAULT_ID_RELAY_WELDED             = 0x2002U, /**< Commanded open but feedback closed (critical) */
    FAULT_ID_RELAY_COIL_OPEN          = 0x2003U, /**< Commanded closed but feedback open */

    /* Communication Fault Domain (0x3000 - 0x30FF) */
    FAULT_ID_COMM_HEARTBEAT_TIMEOUT   = 0x3001U, /**< Loss of keepalive frames */
    FAULT_ID_COMM_CRC_BURST           = 0x3002U, /**< Frame checksum errors exceeding threshold */

    /* ADC Acquisition Fault Domain (0x4000 - 0x40FF) */
    FAULT_ID_ADC_FROZEN               = 0x4001U, /**< Invariant digital reading across N samples */
    FAULT_ID_ADC_OUT_OF_RANGE         = 0x4002U, /**< Stuck-at-rail (0V or Vref) */

    /* System & Structured Recovery Fault Domain (0x5000 - 0x50FF) */
    FAULT_ID_RECOVERY_EXHAUSTED       = 0x5001U, /**< Max recovery verification retries exceeded */
    FAULT_ID_WATCHDOG_TIMEOUT         = 0x5002U, /**< Execution window expired */
    FAULT_ID_EMERGENCY_STOP           = 0x5003U  /**< External manual emergency interlock asserted */
} FaultId_t;

/* ========================================================================== */
/* STRUCTURED RECOVERY STAGES                                                 */
/* ========================================================================== */

/**
 * @brief Multi-Phase Verification Process for FAILSAFE Recovery.
 * Ensures zero immediate, unvalidated transitions back to normal.
 */
typedef enum {
    RECOVERY_STAGE_IDLE               = 0U, /**< System not currently in recovery sequence. */
    RECOVERY_STAGE_CLEARANCE_CHECK    = 1U, /**< Verification that active critical faults are cleared. */
    RECOVERY_STAGE_STABILIZATION      = 2U, /**< Quarantine dwell period with zero new fault events. */
    RECOVERY_STAGE_SUBSYSTEM_VERIFY   = 3U, /**< Active self-test: ADC dynamic dithering, comm frame audit, relay impedance. */
    RECOVERY_STAGE_PROVISIONAL_DEGRADE= 4U, /**< Derated re-engagement dwell in DEGRADED mode. */
    RECOVERY_STAGE_NORMAL_RESTORE     = 5U, /**< Final deterministic transition back to NORMAL. */
    RECOVERY_STAGE_ABORTED            = 6U  /**< Recovery failed -> Escalated to SHUTDOWN. */
} RecoveryStage_t;

/* ========================================================================== */
/* FAULT REGISTRY STRUCTURE                                                   */
/* ========================================================================== */

/**
 * @brief Persistent Fault Entry in the Active Fault Registry.
 */
typedef struct {
    FaultId_t       fault_id;
    FaultSource_t   source;
    FaultSeverity_t severity;
    bool            is_active;
    bool            is_latched;
    uint32_t        first_detected_ms;
    uint32_t        last_seen_ms;
    uint32_t        occurrence_count;
    uint32_t        isolation_aux_data[4]; /**< Telemetry snapshot (e.g., cell index, raw ADC count, relay bits) */
    char            description[64];
} FaultRecord_t;

/* ========================================================================== */
/* STRUCTURED TRANSITION LOG RECORD                                           */
/* ========================================================================== */

/**
 * @brief Structured Audit Log for Every State Transition.
 */
typedef struct {
    uint32_t              sequence_id;
    uint32_t              timestamp_ms;
    FaultOperatingState_t prev_state;
    FaultOperatingState_t new_state;
    FaultId_t             trigger_fault_id;
    FaultSource_t         fault_source;
    FaultSeverity_t       severity;
    char                  reason[128];
} StateTransitionLog_t;

/* ========================================================================== */
/* TRANSITION TABLE ENTRY                                                     */
/* ========================================================================== */

/**
 * @brief Deterministic Transition Table Matrix Rule.
 */
typedef struct {
    FaultOperatingState_t from_state;
    FaultSeverity_t       trigger_severity;
    FaultOperatingState_t to_state;
    bool                  requires_recovery_qualification;
    const char           *description;
} TransitionRule_t;

/* ========================================================================== */
/* HARDWARE SENSOR SNAPSHOT INPUT                                             */
/* ========================================================================== */

/**
 * @brief Raw Subsystem Sensor Telemetry Feed into the State Machine.
 */
typedef struct {
    /* Battery Cell Subsystem (12-cell series string) */
    uint16_t cell_voltages_mv[12];
    int16_t  cell_temperatures_c[4];
    uint16_t cell_count;

    /* Relay Subsystem */
    bool     commanded_main_relay;
    bool     commanded_precharge_relay;
    bool     feedback_main_relay;       /**< Auxiliary contact physical state */
    bool     feedback_precharge_relay;  /**< Auxiliary precharge contact physical state */
    uint16_t relay_transition_age_ms;   /**< Elapsed time since commanded state changed */

    /* Communication Subsystem */
    uint32_t last_comm_heartbeat_ms;
    uint16_t comm_crc_error_count;
    bool     comm_bus_active;

    /* ADC Subsystem (Multi-channel) */
    uint16_t raw_adc_samples[4];        /**< Current channel raw counts */
    uint16_t adc_channel_count;

    /* Supervisor / Interlocks */
    bool     manual_estop_asserted;
} SubsystemTelemetry_t;

/* ========================================================================== */
/* RECOVERY CONTROLLER CONTEXT                                                */
/* ========================================================================== */

/**
 * @brief State Context for the Multi-Stage Verification Pipeline.
 */
typedef struct {
    RecoveryStage_t current_stage;
    uint32_t        stage_entered_timestamp_ms;
    uint32_t        retry_counter;
    uint32_t        max_allowed_retries;
    uint32_t        samples_qualified;
    uint32_t        required_samples;
    bool            recovery_in_progress;
    bool            deadlock_trip_latched;
    uint32_t        stage_timeout_ms;
} RecoveryController_t;

#ifdef __cplusplus
}
#endif

#endif /* FAULT_STATE_MACHINE_TYPES_H */
