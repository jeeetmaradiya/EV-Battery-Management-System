/**
 * @file telemetry_engine.c
 * @brief Production-Grade Event-Driven Telemetry Engine with Offline Circular Buffer,
 *        Non-Blocking Wi-Fi Reconnection State Machine, and RSSI Quality Assessment.
 * 
 * Safety Critical Embedded System Architecture (ISO 26262 ASIL-C / MISRA-C:2012).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "telemetry_engine.h"

/* ========================================================================== */
/* INTERNAL STATE DEFINITIONS                                                 */
/* ========================================================================== */

static struct {
    bool initialized;
    uint32_t current_time_ms;
    uint32_t monotonic_seq_id;
    uint32_t last_packet_time_ms;
    uint32_t last_drain_time_ms;
    uint32_t online_stable_start_ms;
    uint32_t outage_start_ms;

    bms_snapshot_t current_bms;
    bms_snapshot_t last_transmitted_bms;
    bool has_transmitted_baseline;

    offline_queue_t queue;
    wifi_controller_t wifi;
    telemetry_metrics_t metrics;

    telemetry_tx_callback_t tx_callback;
} g_engine;

/* ========================================================================== */
/* PRIVATE HELPER PROTOTYPES                                                  */
/* ========================================================================== */

static void calculate_bms_extrema(bms_snapshot_t *snapshot);
static bool evaluate_bms_deadbands(const bms_snapshot_t *curr, const bms_snapshot_t *prev, char *reason, telemetry_event_type_t *evt_type);
static void enqueue_event(const telemetry_event_packet_t *packet);
static bool dequeue_event(telemetry_event_packet_t *packet);
static void dispatch_packet(telemetry_event_packet_t *packet);
static void update_wifi_fsm(uint32_t now_ms);
static void update_rssi_quality(void);
static uint32_t compute_backoff_ms(uint32_t attempts);

/* ========================================================================== */
/* PUBLIC API IMPLEMENTATION                                                  */
/* ========================================================================== */

void telemetry_engine_init(void) {
    memset(&g_engine, 0, sizeof(g_engine));

    g_engine.initialized = true;
    g_engine.current_time_ms = 0;
    g_engine.monotonic_seq_id = 1;
    g_engine.last_packet_time_ms = 0;
    g_engine.last_drain_time_ms = 0;
    g_engine.has_transmitted_baseline = false;

    /* Initialize queue */
    g_engine.queue.capacity = OFFLINE_QUEUE_CAPACITY;
    g_engine.queue.head = 0;
    g_engine.queue.tail = 0;
    g_engine.queue.count = 0;
    g_engine.queue.peak_depth = 0;
    g_engine.queue.overflow_dropped = 0;

    /* Initialize Wi-Fi Controller */
    g_engine.wifi.state = WIFI_STATE_DISCONNECTED;
    g_engine.wifi.state_entry_time_ms = 0;
    g_engine.wifi.backoff_interval_ms = WIFI_INITIAL_BACKOFF_MS;
    g_engine.wifi.reconnect_attempts = 0;
    g_engine.wifi.raw_rssi_dbm = -65;
    g_engine.wifi.filtered_rssi_dbm = -65;
    g_engine.wifi.physical_link_up = true;
    g_engine.wifi.blynk_server_available = true;
    update_rssi_quality();

    /* Initialize default BMS parameters */
    for (int i = 0; i < TELEMETRY_CELL_COUNT; i++) {
        g_engine.current_bms.cell_voltages_mv[i] = 3700;
    }
    g_engine.current_bms.pack_voltage_mv = 3700 * TELEMETRY_CELL_COUNT;
    g_engine.current_bms.pack_current_ma = 0;
    for (int i = 0; i < TELEMETRY_TEMP_SENSORS; i++) {
        g_engine.current_bms.temperatures_c[i] = 25;
    }
    g_engine.current_bms.fsm_state = BMS_STATE_NORMAL;
    g_engine.current_bms.relay_status_flags = (RELAY_FLAG_MAIN_COMMANDED | RELAY_FLAG_MAIN_FEEDBACK);
    calculate_bms_extrema(&g_engine.current_bms);
}

void telemetry_register_tx_callback(telemetry_tx_callback_t callback) {
    g_engine.tx_callback = callback;
}

void telemetry_engine_reset(void) {
    telemetry_engine_init();
}

