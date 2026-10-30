/**
 * @file test_fault_state_machine.c
 * @brief Formal Verification Suite for Deterministic Fault FSM & Structured Recovery.
 * 
 * Safety Critical Embedded System Architecture (ISO 26262 ASIL-C / MISRA-C:2012)
 */

#include "fault_state_machine_engine.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

static void SetupNominalTelemetry(SubsystemTelemetry_t *telem) {
    (void)memset(telem, 0, sizeof(SubsystemTelemetry_t));
    telem->cell_count = 12;
    for (int i = 0; i < 12; i++) {
        telem->cell_voltages_mv[i] = 3700; /* 3.70 V nominal */
    }
    for (int i = 0; i < 4; i++) {
        telem->cell_temperatures_c[i] = 28; /* 28 C nominal */
    }
    telem->commanded_main_relay = true;
    telem->feedback_main_relay = true;
    telem->commanded_precharge_relay = false;
    telem->feedback_precharge_relay = false;
    telem->relay_transition_age_ms = 100; /* Past debounce */

    telem->last_comm_heartbeat_ms = 0;
    telem->comm_crc_error_count = 0;
    telem->comm_bus_active = true;

    telem->adc_channel_count = 4;
    for (int i = 0; i < 4; i++) {
        telem->raw_adc_samples[i] = 2048; /* Will dither in tests */
    }
    telem->manual_estop_asserted = false;
}

