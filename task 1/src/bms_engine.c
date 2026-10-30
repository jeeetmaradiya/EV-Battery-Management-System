/**
 * @file bms_engine.c
 * @brief Implementation of the Modular Battery Management System Engine.
 * 
 * Provides deterministic, real-time O(N) single-pass analysis, adaptive thresholding,
 * and temporal trend monitoring for an arbitrary number of cells defined at compile time.
 */

#include "bms_engine.h"
#include <stddef.h>

/* ========================================================================== */
/* PUBLIC API IMPLEMENTATION                                                  */
/* ========================================================================== */

Bms_Status_t Bms_Engine_Init(Bms_EngineState_t * const p_state)
{
    if (p_state == NULL) {
        return BMS_STATUS_ERROR_NULL_PTR;
    }

    p_state->filtered_imbalance_mv      = 0.0f;
    p_state->prev_filtered_imbalance_mv = 0.0f;
    p_state->imbalance_rate_mv_s        = 0.0f;
    p_state->current_trend              = BMS_TREND_INITIALIZING;
    p_state->candidate_trend            = BMS_TREND_INITIALIZING;
    p_state->persistence_counter        = 0U;
    p_state->cycle_counter              = 0U;
    p_state->is_initialized             = true;

    return BMS_STATUS_OK;
}

float Bms_Engine_CalculateAdaptiveThreshold(float soc_pct, float discharge_current_a)
{
    float base_threshold = BMS_IMBALANCE_BASE_THRESHOLD_MV;
    float soc_multiplier = 1.0f;

    /* Clamp SoC to valid range [0.0, 100.0] */
    if (soc_pct < 0.0f) {
        soc_pct = 0.0f;
    } else if (soc_pct > 100.0f) {
        soc_pct = 100.0f;
    }

    /* 
     * 1. SoC Non-Linear Knee Compensation:
     * Cells diverge naturally in terminal voltage near the low and high OCV knees
     * because dV/dSoC increases by an order of magnitude.
     */
    if (soc_pct < BMS_SOC_KNEE_LOW_PCT) {
        /* Normalized distance into the lower knee region [0.0 = at knee boundary, 1.0 = completely depleted] */
        float normalized_knee_depth = (BMS_SOC_KNEE_LOW_PCT - soc_pct) / BMS_SOC_KNEE_LOW_PCT;
        /* Quadratic expansion model provides smooth transition and asymptotic growth near 0% SoC */
        soc_multiplier += (BMS_SOC_KNEE_MAX_MULTIPLIER - 1.0f) * (normalized_knee_depth * normalized_knee_depth);
    } else if (soc_pct > BMS_SOC_KNEE_HIGH_PCT) {
        /* High SoC knee region: Normalized depth [0.0 = at knee boundary, 1.0 = 100% full] */
        float normalized_knee_depth = (soc_pct - BMS_SOC_KNEE_HIGH_PCT) / (100.0f - BMS_SOC_KNEE_HIGH_PCT);
        soc_multiplier += 1.0f * (normalized_knee_depth * normalized_knee_depth);
    }

    /*
     * 2. Dynamic Discharge Current (IR Drop) Compensation:
     * Terminal voltage V_term = OCV - I * R_int.
     * Manufacturing delta in internal resistance produces a load-proportional voltage split.
     * We only expand during discharge (current > 0). During charge (regen/plug-in),
     * current is negative and base balancing thresholds apply.
     */
    float dynamic_ir_expansion = 0.0f;
    if (discharge_current_a > 0.0f) {
        dynamic_ir_expansion = discharge_current_a * BMS_DYNAMIC_IR_COEFF_MV_PER_A;
    }

    float adaptive_threshold = (base_threshold * soc_multiplier) + dynamic_ir_expansion;

    /* Safety clamp: never allow threshold to grow unbounded, avoiding masking true short-circuits */
    if (adaptive_threshold > BMS_ADAPTIVE_THRESHOLD_MAX_CLAMP_MV) {
        adaptive_threshold = BMS_ADAPTIVE_THRESHOLD_MAX_CLAMP_MV;
    }

    return adaptive_threshold;
}

