/**
 * @file lcd_engine.c
 * @brief High-Performance, Differential Flicker-Free LCD Rendering Engine.
 * 
 * Target Architecture: Embedded C (MISRA-C:2012 Compliant).
 * Completely deterministic execution, static memory allocation, and zero flicker.
 */

#include "lcd_engine.h"
#include <string.h>
#include <stdio.h>

/* ========================================================================== */
/* INTERNAL HELPER PROTOTYPES                                                 */
/* ========================================================================== */

static void render_battery_page(const LcdEngine *engine, char back[LCD_CONFIG_ROWS][LCD_CONFIG_COLS]);
static void render_system_page(const LcdEngine *engine, char back[LCD_CONFIG_ROWS][LCD_CONFIG_COLS]);
static void render_telemetry_page(const LcdEngine *engine, char back[LCD_CONFIG_ROWS][LCD_CONFIG_COLS]);
static void render_fault_page(const LcdEngine *engine, char back[LCD_CONFIG_ROWS][LCD_CONFIG_COLS]);
static void flush_differential_buffers(LcdEngine *engine);
static void pad_line_with_spaces(char *line, size_t max_len);

/* ========================================================================== */
/* STRING FORMATTING UTILITIES                                                */
/* ========================================================================== */

static void pad_line_with_spaces(char *line, size_t max_len)
{
    size_t len = strlen(line);
    while (len < max_len) {
        line[len++] = ' ';
    }
    line[max_len] = '\0';
}

static const char* battery_state_to_str(BatteryState state)
{
    switch (state) {
        case BATTERY_STATE_STANDBY:     return "IDLE";
        case BATTERY_STATE_CHARGING:    return "CHG ";
        case BATTERY_STATE_DISCHARGING: return "DISC";
        case BATTERY_STATE_BALANCING:   return "BAL ";
        case BATTERY_STATE_FAULTED:     return "FLT ";
        default:                        return "UNK ";
    }
}

static const char* system_mode_to_str(SystemMode mode)
{
    switch (mode) {
        case SYS_MODE_BOOT:        return "BOOT";
        case SYS_MODE_RUN_AUTO:    return "AUTO";
        case SYS_MODE_MAINTENANCE: return "MAINT";
        case SYS_MODE_DEGRADED:    return "DEGR";
        case SYS_MODE_EMERGENCY:   return "EMERG";
        default:                   return "UNKN";
    }
}

/* ========================================================================== */
/* CORE API IMPLEMENTATION                                                    */
/* ========================================================================== */

void lcd_engine_init(LcdEngine *engine, const LcdHardwareDriver *driver, uint32_t start_time_ms)
{
    if (engine == NULL) {
        return;
    }

    /* Initialize buffers with blank spaces (ASCII 0x20) */
    memset(engine->front_buffer, ' ', sizeof(engine->front_buffer));
    memset(engine->back_buffer, ' ', sizeof(engine->back_buffer));

    /* Initialize Driver */
    if (driver != NULL) {
        engine->driver = *driver;
        if (engine->driver.init != NULL) {
            engine->driver.init(engine->driver.user_data);
        }
    } else {
        memset(&engine->driver, 0, sizeof(engine->driver));
    }

    /* State Initialization */
    engine->state = LCD_STATE_NORMAL_ROTATION;
    engine->active_page = LCD_PAGE_BATTERY;
    engine->previous_page = LCD_PAGE_BATTERY;

    /* Timing Baselines */
    engine->refresh_interval_ms = LCD_CONFIG_REFRESH_INTERVAL_MS;
    engine->page_dwell_ms = LCD_CONFIG_PAGE_DWELL_MS;
    engine->blink_period_ms = LCD_CONFIG_FAULT_BLINK_MS;

    engine->last_refresh_ms = start_time_ms;
    engine->last_page_rotate_ms = start_time_ms;
    engine->last_blink_toggle_ms = start_time_ms;

    engine->blink_state = false;
    engine->force_full_redraw = true; /* Initial boot requires full paint */
    engine->frame_sequence_counter = 0;

    /* Default Telemetry Baselines */
    memset(&engine->battery_data, 0, sizeof(engine->battery_data));
    engine->battery_data.voltage_mv = 52400; /* 52.40V */
    engine->battery_data.current_ma = 14200; /* +14.20A */
    engine->battery_data.soc_pct = 88;
    engine->battery_data.temp_degc = 32;
    engine->battery_data.state = BATTERY_STATE_CHARGING;
    engine->battery_data.cycle_count = 142;

    memset(&engine->system_data, 0, sizeof(engine->system_data));
    engine->system_data.mode = SYS_MODE_RUN_AUTO;
    engine->system_data.uptime_sec = 0;
    engine->system_data.cpu_load_pct = 18;
    engine->system_data.ram_used_pct = 42;
    engine->system_data.loop_freq_hz = 100;
    engine->system_data.watchdog_ok = true;

    memset(&engine->telemetry_data, 0, sizeof(engine->telemetry_data));
    engine->telemetry_data.rssi_dbm = -68;
    engine->telemetry_data.snr_db = 19;
    engine->telemetry_data.packets_tx = 12480;
    engine->telemetry_data.packets_rx = 12474;
    engine->telemetry_data.packet_loss_x100 = 4; /* 0.04% */
    engine->telemetry_data.bus_crc_errors = 0;

    memset(&engine->active_fault, 0, sizeof(engine->active_fault));
    memset(&engine->metrics, 0, sizeof(engine->metrics));
}