void telemetry_engine_tick(uint32_t current_time_ms) {
    if (!g_engine.initialized) {
        telemetry_engine_init();
    }
    g_engine.current_time_ms = current_time_ms;

    /* 1. Step Wi-Fi & Blynk Connection State Machine */
    update_wifi_fsm(current_time_ms);

    /* 2. Check if baseline initial telemetry packet needs to be sent once connected */
    if (!g_engine.has_transmitted_baseline && g_engine.wifi.state == WIFI_STATE_BLYNK_CONNECTED) {
        telemetry_event_packet_t base_pkt;
        memset(&base_pkt, 0, sizeof(base_pkt));
        base_pkt.seq_id = g_engine.monotonic_seq_id++;
        base_pkt.capture_timestamp_ms = current_time_ms;
        base_pkt.event_type = EVENT_TYPE_STATE_CHANGE;
        base_pkt.priority = PRIORITY_NORMAL;
        base_pkt.snapshot = g_engine.current_bms;
        snprintf(base_pkt.reason, TELEMETRY_REASON_MAX_LEN, "INITIAL_BASELINE_TELEMETRY");

        dispatch_packet(&base_pkt);
        g_engine.last_transmitted_bms = g_engine.current_bms;
        g_engine.last_packet_time_ms = current_time_ms;
        g_engine.has_transmitted_baseline = true;
    }

    /* 3. Evaluate Quiescent Heartbeat Interval */
    if (g_engine.has_transmitted_baseline && 
        (current_time_ms - g_engine.last_packet_time_ms >= TELEMETRY_HEARTBEAT_INTERVAL_MS)) {
        
        telemetry_event_packet_t hb_pkt;
        memset(&hb_pkt, 0, sizeof(hb_pkt));
        hb_pkt.seq_id = g_engine.monotonic_seq_id++;
        hb_pkt.capture_timestamp_ms = current_time_ms;
        hb_pkt.event_type = EVENT_TYPE_HEARTBEAT;
        hb_pkt.priority = PRIORITY_LOW;
        hb_pkt.snapshot = g_engine.current_bms;
        snprintf(hb_pkt.reason, TELEMETRY_REASON_MAX_LEN, "QUIESCENT_HEARTBEAT_SYNC");

        dispatch_packet(&hb_pkt);
        g_engine.last_packet_time_ms = current_time_ms;
        g_engine.last_transmitted_bms = g_engine.current_bms;
    }

    /* 4. Service Offline Queue Replay (Pacing / Rate-Limiting) */
    if (g_engine.wifi.state == WIFI_STATE_BLYNK_CONNECTED && g_engine.queue.count > 0) {
        if (current_time_ms - g_engine.last_drain_time_ms >= TELEMETRY_DRAIN_INTERVAL_MS) {
            uint16_t burst_count = 0;
            while (g_engine.queue.count > 0 && burst_count < TELEMETRY_DRAIN_BURST_MAX) {
                telemetry_event_packet_t queued_pkt;
                if (dequeue_event(&queued_pkt)) {
                    queued_pkt.origin = DATA_ORIGIN_QUEUED;
                    queued_pkt.transmit_timestamp_ms = current_time_ms;

                    if (g_engine.tx_callback) {
                        g_engine.tx_callback(&queued_pkt);
                    }
                    g_engine.metrics.packets_transmitted_queued++;
                    g_engine.metrics.total_bytes_transmitted += sizeof(telemetry_event_packet_t);
                    burst_count++;
                }
            }
            g_engine.last_drain_time_ms = current_time_ms;

            /* Check if queue is now completely drained */
            if (g_engine.queue.count == 0) {
                telemetry_event_packet_t flush_pkt;
                memset(&flush_pkt, 0, sizeof(flush_pkt));
                flush_pkt.seq_id = g_engine.monotonic_seq_id++;
                flush_pkt.capture_timestamp_ms = current_time_ms;
                flush_pkt.transmit_timestamp_ms = current_time_ms;
                flush_pkt.event_type = EVENT_TYPE_QUEUE_FLUSH;
                flush_pkt.priority = PRIORITY_NORMAL;
                flush_pkt.origin = DATA_ORIGIN_LIVE;
                flush_pkt.snapshot = g_engine.current_bms;
                snprintf(flush_pkt.reason, TELEMETRY_REASON_MAX_LEN, "OFFLINE_QUEUE_RESTORED_LIVE");

                if (g_engine.tx_callback) {
                    g_engine.tx_callback(&flush_pkt);
                }
                g_engine.metrics.packets_transmitted_live++;
            }
        }
    }
}

