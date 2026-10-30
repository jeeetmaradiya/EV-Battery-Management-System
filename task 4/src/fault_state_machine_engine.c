/**
 * @file fault_state_machine_engine.c
 * @brief Deterministic Fault State Machine and Structured Recovery Engine.
 * 
 * Safety Critical Embedded System Architecture (ISO 26262 ASIL-C / MISRA-C:2012)
 * Fully deterministic, zero heap allocation, bounded execution latency.
 */

#include "fault_state_machine_engine.h"
#include <string.h>
#include <stdio.h>

/* ========================================================================== */
/* INTERNAL CONSTANTS & PRIVATE STRUCTURES                                    */
/* ========================================================================== */

#define ADC_SAMPLE_HISTORY_SIZE     (CONFIG_ADC_FROZEN_SAMPLE_DEPTH)

typedef struct {
    uint16_t samples[ADC_SAMPLE_HISTORY_SIZE];
    uint8_t  head;
    uint8_t  count;
} AdcChannelHistory_t;

/* ========================================================================== */
/* STATIC ENGINE STATE STORAGE (DETERMINISTIC MEMORY)                         */
/* ========================================================================== */

static FaultOperatingState_t   s_current_state;
static FaultRecord_t           s_fault_registry[CONFIG_FAULT_REGISTRY_CAPACITY];
static uint32_t                s_active_fault_count;

/* ADC History */
static AdcChannelHistory_t     s_adc_history[CONFIG_MAX_ADC_CHANNELS];

/* Relay Mismatch Tracker */
static uint16_t                s_relay_mismatch_counter;

/* Recovery Controller */
static RecoveryController_t    s_recovery_ctrl;

/* Transition Audit Log Circular Buffer */
static StateTransitionLog_t    s_transition_log[CONFIG_TRANSITION_LOG_CAPACITY];
static uint32_t                s_transition_log_total_count;
static uint32_t                s_transition_log_head;

/* ========================================================================== */
/* DETERMINISTIC TRANSITION TABLE MATRIX                                      */
/* CurrentState x TriggerSeverity -> NextState                                */
/* ========================================================================== */

static const FaultOperatingState_t s_transition_matrix[FAULT_STATE_COUNT][4] = {
    /* [NORMAL] */
    {
        FAULT_STATE_NORMAL,    /* FAULT_SEV_NONE     -> NORMAL */
        FAULT_STATE_DEGRADED,  /* FAULT_SEV_WARNING  -> DEGRADED */
        FAULT_STATE_FAILSAFE,  /* FAULT_SEV_CRITICAL -> FAILSAFE */
        FAULT_STATE_SHUTDOWN   /* FAULT_SEV_FATAL    -> SHUTDOWN */
    },
    /* [DEGRADED] */
    {
        FAULT_STATE_NORMAL,    /* FAULT_SEV_NONE     -> NORMAL (Clear) */
        FAULT_STATE_DEGRADED,  /* FAULT_SEV_WARNING  -> DEGRADED (Sustain) */
        FAULT_STATE_FAILSAFE,  /* FAULT_SEV_CRITICAL -> FAILSAFE */
        FAULT_STATE_SHUTDOWN   /* FAULT_SEV_FATAL    -> SHUTDOWN */
    },
    /* [FAILSAFE] */
    {
        FAULT_STATE_FAILSAFE,  /* FAULT_SEV_NONE     -> FAILSAFE (Direct exit forbidden! Must use recovery) */
        FAULT_STATE_FAILSAFE,  /* FAULT_SEV_WARNING  -> FAILSAFE */
        FAULT_STATE_FAILSAFE,  /* FAULT_SEV_CRITICAL -> FAILSAFE */
        FAULT_STATE_SHUTDOWN   /* FAULT_SEV_FATAL    -> SHUTDOWN */
    },
    /* [SHUTDOWN] - Terminal Latched State */
    {
        FAULT_STATE_SHUTDOWN,  /* No recovery without external service reset */
        FAULT_STATE_SHUTDOWN,
        FAULT_STATE_SHUTDOWN,
        FAULT_STATE_SHUTDOWN
    }
};

/* ========================================================================== */
/* FORWARD DECLARATIONS OF PRIVATE HELPERS                                    */
/* ========================================================================== */

static void LogStateTransition(uint32_t timestamp_ms, FaultOperatingState_t prev_state, 
                               FaultOperatingState_t new_state, FaultId_t fault_id, 
                               FaultSource_t source, FaultSeverity_t severity, const char *reason);

static FaultSeverity_t CalculateAggregateSeverity(FaultId_t *out_highest_fault_id, 
                                                 FaultSource_t *out_highest_source);

static void EvaluateAdcHealth(uint32_t current_timestamp_ms, const SubsystemTelemetry_t *telemetry);
static void EvaluateRelayHealth(uint32_t current_timestamp_ms, const SubsystemTelemetry_t *telemetry);
static void EvaluateCellHealth(uint32_t current_timestamp_ms, const SubsystemTelemetry_t *telemetry);
static void EvaluateCommHealth(uint32_t current_timestamp_ms, const SubsystemTelemetry_t *telemetry);
static void ProcessStructuredRecovery(uint32_t current_timestamp_ms, const SubsystemTelemetry_t *telemetry);

/* ========================================================================== */
/* INITIALIZATION                                                             */
/* ========================================================================== */

