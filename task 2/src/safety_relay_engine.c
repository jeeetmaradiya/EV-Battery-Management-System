/**
 * @file safety_relay_engine.c
 * @brief High-Integrity Non-Blocking Protection Relay and Safety System Engine.
 * 
 * Target: IEC 60255 Protection Relays, IEC 61508 SIL-3 Functional Safety.
 * Real-time Execution: Deterministic O(1) tick cycle, zero dynamic allocation.
 */

#include <string.h>
#include <math.h>
#include "safety_relay_engine.h"

/* ========================================================================== */
/* INTERNAL HELPER PROTOTYPES                                                 */
/* ========================================================================== */

static void LogTransition(RelayContext_t *ctx, uint32_t timestamp_ms, 
                          RelayState_t from_state, RelayState_t to_state, 
                          RelayTransitionReason_t reason, float current_a, 
                          float voltage_v, float variance, float slew_rate);

static void UpdateStatisticalWindow(WindowStats_t *ws, float sample, float delta_t_s);
static float MedianOfWindow(const WindowStats_t *ws);

/* ========================================================================== */
/* PUBLIC INITIALIZATION API                                                  */
/* ========================================================================== */

bool SafetyRelay_Init(RelayContext_t *ctx, uint32_t start_time_ms) {
    if (ctx == NULL) {
        return false;
    }

    memset(ctx, 0, sizeof(RelayContext_t));

    ctx->current_state = RELAY_STATE_BOOT;
    ctx->contacts = RELAY_CONTACTS_OPEN;
    ctx->state_entry_time_ms = start_time_ms;
    ctx->last_tick_time_ms = start_time_ms;
    ctx->trip_debounce_counter_ms = 0;
    ctx->clear_debounce_counter_ms = 0;
    ctx->last_fault_time_ms = 0;
    ctx->re_fault_count = 0;
    ctx->active_flags = ANOMALY_FLAG_NONE;
    ctx->pending_trip_reason = TR_REASON_NONE;
    ctx->ring_head = 0;
    ctx->ring_total_events = 0;
    ctx->is_initialized = true;

    /* Initialize statistical window state */
    ctx->current_window.classification = SIGNAL_CLASS_STEADY_NORMAL;
    ctx->voltage_window.classification = SIGNAL_CLASS_STEADY_NORMAL;

    /* Record boot completion event */
    LogTransition(ctx, start_time_ms, RELAY_STATE_BOOT, RELAY_STATE_NORMAL_MONITORING,
                  TR_REASON_BOOT_COMPLETE, 0.0f, 230.0f, 0.0f, 0.0f);

    ctx->current_state = RELAY_STATE_NORMAL_MONITORING;
    ctx->contacts = RELAY_CONTACTS_CLOSED;
    ctx->state_entry_time_ms = start_time_ms;

    return true;
}

/* ========================================================================== */
/* STATISTICAL PROCESSING & ANOMALY DISCRIMINATOR                             */
/* ========================================================================== */

static void UpdateStatisticalWindow(WindowStats_t *ws, float sample, float delta_t_s) {
    if (delta_t_s <= 0.0001f) {
        delta_t_s = 0.010f; /* 10ms fallback */
    }

    /* Compute instantaneous slew rate */
    if (ws->count > 0) {
        ws->instantaneous_slew_rate = fabsf(sample - ws->prev_sample) / delta_t_s;
    } else {
        ws->instantaneous_slew_rate = 0.0f;
    }
    ws->prev_sample = sample;

    /* Insert into circular array */
    ws->samples[ws->head_idx] = sample;
    ws->head_idx = (ws->head_idx + 1U) % RELAY_WINDOW_SIZE;
    if (ws->count < RELAY_WINDOW_SIZE) {
        ws->count++;
    }

    /* Compute window Mean, Min, Max, Span */
    float sum = 0.0f;
    float min_v = ws->samples[0];
    float max_v = ws->samples[0];

    for (uint32_t i = 0; i < ws->count; i++) {
        float val = ws->samples[i];
        sum += val;
        if (val < min_v) min_v = val;
        if (val > max_v) max_v = val;
    }

    ws->sum = sum;
    ws->mean = sum / (float)ws->count;
    ws->min_val = min_v;
    ws->max_val = max_v;
    ws->span = max_v - min_v;

    /* Compute Variance and Standard Deviation */
    float var_sum = 0.0f;
    for (uint32_t i = 0; i < ws->count; i++) {
        float diff = ws->samples[i] - ws->mean;
        var_sum += (diff * diff);
    }
    ws->variance = var_sum / (float)ws->count;
    ws->std_dev = sqrtf(ws->variance);
}

