/**
 * @file test_lcd_engine.c
 * @brief Embedded Unit Test Suite for Flicker-Free LCD Display Engine.
 * 
 * Verifies:
 *  - Differential partial updates (dirty cell isolation)
 *  - Elimination of lcd.clear() screen blanking
 *  - Automatic non-blocking page rotation timing
 *  - Immediate critical fault override preemption & latching
 *  - Graceful post-fault restoration
 *  - I2C bus bandwidth reduction metrics
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../include/lcd_engine.h"

/* Mock Hardware Driver Capture Structures */
typedef struct {
    uint32_t cursor_call_count;
    uint32_t char_call_count;
    uint32_t span_call_count;
    uint8_t  last_cursor_col;
    uint8_t  last_cursor_row;
    char     physical_vram[LCD_CONFIG_ROWS][LCD_CONFIG_COLS];
} MockHardware;

static MockHardware g_mock;

static void mock_init(void *user_data) {
    (void)user_data;
    memset(&g_mock, 0, sizeof(g_mock));
    memset(g_mock.physical_vram, ' ', sizeof(g_mock.physical_vram));
}

static void mock_set_cursor(uint8_t col, uint8_t row, void *user_data) {
    (void)user_data;
    g_mock.cursor_call_count++;
    g_mock.last_cursor_col = col;
    g_mock.last_cursor_row = row;
}

static void mock_write_char(char c, void *user_data) {
    (void)user_data;
    g_mock.char_call_count++;
    if (g_mock.last_cursor_row < LCD_CONFIG_ROWS && g_mock.last_cursor_col < LCD_CONFIG_COLS) {
        g_mock.physical_vram[g_mock.last_cursor_row][g_mock.last_cursor_col] = c;
        g_mock.last_cursor_col++;
    }
}

static void mock_write_span(const char *str, uint8_t len, void *user_data) {
    (void)user_data;
    g_mock.span_call_count++;
    for (uint8_t i = 0; i < len; i++) {
        mock_write_char(str[i], user_data);
    }
}

static LcdHardwareDriver get_mock_driver(void) {
    LcdHardwareDriver drv;
    drv.init = mock_init;
    drv.set_cursor = mock_set_cursor;
    drv.write_char = mock_write_char;
    drv.write_span = mock_write_span;
    drv.set_backlight = NULL;
    drv.user_data = NULL;
    return drv;
}

/* ========================================================================== */
/* TEST SUITES                                                                */
/* ========================================================================== */

static void test_initial_boot_and_differential_idempotence(void) {
    printf("[TEST 1] Initial Boot & Differential Idempotence... ");
    LcdEngine engine;
    LcdHardwareDriver drv = get_mock_driver();
    
    lcd_engine_init(&engine, &drv, 0);
    assert(engine.state == LCD_STATE_NORMAL_ROTATION);
    assert(engine.active_page == LCD_PAGE_BATTERY);

    /* First tick at t = 100ms: Paints initial screen */
    lcd_engine_tick(&engine, 100);
    assert(engine.metrics.frames_rendered == 1);
    assert(g_mock.char_call_count == LCD_CONFIG_TOTAL_CELLS); // 80 cells painted
    assert(engine.metrics.full_clears_avoided == 1);

    /* Second tick at t = 200ms with UNCHANGED telemetry: Zero hardware writes */
    uint32_t chars_before = g_mock.char_call_count;
    uint32_t cursors_before = g_mock.cursor_call_count;
    lcd_engine_tick(&engine, 200);
    assert(engine.metrics.frames_rendered == 2);
    assert(g_mock.char_call_count == chars_before);     // ZERO new chars written!
    assert(g_mock.cursor_call_count == cursors_before); // ZERO cursor moves!
    assert(engine.metrics.last_frame_dirty_cells == 0); // 0 dirty cells

    printf("PASSED (Zero overhead on static frames)\n");
}

static void test_partial_dirty_span_optimization(void) {
    printf("[TEST 2] Partial Dirty Span Isolation & Bus Minimization... ");
    LcdEngine engine;
    LcdHardwareDriver drv = get_mock_driver();
    lcd_engine_init(&engine, &drv, 0);
    lcd_engine_tick(&engine, 100); // Frame 1: Initial full draw

    /* Now modify ONLY the battery voltage by 0.05V (52.40V -> 52.45V) */
    BatteryTelemetry b = engine.battery_data;
    b.voltage_mv = 52450; // '0' becomes '5'
    lcd_engine_set_battery_data(&engine, &b);

    uint32_t chars_before = g_mock.char_call_count;
    lcd_engine_tick(&engine, 200);

    /* Only the changed character(s) should be updated */
    uint32_t written_delta = g_mock.char_call_count - chars_before;
    assert(written_delta > 0 && written_delta <= 5); // Only numeric field updated
    assert(engine.metrics.last_frame_dirty_cells <= 5);
    assert(engine.metrics.bus_bytes_saved > 0);

    printf("PASSED (Only %u chars written instead of 80)\n", written_delta);
}

