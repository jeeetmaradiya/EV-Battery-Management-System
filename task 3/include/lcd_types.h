/**
 * @file lcd_types.h
 * @brief Data Types, Structures, and Enums for the Flicker-Free LCD Display Engine.
 * 
 * Compliant with MISRA-C:2012 and embedded safety standards.
 */

#ifndef LCD_TYPES_H
#define LCD_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "lcd_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* ENUMERATIONS                                                               */
/* ========================================================================== */

/**
 * @brief Display Information Pages for Cyclical Rotation
 */
typedef enum {
    LCD_PAGE_BATTERY    = 0,  /**< Battery Pack: Voltage, Current, SOC %, Temp, Charging State */
    LCD_PAGE_SYSTEM     = 1,  /**< System State: Mode, Uptime, CPU Load, RAM, Watchdog Heartbeat */
    LCD_PAGE_TELEMETRY  = 2,  /**< Telemetry: RF Signal, Packet Loss %, Comm Bus Status, Errors */
    LCD_PAGE_INVALID    = 3,  /**< Sentinel / Boundary count */
    LCD_PAGE_FAULT      = 0xFF /**< Dedicated Emergency Critical Fault Screen Override */
} LcdPageId;

/**
 * @brief Engine Finite State Machine (FSM) States
 */
typedef enum {
    LCD_STATE_UNINITIALIZED   = 0,  /**< Hardware/buffers not ready */
    LCD_STATE_NORMAL_ROTATION = 1,  /**< Active non-blocking cyclical page rotation */
    LCD_STATE_PAGE_PAUSED     = 2,  /**< Manual operator inspection (rotation paused on single page) */
    LCD_STATE_FAULT_OVERRIDE  = 3,  /**< Immediate override: locked on dedicated fault screen */
    LCD_STATE_STANDBY         = 4   /**< Display dimmed / low-power sleep */
} LcdEngineState;

/**
 * @brief Fault Severity Levels
 */
typedef enum {
    FAULT_SEV_NONE     = 0,  /**< No active fault condition */
    FAULT_SEV_WARNING  = 1,  /**< Non-critical advisory */
    FAULT_SEV_CRITICAL = 2,  /**< Safety trip: triggers immediate LCD preemption override */
    FAULT_SEV_LOCKOUT  = 3   /**< Fatal emergency condition: requires full manual power-cycle */
} FaultSeverity;

/**
 * @brief Standard System Fault Diagnostic Codes
 */
typedef enum {
    FAULT_CODE_NONE             = 0x00,
    FAULT_CODE_OVER_VOLTAGE      = 0x11,  /**< Over-Voltage trip (e.g. > 58.4V pack) */
    FAULT_CODE_UNDER_VOLTAGE     = 0x12,  /**< Under-Voltage cut-off (< 40.0V) */
    FAULT_CODE_OVER_CURRENT      = 0x21,  /**< Over-Current discharge overload */
    FAULT_CODE_SHORT_CIRCUIT     = 0x22,  /**< Direct short-circuit detected */
    FAULT_CODE_OVER_TEMPERATURE  = 0x31,  /**< Thermal runaway limit breached (> 65°C) */
    FAULT_CODE_UNDER_TEMPERATURE = 0x32,  /**< Low temperature charging lock (< 0°C) */
    FAULT_CODE_COMM_LOST         = 0x41,  /**< CAN/RS485 Telemetry link severed */
    FAULT_CODE_WATCHDOG_TRIP     = 0x51,  /**< Safety MCU Watchdog missed heartbeat */
    FAULT_CODE_EMERGENCY_STOP    = 0x99   /**< Physical E-Stop Mushroom pressed */
} LcdFaultCode;

/**
 * @brief Battery State of Operation
 */
typedef enum {
    BATTERY_STATE_STANDBY     = 0,
    BATTERY_STATE_CHARGING    = 1,
    BATTERY_STATE_DISCHARGING = 2,
    BATTERY_STATE_BALANCING   = 3,
    BATTERY_STATE_FAULTED     = 4
} BatteryState;

/**
 * @brief System Controller Operating Mode
 */
typedef enum {
    SYS_MODE_BOOT        = 0,
    SYS_MODE_RUN_AUTO    = 1,
    SYS_MODE_MAINTENANCE = 2,
    SYS_MODE_DEGRADED    = 3,
    SYS_MODE_EMERGENCY   = 4
} SystemMode;

/* ========================================================================== */
/* TELEMETRY DATA STRUCTURES                                                  */
/* ========================================================================== */

/**
 * @brief Battery Pack Telemetry Data
 */
typedef struct {
    uint32_t voltage_mv;        /**< Pack voltage in millivolts (e.g., 52400 = 52.40V) */
    int32_t  current_ma;        /**< Pack current in milliamps (positive = charge, negative = discharge) */
    uint8_t  soc_pct;           /**< State of Charge percentage (0 - 100%) */
    int8_t   temp_degc;         /**< Core cell temperature (-40 to +125°C) */
    BatteryState state;         /**< Current operational status */
    uint16_t cycle_count;       /**< Battery charge cycle count */
} BatteryTelemetry;

/**
 * @brief Controller System Health & Resource Telemetry
 */
typedef struct {
    SystemMode mode;            /**< Controller operational mode */
    uint32_t   uptime_sec;      /**< Controller runtime in seconds */
    uint8_t    cpu_load_pct;    /**< Microcontroller CPU utilization (0 - 100%) */
    uint8_t    ram_used_pct;    /**< Static RAM memory pool utilization (0 - 100%) */
    uint16_t   loop_freq_hz;    /**< Real-time scheduler tick frequency in Hz */
    bool       watchdog_ok;     /**< Hardware window watchdog status */
} SystemTelemetry;