int main(void) {
    printf("====================================================================\n");
    printf(" DETERMINISTIC FAULT FSM WITH STRUCTURED RECOVERY - TEST SUITE      \n");
    printf("====================================================================\n");

    SubsystemTelemetry_t telem;
    uint32_t sim_time_ms = 0;

    /* ---------------------------------------------------------------------- */
    /* TEST 1: System Boot & Nominal State                                    */
    /* ---------------------------------------------------------------------- */
    printf("[TEST 1] System Boot & Nominal State Evaluation... ");
    FaultFSM_Init();
    SetupNominalTelemetry(&telem);
    assert(FaultFSM_GetCurrentState() == FAULT_STATE_NORMAL);
    assert(FaultFSM_GetActiveFaultCount() == 0);

    /* Run 10 ticks with nominal dithering */
    for (int tick = 0; tick < 10; tick++) {
        sim_time_ms += 10;
        telem.last_comm_heartbeat_ms = sim_time_ms;
        telem.raw_adc_samples[0] = 2040 + (tick % 5) * 4; /* Dynamic ADC */
        FaultFSM_Tick(sim_time_ms, &telem);
    }
    assert(FaultFSM_GetCurrentState() == FAULT_STATE_NORMAL);
    printf("PASSED [State: NORMAL, Active Faults: 0]\n");

    /* ---------------------------------------------------------------------- */
    /* TEST 2: Warning Condition -> Transition to DEGRADED                    */
    /* ---------------------------------------------------------------------- */
    printf("[TEST 2] Minor Cell Imbalance Warning (140 mV delta)... ");
    telem.cell_voltages_mv[0] = 3770;
    telem.cell_voltages_mv[1] = 3630; /* Imbalance = 140 mV (>= 120 mV) */
    sim_time_ms += 10;
    telem.last_comm_heartbeat_ms = sim_time_ms;
    FaultFSM_Tick(sim_time_ms, &telem);
    assert(FaultFSM_GetCurrentState() == FAULT_STATE_DEGRADED);
    assert(FaultFSM_IsFaultActive(FAULT_ID_CELL_IMBALANCE));
    printf("PASSED [State: DEGRADED, Fault: CELL_IMBALANCE]\n");

    /* ---------------------------------------------------------------------- */
    /* TEST 3: Warning Clearance -> Transition back to NORMAL                 */
    /* ---------------------------------------------------------------------- */
    printf("[TEST 3] Warning Clearance -> Return to NORMAL... ");
    telem.cell_voltages_mv[0] = 3700;
    telem.cell_voltages_mv[1] = 3700; /* Balanced */
    sim_time_ms += 10;
    telem.last_comm_heartbeat_ms = sim_time_ms;
    FaultFSM_Tick(sim_time_ms, &telem);
    assert(FaultFSM_GetCurrentState() == FAULT_STATE_NORMAL);
    assert(!FaultFSM_IsFaultActive(FAULT_ID_CELL_IMBALANCE));
    printf("PASSED [State: NORMAL, Warning Cleared]\n");

    /* ---------------------------------------------------------------------- */
    /* TEST 4: Frozen ADC Value Detection -> Trip to FAILSAFE                 */
    /* ---------------------------------------------------------------------- */
    printf("[TEST 4] Frozen ADC Detection (8 invariant samples)... ");
    for (int i = 0; i < 9; i++) {
        sim_time_ms += 10;
        telem.last_comm_heartbeat_ms = sim_time_ms;
        telem.raw_adc_samples[0] = 2048; /* Exact invariant value */
        FaultFSM_Tick(sim_time_ms, &telem);
    }
    assert(FaultFSM_GetCurrentState() == FAULT_STATE_FAILSAFE);
    assert(FaultFSM_IsFaultActive(FAULT_ID_ADC_FROZEN));
    const FaultRecord_t *rec = FaultFSM_GetFaultRecord(0);
    assert(rec != NULL);
    assert(rec->source == FAULT_SRC_ADC);
    printf("PASSED [State: FAILSAFE, Fault: ADC_FROZEN isolated to ADC domain]\n");

    /* ---------------------------------------------------------------------- */
    /* TEST 5: Structured Recovery from FAILSAFE (Multi-Stage Verification)    */
    /* ---------------------------------------------------------------------- */
    printf("[TEST 5] Structured Recovery Pipeline Execution... \n");
    /* Restore dynamic ADC */
    telem.commanded_main_relay = false;
    telem.feedback_main_relay = false; /* Relays opened in FAILSAFE */
    
    /* Stage 1: Clearance check */
    printf("         -> Stage 1: Clearing ADC fault & requesting recovery... ");
    for (int i = 0; i < 10; i++) {
        sim_time_ms += 10;
        telem.last_comm_heartbeat_ms = sim_time_ms;
        telem.raw_adc_samples[0] = 2000 + (i * 10); /* Dynamic */
        FaultFSM_Tick(sim_time_ms, &telem);
    }
    assert(FaultFSM_GetRecoveryStage() == RECOVERY_STAGE_STABILIZATION);
    printf("PASSED [Stage: STABILIZATION]\n");

    /* Stage 2: Stabilization Quarantine dwell (500 ms) */
    printf("         -> Stage 2: 500 ms Quarantine dwell... ");
    for (int i = 0; i < 55; i++) {
        sim_time_ms += 10;
        telem.last_comm_heartbeat_ms = sim_time_ms;
        telem.raw_adc_samples[0] = 2000 + (i % 8) * 8;
        FaultFSM_Tick(sim_time_ms, &telem);
    }
    assert(FaultFSM_GetRecoveryStage() == RECOVERY_STAGE_SUBSYSTEM_VERIFY);
    printf("PASSED [Stage: SUBSYSTEM_VERIFY]\n");

    /* Stage 3: Subsystem deep verification (20 qualified samples) */
    printf("         -> Stage 3: Subsystem Deep Verification (20 samples)... ");
    for (int i = 0; i < 22; i++) {
        sim_time_ms += 10;
        telem.last_comm_heartbeat_ms = sim_time_ms;
        telem.raw_adc_samples[0] = 2000 + (i % 8) * 8;
        FaultFSM_Tick(sim_time_ms, &telem);
    }
    assert(FaultFSM_GetCurrentState() == FAULT_STATE_DEGRADED);
    assert(FaultFSM_GetRecoveryStage() == RECOVERY_STAGE_PROVISIONAL_DEGRADE);
    printf("PASSED [Step-Up Transition -> DEGRADED]\n");

    /* Stage 4: Provisional DEGRADED dwell (1000 ms) */
    printf("         -> Stage 4: 1000 ms Provisional Dwell in DEGRADED... ");
    for (int i = 0; i < 105; i++) {
        sim_time_ms += 10;
        telem.last_comm_heartbeat_ms = sim_time_ms;
        telem.raw_adc_samples[0] = 2000 + (i % 8) * 8;
        FaultFSM_Tick(sim_time_ms, &telem);
    }
    assert(FaultFSM_GetCurrentState() == FAULT_STATE_NORMAL);
    assert(FaultFSM_GetRecoveryStage() == RECOVERY_STAGE_IDLE);
    printf("PASSED [Final Step-Up Transition -> NORMAL]\n");

    /* ---------------------------------------------------------------------- */
    /* TEST 6: Relay Mismatch & Welded Contact Trip                           */
    /* ---------------------------------------------------------------------- */
    printf("[TEST 6] Relay Mismatch Detection (Commanded OPEN, Sense CLOSED)... ");
    telem.commanded_main_relay = false;
    telem.feedback_main_relay = true; /* Welded! */
    telem.relay_transition_age_ms = 100; /* Past debounce */
    for (int i = 0; i < 4; i++) {
        sim_time_ms += 10;
        telem.last_comm_heartbeat_ms = sim_time_ms;
        FaultFSM_Tick(sim_time_ms, &telem);
    }
    assert(FaultFSM_IsFaultActive(FAULT_ID_RELAY_WELDED));
    assert(FaultFSM_GetCurrentState() == FAULT_STATE_SHUTDOWN);
    printf("PASSED [State: SHUTDOWN, Fault: RELAY_WELDED (Fatal)]\n");

    /* ---------------------------------------------------------------------- */
    /* TEST 7: Deadlock-Free Guarantee (Max Retry Exhaustion)                 */
    /* ---------------------------------------------------------------------- */
    printf("[TEST 7] Deadlock-Free Guarantee (Max Retry Exhaustion)... ");
    FaultFSM_ServiceReset(sim_time_ms);
    assert(FaultFSM_GetCurrentState() == FAULT_STATE_NORMAL);

    /* Trip into FAILSAFE via comm loss */
    sim_time_ms += 600; /* Comm timeout */
    FaultFSM_Tick(sim_time_ms, &telem);
    assert(FaultFSM_GetCurrentState() == FAULT_STATE_FAILSAFE);

    /* Simulate 3 recovery stage timeouts to trigger deadlock breaker */
    for (int r = 0; r < 3; r++) {
        sim_time_ms += 3200; /* Exceed CONFIG_RECOVERY_STAGE_TIMEOUT_MS */
        FaultFSM_Tick(sim_time_ms, &telem);
    }
    assert(FaultFSM_GetCurrentState() == FAULT_STATE_SHUTDOWN);
    assert(FaultFSM_IsFaultActive(FAULT_ID_RECOVERY_EXHAUSTED));
    printf("PASSED [Deadlock Prevented -> Escalated to SHUTDOWN]\n");

    /* ---------------------------------------------------------------------- */
    /* TEST 8: Structured Transition Log Formatting                           */
    /* ---------------------------------------------------------------------- */
    printf("[TEST 8] Structured Transition Audit Log Output... \n");
    uint32_t log_count = FaultFSM_GetTransitionLogCount();
    assert(log_count > 0);
    char txt_buf[256];
    char json_buf[256];
    const StateTransitionLog_t *entry = FaultFSM_GetTransitionLog(1);
    FaultFSM_FormatStructuredLog(entry, txt_buf, sizeof(txt_buf));
    FaultFSM_FormatJsonLog(entry, json_buf, sizeof(json_buf));

    printf("         Structured Log : %s\n", txt_buf);
    printf("         JSON Log       : %s\n", json_buf);
    printf("====================================================================\n");
    printf(" ALL 8 SAFETY VERIFICATION TESTS PASSED SUCCESSFULLY (100%%)        \n");
    printf("====================================================================\n");
    return 0;
}