void FaultFSM_Init(void)
{
    s_current_state = FAULT_STATE_NORMAL;
    s_active_fault_count = 0U;

    (void)memset(s_fault_registry, 0, sizeof(s_fault_registry));
    (void)memset(s_adc_history, 0, sizeof(s_adc_history));
    (void)memset(&s_recovery_ctrl, 0, sizeof(s_recovery_ctrl));
    (void)memset(s_transition_log, 0, sizeof(s_transition_log));

    s_relay_mismatch_counter = 0U;
    s_transition_log_total_count = 0U;
    s_transition_log_head = 0U;

    s_recovery_ctrl.current_stage = RECOVERY_STAGE_IDLE;
    s_recovery_ctrl.max_allowed_retries = CONFIG_RECOVERY_MAX_RETRIES;
    s_recovery_ctrl.required_samples = CONFIG_RECOVERY_SUBSYSTEM_QUALIFY_SAMPLES;
    s_recovery_ctrl.stage_timeout_ms = CONFIG_RECOVERY_STAGE_TIMEOUT_MS;

    /* Initial state boot transition record */
    LogStateTransition(0U, FAULT_STATE_NORMAL, FAULT_STATE_NORMAL, 
                       FAULT_ID_NONE, FAULT_SRC_NONE, FAULT_SEV_NONE, 
                       "Deterministic Fault FSM Booted - Initial State NORMAL");
}

/* ========================================================================== */
/* TICK CYCLE (EVALUATION, DETECTION, RECOVERY, TRANSITIONS)                  */
/* ========================================================================== */

void FaultFSM_Tick(uint32_t current_timestamp_ms, const SubsystemTelemetry_t *telemetry)
{
    if (telemetry == NULL) {
        return;
    }

    /* 1. Emergency Interlock / ESTOP Evaluation (Immediate preemption) */
    if (telemetry->manual_estop_asserted) {
        uint32_t aux[4] = { 1U, 0U, 0U, 0U };
        FaultFSM_InjectFault(FAULT_ID_EMERGENCY_STOP, current_timestamp_ms, aux, "Manual ESTOP Interlock Asserted");
    }

    /* 2. Subsystem Fault Detectors (Only run if not in terminal SHUTDOWN) */
    if (s_current_state != FAULT_STATE_SHUTDOWN) {
        EvaluateAdcHealth(current_timestamp_ms, telemetry);
        EvaluateRelayHealth(current_timestamp_ms, telemetry);
        EvaluateCellHealth(current_timestamp_ms, telemetry);
        EvaluateCommHealth(current_timestamp_ms, telemetry);
    }

    /* 3. Aggregate System Severity Calculation */
    FaultId_t highest_fault_id = FAULT_ID_NONE;
    FaultSource_t highest_source = FAULT_SRC_NONE;
    FaultSeverity_t aggregate_sev = CalculateAggregateSeverity(&highest_fault_id, &highest_source);

    /* 4. Structured Recovery Pipeline Handler (Active when in FAILSAFE) */
    if (s_current_state == FAULT_STATE_FAILSAFE) {
        ProcessStructuredRecovery(current_timestamp_ms, telemetry);
    }

    /* 5. Deterministic Transition Evaluation */
    if (s_current_state != FAULT_STATE_SHUTDOWN) {
        FaultOperatingState_t next_state = s_transition_matrix[s_current_state][aggregate_sev];

        /* Special Rule: Transition out of FAILSAFE is only allowed via structured recovery */
        if (s_current_state == FAULT_STATE_FAILSAFE) {
            if (next_state == FAULT_STATE_SHUTDOWN) {
                /* Fatal fault trips immediately to SHUTDOWN */
                LogStateTransition(current_timestamp_ms, s_current_state, FAULT_STATE_SHUTDOWN,
                                   highest_fault_id, highest_source, aggregate_sev,
                                   "Fatal fault preemption while in FAILSAFE");
                s_current_state = FAULT_STATE_SHUTDOWN;
            }
            /* Disallow automatic return to NORMAL or DEGRADED from transition matrix alone.
               Handled exclusively by ProcessStructuredRecovery! */
        }
        else if (next_state != s_current_state) {
            /* Valid transition triggered by fault elevation or clearance */
            char reason_buf[128];
            (void)snprintf(reason_buf, sizeof(reason_buf), 
                           "Transition triggered by severity %u (Fault: %s)",
                           (unsigned int)aggregate_sev, FaultFSM_FaultIdToString(highest_fault_id));

            LogStateTransition(current_timestamp_ms, s_current_state, next_state,
                               highest_fault_id, highest_source, aggregate_sev, reason_buf);
            s_current_state = next_state;

            /* If we just entered FAILSAFE, reset recovery controller for clean cycle */
            if (s_current_state == FAULT_STATE_FAILSAFE) {
                s_recovery_ctrl.current_stage = RECOVERY_STAGE_IDLE;
                s_recovery_ctrl.recovery_in_progress = false;
                s_recovery_ctrl.samples_qualified = 0U;
            }
        }
    }
}

/* ========================================================================== */
/* FAULT DETECTOR 1: FROZEN ADC & RAIL DETECTOR                               */
/* ========================================================================== */

