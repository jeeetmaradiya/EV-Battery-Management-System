/**
 * @file telemetry_config.h
 * @brief Configuration Constants and Tunable Parameters for Telemetry Engine.
 * 
 * Safety Critical Embedded System Architecture (ISO 26262 ASIL-C / MISRA-C:2012).
 */

#ifndef TELEMETRY_CONFIG_H
#define TELEMETRY_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* HARDWARE & SYSTEM TOPOLOGY                                                 */
/* ========================================================================== */

#define TELEMETRY_CELL_COUNT                12      /**< 12-Series Li-Ion Battery Pack */
#define TELEMETRY_TEMP_SENSORS              4       /**< 4 Surface NTC Thermistors */
#define TELEMETRY_TICK_MS                   10      /**< Engine periodic step interval in ms */
#define TELEMETRY_REASON_MAX_LEN            64      /**< Event annotation string buffer length */

/* ========================================================================== */
/* EVENT-DRIVEN DEADBAND THRESHOLDS                                           */
/* Telemetry is transmitted ONLY when changes exceed these deadband limits    */
/* ========================================================================== */

#define TELEMETRY_CELL_VOLTAGE_DEADBAND_MV  25      /**< Cell voltage delta threshold (mV) */
#define TELEMETRY_CELL_DELTA_DEADBAND_MV    15      /**< Weakest/strongest delta deadband (mV) */
#define TELEMETRY_PACK_VOLTAGE_DEADBAND_MV  250     /**< Total pack voltage change (mV) */
#define TELEMETRY_CURRENT_DEADBAND_MA       800     /**< Pack current change threshold (mA) */
#define TELEMETRY_TEMP_DEADBAND_C           2       /**< Cell pack temperature deadband (°C) */
#define TELEMETRY_HEARTBEAT_INTERVAL_MS     10000   /**< Quiescent keepalive heartbeat (10s) */

/* ========================================================================== */
/* OFFLINE RING BUFFER STORAGE & REPLAY PACING                                */
/* ========================================================================== */

#define OFFLINE_QUEUE_CAPACITY              64      /**< Fixed-size circular queue capacity */
#define TELEMETRY_DRAIN_BURST_MAX           2       /**< Max packets transmitted per cycle */
#define TELEMETRY_DRAIN_INTERVAL_MS         50      /**< Pacing interval during queue replay */
#define OFFLINE_QUEUE_HIGH_WATERMARK        50      /**< Warning threshold for queue depth */

/* ========================================================================== */
/* NON-BLOCKING WI-FI STATE MACHINE TIMINGS                                   */
/* ========================================================================== */

#define WIFI_INIT_DWELL_MS                  100     /**< Radio warm-up duration */
#define WIFI_CONNECT_TIMEOUT_MS             2500    /**< Max wait for AP association */
#define WIFI_BLYNK_TIMEOUT_MS               2000    /**< Max wait for Blynk handshake */
#define WIFI_INITIAL_BACKOFF_MS             1000    /**< First retry backoff (1s) */
#define WIFI_MAX_BACKOFF_MS                 16000   /**< Maximum exponential backoff cap (16s) */
#define WIFI_BACKOFF_MULTIPLIER             2       /**< Binary exponential backoff factor */
#define WIFI_JITTER_PCT                     15      /**< Pseudo-random timing jitter percentage */
#define WIFI_STABLE_RESET_MS                5000    /**< Stable connection time to reset backoff */
#define WIFI_MAX_CONSECUTIVE_FAILURES       6       /**< Triggers fatal radio restart */

/* ========================================================================== */
/* RSSI FILTERING & QUALITY CLASSIFICATION                                    */
/* ========================================================================== */

#define RSSI_THRESH_EXCELLENT_DBM           (-60)   /**< Excellent link: >= -60 dBm */
#define RSSI_THRESH_GOOD_DBM                (-70)   /**< Good link: -70 dBm .. -61 dBm */
#define RSSI_THRESH_FAIR_DBM                (-80)   /**< Fair link: -80 dBm .. -71 dBm */
#define RSSI_THRESH_POOR_DBM                (-90)   /**< Poor link: -90 dBm .. -81 dBm */
#define RSSI_EMA_WEIGHT_PCT                 25      /**< Exponential moving average weight (25%) */

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_CONFIG_H */
