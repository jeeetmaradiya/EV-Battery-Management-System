/**
 * @file lcd_engine.h
 * @brief Public API Function Prototypes for the Flicker-Free LCD Display Engine.
 * 
 * Features:
 *  - Differential Shadow-Buffer Rendering (dirty cell & dirty span detection)
 *  - Zero-clearing architecture (100% elimination of lcd.clear() visual flashing)
 *  - Non-blocking cooperative timer scheduling (zero delay() calls)
 *  - Automatic multi-page cyclical rotation (Battery, System, Telemetry)
 *  - Immediate Preemptive Critical Fault Override & Latching
 *  - Deterministic bounded execution time suitable for real-time embedded systems.
 */

#ifndef LCD_ENGINE_H
#define LCD_ENGINE_H

#include "lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* CORE ENGINE LIFECYCLE & SCHEDULING APIS                                    */
/* ========================================================================== */

/**
 * @brief Initialize the Flicker-Free LCD Engine.
 * 
 * Clears front and back buffers, registers hardware driver callbacks, initializes
 * default telemetry values, and sets initial non-blocking timer baselines.
 * 
 * @param engine Pointer to LcdEngine instance (allocated statically by caller).
 * @param driver Hardware abstraction interface with low-level write routines.
 * @param start_time_ms Initial system timestamp from get_tick_ms() or millis().
 */
void lcd_engine_init(LcdEngine *engine, const LcdHardwareDriver *driver, uint32_t start_time_ms);

/**
 * @brief Master Non-Blocking Execution Step (Tick).
 * 
 * Must be invoked periodically from the main loop or a periodic RTOS task.
 * Evaluates internal timers (refresh interval, page dwell time, fault blinking),
 * constructs the active canvas in the back buffer, executes differential scanning,
 * and flushes only changed characters to the physical LCD hardware.
 * 
 * Bounded deterministic runtime: typically < 50 microseconds on a 32-bit Cortex-M MCU.
 * 
 * @param engine Pointer to LcdEngine instance.
 * @param current_time_ms Current system timestamp in milliseconds.
 */
void lcd_engine_tick(LcdEngine *engine, uint32_t current_time_ms);

/* ========================================================================== */
/* TELEMETRY DATA PRODUCER APIS                                               */
/* ========================================================================== */

/**
 * @brief Update Battery Telemetry Data (Page 0).
 * 
 * @param engine Pointer to LcdEngine instance.
 * @param battery Pointer to battery telemetry snapshot.
 */
void lcd_engine_set_battery_data(LcdEngine *engine, const BatteryTelemetry *battery);

/**
 * @brief Update System State & Resource Telemetry Data (Page 1).
 * 
 * @param engine Pointer to LcdEngine instance.
 * @param system Pointer to system telemetry snapshot.
 */
void lcd_engine_set_system_data(LcdEngine *engine, const SystemTelemetry *system);

/**
 * @brief Update RF Link & Communication Telemetry Data (Page 2).
 * 
 * @param engine Pointer to LcdEngine instance.
 * @param telemetry Pointer to RF telemetry snapshot.
 */
void lcd_engine_set_telemetry_data(LcdEngine *engine, const RfTelemetry *telemetry);

/* ========================================================================== */
/* FAULT MANAGEMENT & OVERRIDE APIS                                           */
/* ========================================================================== */

/**
 * @brief Raise a Critical Fault Condition & Immediately Override Normal Display.
 * 
 * Preempts normal page rotation on the current tick. Transitions the engine to
 * LCD_STATE_FAULT_OVERRIDE and renders the dedicated safety alarm screen.
 * Rotation remains suspended until the fault is explicitly cleared via lcd_engine_clear_fault().
 * 
 * @param engine Pointer to LcdEngine instance.
 * @param code Diagnostic fault code (e.g. FAULT_CODE_OVER_VOLTAGE).
 * @param severity Severity level (must be >= FAULT_SEV_CRITICAL to trigger override).
 * @param title Short alphanumeric title (e.g. "OVER-VOLTAGE TRIP").
 * @param value_str Measured breached value string (e.g. "59.40V").
 * @param limit_str Permissible safety limit string (e.g. "MAX:58.40V").
 * @param action Prescribed operator mitigation (e.g. "DISCONNECT CHARGER").
 * @param current_time_ms System timestamp of trip event.
 */