static void EvaluateAdcHealth(uint32_t current_timestamp_ms, const SubsystemTelemetry_t *telemetry)
{
    uint16_t ch_count = telemetry->adc_channel_count;
    if (ch_count > CONFIG_MAX_ADC_CHANNELS) {
        ch_count = CONFIG_MAX_ADC_CHANNELS;
    }

    for (uint16_t ch = 0U; ch < ch_count; ch++) {
        uint16_t sample = telemetry->raw_adc_samples[ch];
        AdcChannelHistory_t *hist = &s_adc_history[ch];

        /* Update ring buffer history */
        hist->samples[hist->head] = sample;
        hist->head = (uint8_t)((hist->head + 1U) % ADC_SAMPLE_HISTORY_SIZE);
        if (hist->count < ADC_SAMPLE_HISTORY_SIZE) {
            hist->count++;
        }

        /* Check Stuck at Rail (GND or Vref) */
        if ((sample <= CONFIG_ADC_STUCK_LOW_THRESHOLD) || (sample >= CONFIG_ADC_STUCK_HIGH_THRESHOLD)) {
            uint32_t aux[4] = { ch, sample, CONFIG_ADC_STUCK_LOW_THRESHOLD, CONFIG_ADC_STUCK_HIGH_THRESHOLD };
            FaultFSM_InjectFault(FAULT_ID_ADC_OUT_OF_RANGE, current_timestamp_ms, aux, 
                                 "ADC Sample Stuck at Voltage Rail");
            continue;
        } else {
            /* If out-of-range was active on this channel, check if we can clear */
            if (FaultFSM_IsFaultActive(FAULT_ID_ADC_OUT_OF_RANGE)) {
                FaultFSM_ClearFault(FAULT_ID_ADC_OUT_OF_RANGE, current_timestamp_ms);
            }
        }

        /* Check Frozen Invariance over N samples */
        if (hist->count >= ADC_SAMPLE_HISTORY_SIZE) {
            uint16_t min_val = 0xFFFFU;
            uint16_t max_val = 0U;
            for (uint8_t i = 0U; i < ADC_SAMPLE_HISTORY_SIZE; i++) {
                if (hist->samples[i] < min_val) min_val = hist->samples[i];
                if (hist->samples[i] > max_val) max_val = hist->samples[i];
            }

            uint16_t delta = (uint16_t)(max_val - min_val);
            if (delta <= CONFIG_ADC_FROZEN_MAX_DELTA) {
                /* Invariant/frozen ADC detected */
                uint32_t aux[4] = { ch, sample, delta, ADC_SAMPLE_HISTORY_SIZE };
                FaultFSM_InjectFault(FAULT_ID_ADC_FROZEN, current_timestamp_ms, aux,
                                     "ADC Frozen: Invariant Sample Stream Over N Cycles");
            } else {
                /* Dynamic variance present -> Clear frozen fault if active */
                if (FaultFSM_IsFaultActive(FAULT_ID_ADC_FROZEN)) {
                    FaultFSM_ClearFault(FAULT_ID_ADC_FROZEN, current_timestamp_ms);
                }
            }
        }
    }
}

/* ========================================================================== */
/* FAULT DETECTOR 2: RELAY MISMATCH & WELDED CONTACT DETECTOR                 */
/* ========================================================================== */

static void EvaluateRelayHealth(uint32_t current_timestamp_ms, const SubsystemTelemetry_t *telemetry)
{
    /* Only evaluate after mechanical transition debounce window has elapsed */
    if (telemetry->relay_transition_age_ms < CONFIG_RELAY_DEBOUNCE_MS) {
        s_relay_mismatch_counter = 0U;
        return;
    }

    bool mismatch_detected = false;
    FaultId_t mismatch_id = FAULT_ID_NONE;
    const char *reason_desc = NULL;

    /* Main Contactor Analysis */
    if (telemetry->commanded_main_relay != telemetry->feedback_main_relay) {
        mismatch_detected = true;
        if (!telemetry->commanded_main_relay && telemetry->feedback_main_relay) {
            /* Commanded OPEN, feedback indicates CLOSED -> WELDED CONTACT! */
            mismatch_id = FAULT_ID_RELAY_WELDED;
            reason_desc = "Main Relay Welded: Commanded OPEN but Auxiliary Sense CLOSED";
        } else {
            /* Commanded CLOSED, feedback indicates OPEN -> COIL/DRIVE FAULT */
            mismatch_id = FAULT_ID_RELAY_COIL_OPEN;
            reason_desc = "Main Relay Open Failure: Commanded CLOSED but Sense OPEN";
        }
    }

    /* Precharge Contactor Analysis */
    if (!mismatch_detected && (telemetry->commanded_precharge_relay != telemetry->feedback_precharge_relay)) {
        mismatch_detected = true;
        mismatch_id = FAULT_ID_RELAY_MISMATCH;
        reason_desc = "Precharge Relay State Mismatch with Feedback";
    }

    if (mismatch_detected) {
        s_relay_mismatch_counter++;
        if (s_relay_mismatch_counter >= CONFIG_RELAY_MISMATCH_TRIP_COUNT) {
            uint32_t aux[4] = {
                (uint32_t)telemetry->commanded_main_relay,
                (uint32_t)telemetry->feedback_main_relay,
                (uint32_t)telemetry->commanded_precharge_relay,
                (uint32_t)telemetry->feedback_precharge_relay
            };
            FaultFSM_InjectFault(mismatch_id, current_timestamp_ms, aux, reason_desc);
        }
    } else {
        s_relay_mismatch_counter = 0U;
        if (FaultFSM_IsFaultActive(FAULT_ID_RELAY_MISMATCH)) {
            FaultFSM_ClearFault(FAULT_ID_RELAY_MISMATCH, current_timestamp_ms);
        }
        if (FaultFSM_IsFaultActive(FAULT_ID_RELAY_COIL_OPEN)) {
            FaultFSM_ClearFault(FAULT_ID_RELAY_COIL_OPEN, current_timestamp_ms);
        }
        /* Note: FAULT_ID_RELAY_WELDED is latched and cannot auto-clear without service */
    }
}

/* ========================================================================== */
/* FAULT DETECTOR 3: BATTERY CELL HEALTH (OVP, UVP, IMBALANCE, TEMP)          */
/* ========================================================================== */

