/**
 * @file telemetry_types.h
 * @brief Formal Type Definitions for Event-Driven Telemetry and Live Blynk Dashboard.
 * 
 * Compliant with ISO 26262 ASIL-C / MISRA-C:2012 embedded standards.
 * Defines telemetry event types, priority tiers, circular buffer layout,
 * non-blocking Wi-Fi state machine states, and Blynk virtual pin assignments.
 */

#ifndef TELEMETRY_TYPES_H
#define TELEMETRY_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "telemetry_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* EVENT TYPE & PRIORITY ENUMERATIONS                                         */
/* ========================================================================== */

/**
 * @brief Categorization of telemetry trigger events.
 * Telemetry packets are generated strictly on meaningful events or parameter changes.
 */
typedef enum {
    EVENT_TYPE_NONE             = 0,
    EVENT_TYPE_HEARTBEAT        = 1,  /**< Periodic keepalive fallback when quiescent */
    EVENT_TYPE_CELL_DELTA       = 2,  /**< Significant cell voltage change (|ΔV| >= deadband) */
    EVENT_TYPE_EXTREMA_CHANGE   = 3,  /**< Strongest or weakest cell index/voltage shifted */
    EVENT_TYPE_STATE_CHANGE     = 4,  /**< Operating state transition (NORMAL/DEGRADED/etc.) */
    EVENT_TYPE_FAULT_ASSERTED   = 5,  /**< Hardware or safety fault triggered */
    EVENT_TYPE_FAULT_CLEARED    = 6,  /**< Hardware or safety fault cleared */
    EVENT_TYPE_RELAY_CHANGE     = 7,  /**< Main contactor or precharge relay status toggle */
    EVENT_TYPE_NETWORK_STATUS   = 8,  /**< Wi-Fi / Blynk connection status or RSSI band shift */
    EVENT_TYPE_QUEUE_FLUSH      = 9   /**< Transition from offline backlog to live mode */
} telemetry_event_type_t;

/**
 * @brief Telemetry priority levels for queue scheduling and retention under overflow.
 */
typedef enum {
    PRIORITY_LOW      = 0,  /**< Routine periodic keepalive / diagnostics */
    PRIORITY_NORMAL   = 1,  /**< Delta deadband triggers, RSSI transitions */
    PRIORITY_HIGH     = 2,  /**< Non-critical warnings, relay status changes */
    PRIORITY_CRITICAL = 3   /**< Critical faults, emergency disconnects, failsafe */
} telemetry_priority_t;

/**
 * @brief Data origin indicator to allow operators to distinguish live vs backlog data.
 */
typedef enum {
    DATA_ORIGIN_LIVE    = 0,  /**< Real-time instantaneous telemetry */
    DATA_ORIGIN_QUEUED  = 1   /**< Historical telemetry captured during network outage */
} telemetry_origin_t;

/* ========================================================================== */
/* BMS & SAFETY STATUS ENUMERATIONS                                           */
/* ========================================================================== */

/**
 * @brief Battery Management Operating State Machine (ISO 26262 ASIL-C).
 */
typedef enum {
    BMS_STATE_NORMAL    = 0,  /**< All parameters within nominal thresholds */
    BMS_STATE_DEGRADED  = 1,  /**< Non-critical advisory / derated operation */
    BMS_STATE_FAILSAFE  = 2,  /**< Critical fault: High-voltage contactor tripped */
    BMS_STATE_SHUTDOWN  = 3   /**< Terminal lockout: Manual service reset required */
} bms_fsm_state_t;

/**
 * @brief Relay status bitfield flags.
 */
typedef enum {
    RELAY_FLAG_MAIN_COMMANDED   = (1 << 0),  /**< Main contactor coil energized */
    RELAY_FLAG_MAIN_FEEDBACK    = (1 << 1),  /**< Main auxiliary contact closed */
    RELAY_FLAG_PRECHARGE_CMD    = (1 << 2),  /**< Precharge relay coil energized */
    RELAY_FLAG_PRECHARGE_FB     = (1 << 3),  /**< Precharge auxiliary contact closed */
    RELAY_FLAG_MISMATCH_WARN    = (1 << 4),  /**< Commanded vs feedback mismatch */
    RELAY_FLAG_WELDED_CRITICAL  = (1 << 5)   /**< Commanded OPEN but feedback CLOSED */
} relay_status_flags_t;

/* ========================================================================== */
/* NON-BLOCKING WI-FI STATE MACHINE & RSSI                                    */
/* ========================================================================== */

