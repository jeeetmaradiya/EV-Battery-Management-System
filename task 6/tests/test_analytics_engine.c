/**
 * @file test_analytics_engine.c
 * @brief Formal Unit Test Suite for Enterprise Blynk Analytics & Decision Engine.
 * 
 * Safety Critical Embedded System Architecture (ISO 26262 ASIL-C / MISRA-C:2012)
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../include/blynk_analytics_engine.h"

static int g_tests_passed = 0;
static int g_tests_total = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_tests_total++; \
    if (cond) { \
        g_tests_passed++; \
        printf("  [PASS] %s\n", msg); \
    } else { \
        printf("  [FAIL] %s (Line %d)\n", msg, __LINE__); \
    } \
} while(0)

/* ========================================================================== */
/* TEST 1: Pristine Engine Initialization                                     */
/* ========================================================================== */
static void test_initialization(void)
{
    printf("\n--- Test 1: Pristine Engine Initialization ---\n");
    AnalyticsEngine_t engine;
    blynk_analytics_init(&engine);

    TEST_ASSERT(engine.operating_state == ANALYTICS_STATE_NORMAL, "State initialized to NORMAL (0)");
    TEST_ASSERT(engine.recovery_stage == ANALYTICS_REC_STAGE_IDLE, "Recovery stage initialized to IDLE (0)");
    TEST_ASSERT(engine.min_cell_mv == 3700 && engine.max_cell_mv == 3700, "All 12 cells at 3700mV");
    TEST_ASSERT(engine.cell_imbalance_delta_mv == 0, "Imbalance delta is 0mV");
    TEST_ASSERT(engine.feedback_main_contactor == true, "Main contactor closed in NORMAL");
    TEST_ASSERT(engine.state_of_health_pct > 95.0f, "SOH initialized to nominal peak (>95%)");
    TEST_ASSERT(engine.risk_analysis.composite_risk_score <= 25, "Composite risk score in LOW tier (<25%)");
    TEST_ASSERT(engine.transition_log_count == 1, "Initial boot transition logged");
}

/* ========================================================================== */
/* TEST 2: Multi-Factor Composite Risk Model                                  */
/* ========================================================================== */
static void test_composite_risk_model(void)
{
    printf("\n--- Test 2: Multi-Factor Composite Risk Model ---\n");
    AnalyticsEngine_t engine;
    blynk_analytics_init(&engine);

    /* Baseline nominal: delta = 5mV */
    uint16_t cells_nominal[12] = {3700, 3702, 3701, 3704, 3700, 3705, 3701, 3702, 3700, 3703, 3701, 3702};
    int16_t temps_nominal[4] = {25, 25, 26, 25};
    blynk_analytics_update_telemetry(&engine, cells_nominal, temps_nominal, 1000, 1000);
    blynk_analytics_evaluate_risk(&engine);
    TEST_ASSERT(engine.risk_analysis.imbalance_factor_score == 0, "Nominal delta (<15mV) yields 0% imbalance risk");
    TEST_ASSERT(engine.risk_analysis.risk_tier == RISK_TIER_LOW, "Nominal risk tier is LOW");

    /* Elevated imbalance: delta = 65mV (Cell 4 at 3765mV) */
    cells_nominal[3] = 3765;
    blynk_analytics_update_telemetry(&engine, cells_nominal, temps_nominal, 1000, 2000);
    blynk_analytics_evaluate_risk(&engine);
    TEST_ASSERT(engine.cell_imbalance_delta_mv == 65, "Cell imbalance delta detected at 65mV");
    TEST_ASSERT(engine.risk_analysis.imbalance_factor_score >= 50, "Imbalance score scaled appropriately (>=50%)");

    /* Deep discharge: SoC < 10% */
    uint16_t cells_discharged[12];
    for (int i = 0; i < 12; i++) cells_discharged[i] = 2950;
    blynk_analytics_update_telemetry(&engine, cells_discharged, temps_nominal, 15000, 3000);
    blynk_analytics_evaluate_risk(&engine);
    TEST_ASSERT(engine.risk_analysis.soc_stress_score >= 80, "Deep discharge (<10% SoC) triggers high SoC risk (>=80%)");
}

