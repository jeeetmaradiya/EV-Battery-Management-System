/**
 * @file telemetry_engine.h
 * @brief Public Interface for Event-Driven Telemetry Engine and Wi-Fi Controller.
 * 
 * Safety Critical Embedded System Architecture (ISO 26262 ASIL-C / MISRA-C:2012).
 */

#ifndef TELEMETRY_ENGINE_H
#define TELEMETRY_ENGINE_H

#include "telemetry_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Outbound packet transmission callback prototype.
 * Invoked whenever a packet is dispatched (either LIVE real-time or QUEUED backlog).
 * 
 * @param packet Pointer to the telemetry event packet ready for Blynk serialization.
 */
typedef void (*telemetry_tx_callback_t)(const telemetry_event_packet_t *packet);

/* ========================================================================== */
/* ENGINE LIFECYCLE MANAGEMENT                                                */
/* ========================================================================== */

/**
 * @brief Initialize the telemetry engine, circular offline queue, and Wi-Fi state machine.
 */
void telemetry_engine_init(void);

/**
 * @brief Periodic non-blocking engine tick. Must be invoked at fixed intervals (e.g. 10ms).
 * Handles deadband evaluation, keepalive timer, Wi-Fi reconnection FSM, and queue draining.
 * 
 * @param current_time_ms Current monotonic system uptime in milliseconds.
 */
void telemetry_engine_tick(uint32_t current_time_ms);

/**
 * @brief Register callback for outbound packet transmission.
 */
void telemetry_register_tx_callback(telemetry_tx_callback_t callback);

/* ========================================================================== */
/* SUBSYSTEM INPUTS & EVENT INJECTION                                         */
/* ========================================================================== */

/**
 * @brief Feed updated BMS sensor snapshot into engine.
 * Automatically evaluates deadbands, updates extrema, and triggers event if thresholds crossed.
 * 
 * @param snapshot Pointer to the latest battery readings.
 */
void telemetry_update_bms(const bms_snapshot_t *snapshot);

/**
 * @brief Force injection of a fault condition for testing.
 * Immediately produces a high-priority or critical telemetry event.
 * 
 * @param fault_id 16-bit fault identifier.
 * @param severity Severity level (0..3).
 * @param reason Human-readable fault description.
 */
void telemetry_inject_fault(uint16_t fault_id, uint8_t severity, const char *reason);

/**
 * @brief Clear active faults and restore normal state.
 */
void telemetry_clear_fault(void);

/**
 * @brief Update the BMS operational state machine state.
 * Generates an immediate state change telemetry event.
 */
void telemetry_set_fsm_state(bms_fsm_state_t new_state, const char *reason);

/**
 * @brief Update the relay status flags.
 * Generates an event if contactor states or mismatch flags change.
 */
void telemetry_set_relay_status(uint8_t relay_flags);

/* ========================================================================== */
/* NETWORK & WI-FI CONTROLLER INTERFACE                                       */
/* ========================================================================== */

/**
 * @brief Update physical RF link status (simulates antenna link loss or restoration).
 */
void telemetry_set_wifi_physical_link(bool link_up);

/**
 * @brief Update Blynk cloud server connectivity flag.
 */
void telemetry_set_blynk_server_link(bool server_up);

/**
 * @brief Feed raw RSSI sample into engine for non-blocking exponential smoothing.
 * 
 * @param raw_rssi_dbm Raw RSSI value in dBm (-100 to 0).
 */
void telemetry_set_rssi(int16_t raw_rssi_dbm);

/**
 * @brief Query current Wi-Fi reconnection state machine state.
 */
wifi_fsm_state_t telemetry_wifi_get_state(void);

/**
 * @brief Query smoothed RSSI value in dBm.
 */
int16_t telemetry_wifi_get_rssi(void);

/**
 * @brief Query classified signal quality tier.
 */
rssi_quality_t telemetry_wifi_get_quality(void);

/**
 * @brief Check if fully connected to Blynk cloud (online and ready for live stream).
 */
bool telemetry_wifi_is_online(void);

/**
 * @brief Query number of consecutive failed reconnection attempts.
 */
uint32_t telemetry_wifi_get_reconnect_attempts(void);

/* ========================================================================== */
/* OFFLINE QUEUE & METRICS ACCESS                                             */
/* ========================================================================== */

/**
 * @brief Query current number of queued offline events awaiting uplink.
 */
uint16_t telemetry_queue_get_depth(void);

/**
 * @brief Query peak historical depth of offline queue.
 */
uint16_t telemetry_queue_get_peak(void);

/**
 * @brief Query count of lower-priority events dropped due to queue overflow.
 */
uint32_t telemetry_queue_get_dropped(void);

/**
 * @brief Peek at the oldest packet currently queued without dequeuing.
 */
bool telemetry_queue_peek_oldest(telemetry_event_packet_t *out_packet);

/**
 * @brief Retrieve comprehensive telemetry performance metrics.
 */
void telemetry_get_metrics(telemetry_metrics_t *out_metrics);

/**
 * @brief Reset all internal state machines, queues, and metrics (for test isolation).
 */
void telemetry_engine_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_ENGINE_H */