static float MedianOfWindow(const WindowStats_t *ws) {
    if (ws->count == 0) return 0.0f;
    if (ws->count == 1) return ws->samples[0];

    /* Copy to local buffer for sorting */
    float sorted[RELAY_WINDOW_SIZE];
    uint32_t n = ws->count;
    for (uint32_t i = 0; i < n; i++) {
        sorted[i] = ws->samples[i];
    }

    /* Simple insertion sort */
    for (uint32_t i = 1; i < n; i++) {
        float key = sorted[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && sorted[j] > key) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }

    if ((n % 2U) != 0U) {
        return sorted[n / 2U];
    } else {
        return 0.5f * (sorted[(n / 2U) - 1U] + sorted[n / 2U]);
    }
}

/* ========================================================================== */
/* PRIMARY NON-BLOCKING TICK PROCESSING FUNCTION                              */
/* ========================================================================== */

void SafetyRelay_ProcessTick(RelayContext_t *ctx, uint32_t current_time_ms, float raw_current_a, float raw_voltage_v) {
    if (ctx == NULL || !ctx->is_initialized) {
        return;
    }

    /* Calculate non-blocking delta time */
    uint32_t delta_ms = current_time_ms - ctx->last_tick_time_ms;
    if (delta_ms == 0) {
        delta_ms = 1;
    }
    float delta_t_s = (float)delta_ms / 1000.0f;
    ctx->last_tick_time_ms = current_time_ms;
    ctx->total_ticks_processed++;

    /* Reset transient tick flags */
    ctx->active_flags &= (uint16_t)(~(ANOMALY_FLAG_OUT_OF_RANGE_LOW | 
                                      ANOMALY_FLAG_OUT_OF_RANGE_HIGH | 
                                      ANOMALY_FLAG_UNREALISTIC_SLEW_JUMP | 
                                      ANOMALY_FLAG_SENSOR_FROZEN | 
                                      ANOMALY_FLAG_NOISE_SPIKE_REJECTED));

    /* ---------------------------------------------------------------------- */
    /* 1. SENSOR HEALTH: PHYSICAL OUT-OF-RANGE CHECK (Open Circuit / Short)   */
    /* ---------------------------------------------------------------------- */
    bool sensor_hard_fault = false;

    if (raw_voltage_v < RELAY_SENSOR_V_MIN_VALID) {
        ctx->active_flags |= ANOMALY_FLAG_OUT_OF_RANGE_LOW;
        sensor_hard_fault = true;
    } else if (raw_voltage_v > RELAY_SENSOR_V_MAX_VALID) {
        ctx->active_flags |= ANOMALY_FLAG_OUT_OF_RANGE_HIGH;
        sensor_hard_fault = true;
    }

    if (raw_current_a < RELAY_SENSOR_I_MIN_VALID) {
        ctx->active_flags |= ANOMALY_FLAG_OUT_OF_RANGE_LOW;
        sensor_hard_fault = true;
    } else if (raw_current_a > RELAY_SENSOR_I_MAX_VALID) {
        ctx->active_flags |= ANOMALY_FLAG_OUT_OF_RANGE_HIGH;
        sensor_hard_fault = true;
    }

    /* ---------------------------------------------------------------------- */
    /* 2. STATISTICAL WINDOW PROCESSING & DERIVATIVE ESTIMATION               */
    /* ---------------------------------------------------------------------- */
    UpdateStatisticalWindow(&ctx->voltage_window, raw_voltage_v, delta_t_s);
    UpdateStatisticalWindow(&ctx->current_window, raw_current_a, delta_t_s);

    /* ---------------------------------------------------------------------- */
    /* 3. UNREALISTIC VALUE JUMP (SLEW RATE ANOMALY)                          */
    /* ---------------------------------------------------------------------- */
    if (ctx->current_window.instantaneous_slew_rate > RELAY_MAX_PHYSICAL_SLEW_RATE_A_S) {
        ctx->active_flags |= ANOMALY_FLAG_UNREALISTIC_SLEW_JUMP;
    }

    /* ---------------------------------------------------------------------- */
    /* 4. FROZEN SENSOR READING CHECK (Dead ADC / Locked Bus)                 */
    /* ---------------------------------------------------------------------- */
    /* If window is full, expected active current > 1.0A, but span < epsilon */
    if (ctx->current_window.count >= RELAY_WINDOW_SIZE && 
        fabsf(ctx->current_window.mean) > 1.0f && 
        ctx->current_window.span < RELAY_FREEZE_MIN_SPAN_A) {
        ctx->active_flags |= ANOMALY_FLAG_SENSOR_FROZEN;
        sensor_hard_fault = true;
    }

    /* ---------------------------------------------------------------------- */
    /* 5. DISTINGUISHING GENUINE FAST LOAD CHANGE VS SENSOR NOISE SPIKE       */
    /* ---------------------------------------------------------------------- */
    float effective_current_a = raw_current_a;
    float window_median = MedianOfWindow(&ctx->current_window);
    float residual_from_median = fabsf(raw_current_a - window_median);
    /* Noise margin relative to baseline sensor noise floor to prevent variance inflation */
    float expected_noise_margin = RELAY_NOISE_SPIKE_SIGMA_FACTOR * RELAY_NOMINAL_NOISE_STD_DEV_A;
    if (expected_noise_margin < 1.0f) {
        expected_noise_margin = 1.0f;
    }

    if (residual_from_median > expected_noise_margin || ctx->current_window.consecutive_step_samples > 0) {
        /* Significant deviation from median baseline or active step candidate */
        if (ctx->current_window.consecutive_step_samples == 0) {
            /* First sample of candidate step or spike */
            ctx->current_window.candidate_step_level = raw_current_a;
            ctx->current_window.consecutive_step_samples = 1;
            
            /* Tentatively filter out outlier on first hit to prevent false relay chatter */
            effective_current_a = window_median;
            ctx->current_window.classification = SIGNAL_CLASS_SENSOR_NOISE_SPIKE;
            ctx->active_flags |= ANOMALY_FLAG_NOISE_SPIKE_REJECTED;
            ctx->noise_spikes_rejected_count++;
        } else {
            /* Subsequent sample: check if it stays near candidate step level */
            float diff_from_step = fabsf(raw_current_a - ctx->current_window.candidate_step_level);
            if (diff_from_step <= 2.0f) {
                /* Step persists! Increment confirmation counter */
                ctx->current_window.consecutive_step_samples++;
                if (ctx->current_window.consecutive_step_samples >= RELAY_STEP_PERSISTENCE_MIN_SAMPLES) {
                    /* Confirmed genuine fast load change! */
                    ctx->current_window.classification = SIGNAL_CLASS_GENUINE_LOAD_CHANGE;
                    effective_current_a = raw_current_a; /* Accepted as real physical current */
                } else {
                    /* Still in persistence confirmation window: use median */
                    effective_current_a = window_median;
                }
            } else {
                /* Signal jumped away or collapsed: was an EMI glitch */
                ctx->current_window.consecutive_step_samples = 0;
                ctx->current_window.classification = SIGNAL_CLASS_SENSOR_NOISE_SPIKE;
                ctx->active_flags |= ANOMALY_FLAG_NOISE_SPIKE_REJECTED;
                ctx->noise_spikes_rejected_count++;
                effective_current_a = window_median;
            }
        }
    } else {
        /* Within expected noise envelope */
        ctx->current_window.consecutive_step_samples = 0;
        ctx->current_window.classification = SIGNAL_CLASS_STEADY_NORMAL;
        effective_current_a = raw_current_a;
    }

    ctx->filtered_current_a = effective_current_a;
    ctx->filtered_voltage_v = raw_voltage_v;

    if (sensor_hard_fault) {
        ctx->sensor_anomalies_count++;
    }

    /* ---------------------------------------------------------------------- */
    /* 6. PROTECTION LOGIC EVALUATION WITH DUAL-THRESHOLD HYSTERESIS         */
    /* ---------------------------------------------------------------------- */
    bool fault_trip_condition = false;
    bool fault_reset_condition = false;
    RelayTransitionReason_t current_trip_reason = TR_REASON_NONE;

    /* A. Overcurrent with Hysteresis (Trip: 50A, Reset: 42A) */
    if (effective_current_a >= RELAY_CURRENT_OVERCURRENT_TRIP_A) {
        fault_trip_condition = true;
        current_trip_reason = TR_REASON_OVERCURRENT_DEBOUNCED_TRIP;
        ctx->active_flags |= ANOMALY_FLAG_OVERCURRENT_TRIP;
    } else if (effective_current_a <= RELAY_CURRENT_OVERCURRENT_RESET_A) {
        /* Fully below reset threshold */
        fault_reset_condition = true;
    } else {
        /* Inside Hysteresis Deadband (42A to 50A): maintain prior state! */
        ctx->active_flags |= ANOMALY_FLAG_HYSTERESIS_ACTIVE;
        if (ctx->current_state == RELAY_STATE_TRIPPED_FAULT ||
            ctx->current_state == RELAY_STATE_RECOVERY_COOLDOWN ||
            ctx->current_state == RELAY_STATE_RECOVERY_SOAK) {
            /* If previously tripped, deadband keeps it tripped */
            fault_trip_condition = true;
        } else {
            /* If previously normal, deadband keeps it normal */
            fault_reset_condition = true;
        }
    }

    /* B. Overvoltage & Undervoltage with Hysteresis */
    if (raw_voltage_v >= RELAY_VOLTAGE_OVERVOLT_TRIP_V) {
        fault_trip_condition = true;
        current_trip_reason = TR_REASON_OVERVOLTAGE_DEBOUNCED_TRIP;
        ctx->active_flags |= ANOMALY_FLAG_OVERVOLTAGE_TRIP;
    } else if (raw_voltage_v <= RELAY_VOLTAGE_UNDERVOLT_TRIP_V) {
        fault_trip_condition = true;
        current_trip_reason = TR_REASON_UNDERVOLTAGE_DEBOUNCED_TRIP;
        ctx->active_flags |= ANOMALY_FLAG_UNDERVOLTAGE_TRIP;
    }

    /* C. Severe Sensor Anomalies trigger immediate protection */
    if (sensor_hard_fault) {
        fault_trip_condition = true;
        current_trip_reason = TR_REASON_SENSOR_ANOMALY_TRIP;
    }

    /* ---------------------------------------------------------------------- */
    /* 7. STATE MACHINE EXECUTION (NON-BLOCKING COOPERATIVE TICK)             */
    /* ---------------------------------------------------------------------- */
    uint32_t state_dwell_time = current_time_ms - ctx->state_entry_time_ms;

    switch (ctx->current_state) {

    case RELAY_STATE_NORMAL_MONITORING:
        ctx->contacts = RELAY_CONTACTS_CLOSED;
        ctx->clear_debounce_counter_ms = 0;

        if (sensor_hard_fault) {
            /* Catastrophic hardware fault (wire break, rail short, stuck sensor): Immediate trip */
            ctx->contacts = RELAY_CONTACTS_OPEN;
            ctx->trip_count++;
            ctx->last_fault_time_ms = current_time_ms;
            LogTransition(ctx, current_time_ms, RELAY_STATE_NORMAL_MONITORING,
                          RELAY_STATE_TRIPPED_FAULT, TR_REASON_SENSOR_ANOMALY_TRIP,
                          effective_current_a, raw_voltage_v,
                          ctx->current_window.variance, ctx->current_window.instantaneous_slew_rate);
            ctx->current_state = RELAY_STATE_TRIPPED_FAULT;
            ctx->state_entry_time_ms = current_time_ms;
            ctx->trip_debounce_counter_ms = 0;
            ctx->clear_debounce_counter_ms = 0;
        } else if (fault_trip_condition) {
            /* Transient overload detected: Initiate Trip Debounce Timing */
            ctx->pending_trip_reason = current_trip_reason;
            ctx->trip_debounce_counter_ms = 0;
            LogTransition(ctx, current_time_ms, RELAY_STATE_NORMAL_MONITORING,
                          RELAY_STATE_TRIP_DEBOUNCING, TR_REASON_OVERCURRENT_PENDING,
                          effective_current_a, raw_voltage_v, 
                          ctx->current_window.variance, ctx->current_window.instantaneous_slew_rate);
            ctx->current_state = RELAY_STATE_TRIP_DEBOUNCING;
            ctx->state_entry_time_ms = current_time_ms;
        }
        break;

    case RELAY_STATE_TRIP_DEBOUNCING:
        ctx->contacts = RELAY_CONTACTS_CLOSED;

        if (!fault_trip_condition) {
            /* Transient glitch cleared before debounce expired! Return to normal (Chatter prevented) */
            LogTransition(ctx, current_time_ms, RELAY_STATE_TRIP_DEBOUNCING,
                          RELAY_STATE_NORMAL_MONITORING, TR_REASON_TRANSIENT_CLEARED_BEFORE_DEBOUNCE,
                          effective_current_a, raw_voltage_v,
                          ctx->current_window.variance, ctx->current_window.instantaneous_slew_rate);
            ctx->current_state = RELAY_STATE_NORMAL_MONITORING;
            ctx->state_entry_time_ms = current_time_ms;
            ctx->trip_debounce_counter_ms = 0;
        } else {
            /* Accumulate debounce time */
            ctx->trip_debounce_counter_ms += delta_ms;
            if (ctx->trip_debounce_counter_ms >= RELAY_TRIP_DEBOUNCE_MS || sensor_hard_fault) {
                /* Trip Debounce Expired: TRIP THE RELAY! */
                ctx->contacts = RELAY_CONTACTS_OPEN;
                ctx->trip_count++;
                ctx->last_fault_time_ms = current_time_ms;

                LogTransition(ctx, current_time_ms, RELAY_STATE_TRIP_DEBOUNCING,
                              RELAY_STATE_TRIPPED_FAULT, ctx->pending_trip_reason,
                              effective_current_a, raw_voltage_v,
                              ctx->current_window.variance, ctx->current_window.instantaneous_slew_rate);
                ctx->current_state = RELAY_STATE_TRIPPED_FAULT;
                ctx->state_entry_time_ms = current_time_ms;
                ctx->trip_debounce_counter_ms = 0;
                ctx->clear_debounce_counter_ms = 0;
            }
        }
        break;

    case RELAY_STATE_TRIPPED_FAULT:
        ctx->contacts = RELAY_CONTACTS_OPEN;

        if (fault_trip_condition) {
            /* Fault still active: reset clear debounce counter */
            ctx->clear_debounce_counter_ms = 0;
        } else {
            /* Fault condition absent and signal healthy: accumulate clear debounce */
            ctx->clear_debounce_counter_ms += delta_ms;
            if (ctx->clear_debounce_counter_ms >= RELAY_CLEAR_DEBOUNCE_MS) {
                /* Confirmed clear: Start Timed Recovery Sequence (Cooldown) */
                LogTransition(ctx, current_time_ms, RELAY_STATE_TRIPPED_FAULT,
                              RELAY_STATE_RECOVERY_COOLDOWN, TR_REASON_FAULT_CLEARED_DEBOUNCED,
                              effective_current_a, raw_voltage_v,
                              ctx->current_window.variance, ctx->current_window.instantaneous_slew_rate);
                ctx->current_state = RELAY_STATE_RECOVERY_COOLDOWN;
                ctx->state_entry_time_ms = current_time_ms;
                ctx->clear_debounce_counter_ms = 0;
            }
        }
        break;

    case RELAY_STATE_RECOVERY_COOLDOWN:
        ctx->contacts = RELAY_CONTACTS_OPEN;

        /* If fault recurs during cooldown: abort recovery immediately */
        if (fault_trip_condition) {
            ctx->re_fault_count++;
            if (ctx->re_fault_count >= RELAY_MAX_RE_FAULTS_BEFORE_LOCKOUT) {
                LogTransition(ctx, current_time_ms, RELAY_STATE_RECOVERY_COOLDOWN,
                              RELAY_STATE_LOCKOUT_LATCHED, TR_REASON_LOCKOUT_MAX_REFAULTS,
                              effective_current_a, raw_voltage_v,
                              ctx->current_window.variance, ctx->current_window.instantaneous_slew_rate);
                ctx->current_state = RELAY_STATE_LOCKOUT_LATCHED;
            } else {
                LogTransition(ctx, current_time_ms, RELAY_STATE_RECOVERY_COOLDOWN,
                              RELAY_STATE_TRIPPED_FAULT, TR_REASON_RECOVERY_ABORT_REFAULT,
                              effective_current_a, raw_voltage_v,
                              ctx->current_window.variance, ctx->current_window.instantaneous_slew_rate);
                ctx->current_state = RELAY_STATE_TRIPPED_FAULT;
            }
            ctx->state_entry_time_ms = current_time_ms;
            break;
        }

        /* Check cooldown duration */
        if (state_dwell_time >= RELAY_RECOVERY_COOLDOWN_MS) {
            LogTransition(ctx, current_time_ms, RELAY_STATE_RECOVERY_COOLDOWN,
                          RELAY_STATE_RECOVERY_SOAK, TR_REASON_COOLDOWN_EXPIRED,
                          effective_current_a, raw_voltage_v,
                          ctx->current_window.variance, ctx->current_window.instantaneous_slew_rate);
            ctx->current_state = RELAY_STATE_RECOVERY_SOAK;
            ctx->state_entry_time_ms = current_time_ms;
        }
        break;

    case RELAY_STATE_RECOVERY_SOAK:
        ctx->contacts = RELAY_CONTACTS_OPEN;

        /* If any fault or anomaly recurs during soak pre-check: abort recovery */
        if (fault_trip_condition || (ctx->active_flags & ANOMALY_FLAG_UNREALISTIC_SLEW_JUMP)) {
            ctx->re_fault_count++;
            if (ctx->re_fault_count >= RELAY_MAX_RE_FAULTS_BEFORE_LOCKOUT) {
                LogTransition(ctx, current_time_ms, RELAY_STATE_RECOVERY_SOAK,
                              RELAY_STATE_LOCKOUT_LATCHED, TR_REASON_LOCKOUT_MAX_REFAULTS,
                              effective_current_a, raw_voltage_v,
                              ctx->current_window.variance, ctx->current_window.instantaneous_slew_rate);
                ctx->current_state = RELAY_STATE_LOCKOUT_LATCHED;
            } else {
                LogTransition(ctx, current_time_ms, RELAY_STATE_RECOVERY_SOAK,
                              RELAY_STATE_TRIPPED_FAULT, TR_REASON_RECOVERY_ABORT_REFAULT,
                              effective_current_a, raw_voltage_v,
                              ctx->current_window.variance, ctx->current_window.instantaneous_slew_rate);
                ctx->current_state = RELAY_STATE_TRIPPED_FAULT;
            }
            ctx->state_entry_time_ms = current_time_ms;
            break;
        }

        /* Check soak pre-check duration */
        if (state_dwell_time >= RELAY_RECOVERY_SOAK_PRECHECK_MS) {
            LogTransition(ctx, current_time_ms, RELAY_STATE_RECOVERY_SOAK,
                          RELAY_STATE_RECOVERY_REARMING, TR_REASON_SOAK_PASSED,
                          effective_current_a, raw_voltage_v,
                          ctx->current_window.variance, ctx->current_window.instantaneous_slew_rate);
            ctx->current_state = RELAY_STATE_RECOVERY_REARMING;
            ctx->state_entry_time_ms = current_time_ms;
        }
        break;

    case RELAY_STATE_RECOVERY_REARMING:
        ctx->contacts = RELAY_CONTACTS_OPEN;

        /* If fault recurs during rearming */
        if (fault_trip_condition) {
            LogTransition(ctx, current_time_ms, RELAY_STATE_RECOVERY_REARMING,
                          RELAY_STATE_TRIPPED_FAULT, TR_REASON_RECOVERY_ABORT_REFAULT,
                          effective_current_a, raw_voltage_v,
                          ctx->current_window.variance, ctx->current_window.instantaneous_slew_rate);
            ctx->current_state = RELAY_STATE_TRIPPED_FAULT;
            ctx->state_entry_time_ms = current_time_ms;
            break;
        }

        /* Check rearming timer */
        if (state_dwell_time >= RELAY_RECOVERY_REARMING_MS) {
            /* Close contacts & return to Normal Monitoring */
            ctx->contacts = RELAY_CONTACTS_CLOSED;
            ctx->re_fault_count = 0; /* Reset re-fault counter upon successful restoration */
            LogTransition(ctx, current_time_ms, RELAY_STATE_RECOVERY_REARMING,
                          RELAY_STATE_NORMAL_MONITORING, TR_REASON_REARM_COMPLETE,
                          effective_current_a, raw_voltage_v,
                          ctx->current_window.variance, ctx->current_window.instantaneous_slew_rate);
            ctx->current_state = RELAY_STATE_NORMAL_MONITORING;
            ctx->state_entry_time_ms = current_time_ms;
        }
        break;

    case RELAY_STATE_LOCKOUT_LATCHED:
        /* Relay locked open permanently until supervisory manual reset */
        ctx->contacts = RELAY_CONTACTS_OPEN;
        break;

    case RELAY_STATE_BOOT:
    default:
        ctx->current_state = RELAY_STATE_NORMAL_MONITORING;
        ctx->contacts = RELAY_CONTACTS_CLOSED;
        ctx->state_entry_time_ms = current_time_ms;
        break;
    }
}