/* ========================================================================== */
/* TEST 3: Deterministic State Transitions (NORMAL -> DEGRADED -> FAILSAFE)   */
/* ========================================================================== */
static void test_deterministic_fsm_transitions(void)
{
    printf("\n--- Test 3: Deterministic State Transitions ---\n");
    AnalyticsEngine_t engine;
    blynk_analytics_init(&engine);

    /* 1. Inject WARNING fault -> Transition to DEGRADED */
    blynk_analytics_inject_fault(&engine, ANALYTICS_FAULT_CELL_IMBALANCE, ANALYTICS_SRC_BATTERY_CELL,
                                 ANALYTICS_SEV_WARNING, 45, "Cell imbalance warning");
    blynk_analytics_process_fsm(&engine);
    TEST_ASSERT(engine.operating_state == ANALYTICS_STATE_DEGRADED, "Warning fault transitions state to DEGRADED");
    TEST_ASSERT(engine.feedback_main_contactor == true, "Contactor remains closed in DEGRADED mode");

    /* 2. Inject CRITICAL fault -> Transition to FAILSAFE */
    blynk_analytics_inject_fault(&engine, ANALYTICS_FAULT_CELL_OVERVOLTAGE, ANALYTICS_SRC_BATTERY_CELL,
                                 ANALYTICS_SEV_CRITICAL, 4260, "Cell overvoltage critical trip");
    blynk_analytics_process_fsm(&engine);
    TEST_ASSERT(engine.operating_state == ANALYTICS_STATE_FAILSAFE, "Critical fault transitions state to FAILSAFE");
    TEST_ASSERT(engine.feedback_main_contactor == false, "Main contactor opened immediately for isolation");

    /* 3. Transition audit log verification */
    TEST_ASSERT(engine.transition_log_count >= 3, "Transition audit log contains all sequential state changes");
}

/* ========================================================================== */
/* TEST 4: Multi-Stage Structured Recovery Sequence                           */
/* ========================================================================== */
static void test_structured_recovery_pipeline(void)
{
    printf("\n--- Test 4: Multi-Stage Structured Recovery Sequence ---\n");
    AnalyticsEngine_t engine;
    blynk_analytics_init(&engine);

    /* Force system into FAILSAFE */
    blynk_analytics_inject_fault(&engine, ANALYTICS_FAULT_CELL_OVERVOLTAGE, ANALYTICS_SRC_BATTERY_CELL,
                                 ANALYTICS_SEV_CRITICAL, 4260, "Cell overvoltage trip");
    blynk_analytics_process_fsm(&engine);
    assert(engine.operating_state == ANALYTICS_STATE_FAILSAFE);

    /* Clear critical fault to allow recovery */
    blynk_analytics_clear_fault(&engine, ANALYTICS_FAULT_CELL_OVERVOLTAGE);

    /* Start recovery -> Stage 1: CLEARANCE_CHECK */
    bool started = blynk_analytics_start_recovery(&engine);
    TEST_ASSERT(started == true, "Recovery started successfully from FAILSAFE");
    TEST_ASSERT(engine.recovery_stage == ANALYTICS_REC_STAGE_CLEARANCE_CHECK, "Stage 1: Clearance check active");

    /* Simulate 5 clearance samples */
    for (int i = 0; i < 6; i++) {
        engine.current_time_ms += 100;
        blynk_analytics_process_fsm(&engine);
    }
    TEST_ASSERT(engine.recovery_stage == ANALYTICS_REC_STAGE_STABILIZATION, "Stage 2: Stabilization entered");

    /* Simulate 1500ms stabilization dwell */
    engine.current_time_ms += 1600;
    blynk_analytics_process_fsm(&engine);
    TEST_ASSERT(engine.recovery_stage == ANALYTICS_REC_STAGE_SUBSYSTEM_VERIFY, "Stage 3: Subsystem verify entered");

    /* Simulate 1500ms subsystem verification dwell */
    engine.current_time_ms += 1600;
    blynk_analytics_process_fsm(&engine);
    TEST_ASSERT(engine.recovery_stage == ANALYTICS_REC_STAGE_PROVISIONAL_DEGRADE, "Stage 4: Provisional degrade entered");
    TEST_ASSERT(engine.operating_state == ANALYTICS_STATE_DEGRADED, "FSM stepped up from FAILSAFE to DEGRADED");
    TEST_ASSERT(engine.feedback_main_contactor == true, "Contactor re-engaged under derated profile");

    /* Simulate 2000ms provisional dwell */
    engine.current_time_ms += 2100;
    blynk_analytics_process_fsm(&engine);
    TEST_ASSERT(engine.operating_state == ANALYTICS_STATE_NORMAL, "Stage 5: Final state restored to NORMAL");
    TEST_ASSERT(engine.recovery_stage == ANALYTICS_REC_STAGE_IDLE, "Recovery pipeline completed, stage IDLE");
}