/**
 * @brief Telemetry & Communication Link Statistics
 */
typedef struct {
    int8_t   rssi_dbm;          /**< Wireless link RSSI in dBm (-120 to 0) */
    uint8_t  snr_db;            /**< Signal-to-noise ratio in dB */
    uint32_t packets_tx;        /**< Total packets transmitted */
    uint32_t packets_rx;        /**< Total valid packets received */
    uint16_t packet_loss_x100;  /**< Packet loss percentage scaled by 100 (e.g. 4 = 0.04%) */
    uint16_t bus_crc_errors;    /**< Low-level bus CRC / parity errors */
} RfTelemetry;

/**
 * @brief Active Fault Record
 */
typedef struct {
    bool          active;                   /**< True if a critical fault is currently latched */
    LcdFaultCode  code;                     /**< Diagnostic Fault Code */
    FaultSeverity severity;                 /**< Severity Level */
    uint32_t      timestamp_ms;             /**< System millisecond timestamp when fault tripped */
    char          title[LCD_CONFIG_COLS + 1]; /**< Short fault header (e.g. "OVER-VOLTAGE") */
    char          value_str[12];            /**< Breached value string (e.g. "59.20V") */
    char          limit_str[12];            /**< Safety threshold limit (e.g. "MAX 58.40V") */
    char          action[LCD_CONFIG_COLS + 1];/**< Corrective operator action required */
} FaultRecord;

/* ========================================================================== */
/* HARDWARE DRIVER ABSTRACTION INTERFACE                                      */
/* ========================================================================== */

/**
 * @brief Hardware Driver Function Pointer Table (HAL Abstraction)
 */
typedef struct {
    void (*init)(void *user_data);
    void (*set_cursor)(uint8_t col, uint8_t row, void *user_data);
    void (*write_char)(char c, void *user_data);
    void (*write_span)(const char *str, uint8_t len, void *user_data);
    void (*set_backlight)(bool enabled, void *user_data);
    void *user_data;
} LcdHardwareDriver;

/* ========================================================================== */
/* PERFORMANCE & BUS METRICS                                                  */
/* ========================================================================== */

/**
 * @brief Real-Time Rendering & Bandwidth Statistics
 */
typedef struct {
    uint32_t frames_rendered;           /**< Total refresh frames executed */
    uint32_t chars_written_to_hw;       /**< Actual character bytes sent over I2C/SPI bus */
    uint32_t chars_skipped_unchanged;   /**< Character updates avoided via shadow comparison */
    uint32_t cursor_commands_sent;      /**< Total setCursor() commands dispatched */
    uint32_t full_clears_avoided;       /**< Count of lcd.clear() calls that were eliminated (100% saved) */
    uint32_t page_rotation_count;       /**< Number of automatic cyclical page switches */
    uint32_t fault_preemptions;         /**< Count of instant fault screen override events */
    uint32_t bus_bytes_transmitted;     /**< Total I2C/SPI physical bytes transmitted */
    uint32_t bus_bytes_saved;           /**< Theoretical bus bytes saved compared to naive clear+write */
    uint32_t last_frame_dirty_cells;    /**< Dirty cell count in the most recent frame */
    uint32_t last_frame_time_us;        /**< Execution time of the most recent render diff scan in us */
} LcdMetrics;

/* ========================================================================== */
/* ENGINE CONTEXT STRUCTURE                                                   */
/* ========================================================================== */

/**
 * @brief Master Flicker-Free LCD Engine Context
 * 
 * Completely statically allocated. Zero dynamic memory.
 */
typedef struct {
    /* Dual Shadow Buffers */
    char front_buffer[LCD_CONFIG_ROWS][LCD_CONFIG_COLS]; /**< Exact hardware mirror of LCD DDRAM */
    char back_buffer[LCD_CONFIG_ROWS][LCD_CONFIG_COLS];  /**< Staging canvas for new page generation */
    
    /* State & Navigation */
    LcdEngineState state;               /**< Current FSM state */
    LcdPageId      active_page;         /**< Currently active operational page */
    LcdPageId      previous_page;       /**< Page active prior to fault or pause */
    
    /* Non-Blocking Scheduling Timestamps */
    uint32_t last_refresh_ms;           /**< Timestamp of previous differential flush */
    uint32_t last_page_rotate_ms;       /**< Timestamp of previous cyclical page transition */
    uint32_t last_blink_toggle_ms;      /**< Timestamp of previous fault header flash toggle */
    
    /* Timing Configurations */
    uint16_t refresh_interval_ms;       /**< Configured differential refresh period (e.g. 100ms) */
    uint16_t page_dwell_ms;             /**< Dwell duration before auto-rotating to next page */
    uint16_t blink_period_ms;           /**< Half-period for emergency blinker */
    
    /* State Flags */
    bool     blink_state;               /**< Alternating toggle for visual alert emphasis */
    bool     force_full_redraw;         /**< Flag to invalidate front buffer on page change/recovery */
    uint16_t frame_sequence_counter;    /**< Frame counter for periodic ESD resync */
    
    /* Telemetry Data Caches */
    BatteryTelemetry battery_data;      /**< Page 0 Source Data */
    SystemTelemetry  system_data;       /**< Page 1 Source Data */
    RfTelemetry      telemetry_data;    /**< Page 2 Source Data */
    FaultRecord      active_fault;      /**< Emergency Fault Data */
    
    /* Hardware Driver & Metrics */
    LcdHardwareDriver driver;           /**< Injected hardware driver interface */
    LcdMetrics        metrics;          /**< Real-time telemetry & savings counters */
} LcdEngine;

#ifdef __cplusplus
}
#endif

#endif /* LCD_TYPES_H */
