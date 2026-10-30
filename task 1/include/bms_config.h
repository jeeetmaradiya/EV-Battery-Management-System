/**
 * @file bms_config.h
 * @brief Compile-time configuration for the Modular BMS Engine.
 * 
 * Scalable Architecture: The entire engine scales its buffers, loop bounds,
 * and data structures from this single compile-time constant BMS_CONFIG_CELL_COUNT.
 * 
 * Supports automotive and industrial pack topologies from 4S (e.g. 12V LFP lead-acid replacement)
 * up to 16S (e.g. 48V Light Electric Vehicle / Energy Storage System modules).
 */

#ifndef BMS_CONFIG_H
#define BMS_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ========================================================================== */
/* MASTER SCALABILITY CONFIGURATION                                           */
/* ========================================================================== */

/**
 * @brief Number of series battery cells in the module/pack.
 * 
 * Supported Range: 4 to 32 (default: 16).
 * Modifying this single constant automatically reconfigures:
 *  - Input cell voltage buffers
 *  - Balancing control bitmasks
 *  - Algorithmic loop boundaries
 *  - Diagnostic memory allocations
 */
#ifndef BMS_CONFIG_CELL_COUNT
#define BMS_CONFIG_CELL_COUNT               (16U)
#endif

/* Compile-time range validation */
#if (BMS_CONFIG_CELL_COUNT < 4U) || (BMS_CONFIG_CELL_COUNT > 32U)
#error "BMS_CONFIG_CELL_COUNT must be between 4 and 32 cells for this engine profile."
#endif

/* Bitmask width selection based on cell count */
#if (BMS_CONFIG_CELL_COUNT <= 8U)
typedef uint8_t bms_cell_mask_t;
#elif (BMS_CONFIG_CELL_COUNT <= 16U)
typedef uint16_t bms_cell_mask_t;
#else
typedef uint32_t bms_cell_mask_t;
#endif

/* ========================================================================== */
/* PHYSICAL & VOLTAGE LIMIT SPECIFICATIONS (Fixed-Point Millivolts)           */
/* ========================================================================== */

#define BMS_CELL_VOLTAGE_MIN_VALID_MV       (1500U)  /* Below this is considered open-wire / sensor fault */
#define BMS_CELL_VOLTAGE_MAX_VALID_MV       (4500U)  /* Above this is considered sensor saturation fault */

#define BMS_CELL_OVERVOLTAGE_FAULT_MV       (4250U)  /* Critical cell overvoltage trip */
#define BMS_CELL_UNDERVOLTAGE_FAULT_MV      (2700U)  /* Critical cell undervoltage trip */

/* ========================================================================== */
/* ADAPTIVE IMBALANCE ALGORITHM PARAMETERS                                    */
/* ========================================================================== */

/**
 * @brief Nominal base imbalance threshold in millivolts in the flat OCV region.
 * LiFePO4: ~25-30 mV; NMC/NCA: ~15-20 mV.
 */
#define BMS_IMBALANCE_BASE_THRESHOLD_MV     (25.0f)

/**
 * @brief Low SoC Knee Expansion Threshold (below this SoC%, knee threshold expands)
 */
#define BMS_SOC_KNEE_LOW_PCT                (20.0f)

/**
 * @brief High SoC Knee Expansion Threshold (above this SoC%, knee threshold expands)
 */
#define BMS_SOC_KNEE_HIGH_PCT               (80.0f)

/**
 * @brief Maximum multiplier for threshold expansion at extreme low SoC (< 10%)
 */
#define BMS_SOC_KNEE_MAX_MULTIPLIER         (2.8f)

/**
 * @brief Dynamic IR expansion coefficient (mV per Ampere of discharge current)
 * Accounts for cell internal resistance manufacturing tolerance (e.g. delta IR = 1.5 mOhm)
 */
#define BMS_DYNAMIC_IR_COEFF_MV_PER_A       (1.5f)

/**
 * @brief Maximum allowable adaptive threshold clamp to prevent masking genuine defects
 */
#define BMS_ADAPTIVE_THRESHOLD_MAX_CLAMP_MV (120.0f)

/* ========================================================================== */
/* TEMPORAL TREND FILTERING PARAMETERS                                        */
/* ========================================================================== */

/**
 * @brief Exponential Moving Average (EMA) smoothing factor for delta V (0.0 < alpha <= 1.0)
 * Lower values provide smoother derivatives; higher values provide faster response.
 */
#define BMS_TREND_FILTER_ALPHA              (0.15f)

/**
 * @brief Trend detection deadband rate of change in mV/second.
 * Rates above +0.25 mV/s indicate worsening imbalance.
 * Rates below -0.25 mV/s indicate converging/improving balance.
 */
#define BMS_TREND_RATE_DEADBAND_MV_S        (0.25f)

/**
 * @brief Consecutive sample confirmation count before changing trend status
 */
#define BMS_TREND_PERSISTENCE_COUNT         (5U)

#ifdef __cplusplus
}
#endif

#endif /* BMS_CONFIG_H */