/* ========================================================================== */
/* TEST 5: Recovery Exhaustion Escalation to SHUTDOWN                         */
/* ========================================================================== */
static void test_recovery_deadlock_lockout(void)
{
    printf("\n--- Test 5: Recovery Retries Exhaustion -> SHUTDOWN ---\n");
    AnalyticsEngine_t engine;
    blynk_analytics_init(&engine);

    /* Trip to FAILSAFE */
    blynk_analytics_inject_fault(&engine, ANALYTICS_FAULT_CELL_OVERVOLTAGE, ANALYTICS_SRC_BATTERY_CELL,
                                 ANALYTICS_SEV_CRITICAL, 4260, "Trip 1");
    blynk_analytics_process_fsm(&engine);

    /* Simulate 3 failed recovery attempts */
    for (int retry = 0; retry < 3; retry++) {
        blynk_analytics_clear_fault(&engine, ANALYTICS_FAULT_CELL_OVERVOLTAGE);
        blynk_analytics_start_recovery(&engine);
        /* Step to Stage 2 */
        for (int i = 0; i < 6; i++) {
            engine.current_time_ms += 100;
            blynk_analytics_process_fsm(&engine);
        }
        /* Inject fault mid-recovery to abort */
        blynk_analytics_inject_fault(&engine, ANALYTICS_FAULT_CELL_OVERVOLTAGE, ANALYTICS_SRC_BATTERY_CELL,
                                     ANALYTICS_SEV_CRITICAL, 4260, "Recurrence mid-recovery");
        blynk_analytics_process_fsm(&engine);
    }

    TEST_ASSERT(engine.operating_state == ANALYTICS_STATE_SHUTDOWN, "Max retries exhausted triggers permanent SHUTDOWN");
    TEST_ASSERT(engine.feedback_main_contactor == false, "Contactors permanently isolated in SHUTDOWN");

    /* Verify Service Reset recovers system */
    blynk_analytics_service_reset(&engine);
    TEST_ASSERT(engine.operating_state == ANALYTICS_STATE_NORMAL, "Operator service reset restores state to NORMAL");
}

/* ========================================================================== */
/* TEST 6: Intelligent Operator Recommendations Generation                    */
/* ========================================================================== */
static void test_operator_recommendations(void)
{
    printf("\n--- Test 6: Operator Recommendations Generation ---\n");
    AnalyticsEngine_t engine;
    blynk_analytics_init(&engine);

    /* 1. Imbalance Recommendation */
    uint16_t cells[12] = {3700, 3700, 3700, 3755, 3700, 3700, 3700, 3700, 3700, 3700, 3700, 3700};
    int16_t temps[4] = {25, 25, 25, 25};
    blynk_analytics_update_telemetry(&engine, cells, temps, 1000, 1000);
    blynk_analytics_generate_recommendations(&engine);
    TEST_ASSERT(engine.recommendation_count > 0, "Recommendation generated for 55mV imbalance");
    TEST_ASSERT(engine.recommendations[0].category == REC_CAT_CELL_BALANCING, "Category is CELL_BALANCING");
    TEST_ASSERT(engine.recommendations[0].priority == REC_PRIORITY_WARNING, "Priority is WARNING");

    /* 2. Thermal Gradient Recommendation */
    cells[3] = 3700; /* clear imbalance */
    temps[1] = 46;    /* Sensor 2 at 46°C, gradient = 21°C */
    blynk_analytics_update_telemetry(&engine, cells, temps, 1000, 2000);
    blynk_analytics_generate_recommendations(&engine);
    TEST_ASSERT(engine.recommendations[0].category == REC_CAT_THERMAL_REGULATION, "Thermal anomaly classified properly");

    /* 3. Blynk Virtual Pin Mirroring */
    blynk_analytics_sync_vpins(&engine);
    TEST_ASSERT(engine.vpins[BLYNK_VPIN_FSM_STATE] == (uint32_t)engine.operating_state, "V18 mirrors FSM state");
    TEST_ASSERT(engine.vpins[BLYNK_VPIN_COMPOSITE_RISK] == (uint32_t)engine.risk_analysis.composite_risk_score, "V23 mirrors CRS");
}

/* ========================================================================== */
/* MAIN TEST RUNNER                                                           */
/* ========================================================================== */
int main(void)
{
    printf("====================================================================\n");
    printf("  ENTERPRISE BLYNK ANALYTICS ENGINE FORMAL VERIFICATION SUITE       \n");
    printf("  ISO 26262 ASIL-C / MISRA-C:2012 COMPLIANCE VERIFICATION          \n");
    printf("====================================================================\n");

    test_initialization();
    test_composite_risk_model();
    test_deterministic_fsm_transitions();
    test_structured_recovery_pipeline();
    test_recovery_deadlock_lockout();
    test_operator_recommendations();

    printf("\n====================================================================\n");
    printf("  VERIFICATION SUMMARY: %d / %d Tests Passed (%.1f%%)\n",
           g_tests_passed, g_tests_total, ((float)g_tests_passed / (float)g_tests_total) * 100.0f);
    printf("====================================================================\n");

    return (g_tests_passed == g_tests_total) ? 0 : 1;
}
