/**
 * @file blynk_analytics_types.h
 * @brief Formal Type Definitions for Enterprise Blynk Analytics & Decision Engine.
 * 
 * Safety Critical Embedded System Architecture (ISO 26262 ASIL-C / MISRA-C:2012)
 * Integrates:
 *  - Deterministic Task 4 Fault State Machine (NORMAL, DEGRADED, FAILSAFE, SHUTDOWN)
 *  - Multi-Stage Structured Recovery (Stages 0..6)
 *  - Mathematical Multi-Factor Composite Risk Analysis (CRS)
 *  - Dynamic Health (SOH & SOC) Profiling
 *  - Rule-Based Diagnostic Recommendation Engine
 *  - Structured Fault History & Transition Audit Trail
 *  - Enterprise Blynk Virtual Pin Registry (V0..V34)
 */

#ifndef BLYNK_ANALYTICS_TYPES_H
#define BLYNK_ANALYTICS_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* TASK 4 OPERATING STATES (IDENTICAL ALIGNMENT)                              */
/* ========================================================================== */

typedef enum {
    ANALYTICS_STATE_NORMAL    = 0U, /**< Subsystems nominal. Full engagement & optimal health. */
    ANALYTICS_STATE_DEGRADED  = 1U, /**< Derated power/telemetry, non-critical warning active. */
    ANALYTICS_STATE_FAILSAFE  = 2U, /**< Critical fault tripped. Contactors isolated. Recovery pending. */
    ANALYTICS_STATE_SHUTDOWN  = 3U, /**< Permanent terminal lockout. Service reset required. */
    ANALYTICS_STATE_COUNT     = 4U
} AnalyticsOperatingState_t;

/* ========================================================================== */
/* FAULT SEVERITY CLASSIFICATION                                              */
/* ========================================================================== */

typedef enum {
    ANALYTICS_SEV_NONE        = 0U, /**< Normal nominal operation. */
    ANALYTICS_SEV_WARNING     = 1U, /**< Derating advisory -> Prompts DEGRADED state. */
    ANALYTICS_SEV_CRITICAL    = 2U, /**< Safety hazard -> Prompts immediate FAILSAFE trip. */
    ANALYTICS_SEV_FATAL       = 3U  /**< Catastrophic / unrecoverable -> Prompts SHUTDOWN lockout. */
} AnalyticsFaultSeverity_t;

/* ========================================================================== */
/* FAULT SOURCE DOMAIN ISOLATION                                              */
/* ========================================================================== */

typedef enum {
    ANALYTICS_SRC_NONE            = 0U,
    ANALYTICS_SRC_BATTERY_CELL    = 1U, /**< Cell voltage, delta imbalance, overtemp. */
    ANALYTICS_SRC_RELAY           = 2U, /**< Contactor coil, auxiliary feedback, welding. */
    ANALYTICS_SRC_COMMUNICATION   = 3U, /**< CAN bus, Wi-Fi keepalive, CRC burst. */
    ANALYTICS_SRC_ADC             = 4U, /**< Sensor acquisition, stuck-at rail, frozen counts. */
    ANALYTICS_SRC_SYSTEM          = 5U  /**< Recovery deadlock, watchdog, manual e-stop. */
} AnalyticsFaultSource_t;

/* ========================================================================== */
/* STANDARDIZED FAULT IDENTIFICATION CODES (TASK 4 MATCHING)                  */
/* ========================================================================== */

typedef enum {
    ANALYTICS_FAULT_NONE                     = 0x0000U,

    /* Cell Domain (0x1000) */
    ANALYTICS_FAULT_CELL_OVERVOLTAGE         = 0x1001U,
    ANALYTICS_FAULT_CELL_UNDERVOLTAGE        = 0x1002U,
    ANALYTICS_FAULT_CELL_IMBALANCE           = 0x1003U,
    ANALYTICS_FAULT_CELL_OVERTEMP            = 0x1004U,

    /* Contactor Domain (0x2000) */
    ANALYTICS_FAULT_RELAY_MISMATCH           = 0x2001U,
    ANALYTICS_FAULT_RELAY_WELDED             = 0x2002U,
    ANALYTICS_FAULT_RELAY_COIL_OPEN          = 0x2003U,

    /* Comm Domain (0x3000) */
    ANALYTICS_FAULT_COMM_HEARTBEAT_TIMEOUT   = 0x3001U,
    ANALYTICS_FAULT_COMM_CRC_BURST           = 0x3002U,

    /* ADC Acquisition Domain (0x4000) */
    ANALYTICS_FAULT_ADC_FROZEN               = 0x4001U,
    ANALYTICS_FAULT_ADC_OUT_OF_RANGE         = 0x4002U,

    /* System & Supervisor Domain (0x5000) */
    ANALYTICS_FAULT_RECOVERY_EXHAUSTED       = 0x5001U,
    ANALYTICS_FAULT_WATCHDOG_TIMEOUT         = 0x5002U,
    ANALYTICS_FAULT_EMERGENCY_STOP           = 0x5003U
} AnalyticsFaultId_t;