/* ========================================================================== */
/* MANUAL RESET API                                                           */
/* ========================================================================== */

bool SafetyRelay_ManualReset(RelayContext_t *ctx, uint32_t current_time_ms) {
    if (ctx == NULL || !ctx->is_initialized) {
        return false;
    }

    /* Cannot reset if active overcurrent or broken wire exists */
    if ((ctx->active_flags & (ANOMALY_FLAG_OUT_OF_RANGE_LOW | 
                              ANOMALY_FLAG_OUT_OF_RANGE_HIGH | 
                              ANOMALY_FLAG_OVERCURRENT_TRIP)) != 0) {
        return false;
    }

    RelayState_t prev = ctx->current_state;
    ctx->re_fault_count = 0;
    ctx->trip_debounce_counter_ms = 0;
    ctx->clear_debounce_counter_ms = 0;

    /* Transition directly to Cooldown recovery */
    LogTransition(ctx, current_time_ms, prev, RELAY_STATE_RECOVERY_COOLDOWN,
                  TR_REASON_MANUAL_RESET, ctx->filtered_current_a, 
                  ctx->filtered_voltage_v, ctx->current_window.variance, 
                  ctx->current_window.instantaneous_slew_rate);

    ctx->current_state = RELAY_STATE_RECOVERY_COOLDOWN;
    ctx->contacts = RELAY_CONTACTS_OPEN;
    ctx->state_entry_time_ms = current_time_ms;

    return true;
}

