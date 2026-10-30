/**
 * @file lcd_config.h
 * @brief Compile-Time Configuration Parameters for Flicker-Free LCD Display Engine.
 * 
 * Target Architecture: Embedded C (MISRA-C:2012 Compliant, Zero Dynamic Memory).
 * Supports standard Alphanumeric Character LCDs (HD44780, ST7066U) and character-cell graphical displays.
 */

#ifndef LCD_CONFIG_H
#define LCD_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* DISPLAY GEOMETRY CONFIGURATION                                             */
/* ========================================================================== */

/** LCD Horizontal Column Count (standard 20 columns for 20x4 display) */
#ifndef LCD_CONFIG_COLS
#define LCD_CONFIG_COLS                     20U
#endif

/** LCD Vertical Row Count (standard 4 rows for 20x4 display) */
#ifndef LCD_CONFIG_ROWS
#define LCD_CONFIG_ROWS                     4U
#endif

/** Total character cells on screen */
#define LCD_CONFIG_TOTAL_CELLS              (LCD_CONFIG_COLS * LCD_CONFIG_ROWS)

/* ========================================================================== */
/* TIMING & SCHEDULING CONSTANTS (JUSTIFIED BY LCD RESPONSE CHARACTERISTICS)   */
/* ========================================================================== */

/** 
 * Refresh Interval (in milliseconds).
 * Default: 100ms (10 Hz).
 * Justification: 
 *  - TN/STN LCD liquid crystal optical response time (tr + tf) is 120ms to 200ms.
 *  - Refreshing faster than 10 Hz causes liquid crystal sluggishness/ghosting.
 *  - 10 Hz guarantees responsive human visual update while keeping I2C/SPI bus load < 5%.
 */
#ifndef LCD_CONFIG_REFRESH_INTERVAL_MS
#define LCD_CONFIG_REFRESH_INTERVAL_MS      100U
#endif

/** 
 * Automatic Page Rotation Dwell Time (in milliseconds).
 * Default: 4000ms (4.0 seconds per page).
 * Justification: 
 *  - Complies with ISO 9241 Ergonomics of Human-System Interaction.
 *  - Provides sufficient cognitive dwell time for operator reading (20-40 words/min).
 */
#ifndef LCD_CONFIG_PAGE_DWELL_MS
#define LCD_CONFIG_PAGE_DWELL_MS            4000U
#endif

/** 
 * Fault Indicator Flash Rate (in milliseconds per half-period).
 * Default: 500ms (1.0 Hz square wave blink for critical fault headers).
 */
#ifndef LCD_CONFIG_FAULT_BLINK_MS
#define LCD_CONFIG_FAULT_BLINK_MS           500U
#endif

/** Number of standard cyclical operational pages */
#define LCD_CONFIG_NUM_PAGES                3U

/* ========================================================================== */
/* RENDERING & BUS OPTIMIZATION FLAGS                                         */
/* ========================================================================== */

/** 
 * Enable Run-Length Span Batching for consecutive dirty cells on the same row.
 * Eliminates redundant setCursor() address commands when writing words.
 * 1 = Enabled (High Efficiency), 0 = Discrete cell positioning.
 */
#ifndef LCD_CONFIG_DIFF_BATCH_SPANS
#define LCD_CONFIG_DIFF_BATCH_SPANS         1U
#endif

/**
 * Periodic full refresh frame counter.
 * Forces a complete DDRAM sync every N frames to recover from electrical glitches (ESD).
 * 0 = Disabled, 200 = Every 20 seconds at 10 Hz.
 */
#ifndef LCD_CONFIG_ESD_RESYNC_FRAMES
#define LCD_CONFIG_ESD_RESYNC_FRAMES        200U
#endif

/* ========================================================================== */
/* HARDWARE BUS SIMULATION CHARACTERISTICS                                    */
/* ========================================================================== */

/** Bus Baud Rate (Hz) - Standard Mode I2C = 100 kHz, Fast Mode = 400 kHz */
#define LCD_CONFIG_I2C_BAUD_HZ              100000UL

/** I2C Bytes per character (PCF8574 4-bit mode: High nibble EN pulse + Low nibble EN pulse = 4 bytes) */
#define LCD_CONFIG_I2C_BYTES_PER_CHAR       4U

/** I2C Bytes per setCursor command (Address setup command = 4 bytes) */
#define LCD_CONFIG_I2C_BYTES_PER_CURSOR     4U

/** Execution delay of HD44780 clear screen command (microseconds: 1520us = 1.52ms) */
#define LCD_CONFIG_HD44780_CLEAR_TIME_US    1520UL

/** Execution delay of standard character write (microseconds: 37us) */
#define LCD_CONFIG_HD44780_WRITE_TIME_US    37UL

#ifdef __cplusplus
}
#endif

#endif /* LCD_CONFIG_H */