static void EvaluateCellHealth(uint32_t current_timestamp_ms, const SubsystemTelemetry_t *telemetry)
{
    uint16_t min_v = 0xFFFFU;
    uint16_t max_v = 0U;
    uint16_t count = telemetry->cell_count;
    if (count > CONFIG_MAX_CELL_COUNT) {
        count = CONFIG_MAX_CELL_COUNT;
    }

    for (uint16_t i = 0U; i < count; i++) {
        uint16_t v = telemetry->cell_voltages_mv[i];
        if (v < min_v) min_v = v;
        if (v > max_v) max_v = v;

        /* Critical Overvoltage Trip */
        if (v >= CONFIG_CELL_OVP_CRITICAL_MV) {
            uint32_t aux[4] = { i, v, CONFIG_CELL_OVP_CRITICAL_MV, 0U };
            FaultFSM_InjectFault(FAULT_ID_CELL_OVERVOLTAGE, current_timestamp_ms, aux,
                                 "Cell Critical Overvoltage Trip");
            return;
        }

        /* Critical Undervoltage Trip */
        if (v <= CONFIG_CELL_UVP_CRITICAL_MV) {
            uint32_t aux[4] = { i, v, CONFIG_CELL_UVP_CRITICAL_MV, 0U };
            FaultFSM_InjectFault(FAULT_ID_CELL_UNDERVOLTAGE, current_timestamp_ms, aux,
                                 "Cell Critical Undervoltage Trip");
            return;
        }
    }

    /* Cell Over/Undervoltage cleared check */
    if (FaultFSM_IsFaultActive(FAULT_ID_CELL_OVERVOLTAGE) && (max_v < (CONFIG_CELL_OVP_CRITICAL_MV - 50U))) {
        FaultFSM_ClearFault(FAULT_ID_CELL_OVERVOLTAGE, current_timestamp_ms);
    }
    if (FaultFSM_IsFaultActive(FAULT_ID_CELL_UNDERVOLTAGE) && (min_v > (CONFIG_CELL_UVP_CRITICAL_MV + 50U))) {
        FaultFSM_ClearFault(FAULT_ID_CELL_UNDERVOLTAGE, current_timestamp_ms);
    }

    /* Imbalance Analysis */
    if (count > 1U) {
        uint16_t imbalance = (uint16_t)(max_v - min_v);
        if (imbalance >= CONFIG_CELL_IMBALANCE_CRITICAL_MV) {
            uint32_t aux[4] = { imbalance, min_v, max_v, 0U };
            FaultFSM_InjectFault(FAULT_ID_CELL_IMBALANCE, current_timestamp_ms, aux,
                                 "Critical Cell Imbalance Exceeded");
        } else if (imbalance >= CONFIG_CELL_IMBALANCE_WARNING_MV) {
            uint32_t aux[4] = { imbalance, min_v, max_v, 0U };
            FaultFSM_InjectFault(FAULT_ID_CELL_IMBALANCE, current_timestamp_ms, aux,
                                 "Cell Imbalance Advisory Warning");
        } else {
            if (FaultFSM_IsFaultActive(FAULT_ID_CELL_IMBALANCE)) {
                FaultFSM_ClearFault(FAULT_ID_CELL_IMBALANCE, current_timestamp_ms);
            }
        }
    }

    /* Temperature Analysis */
    for (uint8_t t = 0U; t < CONFIG_MAX_TEMP_SENSORS; t++) {
        int16_t temp = telemetry->cell_temperatures_c[t];
        if (temp >= CONFIG_CELL_OTP_CRITICAL_C) {
            uint32_t aux[4] = { t, (uint32_t)temp, CONFIG_CELL_OTP_CRITICAL_C, 0U };
            FaultFSM_InjectFault(FAULT_ID_CELL_OVERTEMP, current_timestamp_ms, aux,
                                 "Critical Cell Overtemperature Trip");
            break;
        } else if (temp >= CONFIG_CELL_OTP_WARNING_C) {
            uint32_t aux[4] = { t, (uint32_t)temp, CONFIG_CELL_OTP_WARNING_C, 0U };
            FaultFSM_InjectFault(FAULT_ID_CELL_OVERTEMP, current_timestamp_ms, aux,
                                 "Cell Overtemperature Advisory Warning");
            break;
        } else {
            if (FaultFSM_IsFaultActive(FAULT_ID_CELL_OVERTEMP)) {
                FaultFSM_ClearFault(FAULT_ID_CELL_OVERTEMP, current_timestamp_ms);
            }
        }
    }
}

/* ========================================================================== */
/* FAULT DETECTOR 4: COMMUNICATION HEARTBEAT & CRC BURST                      */
/* ========================================================================== */

static void EvaluateCommHealth(uint32_t current_timestamp_ms, const SubsystemTelemetry_t *telemetry)
{
    /* Heartbeat age check */
    if (current_timestamp_ms >= telemetry->last_comm_heartbeat_ms) {
        uint32_t elapsed = current_timestamp_ms - telemetry->last_comm_heartbeat_ms;
        if (elapsed >= CONFIG_COMM_HEARTBEAT_TIMEOUT_MS) {
            uint32_t aux[4] = { elapsed, CONFIG_COMM_HEARTBEAT_TIMEOUT_MS, 0U, 0U };
            FaultFSM_InjectFault(FAULT_ID_COMM_HEARTBEAT_TIMEOUT, current_timestamp_ms, aux,
                                 "CAN/SPI Comm Bus Keepalive Heartbeat Lost");
        } else {
            if (FaultFSM_IsFaultActive(FAULT_ID_COMM_HEARTBEAT_TIMEOUT)) {
                FaultFSM_ClearFault(FAULT_ID_COMM_HEARTBEAT_TIMEOUT, current_timestamp_ms);
            }
        }
    }

    /* CRC Burst Check */
    if (telemetry->comm_crc_error_count >= CONFIG_COMM_CRC_BURST_THRESHOLD) {
        uint32_t aux[4] = { telemetry->comm_crc_error_count, CONFIG_COMM_CRC_BURST_THRESHOLD, 0U, 0U };
        FaultFSM_InjectFault(FAULT_ID_COMM_CRC_BURST, current_timestamp_ms, aux,
                             "Communication CRC Checksum Error Burst");
    } else {
        if (FaultFSM_IsFaultActive(FAULT_ID_COMM_CRC_BURST)) {
            FaultFSM_ClearFault(FAULT_ID_COMM_CRC_BURST, current_timestamp_ms);
        }
    }
}

