/**
 * @file blynk_analytics_engine.h
 * @brief Public C API for Enterprise Blynk Analytics & Decision Engine.
 * 
 * Safety Critical Embedded System Architecture (ISO 26262 ASIL-C / MISRA-C:2012)
 */

#ifndef BLYNK_ANALYTICS_ENGINE_H
#define BLYNK_ANALYTICS_ENGINE_H

#include "blynk_analytics_types.h"
#include "blynk_analytics_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* ENGINE CONTEXT STRUCTURE                                                   */
/* ========================================================================== */

typedef struct {
    /* System Clocks & Uptime */
    uint32_t                 current_time_ms;
    uint32_t                 start_time_ms;
    uint32_t                 system_uptime_sec;
    uint32_t                 tick_counter;

    /* Live Cell Stack Telemetry */
    uint16_t                 cell_voltages_mv[BLYNK_ANALYTICS_CELL_COUNT];
    int16_t                  temperatures_c[BLYNK_ANALYTICS_TEMP_SENSOR_COUNT];
    uint32_t                 pack_voltage_mv;
    int32_t                  pack_current_ma;
    uint16_t                 min_cell_mv;
    uint16_t                 max_cell_mv;
    uint8_t                  weakest_cell_idx;
    uint8_t                  strongest_cell_idx;
    uint16_t                 cell_imbalance_delta_mv;
    float                    imbalance_drift_rate_mv_sec;

    /* Health Metrics */
    float                    state_of_charge_pct;
    float                    state_of_health_pct;

    /* Contactor Auxiliary Telemetry */
    bool                     commanded_main_contactor;
    bool                     commanded_precharge_contactor;
    bool                     feedback_main_contactor;
    bool                     feedback_precharge_contactor;
    uint32_t                 contactor_cycle_count;
    uint32_t                 contactor_transition_timestamp_ms;

    /* Task 4 Operating State Machine & Recovery */
    AnalyticsOperatingState_t operating_state;
    AnalyticsRecoveryStage_t  recovery_stage;
    uint32_t                 stage_entered_timestamp_ms;
    uint32_t                 recovery_retry_counter;
    uint32_t                 clearance_sample_count;

    /* Calculated Composite Risk Breakdown */
    CompositeRiskScore_t     risk_analysis;
    uint8_t                  previous_risk_score;

    /* Structured Fault Registry */
    StructuredFaultEntry_t   fault_registry[STRUCTURED_FAULT_REGISTRY_CAPACITY];
    uint32_t                 fault_registry_count;
    uint32_t                 total_cumulative_faults;
    uint32_t                 active_fault_count;

    /* State Transition Audit Trail */
    StateTransitionEntry_t   transition_log[STATE_TRANSITION_LOG_CAPACITY];
    uint32_t                 transition_log_count;
    uint32_t                 transition_seq_counter;

    /* Intelligent Operator Recommendations */
    OperatorRecommendation_t recommendations[OPERATOR_RECOMMENDATION_CAPACITY];
    uint32_t                 recommendation_count;
    uint32_t                 recommendation_seq_counter;

    /* Blynk Virtual Pin Bus Mirror */
    uint32_t                 vpins[35];
    char                     primary_directive_text[128];
} AnalyticsEngine_t;

/* ========================================================================== */
/* PUBLIC API INTERFACE                                                       */
/* ========================================================================== */

/**
 * @brief Initialize the Analytics & Decision Engine to pristine nominal state.
 */
void blynk_analytics_init(AnalyticsEngine_t *engine);

/**
 * @brief Ingest high-resolution physical sensor telemetry.
 */
void blynk_analytics_update_telemetry(
    AnalyticsEngine_t *engine,
    const uint16_t     cell_voltages_mv[BLYNK_ANALYTICS_CELL_COUNT],
    const int16_t      temperatures_c[BLYNK_ANALYTICS_TEMP_SENSOR_COUNT],
    int32_t            pack_current_ma,
    uint32_t           timestamp_ms
);

/**
 * @brief Perform multi-factor mathematical risk calculation.
 */
void blynk_analytics_evaluate_risk(AnalyticsEngine_t *engine);

/**
 * @brief Synthesize live telemetry and risk into human-readable operator recommendations.
 */
void blynk_analytics_generate_recommendations(AnalyticsEngine_t *engine);

/**
 * @brief Execute one cycle of the deterministic Task 4 Operating State Machine.
 */
void blynk_analytics_process_fsm(AnalyticsEngine_t *engine);

/**
 * @brief Initiate deterministic 5-stage structured recovery out of FAILSAFE.
 */
bool blynk_analytics_start_recovery(AnalyticsEngine_t *engine);

/**
 * @brief Inject a diagnostic fault into the structured fault registry.
 */
bool blynk_analytics_inject_fault(
    AnalyticsEngine_t         *engine,
    AnalyticsFaultId_t         fault_id,
    AnalyticsFaultSource_t     source,
    AnalyticsFaultSeverity_t   severity,
    uint32_t                   aux_metric,
    const char                *description
);

/**
 * @brief Clear an active fault condition from the registry.
 */
bool blynk_analytics_clear_fault(AnalyticsEngine_t *engine, AnalyticsFaultId_t fault_id);

/**
 * @brief Execute authorized operator service reset to exit SHUTDOWN.
 */
bool blynk_analytics_service_reset(AnalyticsEngine_t *engine);

/**
 * @brief Populate Executive Summary structure with live metrics.
 */
void blynk_analytics_get_executive_summary(
    const AnalyticsEngine_t *engine,
    ExecutiveSummary_t      *summary
);

/**
 * @brief Update and synchronize all Blynk Virtual Pins (V0..V34).
 */
void blynk_analytics_sync_vpins(AnalyticsEngine_t *engine);

#ifdef __cplusplus
}
#endif

#endif /* BLYNK_ANALYTICS_ENGINE_H */