/**
 * @brief Non-blocking Wi-Fi reconnection state machine states.
 * Executes deterministically without thread blocking or delay() calls.
 */
typedef enum {
    WIFI_STATE_DISCONNECTED     = 0,  /**< Radio interface idle or unlinked */
    WIFI_STATE_INIT             = 1,  /**< Non-blocking radio stack initialization */
    WIFI_STATE_CONNECTING       = 2,  /**< Non-blocking AP association and DHCP lease */
    WIFI_STATE_CONNECTED        = 3,  /**< Wi-Fi link up; IP assigned */
    WIFI_STATE_BLYNK_CONNECTING = 4,  /**< Handshake with Blynk IoT server in progress */
    WIFI_STATE_BLYNK_CONNECTED  = 5,  /**< Fully authenticated; real-time pipe active */
    WIFI_STATE_BACKOFF_WAIT     = 6,  /**< Non-blocking exponential backoff dwell time */
    WIFI_STATE_FATAL_RECOVERY   = 7   /**< Exceeded backoff thresholds; hardware reset */
} wifi_fsm_state_t;

/**
 * @brief RSSI communication quality classifications.
 */
typedef enum {
    RSSI_QUAL_EXCELLENT = 0,  /**< RSSI >= -60 dBm (Signal: 90-100%) */
    RSSI_QUAL_GOOD      = 1,  /**< -70 dBm <= RSSI < -60 dBm (Signal: 70-89%) */
    RSSI_QUAL_FAIR      = 2,  /**< -80 dBm <= RSSI < -70 dBm (Signal: 45-69%) */
    RSSI_QUAL_POOR      = 3,  /**< -90 dBm <= RSSI < -80 dBm (Signal: 15-44%) */
    RSSI_QUAL_LOST      = 4   /**< RSSI < -90 dBm or disconnected (Signal: 0%) */
} rssi_quality_t;

/* ========================================================================== */
/* BLYNK VIRTUAL PIN ASSIGNMENTS                                              */
/* ========================================================================== */

typedef enum {
    BLYNK_VPIN_PACK_VOLTAGE     = 0,   /**< V0: Pack Total Voltage (V) */
    BLYNK_VPIN_PACK_CURRENT     = 1,   /**< V1: Pack Current (A) */
    BLYNK_VPIN_PACK_TEMP        = 2,   /**< V2: Maximum Temperature (°C) */
    BLYNK_VPIN_WEAKEST_VOLT     = 3,   /**< V3: Weakest Cell Voltage (mV) */
    BLYNK_VPIN_STRONGEST_VOLT   = 4,   /**< V4: Strongest Cell Voltage (mV) */
    BLYNK_VPIN_CELL_DELTA       = 5,   /**< V5: Cell Imbalance Delta (mV) */
    BLYNK_VPIN_STRONGEST_INDEX  = 6,   /**< V6: Strongest Cell 1-based Index */
    BLYNK_VPIN_WEAKEST_INDEX    = 7,   /**< V7: Weakest Cell 1-based Index */
    BLYNK_VPIN_FSM_STATE        = 8,   /**< V8: BMS FSM State (0..3) */
    BLYNK_VPIN_ACTIVE_FAULT     = 9,   /**< V9: Active Fault ID (Hex/Int) */
    BLYNK_VPIN_RELAY_STATUS     = 10,  /**< V10: Relay Status Bitfield */
    BLYNK_VPIN_WIFI_RSSI        = 11,  /**< V11: Filtered RSSI (dBm) */
    BLYNK_VPIN_QUEUE_DEPTH      = 12,  /**< V12: Offline Queue Depth */
    BLYNK_VPIN_STREAM_MODE      = 13,  /**< V13: Telemetry Stream Origin (0=LIVE, 1=QUEUED) */
    BLYNK_VPIN_SEQ_ID           = 14,  /**< V14: Monotonic Packet Sequence ID */
    BLYNK_VPIN_WIFI_STATE       = 15,  /**< V15: Non-blocking Wi-Fi FSM State */
    BLYNK_VPIN_CELL_BASE        = 16,  /**< V16..V27: Cell Voltages (mV) for Cells 1..12 */
    BLYNK_VPIN_DROPPED_PACKETS  = 28,  /**< V28: Offline Queue Dropped Packets */
    BLYNK_VPIN_DRAIN_PCT        = 29   /**< V29: Backlog Drain Progress Percentage */
} blynk_vpin_t;

/* ========================================================================== */
/* DATA STRUCTURES                                                            */
/* ========================================================================== */