void lcd_engine_trigger_fault(LcdEngine *engine, 
                              LcdFaultCode code, 
                              FaultSeverity severity,
                              const char *title, 
                              const char *value_str, 
                              const char *limit_str, 
                              const char *action, 
                              uint32_t current_time_ms);

/**
 * @brief Explicitly Clear the Latched Critical Fault.
 * 
 * Resets the fault state if the underlying critical condition is cleared.
 * Restores the engine to LCD_STATE_NORMAL_ROTATION and transitions smoothly back
 * to the operational display pages without any screen flicker or residue.
 * 
 * @param engine Pointer to LcdEngine instance.
 * @param current_time_ms Current system timestamp.
 * @return true if fault was cleared, false if no fault was active.
 */
bool lcd_engine_clear_fault(LcdEngine *engine, uint32_t current_time_ms);

/**
 * @brief Check if the LCD Engine is currently in Fault Override Mode.
 * 
 * @param engine Pointer to LcdEngine instance.
 * @return true if locked on fault screen, false otherwise.
 */
bool lcd_engine_is_fault_active(const LcdEngine *engine);

/* ========================================================================== */
/* OPERATOR NAVIGATION & PAGE CONTROLS                                        */
/* ========================================================================== */

/**
 * @brief Pause or Resume Automatic Page Rotation.
 * 
 * When paused, the display remains on the currently visible page for manual inspection.
 * 
 * @param engine Pointer to LcdEngine instance.
 * @param pause True to freeze on current page, false to resume auto-cycling.
 */
void lcd_engine_pause_rotation(LcdEngine *engine, bool pause);

/**
 * @brief Manually Advance to Next Information Page.
 * 
 * @param engine Pointer to LcdEngine instance.
 * @param current_time_ms Current system timestamp (resets dwell counter).
 */
void lcd_engine_next_page(LcdEngine *engine, uint32_t current_time_ms);

/**
 * @brief Manually Step Back to Previous Information Page.
 * 
 * @param engine Pointer to LcdEngine instance.
 * @param current_time_ms Current system timestamp (resets dwell counter).
 */
void lcd_engine_prev_page(LcdEngine *engine, uint32_t current_time_ms);

/**
 * @brief Directly Select a Specific Information Page.
 * 
 * @param engine Pointer to LcdEngine instance.
 * @param page Target page ID (LCD_PAGE_BATTERY, LCD_PAGE_SYSTEM, etc.).
 * @param current_time_ms Current system timestamp.
 */
void lcd_engine_select_page(LcdEngine *engine, LcdPageId page, uint32_t current_time_ms);

/**
 * @brief Configure Refresh Interval and Page Dwell Times Dynamically.
 * 
 * @param engine Pointer to LcdEngine instance.
 * @param refresh_interval_ms Time between differential screen updates (50 - 500ms).
 * @param page_dwell_ms Time to display each page before auto-rotation (1000 - 30000ms).
 */
void lcd_engine_set_timing(LcdEngine *engine, uint16_t refresh_interval_ms, uint16_t page_dwell_ms);

/**
 * @brief Invalidate Front Buffer and Force Full Hardware Resync on Next Tick.
 * 
 * Used for recovering from severe bus noise or electrical transients.
 * 
 * @param engine Pointer to LcdEngine instance.
 */
void lcd_engine_force_full_refresh(LcdEngine *engine);

/* ========================================================================== */
/* DIAGNOSTICS & METRICS RETRIEVAL                                            */
/* ========================================================================== */

/**
 * @brief Retrieve Real-Time Performance & Bandwidth Reduction Metrics.
 * 
 * @param engine Pointer to LcdEngine instance.
 * @return Const pointer to internal metrics struct.
 */
const LcdMetrics* lcd_engine_get_metrics(const LcdEngine *engine);

/**
 * @brief Copy Raw Front and Back Buffer Snapshots for Telemetry / Inspection.
 * 
 * @param engine Pointer to LcdEngine instance.
 * @param front 2D array [ROWS][COLS] receiving front buffer copy.
 * @param back 2D array [ROWS][COLS] receiving back buffer copy.
 */
void lcd_engine_get_buffer_snapshot(const LcdEngine *engine, 
                                    char front[LCD_CONFIG_ROWS][LCD_CONFIG_COLS], 
                                    char back[LCD_CONFIG_ROWS][LCD_CONFIG_COLS]);

#ifdef __cplusplus
}
#endif

#endif /* LCD_ENGINE_H */