/* ========================================================================== */
/* DETERMINISTIC TRANSITION LOGGING IMPLEMENTATION                            */
/* ========================================================================== */

static void LogTransition(RelayContext_t *ctx, uint32_t timestamp_ms, 
                          RelayState_t from_state, RelayState_t to_state, 
                          RelayTransitionReason_t reason, float current_a, 
                          float voltage_v, float variance, float slew_rate) {
    if (ctx == NULL) return;

    RelayTransitionEvent_t *ev = &ctx->event_ring[ctx->ring_head];
    ev->timestamp_ms = timestamp_ms;
    ev->from_state = from_state;
    ev->to_state = to_state;
    ev->reason = reason;
    ev->current_a = current_a;
    ev->voltage_v = voltage_v;
    ev->anomaly_flags = ctx->active_flags;
    ev->rolling_variance = variance;
    ev->slew_rate_a_s = slew_rate;

    ctx->ring_head = (ctx->ring_head + 1U) % RELAY_EVENT_LOG_CAPACITY;
    ctx->ring_total_events++;
}

uint32_t SafetyRelay_GetEventCount(const RelayContext_t *ctx) {
    if (ctx == NULL) return 0;
    return ctx->ring_total_events;
}

bool SafetyRelay_GetEventAt(const RelayContext_t *ctx, uint32_t index, RelayTransitionEvent_t *out_event) {
    if (ctx == NULL || out_event == NULL) return false;

    uint32_t available = (ctx->ring_total_events < RELAY_EVENT_LOG_CAPACITY) ? 
                          ctx->ring_total_events : RELAY_EVENT_LOG_CAPACITY;

    if (index >= available) return false;

    /* Calculate circular offset */
    uint32_t start_idx = 0;
    if (ctx->ring_total_events > RELAY_EVENT_LOG_CAPACITY) {
        start_idx = ctx->ring_head; /* Oldest element is at current head */
    }
    uint32_t actual_idx = (start_idx + index) % RELAY_EVENT_LOG_CAPACITY;

    *out_event = ctx->event_ring[actual_idx];
    return true;
}