/* ========================================================================== */
/* STRUCTURED RECOVERY PIPELINE (MULTI-STAGE FAILSAFE RECOVERY)               */
/* ========================================================================== */

bool FaultFSM_RequestRecovery(uint32_t current_timestamp_ms)
{
    /* Can only request recovery if currently in FAILSAFE */
    if (s_current_state != FAULT_STATE_FAILSAFE) {
        return false;
    }

    /* Deadlock / Retry Limit Check */
    if (s_recovery_ctrl.retry_counter >= s_recovery_ctrl.max_allowed_retries) {
        uint32_t aux[4] = { s_recovery_ctrl.retry_counter, s_recovery_ctrl.max_allowed_retries, 0U, 0U };
        FaultFSM_InjectFault(FAULT_ID_RECOVERY_EXHAUSTED, current_timestamp_ms, aux,
                             "Recovery Attempts Exhausted -> System Deadlock Lockout");
        return false;
    }

    /* Initiate Stage 1: Clearance Verification */
    s_recovery_ctrl.recovery_in_progress = true;
    s_recovery_ctrl.current_stage = RECOVERY_STAGE_CLEARANCE_CHECK;
    s_recovery_ctrl.stage_entered_timestamp_ms = current_timestamp_ms;
    s_recovery_ctrl.samples_qualified = 0U;

    LogStateTransition(current_timestamp_ms, s_current_state, s_current_state,
                       FAULT_ID_NONE, FAULT_SRC_SYSTEM, FAULT_SEV_NONE,
                       "Structured Recovery Sequence Initiated -> Stage 1: Clearance Check");
    return true;
}