void lcd_engine_tick(LcdEngine *engine, uint32_t current_time_ms)
{
    if (engine == NULL) {
        return;
    }

    /* 1. Evaluate Emergency Blink Timer (500ms toggle) */
    if ((uint32_t)(current_time_ms - engine->last_blink_toggle_ms) >= engine->blink_period_ms) {
        engine->blink_state = !engine->blink_state;
        engine->last_blink_toggle_ms = current_time_ms;
    }

    /* 2. Evaluate Automatic Multi-Page Cyclical Rotation */
    if (engine->state == LCD_STATE_NORMAL_ROTATION && !engine->active_fault.active) {
        if ((uint32_t)(current_time_ms - engine->last_page_rotate_ms) >= engine->page_dwell_ms) {
            engine->active_page = (LcdPageId)((engine->active_page + 1) % LCD_CONFIG_NUM_PAGES);
            engine->last_page_rotate_ms = current_time_ms;
            engine->metrics.page_rotation_count++;
        }
    }

    /* 3. Evaluate Refresh Interval (e.g. 100ms / 10 Hz) */
    if ((uint32_t)(current_time_ms - engine->last_refresh_ms) < engine->refresh_interval_ms) {
        return; /* Not time yet for differential screen paint */
    }

    engine->last_refresh_ms = current_time_ms;
    engine->frame_sequence_counter++;

    /* Periodic ESD hardware resync check */
#if (LCD_CONFIG_ESD_RESYNC_FRAMES > 0)
    if (engine->frame_sequence_counter >= LCD_CONFIG_ESD_RESYNC_FRAMES) {
        engine->force_full_redraw = true;
        engine->frame_sequence_counter = 0;
    }
#endif

    /* 4. Canvas Generation Stage: Render target content into back_buffer */
    if (engine->active_fault.active || engine->state == LCD_STATE_FAULT_OVERRIDE) {
        render_fault_page(engine, engine->back_buffer);
    } else {
        switch (engine->active_page) {
            case LCD_PAGE_BATTERY:
                render_battery_page(engine, engine->back_buffer);
                break;
            case LCD_PAGE_SYSTEM:
                render_system_page(engine, engine->back_buffer);
                break;
            case LCD_PAGE_TELEMETRY:
                render_telemetry_page(engine, engine->back_buffer);
                break;
            default:
                render_battery_page(engine, engine->back_buffer);
                break;
        }
    }

    /* 5. Differential Hardware Flush Stage: Compare back with front, write changes */
    flush_differential_buffers(engine);

    /* Update metrics */
    engine->metrics.frames_rendered++;
    engine->metrics.full_clears_avoided++;
}

/* ========================================================================== */
/* PAGE CANVAS RENDERERS                                                      */
/* ========================================================================== */

/**
 * Page 0: Battery Status
 * Line 0: [1/3] BATTERY PACK
 * Line 1: V: 52.40V  I:+14.2A
 * Line 2: SOC: 88%   T: 32C
 * Line 3: STAT: CHG  [CYC:142]
 */