/* ========================================================================== */
/* STRUCTURED MULTI-STAGE RECOVERY STAGES (TASK 4 ALIGNMENT)                  */
/* ========================================================================== */

typedef enum {
    ANALYTICS_REC_STAGE_IDLE               = 0U, /**< System not in recovery. */
    ANALYTICS_REC_STAGE_CLEARANCE_CHECK    = 1U, /**< Verifying critical fault condition clearance. */
    ANALYTICS_REC_STAGE_STABILIZATION      = 2U, /**< Dwell quarantine period with zero new faults. */
    ANALYTICS_REC_STAGE_SUBSYSTEM_VERIFY   = 3U, /**< Active self-test (ADC, comm audit, contactor check). */
    ANALYTICS_REC_STAGE_PROVISIONAL_DEGRADE= 4U, /**< Derated re-engagement dwell in DEGRADED mode. */
    ANALYTICS_REC_STAGE_NORMAL_RESTORE     = 5U, /**< Final deterministic transition back to NORMAL. */
    ANALYTICS_REC_STAGE_ABORTED            = 6U  /**< Recovery failed -> Escalated to SHUTDOWN. */
} AnalyticsRecoveryStage_t;

/* ========================================================================== */
/* COMPOSITE RISK TIERS & VECTORS                                             */
/* ========================================================================== */

typedef enum {
    RISK_TIER_LOW        = 0U, /**< 0% - 25%: Safe nominal envelope (Green #10B981) */
    RISK_TIER_MODERATE   = 1U, /**< 26% - 50%: Elevated stress, derating advised (Amber #F59E0B) */
    RISK_TIER_HIGH       = 2U, /**< 51% - 75%: Pre-trip hazard, active intervention (Orange #F97316) */
    RISK_TIER_CRITICAL   = 3U  /**< 76% - 100%: Imminent hazard / Failsafe active (Crimson #EF4444) */
} AnalyticsRiskTier_t;

/**
 * @brief Multi-Factor Composite Risk Score (CRS) Vector Structure.
 */
typedef struct {
    uint8_t             imbalance_factor_score;  /**< 0 - 100% based on cell delta & drift rate */
    uint8_t             fault_frequency_score;   /**< 0 - 100% based on fault recency & severity weight */
    uint8_t             soc_stress_score;        /**< 0 - 100% based on deep discharge/overcharge envelope */
    uint8_t             thermal_stress_score;    /**< 0 - 100% based on absolute temp & inter-cell gradient */
    uint8_t             contactor_wear_score;    /**< 0 - 100% based on cycle fatigue & feedback lag */
    uint8_t             composite_risk_score;    /**< Weighted composite score (0 - 100%) */
    AnalyticsRiskTier_t risk_tier;               /**< Categorical safety tier */
    int8_t              trend_direction;         /**< -1 = Improving, 0 = Steady, +1 = Escalating */
} CompositeRiskScore_t;

/* ========================================================================== */
/* INTELLIGENT OPERATOR RECOMMENDATION TAXONOMY                               */
/* ========================================================================== */

typedef enum {
    REC_PRIORITY_INFO      = 0U, /**< General operational status / healthy routine check. */
    REC_PRIORITY_ADVISORY  = 1U, /**< Mild optimization, preventive inspection recommended. */
    REC_PRIORITY_WARNING   = 2U, /**< Elevated risk, operator action required to avoid derating. */
    REC_PRIORITY_CRITICAL  = 3U  /**< Immediate action mandatory to protect safety isolation. */
} RecommendationPriority_t;

typedef enum {
    REC_CAT_CELL_BALANCING       = 0U, /**< Imbalance mitigation & cell health. */
    REC_CAT_THERMAL_REGULATION   = 1U, /**< Cooling loop, fan duct, gradient mitigation. */
    REC_CAT_CONTACTOR_MAINTENANCE= 2U, /**< Relay contacts, precharge resistor, cycle fatigue. */
    REC_CAT_SOC_CYCLING          = 3U, /**< Deep discharge / high voltage stress avoidance. */
    REC_CAT_COMM_HARDWARE        = 4U, /**< CAN bus busline, wiring harness, termination. */
    REC_CAT_SYSTEM_HEALTH        = 5U  /**< Overall supervisor & routine preventive care. */
} RecommendationCategory_t;

/**
 * @brief Actionable Operator Maintenance Suggestion.
 */
typedef struct {
    uint32_t                 recommendation_id;
    RecommendationPriority_t priority;
    RecommendationCategory_t category;
    char                     title[64];
    char                     diagnostic_evidence[160];
    char                     action_directive[160];
    uint32_t                 timestamp_ms;
    bool                     is_active;
} OperatorRecommendation_t;

/* ========================================================================== */
/* STRUCTURED FAULT AUDIT RECORD                                              */
/* ========================================================================== */