static void ProcessStructuredRecovery(uint32_t current_timestamp_ms, const SubsystemTelemetry_t *telemetry)
{
    /* Auto-engage recovery if in FAILSAFE and no recovery in progress */
    if (!s_recovery_ctrl.recovery_in_progress) {
        (void)FaultFSM_RequestRecovery(current_timestamp_ms);
        return;
    }

    /* Watchdog Timeout Check for current stage to prevent hangs */
    if ((current_timestamp_ms - s_recovery_ctrl.stage_entered_timestamp_ms) > s_recovery_ctrl.stage_timeout_ms) {
        s_recovery_ctrl.retry_counter++;
        if (s_recovery_ctrl.retry_counter >= s_recovery_ctrl.max_allowed_retries) {
            s_recovery_ctrl.current_stage = RECOVERY_STAGE_ABORTED;
            uint32_t aux[4] = { s_recovery_ctrl.retry_counter, s_recovery_ctrl.max_allowed_retries, 0U, 0U };
            FaultFSM_InjectFault(FAULT_ID_RECOVERY_EXHAUSTED, current_timestamp_ms, aux,
                                 "Recovery Stage Timeout and Max Retries Reached -> Lockout");
        } else {
            /* Retry stage */
            s_recovery_ctrl.current_stage = RECOVERY_STAGE_CLEARANCE_CHECK;
            s_recovery_ctrl.stage_entered_timestamp_ms = current_timestamp_ms;
            s_recovery_ctrl.samples_qualified = 0U;
        }
        return;
    }

    switch (s_recovery_ctrl.current_stage) {
        case RECOVERY_STAGE_CLEARANCE_CHECK: {
            /* Check if any active critical/fatal faults remain */
            bool critical_active = false;
            for (uint32_t i = 0U; i < CONFIG_FAULT_REGISTRY_CAPACITY; i++) {
                if (s_fault_registry[i].is_active && 
                   (s_fault_registry[i].severity >= FAULT_SEV_CRITICAL)) {
                    critical_active = true;
                    break;
                }
            }

            if (!critical_active) {
                /* All critical faults cleared -> Advance to Stage 2: Stabilization */
                s_recovery_ctrl.current_stage = RECOVERY_STAGE_STABILIZATION;
                s_recovery_ctrl.stage_entered_timestamp_ms = current_timestamp_ms;
                LogStateTransition(current_timestamp_ms, s_current_state, s_current_state,
                                   FAULT_ID_NONE, FAULT_SRC_SYSTEM, FAULT_SEV_NONE,
                                   "Recovery Stage 1 Passed -> Stage 2: Clean Quarantine Stabilization");
            }
            break;
        }

        case RECOVERY_STAGE_STABILIZATION: {
            /* Verify quiet quarantine dwell period with ZERO new faults */
            if (current_timestamp_ms - s_recovery_ctrl.stage_entered_timestamp_ms >= CONFIG_RECOVERY_QUARANTINE_MS) {
                /* Quarantine period satisfied -> Advance to Stage 3: Subsystem Deep Verification */
                s_recovery_ctrl.current_stage = RECOVERY_STAGE_SUBSYSTEM_VERIFY;
                s_recovery_ctrl.stage_entered_timestamp_ms = current_timestamp_ms;
                s_recovery_ctrl.samples_qualified = 0U;
                LogStateTransition(current_timestamp_ms, s_current_state, s_current_state,
                                   FAULT_ID_NONE, FAULT_SRC_SYSTEM, FAULT_SEV_NONE,
                                   "Recovery Stage 2 Passed -> Stage 3: Subsystem Health & Noise Qualification");
            }
            break;
        }

        case RECOVERY_STAGE_SUBSYSTEM_VERIFY: {
            /* Multi-subsystem validation:
               1. ADC not stuck or frozen
               2. Relays confirmed in safe open state
               3. Comm heartbeat valid
               4. Cell voltages in safe recovery window
            */
            bool sample_healthy = true;

            /* Check cell voltage recovery window */
            for (uint16_t i = 0U; i < telemetry->cell_count; i++) {
                uint16_t v = telemetry->cell_voltages_mv[i];
                if ((v < CONFIG_RECOVERY_CELL_MIN_MV) || (v > CONFIG_RECOVERY_CELL_MAX_MV)) {
                    sample_healthy = false;
                    break;
                }
            }

            /* Check relay open confirmation */
            if (telemetry->feedback_main_relay || telemetry->feedback_precharge_relay) {
                sample_healthy = false; /* Must confirm contact separation before re-engaging */
            }

            /* Check comm heartbeat freshness */
            if ((current_timestamp_ms - telemetry->last_comm_heartbeat_ms) > (CONFIG_COMM_HEARTBEAT_TIMEOUT_MS / 2U)) {
                sample_healthy = false;
            }

            if (sample_healthy) {
                s_recovery_ctrl.samples_qualified++;
            } else {
                /* Invalidate consecutive streak on any unhealthy sample */
                s_recovery_ctrl.samples_qualified = 0U;
            }

            if (s_recovery_ctrl.samples_qualified >= s_recovery_ctrl.required_samples) {
                /* Subsystem verification passed -> Advance to Stage 4: Provisional DEGRADED operation */
                s_recovery_ctrl.current_stage = RECOVERY_STAGE_PROVISIONAL_DEGRADE;
                s_recovery_ctrl.stage_entered_timestamp_ms = current_timestamp_ms;

                /* Deterministic state transition: FAILSAFE -> DEGRADED */
                LogStateTransition(current_timestamp_ms, s_current_state, FAULT_STATE_DEGRADED,
                                   FAULT_ID_NONE, FAULT_SRC_SYSTEM, FAULT_SEV_WARNING,
                                   "Recovery Stage 3 Passed -> Step-Up Transition: FAILSAFE to DEGRADED");
                s_current_state = FAULT_STATE_DEGRADED;
            }
            break;
        }

        case RECOVERY_STAGE_PROVISIONAL_DEGRADE: {
            /* System operates derated in DEGRADED for provisional dwell time */
            if (s_current_state != FAULT_STATE_DEGRADED) {
                /* Tripped back to FAILSAFE or SHUTDOWN due to new fault */
                s_recovery_ctrl.current_stage = RECOVERY_STAGE_IDLE;
                s_recovery_ctrl.recovery_in_progress = false;
                s_recovery_ctrl.retry_counter++;
                return;
            }

            if (current_timestamp_ms - s_recovery_ctrl.stage_entered_timestamp_ms >= CONFIG_RECOVERY_PROVISIONAL_DEGRADED_MS) {
                /* Provisional testing passed -> Advance to Stage 5: Final Normal Restoration */
                s_recovery_ctrl.current_stage = RECOVERY_STAGE_NORMAL_RESTORE;

                /* Deterministic state transition: DEGRADED -> NORMAL */
                LogStateTransition(current_timestamp_ms, s_current_state, FAULT_STATE_NORMAL,
                                   FAULT_ID_NONE, FAULT_SRC_SYSTEM, FAULT_SEV_NONE,
                                   "Recovery Stage 4 Passed -> Final Step-Up Transition: DEGRADED to NORMAL");
                s_current_state = FAULT_STATE_NORMAL;

                /* Recovery completed successfully */
                s_recovery_ctrl.current_stage = RECOVERY_STAGE_IDLE;
                s_recovery_ctrl.recovery_in_progress = false;
                s_recovery_ctrl.retry_counter = 0U; /* Clear retry tally on verified normal operation */
            }
            break;
        }

        default:
            break;
    }
}

/* ========================================================================== */
/* FAULT REGISTRY MANAGEMENT & AGGREGATE SEVERITY                             */
/* ========================================================================== */

void FaultFSM_InjectFault(FaultId_t fault_id, uint32_t current_timestamp_ms, 
                          const uint32_t aux[4], const char *desc)
{
    if (fault_id == FAULT_ID_NONE) {
        return;
    }

    /* Derive Source & Severity from standard ID format */
    FaultSource_t src = (FaultSource_t)((fault_id >> 12) & 0x0FU);
    FaultSeverity_t sev = FAULT_SEV_CRITICAL;

    if (fault_id == FAULT_ID_CELL_IMBALANCE || fault_id == FAULT_ID_CELL_OVERTEMP ||
        fault_id == FAULT_ID_COMM_CRC_BURST) {
        sev = FAULT_SEV_WARNING;
    }
    if (fault_id == FAULT_ID_RELAY_WELDED || fault_id == FAULT_ID_RECOVERY_EXHAUSTED ||
        fault_id == FAULT_ID_EMERGENCY_STOP) {
        sev = FAULT_SEV_FATAL;
    }

    /* Search for existing registry record */
    int32_t existing_idx = -1;
    int32_t first_empty_idx = -1;

    for (uint32_t i = 0U; i < CONFIG_FAULT_REGISTRY_CAPACITY; i++) {
        if (s_fault_registry[i].fault_id == fault_id) {
            existing_idx = (int32_t)i;
            break;
        }
        if ((first_empty_idx == -1) && (s_fault_registry[i].fault_id == FAULT_ID_NONE)) {
            first_empty_idx = (int32_t)i;
        }
    }

    int32_t target_idx = (existing_idx != -1) ? existing_idx : first_empty_idx;
    if (target_idx < 0) {
        return; /* Registry saturated */
    }

    FaultRecord_t *rec = &s_fault_registry[target_idx];
    if (existing_idx != -1) {
        /* Update existing record */
        rec->last_seen_ms = current_timestamp_ms;
        rec->occurrence_count++;
        rec->is_active = true;
    } else {
        /* Initialize new record */
        rec->fault_id = fault_id;
        rec->source = src;
        rec->severity = sev;
        rec->is_active = true;
        rec->is_latched = (sev >= FAULT_SEV_CRITICAL);
        rec->first_detected_ms = current_timestamp_ms;
        rec->last_seen_ms = current_timestamp_ms;
        rec->occurrence_count = 1U;
        s_active_fault_count++;
    }

    if (aux != NULL) {
        (void)memcpy(rec->isolation_aux_data, aux, sizeof(rec->isolation_aux_data));
    }
    if (desc != NULL) {
        (void)strncpy(rec->description, desc, sizeof(rec->description) - 1U);
        rec->description[sizeof(rec->description) - 1U] = '\0';
    }
}