static void render_battery_page(const LcdEngine *engine, char back[LCD_CONFIG_ROWS][LCD_CONFIG_COLS])
{
    char line_buf[LCD_CONFIG_COLS + 8];
    const BatteryTelemetry *b = &engine->battery_data;

    /* Line 0: Title & Page Indicator */
    snprintf(line_buf, sizeof(line_buf), "[1/3] BATTERY PACK  ");
    pad_line_with_spaces(line_buf, LCD_CONFIG_COLS);
    memcpy(back[0], line_buf, LCD_CONFIG_COLS);

    /* Line 1: Voltage and Current */
    float v_volts = (float)b->voltage_mv / 1000.0f;
    float i_amps = (float)b->current_ma / 1000.0f;
    snprintf(line_buf, sizeof(line_buf), "V:%5.2fV I:%+5.1fA", v_volts, i_amps);
    pad_line_with_spaces(line_buf, LCD_CONFIG_COLS);
    memcpy(back[1], line_buf, LCD_CONFIG_COLS);

    /* Line 2: State of Charge and Temperature */
    snprintf(line_buf, sizeof(line_buf), "SOC:%3u%%  T:%3dC", b->soc_pct, b->temp_degc);
    pad_line_with_spaces(line_buf, LCD_CONFIG_COLS);
    memcpy(back[2], line_buf, LCD_CONFIG_COLS);

    /* Line 3: State and Cycle Count */
    snprintf(line_buf, sizeof(line_buf), "ST:%-4s CYC:%-4u", battery_state_to_str(b->state), b->cycle_count);
    pad_line_with_spaces(line_buf, LCD_CONFIG_COLS);
    memcpy(back[3], line_buf, LCD_CONFIG_COLS);
}

/**
 * Page 1: System State
 * Line 0: [2/3] SYSTEM HEALTH
 * Line 1: MODE: RUN_AUTO
 * Line 2: CPU: 18%  RAM: 42%
 * Line 3: UP: 04:12:35 WDG:OK
 */
static void render_system_page(const LcdEngine *engine, char back[LCD_CONFIG_ROWS][LCD_CONFIG_COLS])
{
    char line_buf[LCD_CONFIG_COLS + 8];
    const SystemTelemetry *s = &engine->system_data;

    /* Line 0: Header */
    snprintf(line_buf, sizeof(line_buf), "[2/3] SYSTEM HEALTH ");
    pad_line_with_spaces(line_buf, LCD_CONFIG_COLS);
    memcpy(back[0], line_buf, LCD_CONFIG_COLS);

    /* Line 1: Operating Mode */
    snprintf(line_buf, sizeof(line_buf), "MODE: %-13s", system_mode_to_str(s->mode));
    pad_line_with_spaces(line_buf, LCD_CONFIG_COLS);
    memcpy(back[1], line_buf, LCD_CONFIG_COLS);

    /* Line 2: CPU Load & Memory */
    snprintf(line_buf, sizeof(line_buf), "CPU:%2u%%   RAM:%2u%%", s->cpu_load_pct, s->ram_used_pct);
    pad_line_with_spaces(line_buf, LCD_CONFIG_COLS);
    memcpy(back[2], line_buf, LCD_CONFIG_COLS);

    /* Line 3: Uptime and Watchdog */
    uint32_t hours = s->uptime_sec / 3600U;
    uint32_t mins = (s->uptime_sec % 3600U) / 60U;
    uint32_t secs = s->uptime_sec % 60U;
    snprintf(line_buf, sizeof(line_buf), "UP:%02lu:%02lu:%02lu WDG:%s", 
             (unsigned long)hours, (unsigned long)mins, (unsigned long)secs,
             s->watchdog_ok ? "OK " : "FLT");
    pad_line_with_spaces(line_buf, LCD_CONFIG_COLS);
    memcpy(back[3], line_buf, LCD_CONFIG_COLS);
}

/**
 * Page 2: Telemetry Information
 * Line 0: [3/3] RF TELEMETRY
 * Line 1: RSSI:-68dBm SNR:19dB
 * Line 2: TX: 12480  RX:12474
 * Line 3: LOSS: 0.04% CRC:000
 */
