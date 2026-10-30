/**
 * @file bms_engine.h
 * @brief Public API Interface for the Modular Battery Management System Engine.
 * 
 * Provides a clean, modular, and decoupled abstraction for battery cell analysis.
 * Downstream firmware modules (active/passive balancing, safety contactors,
 * telemetry, thermal management) interface exclusively through this API.
 */

#ifndef BMS_ENGINE_H
#define BMS_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "bms_types.h"

/* ========================================================================== */
/* PUBLIC API FUNCTION DECLARATIONS                                           */
/* ========================================================================== */

/**
 * @brief Initializes or resets the BMS Engine persistent state.
 * 
 * Sets up internal low-pass filters, cycle counters, and state machines.
 * Must be invoked once prior to calling Bms_Engine_Process().
 * 
 * @param[out] p_state Pointer to caller-allocated engine state container.
 * @return Bms_Status_t BMS_STATUS_OK on success, or error code.
 */
Bms_Status_t Bms_Engine_Init(Bms_EngineState_t * const p_state);

/**
 * @brief Core single-pass execution engine for cell analysis.
 * 
 * Performs:
 *  1. Boundary and sensor sanity checks.
 *  2. O(N) single-pass scan for weakest (min) and strongest (max) cells and pack sum.
 *  3. Calculation of raw and filtered voltage imbalances.
 *  4. Evaluation of adaptive imbalance thresholds (SoC non-linear knee + C-rate IR compensation).
 *  5. Temporal trend analysis (derivative filter, deadband, persistence counter).
 *  6. Fault flag evaluation and balancing bitmask generation.
 * 
 * @param[in,out] p_state  Pointer to persistent engine state.
 * @param[in]     p_inputs Pointer to current sensory snapshot.
 * @param[out]    p_report Pointer to output report populated by the engine.
 * @return Bms_Status_t BMS_STATUS_OK on success, or error code.
 */
Bms_Status_t Bms_Engine_Process(
    Bms_EngineState_t * const p_state,
    const Bms_PackInputs_t * const p_inputs,
    Bms_AnalysisReport_t * const p_report
);

/**
 * @brief Standalone calculation of the adaptive imbalance threshold.
 * 
 * Exposes the threshold model so external diagnostic or sizing tools
 * can evaluate the expected threshold for arbitrary SoC and load conditions.
 * 
 * @param[in] soc_pct             State of charge in percent (0.0 to 100.0).
 * @param[in] discharge_current_a Instantaneous pack current (A, > 0 = discharge).
 * @return float Calculated threshold in millivolts.
 */
float Bms_Engine_CalculateAdaptiveThreshold(float soc_pct, float discharge_current_a);

/**
 * @brief Helper utility to convert trend enum into human-readable string.
 * 
 * @param[in] trend Enum value.
 * @return const char* String description ("STEADY", "INCREASING", etc.)
 */
const char* Bms_Engine_TrendToString(Bms_ImbalanceTrend_t trend);

/**
 * @brief Returns the compile-time configured cell count.
 * 
 * @return uint16_t Configured cell count.
 */
uint16_t Bms_Engine_GetConfiguredCellCount(void);

#ifdef __cplusplus
}
#endif

#endif /* BMS_ENGINE_H */