void telemetry_update_bms(const bms_snapshot_t *snapshot) {
    if (!snapshot) return;

    g_engine.current_bms = *snapshot;
    calculate_bms_extrema(&g_engine.current_bms);

    if (!g_engine.has_transmitted_baseline) {
        g_engine.last_transmitted_bms = g_engine.current_bms;
        g_engine.has_transmitted_baseline = true;
        return;
    }

    char reason[TELEMETRY_REASON_MAX_LEN] = {0};
    telemetry_event_type_t evt_type = EVENT_TYPE_NONE;

    if (evaluate_bms_deadbands(&g_engine.current_bms, &g_engine.last_transmitted_bms, reason, &evt_type)) {
        telemetry_event_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.seq_id = g_engine.monotonic_seq_id++;
        pkt.capture_timestamp_ms = g_engine.current_time_ms;
        pkt.event_type = evt_type;
        pkt.priority = PRIORITY_NORMAL;
        pkt.snapshot = g_engine.current_bms;
        strncpy(pkt.reason, reason, TELEMETRY_REASON_MAX_LEN - 1);

        dispatch_packet(&pkt);
        g_engine.last_transmitted_bms = g_engine.current_bms;
        g_engine.last_packet_time_ms = g_engine.current_time_ms;
    } else {
        g_engine.metrics.deadband_suppressed_ticks++;
    }
}

void telemetry_inject_fault(uint16_t fault_id, uint8_t severity, const char *reason) {
    g_engine.current_bms.active_fault_id = fault_id;
    g_engine.current_bms.fault_severity = severity;

    if (severity >= 2) {
        g_engine.current_bms.fsm_state = BMS_STATE_FAILSAFE;
        g_engine.current_bms.relay_status_flags &= (uint8_t)~(RELAY_FLAG_MAIN_COMMANDED | RELAY_FLAG_MAIN_FEEDBACK);
    } else if (severity == 1) {
        g_engine.current_bms.fsm_state = BMS_STATE_DEGRADED;
    }

    telemetry_event_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.seq_id = g_engine.monotonic_seq_id++;
    pkt.capture_timestamp_ms = g_engine.current_time_ms;
    pkt.event_type = EVENT_TYPE_FAULT_ASSERTED;
    pkt.priority = (severity >= 2) ? PRIORITY_CRITICAL : PRIORITY_HIGH;
    pkt.snapshot = g_engine.current_bms;
    if (reason && reason[0] != '\0') {
        snprintf(pkt.reason, TELEMETRY_REASON_MAX_LEN, "FAULT: %s", reason);
    } else {
        snprintf(pkt.reason, TELEMETRY_REASON_MAX_LEN, "FAULT: 0x%04X (Sev %u)", fault_id, severity);
    }

    dispatch_packet(&pkt);
    g_engine.last_transmitted_bms = g_engine.current_bms;
    g_engine.last_packet_time_ms = g_engine.current_time_ms;
}

void telemetry_clear_fault(void) {
    if (g_engine.current_bms.active_fault_id != 0 || g_engine.current_bms.fsm_state != BMS_STATE_NORMAL) {
        g_engine.current_bms.active_fault_id = 0;
        g_engine.current_bms.fault_severity = 0;
        g_engine.current_bms.fsm_state = BMS_STATE_NORMAL;
        g_engine.current_bms.relay_status_flags = (RELAY_FLAG_MAIN_COMMANDED | RELAY_FLAG_MAIN_FEEDBACK);

        telemetry_event_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.seq_id = g_engine.monotonic_seq_id++;
        pkt.capture_timestamp_ms = g_engine.current_time_ms;
        pkt.event_type = EVENT_TYPE_FAULT_CLEARED;
        pkt.priority = PRIORITY_HIGH;
        pkt.snapshot = g_engine.current_bms;
        snprintf(pkt.reason, TELEMETRY_REASON_MAX_LEN, "FAULTS_CLEARED_NORMAL_RESTORED");

        dispatch_packet(&pkt);
        g_engine.last_transmitted_bms = g_engine.current_bms;
        g_engine.last_packet_time_ms = g_engine.current_time_ms;
    }
}