/**
 * @brief Instantaneous snapshot of battery subsystem parameters.
 */
typedef struct {
    uint32_t cell_voltages_mv[TELEMETRY_CELL_COUNT]; /**< Cell voltages in mV */
    uint32_t pack_voltage_mv;                       /**< Total pack voltage in mV */
    int32_t  pack_current_ma;                       /**< Current in mA (+ Charge, - Discharge) */
    int16_t  temperatures_c[TELEMETRY_TEMP_SENSORS];/**< Temperature readings in °C */
    uint8_t  strongest_cell_idx;                    /**< 0-based index of highest voltage cell */
    uint8_t  weakest_cell_idx;                      /**< 0-based index of lowest voltage cell */
    uint32_t strongest_cell_mv;                     /**< Voltage of strongest cell */
    uint32_t weakest_cell_mv;                       /**< Voltage of weakest cell */
    uint32_t cell_delta_mv;                         /**< Max cell - Min cell delta in mV */
    bms_fsm_state_t fsm_state;                      /**< Operational state */
    uint16_t active_fault_id;                       /**< 16-bit fault code (0 if none) */
    uint8_t  fault_severity;                        /**< Fault severity level (0..3) */
    uint8_t  relay_status_flags;                    /**< Bitfield of relay states */
} bms_snapshot_t;

/**
 * @brief Discrete telemetry event packet formatted for transmission or offline queueing.
 */
typedef struct {
    uint32_t seq_id;                                /**< Monotonic sequence identifier */
    uint32_t capture_timestamp_ms;                  /**< Tick timestamp when event occurred */
    uint32_t transmit_timestamp_ms;                 /**< Tick timestamp when transmitted */
    telemetry_event_type_t event_type;              /**< Trigger classification */
    telemetry_priority_t priority;                  /**< Scheduling priority */
    telemetry_origin_t origin;                      /**< LIVE (0) vs QUEUED (1) */
    bms_snapshot_t snapshot;                        /**< Full subsystem telemetry payload */
    char reason[TELEMETRY_REASON_MAX_LEN];          /**< Null-terminated event description */
} telemetry_event_packet_t;

/**
 * @brief Fixed-size circular queue (ring buffer) for offline telemetry persistence.
 */
typedef struct {
    telemetry_event_packet_t buffer[OFFLINE_QUEUE_CAPACITY];
    uint16_t head;                                  /**< Write index */
    uint16_t tail;                                  /**< Read index */
    uint16_t count;                                 /**< Current number of stored events */
    uint16_t capacity;                              /**< Maximum storage slots */
    uint32_t total_enqueued;                        /**< Cumulative events entered into queue */
    uint32_t total_dequeued;                        /**< Cumulative events drained from queue */
    uint32_t overflow_dropped;                      /**< Count of lower-priority dropped events */
    uint16_t peak_depth;                            /**< High watermark of queue utilization */
} offline_queue_t;

/**
 * @brief Wi-Fi reconnection state machine control block.
 */
typedef struct {
    wifi_fsm_state_t state;                         /**< Current state machine state */
    uint32_t state_entry_time_ms;                   /**< Timestamp when current state was entered */
    uint32_t backoff_interval_ms;                   /**< Current exponential backoff interval */
    uint32_t reconnect_attempts;                    /**< Consecutive failed reconnection attempts */
    int16_t  raw_rssi_dbm;                          /**< Instantaneous RSSI reading */
    int16_t  filtered_rssi_dbm;                     /**< Smoothed RSSI reading */
    rssi_quality_t rssi_quality;                    /**< Classified signal strength tier */
    bool     physical_link_up;                      /**< Physical RF link status flag */
    bool     blynk_server_available;                /**< Blynk cloud server reachability flag */
} wifi_controller_t;

/**
 * @brief Comprehensive telemetry engine metrics.
 */
typedef struct {
    uint32_t events_generated;                      /**< Total meaningful events identified */
    uint32_t deadband_suppressed_ticks;             /**< Ticks skipped due to sub-deadband changes */
    uint32_t packets_transmitted_live;              /**< Real-time packets sent directly */
    uint32_t packets_transmitted_queued;            /**< Offline backlog packets flushed */
    uint32_t total_bytes_transmitted;               /**< Estimated serialized bytes uplinked */
    uint32_t network_outage_count;                  /**< Total network drop events */
    uint32_t total_outage_duration_ms;              /**< Total time spent in disconnected states */
} telemetry_metrics_t;

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_TYPES_H */