Bms_Status_t Bms_Engine_Process(
    Bms_EngineState_t * const p_state,
    const Bms_PackInputs_t * const p_inputs,
    Bms_AnalysisReport_t * const p_report)
{
    /* Parameter sanity verification */
    if ((p_state == NULL) || (p_inputs == NULL) || (p_report == NULL)) {
        return BMS_STATUS_ERROR_NULL_PTR;
    }

    if (!p_state->is_initialized) {
        return BMS_STATUS_ERROR_NOT_INIT;
    }

    uint16_t min_v = 0xFFFFU;
    uint16_t max_v = 0x0000U;
    uint8_t  min_idx = 0U;
    uint8_t  max_idx = 0U;
    uint32_t sum_v = 0U;
    uint32_t faults = BMS_FAULT_NONE;

    /*
     * STEP 1: Single-Pass O(N) Traversal of Cell Array
     * Computes weakest cell, strongest cell, pack sum, and checks limits simultaneously.
     */
    for (uint8_t i = 0U; i < BMS_CONFIG_CELL_COUNT; ++i) {
        uint16_t v = p_inputs->cell_voltages_mv[i];

        /* Sensor validity range check */
        if ((v < BMS_CELL_VOLTAGE_MIN_VALID_MV) || (v > BMS_CELL_VOLTAGE_MAX_VALID_MV)) {
            faults |= BMS_FAULT_SENSOR_OPEN_WIRE;
        }

        /* Overvoltage / Undervoltage safety trips */
        if (v >= BMS_CELL_OVERVOLTAGE_FAULT_MV) {
            faults |= BMS_FAULT_CELL_OVERVOLTAGE;
        }
        if (v <= BMS_CELL_UNDERVOLTAGE_FAULT_MV) {
            faults |= BMS_FAULT_CELL_UNDERVOLTAGE;
        }

        /* Extrema search */
        if (v < min_v) {
            min_v = v;
            min_idx = i;
        }
        if (v > max_v) {
            max_v = v;
            max_idx = i;
        }

        sum_v += v;
    }

    /* STEP 2: Imbalance & Statistical Calculations */
    uint16_t raw_imbalance = max_v - min_v;
    uint16_t avg_v = (uint16_t)(sum_v / BMS_CONFIG_CELL_COUNT);

    /* STEP 3: Adaptive Imbalance Threshold Evaluation */
    float adaptive_thresh = Bms_Engine_CalculateAdaptiveThreshold(
        p_inputs->pack_soc_pct,
        p_inputs->pack_current_a
    );

    /* STEP 4: Temporal Filtering & Imbalance Trend Detection */
    float effective_dt = (p_inputs->dt_seconds > 0.0001f) ? p_inputs->dt_seconds : 0.1f;
    float filtered_imbalance;
    float rate_mv_s;

    if (p_state->cycle_counter == 0U) {
        /* Cold start: initialize filter directly with first sample */
        filtered_imbalance = (float)raw_imbalance;
        p_state->prev_filtered_imbalance_mv = (float)raw_imbalance;
        rate_mv_s = 0.0f;
        p_state->current_trend = BMS_TREND_INITIALIZING;
        p_state->candidate_trend = BMS_TREND_INITIALIZING;
        p_state->persistence_counter = 0U;
    } else {
        /* Exponential Moving Average (EMA) low-pass filter */
        filtered_imbalance = (BMS_TREND_FILTER_ALPHA * (float)raw_imbalance) +
                             ((1.0f - BMS_TREND_FILTER_ALPHA) * p_state->filtered_imbalance_mv);

        /* Discrete time numerical derivative: d(delta V) / dt */
        rate_mv_s = (filtered_imbalance - p_state->prev_filtered_imbalance_mv) / effective_dt;
        p_state->prev_filtered_imbalance_mv = filtered_imbalance;

        /* Evaluate candidate trend with noise deadband */
        Bms_ImbalanceTrend_t candidate;
        if (rate_mv_s > BMS_TREND_RATE_DEADBAND_MV_S) {
            candidate = BMS_TREND_INCREASING;
        } else if (rate_mv_s < -BMS_TREND_RATE_DEADBAND_MV_S) {
            candidate = BMS_TREND_DECREASING;
        } else {
            candidate = BMS_TREND_STEADY;
        }

        /* Persistence filter to prevent chatter on noisy boundaries */
        if (candidate == p_state->candidate_trend) {
            if (p_state->persistence_counter < 0xFFFFU) {
                p_state->persistence_counter++;
            }
            if (p_state->persistence_counter >= BMS_TREND_PERSISTENCE_COUNT) {
                p_state->current_trend = candidate;
            }
        } else {
            p_state->candidate_trend = candidate;
            p_state->persistence_counter = 1U;
        }
    }

    /* Update internal state variables */
    p_state->filtered_imbalance_mv = filtered_imbalance;
    p_state->imbalance_rate_mv_s   = rate_mv_s;
    p_state->cycle_counter++;

    /* STEP 5: Imbalance Threshold Classification & Rapid Divergence Trip */
    if (filtered_imbalance > adaptive_thresh) {
        faults |= BMS_FAULT_IMBALANCE_WARNING;
        if (filtered_imbalance > (1.5f * adaptive_thresh)) {
            faults |= BMS_FAULT_IMBALANCE_CRITICAL;
        }
    }

    if ((rate_mv_s > (3.0f * BMS_TREND_RATE_DEADBAND_MV_S)) && (filtered_imbalance > (0.8f * adaptive_thresh))) {
        faults |= BMS_FAULT_RAPID_DIVERGENCE;
    }

    /* STEP 6: Balancing Recommendation Bitmask Generation */
    bms_cell_mask_t balancing_mask = 0U;
    /* 
     * Recommend balancing on cells that exceed the weakest cell by more than
     * the base imbalance threshold, provided we are in a safe balancing window.
     */
    uint16_t balancing_target_cutoff = min_v + (uint16_t)BMS_IMBALANCE_BASE_THRESHOLD_MV;
    for (uint8_t i = 0U; i < BMS_CONFIG_CELL_COUNT; ++i) {
        if (p_inputs->cell_voltages_mv[i] > balancing_target_cutoff) {
            balancing_mask |= (bms_cell_mask_t)(1U << i);
        }
    }

    /* STEP 7: Populate Output Analysis Report */
    p_report->weakest_cell_idx          = min_idx;
    p_report->strongest_cell_idx         = max_idx;
    p_report->weakest_cell_voltage_mv    = min_v;
    p_report->strongest_cell_voltage_mv  = max_v;
    p_report->total_pack_voltage_mv      = sum_v;
    p_report->average_cell_voltage_mv    = avg_v;
    p_report->raw_imbalance_mv           = raw_imbalance;
    p_report->filtered_imbalance_mv      = filtered_imbalance;
    p_report->adaptive_threshold_mv      = adaptive_thresh;
    p_report->imbalance_ratio            = (adaptive_thresh > 0.001f) ? (filtered_imbalance / adaptive_thresh) : 0.0f;
    p_report->trend_direction            = p_state->current_trend;
    p_report->imbalance_rate_mv_s        = rate_mv_s;
    p_report->balancing_mask             = balancing_mask;
    p_report->fault_flags                = faults;
    p_report->cycle_count                = p_state->cycle_counter;

    return BMS_STATUS_OK;
}

const char* Bms_Engine_TrendToString(Bms_ImbalanceTrend_t trend)
{
    switch (trend) {
        case BMS_TREND_INITIALIZING: return "INITIALIZING";
        case BMS_TREND_STEADY:       return "STEADY";
        case BMS_TREND_INCREASING:   return "INCREASING";
        case BMS_TREND_DECREASING:   return "DECREASING";
        default:                     return "UNKNOWN";
    }
}

uint16_t Bms_Engine_GetConfiguredCellCount(void)
{
    return (uint16_t)BMS_CONFIG_CELL_COUNT;
}