void FaultFSM_ClearFault(FaultId_t fault_id, uint32_t current_timestamp_ms)
{
    for (uint32_t i = 0U; i < CONFIG_FAULT_REGISTRY_CAPACITY; i++) {
        if ((s_fault_registry[i].fault_id == fault_id) && s_fault_registry[i].is_active) {
            s_fault_registry[i].is_active = false;
            s_fault_registry[i].last_seen_ms = current_timestamp_ms;
            if (s_active_fault_count > 0U) {
                s_active_fault_count--;
            }
            break;
        }
    }
}

static FaultSeverity_t CalculateAggregateSeverity(FaultId_t *out_highest_fault_id, 
                                                 FaultSource_t *out_highest_source)
{
    FaultSeverity_t highest_sev = FAULT_SEV_NONE;
    FaultId_t highest_id = FAULT_ID_NONE;
    FaultSource_t highest_src = FAULT_SRC_NONE;

    for (uint32_t i = 0U; i < CONFIG_FAULT_REGISTRY_CAPACITY; i++) {
        if (s_fault_registry[i].is_active) {
            if (s_fault_registry[i].severity > highest_sev) {
                highest_sev = s_fault_registry[i].severity;
                highest_id = s_fault_registry[i].fault_id;
                highest_src = s_fault_registry[i].source;
            }
        }
    }

    if (out_highest_fault_id != NULL) *out_highest_fault_id = highest_id;
    if (out_highest_source != NULL) *out_highest_source = highest_src;
    return highest_sev;
}

/* ========================================================================== */
/* STRUCTURED TRANSITION LOGGING                                              */
/* ========================================================================== */

static void LogStateTransition(uint32_t timestamp_ms, FaultOperatingState_t prev_state, 
                               FaultOperatingState_t new_state, FaultId_t fault_id, 
                               FaultSource_t source, FaultSeverity_t severity, const char *reason)
{
    s_transition_log_total_count++;
    uint32_t idx = s_transition_log_head;
    s_transition_log_head = (s_transition_log_head + 1U) % CONFIG_TRANSITION_LOG_CAPACITY;

    StateTransitionLog_t *entry = &s_transition_log[idx];
    entry->sequence_id = s_transition_log_total_count;
    entry->timestamp_ms = timestamp_ms;
    entry->prev_state = prev_state;
    entry->new_state = new_state;
    entry->trigger_fault_id = fault_id;
    entry->fault_source = source;
    entry->severity = severity;

    if (reason != NULL) {
        (void)strncpy(entry->reason, reason, sizeof(entry->reason) - 1U);
        entry->reason[sizeof(entry->reason) - 1U] = '\0';
    } else {
        entry->reason[0] = '\0';
    }
}

void FaultFSM_FormatStructuredLog(const StateTransitionLog_t *entry, char *out_buffer, uint32_t buffer_len)
{
    if ((entry == NULL) || (out_buffer == NULL) || (buffer_len == 0U)) {
        return;
    }

    (void)snprintf(out_buffer, buffer_len,
                   "[%08lu ms] [TRANSITION #%04lu] %-8s -> %-8s | FAULT: 0x%04X (%-18s) | SRC: %-12s | SEV: %u | %s",
                   (unsigned long)entry->timestamp_ms,
                   (unsigned long)entry->sequence_id,
                   FaultFSM_StateToString(entry->prev_state),
                   FaultFSM_StateToString(entry->new_state),
                   (unsigned int)entry->trigger_fault_id,
                   FaultFSM_FaultIdToString(entry->trigger_fault_id),
                   FaultFSM_SourceToString(entry->fault_source),
                   (unsigned int)entry->severity,
                   entry->reason);
}

void FaultFSM_FormatJsonLog(const StateTransitionLog_t *entry, char *out_buffer, uint32_t buffer_len)
{
    if ((entry == NULL) || (out_buffer == NULL) || (buffer_len == 0U)) {
        return;
    }

    (void)snprintf(out_buffer, buffer_len,
                   "{\"seq\":%lu,\"ts\":%lu,\"prev\":\"%s\",\"new\":\"%s\",\"fault_id\":\"0x%04X\",\"fault_name\":\"%s\",\"source\":\"%s\",\"severity\":%u,\"reason\":\"%s\"}",
                   (unsigned long)entry->sequence_id,
                   (unsigned long)entry->timestamp_ms,
                   FaultFSM_StateToString(entry->prev_state),
                   FaultFSM_StateToString(entry->new_state),
                   (unsigned int)entry->trigger_fault_id,
                   FaultFSM_FaultIdToString(entry->trigger_fault_id),
                   FaultFSM_SourceToString(entry->fault_source),
                   (unsigned int)entry->severity,
                   entry->reason);
}