/* ========================================================================== */
/* TELEMETRY & STRING CONVERSIONS                                             */
/* ========================================================================== */

void SafetyRelay_GetTelemetry(const RelayContext_t *ctx, RelayTelemetry_t *out_telem) {
    if (ctx == NULL || out_telem == NULL) return;

    out_telem->timestamp_ms = ctx->last_tick_time_ms;
    out_telem->state = ctx->current_state;
    out_telem->contacts = ctx->contacts;
    out_telem->current_a = ctx->current_window.prev_sample;
    out_telem->filtered_current_a = ctx->filtered_current_a;
    out_telem->voltage_v = ctx->filtered_voltage_v;
    out_telem->active_flags = ctx->active_flags;
    out_telem->signal_class = ctx->current_window.classification;

    out_telem->state_dwell_time_ms = ctx->last_tick_time_ms - ctx->state_entry_time_ms;
    out_telem->trip_debounce_counter_ms = ctx->trip_debounce_counter_ms;
    out_telem->clear_debounce_counter_ms = ctx->clear_debounce_counter_ms;

    out_telem->current_mean = ctx->current_window.mean;
    out_telem->current_variance = ctx->current_window.variance;
    out_telem->current_slew_rate_a_s = ctx->current_window.instantaneous_slew_rate;
    out_telem->current_window_span = ctx->current_window.span;

    out_telem->total_transitions = ctx->ring_total_events;
    out_telem->trip_count = ctx->trip_count;
    out_telem->re_fault_count = ctx->re_fault_count;
    out_telem->noise_spikes_rejected = ctx->noise_spikes_rejected_count;
    out_telem->anomalies_detected = ctx->sensor_anomalies_count;
}