void telemetry_set_fsm_state(bms_fsm_state_t new_state, const char *reason) {
    if (g_engine.current_bms.fsm_state != new_state) {
        g_engine.current_bms.fsm_state = new_state;

        telemetry_event_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.seq_id = g_engine.monotonic_seq_id++;
        pkt.capture_timestamp_ms = g_engine.current_time_ms;
        pkt.event_type = EVENT_TYPE_STATE_CHANGE;
        pkt.priority = (new_state >= BMS_STATE_FAILSAFE) ? PRIORITY_CRITICAL : PRIORITY_HIGH;
        pkt.snapshot = g_engine.current_bms;
        if (reason && reason[0] != '\0') {
            snprintf(pkt.reason, TELEMETRY_REASON_MAX_LEN, "STATE->%u: %s", new_state, reason);
        } else {
            snprintf(pkt.reason, TELEMETRY_REASON_MAX_LEN, "STATE_CHANGE_TO_%u", new_state);
        }

        dispatch_packet(&pkt);
        g_engine.last_transmitted_bms = g_engine.current_bms;
        g_engine.last_packet_time_ms = g_engine.current_time_ms;
    }
}

void telemetry_set_relay_status(uint8_t relay_flags) {
    if (g_engine.current_bms.relay_status_flags != relay_flags) {
        uint8_t old_flags = g_engine.current_bms.relay_status_flags;
        g_engine.current_bms.relay_status_flags = relay_flags;

        telemetry_event_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.seq_id = g_engine.monotonic_seq_id++;
        pkt.capture_timestamp_ms = g_engine.current_time_ms;
        pkt.event_type = EVENT_TYPE_RELAY_CHANGE;
        pkt.priority = (relay_flags & (RELAY_FLAG_MISMATCH_WARN | RELAY_FLAG_WELDED_CRITICAL)) ? 
                       PRIORITY_CRITICAL : PRIORITY_HIGH;
        pkt.snapshot = g_engine.current_bms;
        snprintf(pkt.reason, TELEMETRY_REASON_MAX_LEN, "RELAY_STATUS_TRANSITION 0x%02X->0x%02X", old_flags, relay_flags);

        dispatch_packet(&pkt);
        g_engine.last_transmitted_bms = g_engine.current_bms;
        g_engine.last_packet_time_ms = g_engine.current_time_ms;
    }
}

void telemetry_set_wifi_physical_link(bool link_up) {
    g_engine.wifi.physical_link_up = link_up;
    if (!link_up && g_engine.wifi.state == WIFI_STATE_BLYNK_CONNECTED) {
        g_engine.outage_start_ms = g_engine.current_time_ms;
        g_engine.metrics.network_outage_count++;
    }
}

void telemetry_set_blynk_server_link(bool server_up) {
    g_engine.wifi.blynk_server_available = server_up;
    if (!server_up && g_engine.wifi.state == WIFI_STATE_BLYNK_CONNECTED) {
        g_engine.outage_start_ms = g_engine.current_time_ms;
        g_engine.metrics.network_outage_count++;
    }
}

void telemetry_set_rssi(int16_t raw_rssi_dbm) {
    g_engine.wifi.raw_rssi_dbm = raw_rssi_dbm;
    /* Exponential Moving Average Smoothing */
    int32_t smoothed = ((int32_t)g_engine.wifi.filtered_rssi_dbm * (100 - RSSI_EMA_WEIGHT_PCT) +
                        (int32_t)raw_rssi_dbm * RSSI_EMA_WEIGHT_PCT) / 100;
    g_engine.wifi.filtered_rssi_dbm = (int16_t)smoothed;

    rssi_quality_t old_qual = g_engine.wifi.rssi_quality;
    update_rssi_quality();

    /* Trigger event if RSSI quality tier changed significantly */
    if (g_engine.wifi.rssi_quality != old_qual && g_engine.wifi.state == WIFI_STATE_BLYNK_CONNECTED) {
        telemetry_event_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.seq_id = g_engine.monotonic_seq_id++;
        pkt.capture_timestamp_ms = g_engine.current_time_ms;
        pkt.event_type = EVENT_TYPE_NETWORK_STATUS;
        pkt.priority = (g_engine.wifi.rssi_quality >= RSSI_QUAL_POOR) ? PRIORITY_HIGH : PRIORITY_NORMAL;
        pkt.snapshot = g_engine.current_bms;
        snprintf(pkt.reason, TELEMETRY_REASON_MAX_LEN, "RSSI_TIER_CHANGED_%d->%d (%d dBm)",
                 old_qual, g_engine.wifi.rssi_quality, g_engine.wifi.filtered_rssi_dbm);

        dispatch_packet(&pkt);
        g_engine.last_packet_time_ms = g_engine.current_time_ms;
    }
}