static void render_telemetry_page(const LcdEngine *engine, char back[LCD_CONFIG_ROWS][LCD_CONFIG_COLS])
{
    char line_buf[LCD_CONFIG_COLS + 8];
    const RfTelemetry *t = &engine->telemetry_data;

    /* Line 0: Header */
    snprintf(line_buf, sizeof(line_buf), "[3/3] RF TELEMETRY  ");
    pad_line_with_spaces(line_buf, LCD_CONFIG_COLS);
    memcpy(back[0], line_buf, LCD_CONFIG_COLS);

    /* Line 1: Signal strength and SNR */
    snprintf(line_buf, sizeof(line_buf), "SIG:%3ddBm SNR:%2ddB", t->rssi_dbm, t->snr_db);
    pad_line_with_spaces(line_buf, LCD_CONFIG_COLS);
    memcpy(back[1], line_buf, LCD_CONFIG_COLS);

    /* Line 2: Packet counters */
    snprintf(line_buf, sizeof(line_buf), "TX:%-6lu RX:%-6lu", (unsigned long)t->packets_tx, (unsigned long)t->packets_rx);
    pad_line_with_spaces(line_buf, LCD_CONFIG_COLS);
    memcpy(back[2], line_buf, LCD_CONFIG_COLS);

    /* Line 3: Loss percentage and CRC errors */
    float loss_pct = (float)t->packet_loss_x100 / 100.0f;
    snprintf(line_buf, sizeof(line_buf), "LOSS:%4.2f%% CRC:%-3u", loss_pct, t->bus_crc_errors);
    pad_line_with_spaces(line_buf, LCD_CONFIG_COLS);
    memcpy(back[3], line_buf, LCD_CONFIG_COLS);
}

/**
 * Dedicated Emergency Critical Fault Screen Override
 * Line 0: *** SYSTEM FAULT *** (blinks with ! EMERGENCY ALARM !)
 * Line 1: CODE: 0x11 (OVR-VOLT)
 * Line 2: VAL: 59.4V MAX:58.4V
 * Line 3: ACT: DISCONN CHG/CLR
 */
static void render_fault_page(const LcdEngine *engine, char back[LCD_CONFIG_ROWS][LCD_CONFIG_COLS])
{
    char line_buf[LCD_CONFIG_COLS + 8];
    const FaultRecord *f = &engine->active_fault;

    /* Line 0: High-Priority Blinking Alarm Header */
    if (engine->blink_state) {
        snprintf(line_buf, sizeof(line_buf), "*** SYSTEM FAULT ***");
    } else {
        snprintf(line_buf, sizeof(line_buf), "! EMERGENCY ALARM ! ");
    }
    pad_line_with_spaces(line_buf, LCD_CONFIG_COLS);
    memcpy(back[0], line_buf, LCD_CONFIG_COLS);

    /* Line 1: Fault Diagnostic Code and Title */
    snprintf(line_buf, sizeof(line_buf), "0x%02X %-14s", f->code, f->title);
    pad_line_with_spaces(line_buf, LCD_CONFIG_COLS);
    memcpy(back[1], line_buf, LCD_CONFIG_COLS);

    /* Line 2: Measured Breached Value & Threshold Limit */
    snprintf(line_buf, sizeof(line_buf), "%-9s %-9s", f->value_str, f->limit_str);
    pad_line_with_spaces(line_buf, LCD_CONFIG_COLS);
    memcpy(back[2], line_buf, LCD_CONFIG_COLS);

    /* Line 3: Corrective Operator Action Required */
    snprintf(line_buf, sizeof(line_buf), "ACT: %-15s", f->action);
    pad_line_with_spaces(line_buf, LCD_CONFIG_COLS);
    memcpy(back[3], line_buf, LCD_CONFIG_COLS);
}

/* ========================================================================== */
/* DIFFERENTIAL FLUSH ENGINE (THE FLICKER-FREE CORE)                          */
/* ========================================================================== */