static void test_automatic_page_rotation(void) {
    printf("[TEST 3] Automatic Non-Blocking Page Rotation... ");
    LcdEngine engine;
    LcdHardwareDriver drv = get_mock_driver();
    lcd_engine_init(&engine, &drv, 0);

    /* t = 100ms: Page 0 (Battery) */
    lcd_engine_tick(&engine, 100);
    assert(engine.active_page == LCD_PAGE_BATTERY);

    /* t = 3900ms: Still on Page 0 (Dwell is 4000ms) */
    lcd_engine_tick(&engine, 3900);
    assert(engine.active_page == LCD_PAGE_BATTERY);

    /* t = 4050ms: Rotated to Page 1 (System) */
    lcd_engine_tick(&engine, 4050);
    assert(engine.active_page == LCD_PAGE_SYSTEM);
    assert(engine.metrics.page_rotation_count == 1);

    /* t = 8050ms: Rotated to Page 2 (Telemetry) */
    lcd_engine_tick(&engine, 8050);
    assert(engine.active_page == LCD_PAGE_TELEMETRY);
    assert(engine.metrics.page_rotation_count == 2);

    /* t = 12050ms: Wrapped back to Page 0 (Battery) */
    lcd_engine_tick(&engine, 12050);
    assert(engine.active_page == LCD_PAGE_BATTERY);
    assert(engine.metrics.page_rotation_count == 3);

    printf("PASSED (Accurate 4.0s cyclical dwell)\n");
}

static void test_critical_fault_preemption_and_latching(void) {
    printf("[TEST 4] Critical Fault Preemption, Locking & Restoration... ");
    LcdEngine engine;
    LcdHardwareDriver drv = get_mock_driver();
    lcd_engine_init(&engine, &drv, 0);
    lcd_engine_tick(&engine, 100);

    /* Trigger Critical Over-Voltage Trip */
    lcd_engine_trigger_fault(&engine, 
                             FAULT_CODE_OVER_VOLTAGE, 
                             FAULT_SEV_CRITICAL,
                             "OVER-VOLTAGE TRIP", 
                             "59.40V", 
                             "MAX:58.40V", 
                             "DISCONN CHARGER", 
                             1500);

    assert(engine.state == LCD_STATE_FAULT_OVERRIDE);
    assert(engine.active_fault.active == true);

    /* Next tick at 1550ms: Must immediately render fault screen */
    lcd_engine_tick(&engine, 1550);
    assert(memcmp(engine.back_buffer[1], "0x11", 4) == 0); // Hex fault code rendered

    /* Advance time by 10,000ms: Rotation MUST REMAIN LOCKED */
    for (uint32_t t = 1600; t <= 11600; t += 500) {
        lcd_engine_tick(&engine, t);
        assert(engine.state == LCD_STATE_FAULT_OVERRIDE);
        assert(engine.active_fault.active == true);
    }
    assert(engine.metrics.page_rotation_count == 0); // No rotation occurred

    /* Clear fault explicitly */
    bool cleared = lcd_engine_clear_fault(&engine, 12000);
    assert(cleared == true);
    assert(engine.state == LCD_STATE_NORMAL_ROTATION);
    assert(engine.active_fault.active == false);

    /* Tick at 12050ms: Seamlessly restores normal operational page */
    lcd_engine_tick(&engine, 12050);
    assert(engine.active_page == LCD_PAGE_BATTERY);

    printf("PASSED (Preemption, Lockout, and Smooth Restoration verified)\n");
}

int main(void) {
    printf("=================================================================\n");
    printf("FLICKER-FREE LCD DISPLAY ENGINE - C EMBEDDED UNIT TEST BENCH     \n");
    printf("=================================================================\n");
    test_initial_boot_and_differential_idempotence();
    test_partial_dirty_span_optimization();
    test_automatic_page_rotation();
    test_critical_fault_preemption_and_latching();
    printf("=================================================================\n");
    printf("ALL EMBEDDED UNIT TESTS PASSED SUCCESSFULLY! (100%% OK)\n");
    printf("=================================================================\n");
    return 0;
}