wifi_fsm_state_t telemetry_wifi_get_state(void) {
    return g_engine.wifi.state;
}

int16_t telemetry_wifi_get_rssi(void) {
    return g_engine.wifi.filtered_rssi_dbm;
}

rssi_quality_t telemetry_wifi_get_quality(void) {
    return g_engine.wifi.rssi_quality;
}

bool telemetry_wifi_is_online(void) {
    return (g_engine.wifi.state == WIFI_STATE_BLYNK_CONNECTED);
}

uint32_t telemetry_wifi_get_reconnect_attempts(void) {
    return g_engine.wifi.reconnect_attempts;
}

uint16_t telemetry_queue_get_depth(void) {
    return g_engine.queue.count;
}

uint16_t telemetry_queue_get_peak(void) {
    return g_engine.queue.peak_depth;
}

uint32_t telemetry_queue_get_dropped(void) {
    return g_engine.queue.overflow_dropped;
}

bool telemetry_queue_peek_oldest(telemetry_event_packet_t *out_packet) {
    if (!out_packet || g_engine.queue.count == 0) return false;
    *out_packet = g_engine.queue.buffer[g_engine.queue.tail];
    return true;
}

void telemetry_get_metrics(telemetry_metrics_t *out_metrics) {
    if (!out_metrics) return;
    *out_metrics = g_engine.metrics;
}

/* ========================================================================== */
/* PRIVATE INTERNAL LOGIC                                                     */
/* ========================================================================== */

static void calculate_bms_extrema(bms_snapshot_t *snapshot) {
    if (!snapshot) return;

    uint32_t min_v = snapshot->cell_voltages_mv[0];
    uint32_t max_v = snapshot->cell_voltages_mv[0];
    uint8_t min_idx = 0;
    uint8_t max_idx = 0;
    uint32_t sum_v = 0;

    for (uint8_t i = 0; i < TELEMETRY_CELL_COUNT; i++) {
        uint32_t v = snapshot->cell_voltages_mv[i];
        sum_v += v;
        if (v < min_v) {
            min_v = v;
            min_idx = i;
        }
        if (v > max_v) {
            max_v = v;
            max_idx = i;
        }
    }

    snapshot->weakest_cell_idx = min_idx;
    snapshot->weakest_cell_mv = min_v;
    snapshot->strongest_cell_idx = max_idx;
    snapshot->strongest_cell_mv = max_v;
    snapshot->cell_delta_mv = (max_v >= min_v) ? (max_v - min_v) : 0;
    snapshot->pack_voltage_mv = sum_v;
}