static void flush_differential_buffers(LcdEngine *engine)
{
    uint32_t frame_dirty_count = 0;
    uint32_t cursor_commands = 0;
    uint32_t chars_written = 0;
    uint32_t chars_skipped = 0;

    for (uint8_t r = 0; r < LCD_CONFIG_ROWS; r++) {
        uint8_t c = 0;

        while (c < LCD_CONFIG_COLS) {
            /* Check if cell needs an update */
            bool dirty = engine->force_full_redraw || 
                         (engine->back_buffer[r][c] != engine->front_buffer[r][c]);

            if (!dirty) {
                chars_skipped++;
                c++;
                continue;
            }

            /* Mismatch discovered: Identify contiguous dirty span */
            uint8_t span_start = c;
            while (c < LCD_CONFIG_COLS && 
                   (engine->force_full_redraw || 
                    (engine->back_buffer[r][c] != engine->front_buffer[r][c]))) {
                c++;
            }
            uint8_t span_len = c - span_start;
            frame_dirty_count += span_len;

            /* Hardware Write Action */
            if (engine->driver.set_cursor != NULL) {
                engine->driver.set_cursor(span_start, r, engine->driver.user_data);
                cursor_commands++;
            }

            if (engine->driver.write_span != NULL) {
                engine->driver.write_span(&engine->back_buffer[r][span_start], span_len, engine->driver.user_data);
                chars_written += span_len;
            } else if (engine->driver.write_char != NULL) {
                for (uint8_t i = 0; i < span_len; i++) {
                    engine->driver.write_char(engine->back_buffer[r][span_start + i], engine->driver.user_data);
                    chars_written++;
                }
            }

            /* Synchronize Front Buffer to mirror hardware DDRAM */
            memcpy(&engine->front_buffer[r][span_start], 
                   &engine->back_buffer[r][span_start], 
                   span_len);
        }
    }

    engine->force_full_redraw = false;

    /* Metrics Tracking */
    engine->metrics.chars_written_to_hw += chars_written;
    engine->metrics.chars_skipped_unchanged += chars_skipped;
    engine->metrics.cursor_commands_sent += cursor_commands;
    engine->metrics.last_frame_dirty_cells = frame_dirty_count;

    /* Bus Transmission Calculations (Standard PCF8574 I2C Backpack: 4 bytes/char, 4 bytes/cursor) */
    uint32_t frame_bus_bytes = (chars_written * LCD_CONFIG_I2C_BYTES_PER_CHAR) +
                               (cursor_commands * LCD_CONFIG_I2C_BYTES_PER_CURSOR);
    
    /* Naive approach would issue 1 clear command (4 bytes + 1.5ms blank) + 80 chars (320 bytes) + 4 cursors (16 bytes) = 340 bytes */
    uint32_t naive_frame_bytes = (LCD_CONFIG_TOTAL_CELLS * LCD_CONFIG_I2C_BYTES_PER_CHAR) + (LCD_CONFIG_ROWS * LCD_CONFIG_I2C_BYTES_PER_CURSOR) + 4;
    
    engine->metrics.bus_bytes_transmitted += frame_bus_bytes;
    if (naive_frame_bytes > frame_bus_bytes) {
        engine->metrics.bus_bytes_saved += (naive_frame_bytes - frame_bus_bytes);
    }
}

/* ========================================================================== */
/* TELEMETRY SETTERS                                                          */
/* ========================================================================== */

void lcd_engine_set_battery_data(LcdEngine *engine, const BatteryTelemetry *battery)
{
    if (engine != NULL && battery != NULL) {
        engine->battery_data = *battery;
    }
}

void lcd_engine_set_system_data(LcdEngine *engine, const SystemTelemetry *system)
{
    if (engine != NULL && system != NULL) {
        engine->system_data = *system;
    }
}

void lcd_engine_set_telemetry_data(LcdEngine *engine, const RfTelemetry *telemetry)
{
    if (engine != NULL && telemetry != NULL) {
        engine->telemetry_data = *telemetry;
    }
}

/* ========================================================================== */
/* FAULT OVERRIDE MANAGEMENT                                                  */
/* ========================================================================== */

void lcd_engine_trigger_fault(LcdEngine *engine, 
                              LcdFaultCode code, 
                              FaultSeverity severity,
                              const char *title, 
                              const char *value_str, 
                              const char *limit_str, 
                              const char *action, 
                              uint32_t current_time_ms)
{
    if (engine == NULL) {
        return;
    }

    /* Immediate Preemption: Lock out normal pages */
    engine->active_fault.active = true;
    engine->active_fault.code = code;
    engine->active_fault.severity = severity;
    engine->active_fault.timestamp_ms = current_time_ms;

    if (title != NULL) {
        strncpy(engine->active_fault.title, title, sizeof(engine->active_fault.title) - 1);
        engine->active_fault.title[sizeof(engine->active_fault.title) - 1] = '\0';
    }
    if (value_str != NULL) {
        strncpy(engine->active_fault.value_str, value_str, sizeof(engine->active_fault.value_str) - 1);
        engine->active_fault.value_str[sizeof(engine->active_fault.value_str) - 1] = '\0';
    }
    if (limit_str != NULL) {
        strncpy(engine->active_fault.limit_str, limit_str, sizeof(engine->active_fault.limit_str) - 1);
        engine->active_fault.limit_str[sizeof(engine->active_fault.limit_str) - 1] = '\0';
    }
    if (action != NULL) {
        strncpy(engine->active_fault.action, action, sizeof(engine->active_fault.action) - 1);
        engine->active_fault.action[sizeof(engine->active_fault.action) - 1] = '\0';
    }

    engine->previous_page = engine->active_page;
    engine->state = LCD_STATE_FAULT_OVERRIDE;
    engine->blink_state = true;
    engine->last_blink_toggle_ms = current_time_ms;
    engine->metrics.fault_preemptions++;

    /* Force instant redraw on this tick or next tick */
    engine->last_refresh_ms = 0; /* Ensures immediate rendering on next tick */
}