const char* SafetyRelay_StateToString(RelayState_t state) {
    switch (state) {
        case RELAY_STATE_BOOT:                 return "BOOT";
        case RELAY_STATE_NORMAL_MONITORING:    return "NORMAL_MONITORING";
        case RELAY_STATE_TRIP_DEBOUNCING:      return "TRIP_DEBOUNCING";
        case RELAY_STATE_TRIPPED_FAULT:        return "TRIPPED_FAULT";
        case RELAY_STATE_RECOVERY_COOLDOWN:    return "RECOVERY_COOLDOWN";
        case RELAY_STATE_RECOVERY_SOAK:        return "RECOVERY_SOAK";
        case RELAY_STATE_RECOVERY_REARMING:    return "RECOVERY_REARMING";
        case RELAY_STATE_LOCKOUT_LATCHED:      return "LOCKOUT_LATCHED";
        default:                               return "UNKNOWN";
    }
}

const char* SafetyRelay_ReasonToString(RelayTransitionReason_t reason) {
    switch (reason) {
        case TR_REASON_NONE:                            return "NONE";
        case TR_REASON_BOOT_COMPLETE:                   return "BOOT_COMPLETE";
        case TR_REASON_OVERCURRENT_PENDING:             return "OVERCURRENT_PENDING";
        case TR_REASON_OVERCURRENT_DEBOUNCED_TRIP:      return "OVERCURRENT_DEBOUNCED_TRIP";
        case TR_REASON_OVERVOLTAGE_DEBOUNCED_TRIP:      return "OVERVOLTAGE_DEBOUNCED_TRIP";
        case TR_REASON_UNDERVOLTAGE_DEBOUNCED_TRIP:     return "UNDERVOLTAGE_DEBOUNCED_TRIP";
        case TR_REASON_SENSOR_ANOMALY_TRIP:             return "SENSOR_ANOMALY_TRIP";
        case TR_REASON_TRANSIENT_CLEARED_BEFORE_DEBOUNCE: return "TRANSIENT_CLEARED_BEFORE_DEBOUNCE";
        case TR_REASON_FAULT_CLEARED_DEBOUNCED:         return "FAULT_CLEARED_DEBOUNCED";
        case TR_REASON_COOLDOWN_EXPIRED:                return "COOLDOWN_EXPIRED";
        case TR_REASON_SOAK_PASSED:                     return "SOAK_PASSED";
        case TR_REASON_REARM_COMPLETE:                  return "REARM_COMPLETE";
        case TR_REASON_RECOVERY_ABORT_REFAULT:          return "RECOVERY_ABORT_REFAULT";
        case TR_REASON_LOCKOUT_MAX_REFAULTS:            return "LOCKOUT_MAX_REFAULTS";
        case TR_REASON_MANUAL_RESET:                    return "MANUAL_RESET";
        default:                                        return "UNKNOWN_REASON";
    }
}

const char* SafetyRelay_SignalClassToString(SignalClass_t sclass) {
    switch (sclass) {
        case SIGNAL_CLASS_STEADY_NORMAL:       return "STEADY_NORMAL";
        case SIGNAL_CLASS_GENUINE_LOAD_CHANGE: return "GENUINE_LOAD_CHANGE";
        case SIGNAL_CLASS_SENSOR_NOISE_SPIKE:  return "SENSOR_NOISE_SPIKE";
        case SIGNAL_CLASS_ANOMALY_DETECTED:    return "ANOMALY_DETECTED";
        default:                               return "UNKNOWN_CLASS";
    }
}