typedef struct {
    uint32_t                 fault_seq_id;
    AnalyticsFaultId_t       fault_id;
    AnalyticsFaultSource_t   source;
    AnalyticsFaultSeverity_t severity;
    uint32_t                 asserted_timestamp_ms;
    uint32_t                 cleared_timestamp_ms;
    bool                     is_active;
    bool                     is_latched;
    uint32_t                 occurrence_count;
    uint32_t                 aux_data_snapshot; /**< e.g., cell index or millivolt delta */
    char                     description[64];
} StructuredFaultEntry_t;

/* ========================================================================== */
/* STATE TRANSITION AUDIT LOG RECORD                                          */
/* ========================================================================== */

typedef struct {
    uint32_t                 transition_seq_id;
    uint32_t                 timestamp_ms;
    AnalyticsOperatingState_t from_state;
    AnalyticsOperatingState_t to_state;
    AnalyticsFaultId_t       trigger_fault_id;
    AnalyticsFaultSeverity_t severity;
    char                     reason[96];
} StateTransitionEntry_t;

/* ========================================================================== */
/* EXECUTIVE SUMMARY METRICS                                                  */
/* ========================================================================== */

typedef struct {
    float                    overall_soh_percent;        /**< State of Health (0.0% - 100.0%) */
    uint32_t                 system_uptime_seconds;      /**< Total continuous execution uptime */
    uint32_t                 total_cumulative_faults;    /**< Total lifetime faults logged */
    uint32_t                 active_fault_count;         /**< Currently active unresolved faults */
    AnalyticsOperatingState_t current_state;             /**< NORMAL, DEGRADED, FAILSAFE, SHUTDOWN */
    AnalyticsRecoveryStage_t active_recovery_stage;      /**< Current recovery stage (0..6) */
    uint8_t                  composite_risk_score;       /**< 0 - 100% */
    bool                     contactor_main_closed;      /**< Auxiliary feedback true=closed */
    bool                     contactor_precharge_closed; /**< Auxiliary precharge feedback */
    uint8_t                  active_recommendations;     /**< Count of active maintenance directives */
} ExecutiveSummary_t;

/* ========================================================================== */
/* BLYNK VIRTUAL PIN MAP DEFINITIONS                                          */
/* ========================================================================== */

#define BLYNK_VPIN_CELL_BASE           0U  /* V0..V11: Cell Voltages (mV) */
#define BLYNK_VPIN_PACK_VOLTAGE        12U /* V12: Pack Voltage (mV) */
#define BLYNK_VPIN_PACK_CURRENT        13U /* V13: Pack Current (mA) */
#define BLYNK_VPIN_PACK_TEMP           14U /* V14: Pack Avg Temperature (deg C) */
#define BLYNK_VPIN_MAX_IMBALANCE       15U /* V15: Max Cell Delta Imbalance (mV) */
#define BLYNK_VPIN_WEAKEST_CELL        16U /* V16: Weakest Cell Index (0..11) */
#define BLYNK_VPIN_STRONGEST_CELL      17U /* V17: Strongest Cell Index (0..11) */
#define BLYNK_VPIN_FSM_STATE           18U /* V18: FSM State (0..3) */
#define BLYNK_VPIN_ACTIVE_FAULT_ID     19U /* V19: Active Fault ID (Hex / Int) */
#define BLYNK_VPIN_FAULT_SEVERITY      20U /* V20: Severity Tier (0..3) */
#define BLYNK_VPIN_MAIN_RELAY_FB       21U /* V21: Main Contactor Feedback (0/1) */
#define BLYNK_VPIN_PRECHARGE_RELAY_FB  22U /* V22: Precharge Feedback (0/1) */
#define BLYNK_VPIN_COMPOSITE_RISK      23U /* V23: Composite Risk Score (0..100) */
#define BLYNK_VPIN_BATTERY_SOH         24U /* V24: Battery State of Health (%) */
#define BLYNK_VPIN_BATTERY_SOC         25U /* V25: Battery State of Charge (%) */
#define BLYNK_VPIN_RECOMMENDATION_SEV  26U /* V26: Highest Recommendation Priority */
#define BLYNK_VPIN_TOTAL_FAULTS        27U /* V27: Total Cumulative Fault Count */
#define BLYNK_VPIN_RECOVERY_STAGE      28U /* V28: Active Recovery Stage (0..6) */
#define BLYNK_VPIN_UPTIME_SECONDS      29U /* V29: Uptime in Seconds */
#define BLYNK_VPIN_RISK_IMBALANCE      30U /* V30: Risk Factor: Imbalance (%) */
#define BLYNK_VPIN_RISK_FAULT_RATE     31U /* V31: Risk Factor: Fault Rate (%) */
#define BLYNK_VPIN_RISK_SOC            32U /* V32: Risk Factor: SoC Stress (%) */
#define BLYNK_VPIN_RISK_THERMAL        33U /* V33: Risk Factor: Thermal Gradient (%) */
#define BLYNK_VPIN_DIRECTIVE_TEXT      34U /* V34: Primary Maintenance Directive Text */

#ifdef __cplusplus
}
#endif

#endif /* BLYNK_ANALYTICS_TYPES_H */