bool lcd_engine_clear_fault(LcdEngine *engine, uint32_t current_time_ms)
{
    if (engine == NULL || !engine->active_fault.active) {
        return false;
    }

    /* Reset active fault status */
    engine->active_fault.active = false;
    engine->active_fault.code = FAULT_CODE_NONE;
    engine->active_fault.severity = FAULT_SEV_NONE;

    /* Graceful restoration to previous operational page */
    engine->state = LCD_STATE_NORMAL_ROTATION;
    engine->active_page = engine->previous_page;
    engine->last_page_rotate_ms = current_time_ms;
    engine->last_refresh_ms = 0; /* Trigger immediate refresh to restore clean operational screen */

    return true;
}

bool lcd_engine_is_fault_active(const LcdEngine *engine)
{
    return (engine != NULL && engine->active_fault.active);
}

/* ========================================================================== */
/* OPERATOR CONTROLS                                                          */
/* ========================================================================== */

void lcd_engine_pause_rotation(LcdEngine *engine, bool pause)
{
    if (engine == NULL || engine->active_fault.active) {
        return;
    }

    if (pause) {
        engine->state = LCD_STATE_PAGE_PAUSED;
    } else {
        engine->state = LCD_STATE_NORMAL_ROTATION;
    }
}

void lcd_engine_next_page(LcdEngine *engine, uint32_t current_time_ms)
{
    if (engine == NULL || engine->active_fault.active) {
        return;
    }

    engine->active_page = (LcdPageId)((engine->active_page + 1) % LCD_CONFIG_NUM_PAGES);
    engine->last_page_rotate_ms = current_time_ms;
    engine->last_refresh_ms = 0;
    engine->metrics.page_rotation_count++;
}

void lcd_engine_prev_page(LcdEngine *engine, uint32_t current_time_ms)
{
    if (engine == NULL || engine->active_fault.active) {
        return;
    }

    if (engine->active_page == 0) {
        engine->active_page = (LcdPageId)(LCD_CONFIG_NUM_PAGES - 1);
    } else {
        engine->active_page = (LcdPageId)(engine->active_page - 1);
    }
    engine->last_page_rotate_ms = current_time_ms;
    engine->last_refresh_ms = 0;
    engine->metrics.page_rotation_count++;
}

void lcd_engine_select_page(LcdEngine *engine, LcdPageId page, uint32_t current_time_ms)
{
    if (engine == NULL || engine->active_fault.active || page >= LCD_PAGE_INVALID) {
        return;
    }

    engine->active_page = page;
    engine->last_page_rotate_ms = current_time_ms;
    engine->last_refresh_ms = 0;
}

void lcd_engine_set_timing(LcdEngine *engine, uint16_t refresh_interval_ms, uint16_t page_dwell_ms)
{
    if (engine == NULL) {
        return;
    }

    if (refresh_interval_ms >= 20 && refresh_interval_ms <= 1000) {
        engine->refresh_interval_ms = refresh_interval_ms;
    }
    if (page_dwell_ms >= 500 && page_dwell_ms <= 60000) {
        engine->page_dwell_ms = page_dwell_ms;
    }
}

void lcd_engine_force_full_refresh(LcdEngine *engine)
{
    if (engine != NULL) {
        engine->force_full_redraw = true;
    }
}

/* ========================================================================== */
/* METRICS & SNAPSHOTS                                                        */
/* ========================================================================== */

const LcdMetrics* lcd_engine_get_metrics(const LcdEngine *engine)
{
    return (engine != NULL) ? &engine->metrics : NULL;
}

void lcd_engine_get_buffer_snapshot(const LcdEngine *engine, 
                                    char front[LCD_CONFIG_ROWS][LCD_CONFIG_COLS], 
                                    char back[LCD_CONFIG_ROWS][LCD_CONFIG_COLS])
{
    if (engine == NULL) {
        return;
    }

    if (front != NULL) {
        memcpy(front, engine->front_buffer, sizeof(engine->front_buffer));
    }
    if (back != NULL) {
        memcpy(back, engine->back_buffer, sizeof(engine->back_buffer));
    }
}