static bool evaluate_bms_deadbands(const bms_snapshot_t *curr, const bms_snapshot_t *prev,
                                  char *reason, telemetry_event_type_t *evt_type) {
    /* 1. Check Extrema change (meaningful strongest / weakest cell index shifted) */
    if ((curr->cell_delta_mv >= TELEMETRY_CELL_DELTA_DEADBAND_MV || prev->cell_delta_mv >= TELEMETRY_CELL_DELTA_DEADBAND_MV) &&
        (curr->strongest_cell_idx != prev->strongest_cell_idx ||
         curr->weakest_cell_idx != prev->weakest_cell_idx)) {
        snprintf(reason, TELEMETRY_REASON_MAX_LEN, "EXTREMA_INDEX_SHIFT: Strongest=C%u Weakest=C%u",
                 curr->strongest_cell_idx + 1, curr->weakest_cell_idx + 1);
        *evt_type = EVENT_TYPE_EXTREMA_CHANGE;
        return true;
    }

    /* 2. Check Imbalance Delta Deadband */
    int32_t delta_diff = (int32_t)curr->cell_delta_mv - (int32_t)prev->cell_delta_mv;
    if (abs(delta_diff) >= (int32_t)TELEMETRY_CELL_DELTA_DEADBAND_MV) {
        snprintf(reason, TELEMETRY_REASON_MAX_LEN, "IMBALANCE_DELTA_SHIFT: %umV (diff %dmV)",
                 curr->cell_delta_mv, delta_diff);
        *evt_type = EVENT_TYPE_CELL_DELTA;
        return true;
    }

    /* 3. Check Individual Cell Deadbands */
    for (uint8_t i = 0; i < TELEMETRY_CELL_COUNT; i++) {
        int32_t v_diff = (int32_t)curr->cell_voltages_mv[i] - (int32_t)prev->cell_voltages_mv[i];
        if (abs(v_diff) >= (int32_t)TELEMETRY_CELL_VOLTAGE_DEADBAND_MV) {
            snprintf(reason, TELEMETRY_REASON_MAX_LEN, "CELL_%u_DELTA: %umV (diff %dmV)",
                     i + 1, curr->cell_voltages_mv[i], v_diff);
            *evt_type = EVENT_TYPE_CELL_DELTA;
            return true;
        }
    }

    /* 4. Check Pack Voltage Deadband */
    int32_t pack_diff = (int32_t)curr->pack_voltage_mv - (int32_t)prev->pack_voltage_mv;
    if (abs(pack_diff) >= (int32_t)TELEMETRY_PACK_VOLTAGE_DEADBAND_MV) {
        snprintf(reason, TELEMETRY_REASON_MAX_LEN, "PACK_VOLT_DELTA: %umV (diff %dmV)",
                 curr->pack_voltage_mv, pack_diff);
        *evt_type = EVENT_TYPE_CELL_DELTA;
        return true;
    }

    /* 5. Check Current Deadband */
    int32_t curr_diff = curr->pack_current_ma - prev->pack_current_ma;
    if (abs(curr_diff) >= (int32_t)TELEMETRY_CURRENT_DEADBAND_MA) {
        snprintf(reason, TELEMETRY_REASON_MAX_LEN, "PACK_CURRENT_DELTA: %dmA (diff %dmA)",
                 curr->pack_current_ma, curr_diff);
        *evt_type = EVENT_TYPE_CELL_DELTA;
        return true;
    }

    /* 6. Check Temperature Deadband */
    for (uint8_t i = 0; i < TELEMETRY_TEMP_SENSORS; i++) {
        int16_t t_diff = curr->temperatures_c[i] - prev->temperatures_c[i];
        if (abs(t_diff) >= (int16_t)TELEMETRY_TEMP_DEADBAND_C) {
            snprintf(reason, TELEMETRY_REASON_MAX_LEN, "TEMP_SENSOR_%u_DELTA: %dC (diff %dC)",
                     i + 1, curr->temperatures_c[i], t_diff);
            *evt_type = EVENT_TYPE_CELL_DELTA;
            return true;
        }
    }

    return false;
}

static void enqueue_event(const telemetry_event_packet_t *packet) {
    if (!packet) return;

    /* When queue is full: apply priority retention policy */
    if (g_engine.queue.count >= g_engine.queue.capacity) {
        /* If incoming packet is CRITICAL or HIGH, find lowest priority event to drop */
        if (packet->priority >= PRIORITY_HIGH) {
            int drop_idx = -1;
            telemetry_priority_t lowest_prio = packet->priority;

            for (uint16_t i = 0; i < g_engine.queue.count; i++) {
                uint16_t idx = (g_engine.queue.tail + i) % g_engine.queue.capacity;
                if (g_engine.queue.buffer[idx].priority < lowest_prio) {
                    lowest_prio = g_engine.queue.buffer[idx].priority;
                    drop_idx = idx;
                }
            }

            if (drop_idx != -1) {
                /* Replace the lowest-priority packet in-place */
                g_engine.queue.buffer[drop_idx] = *packet;
                g_engine.queue.buffer[drop_idx].origin = DATA_ORIGIN_QUEUED;
                g_engine.queue.overflow_dropped++;
                g_engine.queue.total_enqueued++;
                return;
            } else {
                /* Overwrite oldest at tail */
                g_engine.queue.tail = (g_engine.queue.tail + 1) % g_engine.queue.capacity;
                g_engine.queue.count--;
                g_engine.queue.overflow_dropped++;
            }
        } else {
            /* Routine low/normal delta event: drop incoming packet to preserve critical backlog */
            g_engine.queue.overflow_dropped++;
            return;
        }
    }

    /* Standard Enqueue */
    g_engine.queue.buffer[g_engine.queue.head] = *packet;
    g_engine.queue.buffer[g_engine.queue.head].origin = DATA_ORIGIN_QUEUED;
    g_engine.queue.head = (g_engine.queue.head + 1) % g_engine.queue.capacity;
    g_engine.queue.count++;
    g_engine.queue.total_enqueued++;

    if (g_engine.queue.count > g_engine.queue.peak_depth) {
        g_engine.queue.peak_depth = g_engine.queue.count;
    }
}

static bool dequeue_event(telemetry_event_packet_t *packet) {
    if (!packet || g_engine.queue.count == 0) return false;

    *packet = g_engine.queue.buffer[g_engine.queue.tail];
    g_engine.queue.tail = (g_engine.queue.tail + 1) % g_engine.queue.capacity;
    g_engine.queue.count--;
    g_engine.queue.total_dequeued++;
    return true;
}

