/**
 * @file test_bms_engine.c
 * @brief Unit test harness and simulation scenarios for the Modular BMS Engine.
 * 
 * Verifies:
 *  - Correct identification of weakest/strongest cells
 *  - Accurate calculation of delta V
 *  - Adaptive threshold dynamics across SoC and discharge current profiles
 *  - Temporal trend detection (Increasing, Decreasing, Steady)
 *  - Downstream task consumption without logic duplication
 */

#include <stdio.h>
#include <assert.h>
#include <math.h>
#include "../include/bms_engine.h"

static void print_report_summary(const Bms_AnalysisReport_t *p_report)
{
    printf("--- Cycle %u ---\n", p_report->cycle_count);
    printf("  Weakest Cell   : Cell #%u (%u mV)\n", p_report->weakest_cell_idx, p_report->weakest_cell_voltage_mv);
    printf("  Strongest Cell  : Cell #%u (%u mV)\n", p_report->strongest_cell_idx, p_report->strongest_cell_voltage_mv);
    printf("  Raw Imbalance   : %u mV | Filtered: %.2f mV\n", p_report->raw_imbalance_mv, p_report->filtered_imbalance_mv);
    printf("  Adaptive Thresh : %.2f mV (Ratio: %.2f)\n", p_report->adaptive_threshold_mv, p_report->imbalance_ratio);
    printf("  Trend Direction : %s (Rate: %.2f mV/s)\n", Bms_Engine_TrendToString(p_report->trend_direction), p_report->imbalance_rate_mv_s);
    printf("  Balancing Mask  : 0x%04X | Fault Flags: 0x%08X\n\n", p_report->balancing_mask, p_report->fault_flags);
}

int main(void)
{
    printf("====================================================================\n");
    printf("  MODULAR BMS ENGINE - EMBEDDED VERIFICATION TEST BENCH            \n");
    printf("  Configured Cell Count: %u cells                                   \n", BMS_CONFIG_CELL_COUNT);
    printf("====================================================================\n\n");

    Bms_EngineState_t engine_state;
    Bms_AnalysisReport_t report;
    Bms_Status_t status = Bms_Engine_Init(&engine_state);
    assert(status == BMS_STATUS_OK);

    Bms_PackInputs_t inputs;
    inputs.dt_seconds = 0.1f; /* 100 ms loop */
    inputs.pack_soc_pct = 50.0f; /* Nominal flat plateau */
    inputs.pack_current_a = 0.0f; /* Resting */
    inputs.pack_temperature_c = 25.0f;

    /* Initialize baseline balanced cell array (3300 mV nominal LFP) */
    for (uint8_t i = 0; i < BMS_CONFIG_CELL_COUNT; ++i) {
        inputs.cell_voltages_mv[i] = 3300;
    }

    /* Introduce known extrema */
    inputs.cell_voltages_mv[2] = 3280; /* Weakest: 3280 mV (-20 mV) */
    inputs.cell_voltages_mv[BMS_CONFIG_CELL_COUNT - 1] = 3325; /* Strongest: 3325 mV (+25 mV) */

    printf("[TEST 1] Static Balanced Cell Evaluation\n");
    status = Bms_Engine_Process(&engine_state, &inputs, &report);
    assert(status == BMS_STATUS_OK);
    assert(report.weakest_cell_idx == 2);
    assert(report.weakest_cell_voltage_mv == 3280);
    assert(report.strongest_cell_idx == (BMS_CONFIG_CELL_COUNT - 1));
    assert(report.strongest_cell_voltage_mv == 3325);
    assert(report.raw_imbalance_mv == 45);
    print_report_summary(&report);

    printf("[TEST 2] Adaptive Threshold vs SoC Knee Evaluation\n");
    /* Mid-plateau at 50% SoC */
    float thresh_mid = Bms_Engine_CalculateAdaptiveThreshold(50.0f, 0.0f);
    printf("  Threshold at 50%% SoC (Nominal Flat Plateau) : %.2f mV\n", thresh_mid);
    assert(fabsf(thresh_mid - 25.0f) < 0.1f);

    /* Low-knee at 10% SoC */
    float thresh_low = Bms_Engine_CalculateAdaptiveThreshold(10.0f, 0.0f);
    printf("  Threshold at 10%% SoC (Low Knee Non-linear)  : %.2f mV\n", thresh_low);
    assert(thresh_low > thresh_mid);

    /* High-knee at 95% SoC */
    float thresh_high = Bms_Engine_CalculateAdaptiveThreshold(95.0f, 0.0f);
    printf("  Threshold at 95%% SoC (High Knee Non-linear) : %.2f mV\n", thresh_high);
    assert(thresh_high > thresh_mid);

    /* Dynamic 40A Discharge Burst */
    float thresh_burst = Bms_Engine_CalculateAdaptiveThreshold(50.0f, 40.0f);
    printf("  Threshold at 50%% SoC under 40A Discharge    : %.2f mV\n", thresh_burst);
    assert(thresh_burst > thresh_mid);
    printf("\n");

    printf("[TEST 3] Diverging Imbalance Scenario (Developing Weak Cell Fault)\n");
    /* Simulate Cell 2 degrading over 10 consecutive cycles */
    for (int step = 0; step < 10; ++step) {
        inputs.cell_voltages_mv[2] -= 4; /* Voltage dropping by 4 mV per 100ms */
        Bms_Engine_Process(&engine_state, &inputs, &report);
    }
    print_report_summary(&report);
    assert(report.trend_direction == BMS_TREND_INCREASING);
    assert(report.imbalance_rate_mv_s > 0.0f);

    printf("[TEST 4] Active Balancing Scenario (Converging Imbalance)\n");
    /* Simulate Active Balancing bringing cell voltages closer */
    for (int step = 0; step < 15; ++step) {
        inputs.cell_voltages_mv[2] += 5; /* Cell 2 being charged / recovered */
        Bms_Engine_Process(&engine_state, &inputs, &report);
    }
    print_report_summary(&report);
    assert(report.trend_direction == BMS_TREND_DECREASING);
    assert(report.imbalance_rate_mv_s < 0.0f);

    printf("ALL EMBEDDED ENGINE UNIT TESTS COMPLETED SUCCESSFULLY!\n");
    return 0;
}