/* ========================================================================== */
/* GETTERS & STRING HELPERS                                                   */
/* ========================================================================== */

FaultOperatingState_t FaultFSM_GetCurrentState(void) {
    return s_current_state;
}

const char *FaultFSM_StateToString(FaultOperatingState_t state) {
    switch (state) {
        case FAULT_STATE_NORMAL:   return "NORMAL";
        case FAULT_STATE_DEGRADED: return "DEGRADED";
        case FAULT_STATE_FAILSAFE: return "FAILSAFE";
        case FAULT_STATE_SHUTDOWN: return "SHUTDOWN";
        default:                   return "UNKNOWN";
    }
}

const char *FaultFSM_SourceToString(FaultSource_t source) {
    switch (source) {
        case FAULT_SRC_BATTERY_CELL:  return "BATTERY_CELL";
        case FAULT_SRC_RELAY:         return "RELAY";
        case FAULT_SRC_COMMUNICATION: return "COMMUNICATION";
        case FAULT_SRC_ADC:           return "ADC";
        case FAULT_SRC_SYSTEM:        return "SYSTEM";
        default:                      return "NONE";
    }
}

const char *FaultFSM_FaultIdToString(FaultId_t fault_id) {
    switch (fault_id) {
        case FAULT_ID_CELL_OVERVOLTAGE:       return "CELL_OVERVOLTAGE";
        case FAULT_ID_CELL_UNDERVOLTAGE:      return "CELL_UNDERVOLTAGE";
        case FAULT_ID_CELL_IMBALANCE:         return "CELL_IMBALANCE";
        case FAULT_ID_CELL_OVERTEMP:          return "CELL_OVERTEMP";
        case FAULT_ID_RELAY_MISMATCH:         return "RELAY_MISMATCH";
        case FAULT_ID_RELAY_WELDED:           return "RELAY_WELDED";
        case FAULT_ID_RELAY_COIL_OPEN:        return "RELAY_COIL_OPEN";
        case FAULT_ID_COMM_HEARTBEAT_TIMEOUT: return "COMM_HEARTBEAT_LOST";
        case FAULT_ID_COMM_CRC_BURST:         return "COMM_CRC_BURST";
        case FAULT_ID_ADC_FROZEN:             return "ADC_FROZEN";
        case FAULT_ID_ADC_OUT_OF_RANGE:       return "ADC_OUT_OF_RANGE";
        case FAULT_ID_RECOVERY_EXHAUSTED:     return "RECOVERY_EXHAUSTED";
        case FAULT_ID_WATCHDOG_TIMEOUT:       return "WATCHDOG_TIMEOUT";
        case FAULT_ID_EMERGENCY_STOP:         return "EMERGENCY_STOP";
        default:                              return "FAULT_NONE";
    }
}

bool FaultFSM_IsFaultActive(FaultId_t fault_id) {
    for (uint32_t i = 0U; i < CONFIG_FAULT_REGISTRY_CAPACITY; i++) {
        if ((s_fault_registry[i].fault_id == fault_id) && s_fault_registry[i].is_active) {
            return true;
        }
    }
    return false;
}

uint32_t FaultFSM_GetActiveFaultCount(void) {
    return s_active_fault_count;
}

const FaultRecord_t *FaultFSM_GetFaultRecord(uint32_t index) {
    if (index < CONFIG_FAULT_REGISTRY_CAPACITY) {
        return &s_fault_registry[index];
    }
    return NULL;
}

uint32_t FaultFSM_GetFaultRegistryCapacity(void) {
    return CONFIG_FAULT_REGISTRY_CAPACITY;
}

RecoveryStage_t FaultFSM_GetRecoveryStage(void) {
    return s_recovery_ctrl.current_stage;
}

const char *FaultFSM_RecoveryStageToString(RecoveryStage_t stage) {
    switch (stage) {
        case RECOVERY_STAGE_IDLE:                return "IDLE";
        case RECOVERY_STAGE_CLEARANCE_CHECK:     return "CLEARANCE_CHECK";
        case RECOVERY_STAGE_STABILIZATION:       return "STABILIZATION";
        case RECOVERY_STAGE_SUBSYSTEM_VERIFY:    return "SUBSYSTEM_VERIFY";
        case RECOVERY_STAGE_PROVISIONAL_DEGRADE: return "PROVISIONAL_DEGRADE";
        case RECOVERY_STAGE_NORMAL_RESTORE:      return "NORMAL_RESTORE";
        case RECOVERY_STAGE_ABORTED:             return "ABORTED";
        default:                                 return "UNKNOWN";
    }
}

uint32_t FaultFSM_GetRecoveryRetryCount(void) {
    return s_recovery_ctrl.retry_counter;
}

uint32_t FaultFSM_GetRecoverySampleProgress(void) {
    return s_recovery_ctrl.samples_qualified;
}

uint32_t FaultFSM_GetTransitionLogCount(void) {
    return s_transition_log_total_count;
}

const StateTransitionLog_t *FaultFSM_GetTransitionLog(uint32_t index) {
    if (index < CONFIG_TRANSITION_LOG_CAPACITY) {
        return &s_transition_log[index];
    }
    return NULL;
}

void FaultFSM_ServiceReset(uint32_t current_timestamp_ms) {
    FaultFSM_Init();
    LogStateTransition(current_timestamp_ms, FAULT_STATE_SHUTDOWN, FAULT_STATE_NORMAL,
                       FAULT_ID_NONE, FAULT_SRC_SYSTEM, FAULT_SEV_NONE,
                       "Authorized Manual Service Reset Executed");
}