static void dispatch_packet(telemetry_event_packet_t *packet) {
    if (!packet) return;

    g_engine.metrics.events_generated++;

    /* If online with Blynk AND no backlog is waiting, transmit directly as LIVE */
    if (g_engine.wifi.state == WIFI_STATE_BLYNK_CONNECTED && g_engine.queue.count == 0) {
        packet->origin = DATA_ORIGIN_LIVE;
        packet->transmit_timestamp_ms = g_engine.current_time_ms;

        if (g_engine.tx_callback) {
            g_engine.tx_callback(packet);
        }
        g_engine.metrics.packets_transmitted_live++;
        g_engine.metrics.total_bytes_transmitted += sizeof(telemetry_event_packet_t);
    } else {
        /* Disconnected or backlog actively draining: push into offline circular queue */
        packet->origin = DATA_ORIGIN_QUEUED;
        enqueue_event(packet);
    }
}

static void update_rssi_quality(void) {
    if (!g_engine.wifi.physical_link_up || g_engine.wifi.filtered_rssi_dbm < RSSI_THRESH_POOR_DBM) {
        g_engine.wifi.rssi_quality = RSSI_QUAL_LOST;
    } else if (g_engine.wifi.filtered_rssi_dbm >= RSSI_THRESH_EXCELLENT_DBM) {
        g_engine.wifi.rssi_quality = RSSI_QUAL_EXCELLENT;
    } else if (g_engine.wifi.filtered_rssi_dbm >= RSSI_THRESH_GOOD_DBM) {
        g_engine.wifi.rssi_quality = RSSI_QUAL_GOOD;
    } else if (g_engine.wifi.filtered_rssi_dbm >= RSSI_THRESH_FAIR_DBM) {
        g_engine.wifi.rssi_quality = RSSI_QUAL_FAIR;
    } else {
        g_engine.wifi.rssi_quality = RSSI_QUAL_POOR;
    }
}

static uint32_t compute_backoff_ms(uint32_t attempts) {
    if (attempts == 0) return WIFI_INITIAL_BACKOFF_MS;

    uint32_t backoff = WIFI_INITIAL_BACKOFF_MS;
    for (uint32_t i = 1; i < attempts; i++) {
        backoff *= WIFI_BACKOFF_MULTIPLIER;
        if (backoff >= WIFI_MAX_BACKOFF_MS) {
            backoff = WIFI_MAX_BACKOFF_MS;
            break;
        }
    }

    /* Add pseudo-random deterministic jitter (±15%) */
    int32_t jitter_range = (int32_t)(backoff * WIFI_JITTER_PCT) / 100;
    int32_t jitter = (attempts % 2 == 0) ? jitter_range : -jitter_range;
    return (uint32_t)((int32_t)backoff + jitter);
}

static void update_wifi_fsm(uint32_t now_ms) {
    uint32_t elapsed_in_state = now_ms - g_engine.wifi.state_entry_time_ms;

    switch (g_engine.wifi.state) {
        case WIFI_STATE_DISCONNECTED:
            if (g_engine.wifi.physical_link_up) {
                g_engine.wifi.state = WIFI_STATE_INIT;
                g_engine.wifi.state_entry_time_ms = now_ms;
            }
            break;

        case WIFI_STATE_INIT:
            if (!g_engine.wifi.physical_link_up) {
                g_engine.wifi.state = WIFI_STATE_DISCONNECTED;
                g_engine.wifi.state_entry_time_ms = now_ms;
            } else if (elapsed_in_state >= WIFI_INIT_DWELL_MS) {
                g_engine.wifi.state = WIFI_STATE_CONNECTING;
                g_engine.wifi.state_entry_time_ms = now_ms;
            }
            break;

        case WIFI_STATE_CONNECTING:
            if (!g_engine.wifi.physical_link_up) {
                g_engine.wifi.reconnect_attempts++;
                g_engine.wifi.backoff_interval_ms = compute_backoff_ms(g_engine.wifi.reconnect_attempts);
                g_engine.wifi.state = WIFI_STATE_BACKOFF_WAIT;
                g_engine.wifi.state_entry_time_ms = now_ms;
            } else if (elapsed_in_state >= 300) { /* AP connection successful */
                g_engine.wifi.state = WIFI_STATE_CONNECTED;
                g_engine.wifi.state_entry_time_ms = now_ms;
            } else if (elapsed_in_state >= WIFI_CONNECT_TIMEOUT_MS) {
                g_engine.wifi.reconnect_attempts++;
                g_engine.wifi.backoff_interval_ms = compute_backoff_ms(g_engine.wifi.reconnect_attempts);
                g_engine.wifi.state = WIFI_STATE_BACKOFF_WAIT;
                g_engine.wifi.state_entry_time_ms = now_ms;
            }
            break;

        case WIFI_STATE_CONNECTED:
            if (!g_engine.wifi.physical_link_up) {
                g_engine.wifi.state = WIFI_STATE_DISCONNECTED;
                g_engine.wifi.state_entry_time_ms = now_ms;
            } else if (g_engine.wifi.blynk_server_available) {
                g_engine.wifi.state = WIFI_STATE_BLYNK_CONNECTING;
                g_engine.wifi.state_entry_time_ms = now_ms;
            }
            break;

        case WIFI_STATE_BLYNK_CONNECTING:
            if (!g_engine.wifi.physical_link_up || !g_engine.wifi.blynk_server_available) {
                g_engine.wifi.reconnect_attempts++;
                g_engine.wifi.backoff_interval_ms = compute_backoff_ms(g_engine.wifi.reconnect_attempts);
                g_engine.wifi.state = WIFI_STATE_BACKOFF_WAIT;
                g_engine.wifi.state_entry_time_ms = now_ms;
            } else if (elapsed_in_state >= 200) { /* Blynk session handshake complete */
                g_engine.wifi.state = WIFI_STATE_BLYNK_CONNECTED;
                g_engine.wifi.state_entry_time_ms = now_ms;
                g_engine.online_stable_start_ms = now_ms;

                if (g_engine.outage_start_ms > 0) {
                    g_engine.metrics.total_outage_duration_ms += (now_ms - g_engine.outage_start_ms);
                    g_engine.outage_start_ms = 0;
                }
            } else if (elapsed_in_state >= WIFI_BLYNK_TIMEOUT_MS) {
                g_engine.wifi.reconnect_attempts++;
                g_engine.wifi.backoff_interval_ms = compute_backoff_ms(g_engine.wifi.reconnect_attempts);
                g_engine.wifi.state = WIFI_STATE_BACKOFF_WAIT;
                g_engine.wifi.state_entry_time_ms = now_ms;
            }
            break;

        case WIFI_STATE_BLYNK_CONNECTED:
            if (!g_engine.wifi.physical_link_up || !g_engine.wifi.blynk_server_available) {
                g_engine.wifi.reconnect_attempts++;
                g_engine.wifi.backoff_interval_ms = compute_backoff_ms(g_engine.wifi.reconnect_attempts);
                g_engine.wifi.state = WIFI_STATE_BACKOFF_WAIT;
                g_engine.wifi.state_entry_time_ms = now_ms;
            } else if (now_ms - g_engine.online_stable_start_ms >= WIFI_STABLE_RESET_MS) {
                /* Connection is proven stable: reset retry backoff counters */
                g_engine.wifi.reconnect_attempts = 0;
                g_engine.wifi.backoff_interval_ms = WIFI_INITIAL_BACKOFF_MS;
            }
            break;

        case WIFI_STATE_BACKOFF_WAIT:
            if (g_engine.wifi.reconnect_attempts >= WIFI_MAX_CONSECUTIVE_FAILURES) {
                g_engine.wifi.state = WIFI_STATE_FATAL_RECOVERY;
                g_engine.wifi.state_entry_time_ms = now_ms;
            } else if (elapsed_in_state >= g_engine.wifi.backoff_interval_ms) {
                g_engine.wifi.state = WIFI_STATE_INIT;
                g_engine.wifi.state_entry_time_ms = now_ms;
            }
            break;

        case WIFI_STATE_FATAL_RECOVERY:
            if (elapsed_in_state >= 500) { /* Simulated radio reboot dwell */
                g_engine.wifi.reconnect_attempts = 1;
                g_engine.wifi.backoff_interval_ms = WIFI_INITIAL_BACKOFF_MS;
                g_engine.wifi.state = WIFI_STATE_INIT;
                g_engine.wifi.state_entry_time_ms = now_ms;
            }
            break;

        default:
            g_engine.wifi.state = WIFI_STATE_DISCONNECTED;
            g_engine.wifi.state_entry_time_ms = now_ms;
            break;
    }
}
