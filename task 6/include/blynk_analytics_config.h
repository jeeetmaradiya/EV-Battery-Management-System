/**
 * @file blynk_analytics_config.h
 * @brief Safety Configuration Thresholds & Tuning Parameters for Analytics Engine.
 * 
 * Safety Critical Embedded System Architecture (ISO 26262 ASIL-C / MISRA-C:2012)
 */

#ifndef BLYNK_ANALYTICS_CONFIG_H
#define BLYNK_ANALYTICS_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* HARDWARE CELL & STRING TOPOLOGY                                            */
/* ========================================================================== */

#define BLYNK_ANALYTICS_CELL_COUNT               12U
#define BLYNK_ANALYTICS_TEMP_SENSOR_COUNT        4U

/* ========================================================================== */
/* CELL VOLTAGE SAFETY THRESHOLDS (mV)                                        */
/* ========================================================================== */

#define CELL_UNDERVOLTAGE_CRITICAL_MV            2800U
#define CELL_UNDERVOLTAGE_WARNING_MV             3000U
#define CELL_NOMINAL_RESTING_MV                  3700U
#define CELL_OVERVOLTAGE_WARNING_MV              4200U
#define CELL_OVERVOLTAGE_CRITICAL_MV             4250U

/* ========================================================================== */
/* CELL IMBALANCE THRESHOLDS (mV)                                             */
/* ========================================================================== */

#define CELL_IMBALANCE_NOMINAL_LIMIT_MV          15U
#define CELL_IMBALANCE_WARNING_MV                40U
#define CELL_IMBALANCE_CRITICAL_MV               80U
#define CELL_IMBALANCE_DRIFT_RATE_ALERT_MV_SEC   0.5f

/* ========================================================================== */
/* THERMAL SAFETY THRESHOLDS (deg C)                                          */
/* ========================================================================== */

#define TEMP_OPTIMAL_MIN_C                       15
#define TEMP_OPTIMAL_MAX_C                       35
#define TEMP_WARNING_HIGH_C                      45
#define TEMP_CRITICAL_HIGH_C                     55
#define TEMP_INTER_CELL_GRADIENT_WARN_C          5
#define TEMP_INTER_CELL_GRADIENT_CRIT_C          10

/* ========================================================================== */
/* STATE OF CHARGE (SoC) RISK ENVELOPES (%)                                   */
/* ========================================================================== */

#define SOC_DEEP_DISCHARGE_CRITICAL_PCT          10.0f
#define SOC_DEEP_DISCHARGE_WARNING_PCT           20.0f
#define SOC_NOMINAL_LOW_PCT                      25.0f
#define SOC_NOMINAL_HIGH_PCT                     80.0f
#define SOC_HIGH_VOLTAGE_STRESS_WARN_PCT         90.0f
#define SOC_OVERCHARGE_CRITICAL_PCT              95.0f

/* ========================================================================== */
/* MULTI-FACTOR COMPOSITE RISK SCORE (CRS) WEIGHTS                            */
/* (Sum of weights equals 1.000)                                              */
/* ========================================================================== */

#define RISK_WEIGHT_IMBALANCE                    0.30f
#define RISK_WEIGHT_FAULT_FREQUENCY              0.25f
#define RISK_WEIGHT_SOC_STRESS                   0.20f
#define RISK_WEIGHT_THERMAL                      0.15f
#define RISK_WEIGHT_CONTACTOR_WEAR               0.10f

/* Risk Tier Thresholds */
#define RISK_SCORE_TIER_MODERATE_THRESHOLD       26U
#define RISK_SCORE_TIER_HIGH_THRESHOLD           51U
#define RISK_SCORE_TIER_CRITICAL_THRESHOLD       76U

/* ========================================================================== */
/* STRUCTURED RECOVERY CONFIGURATION (TASK 4 ALIGNMENT)                       */
/* ========================================================================== */

#define RECOVERY_CLEARANCE_CHECK_SAMPLES         5U
#define RECOVERY_STABILIZATION_DWELL_MS          1500U
#define RECOVERY_SUBSYSTEM_VERIFY_DWELL_MS       1500U
#define RECOVERY_PROVISIONAL_DEGRADE_DWELL_MS    2000U
#define RECOVERY_MAX_ALLOWED_RETRIES             3U

/* ========================================================================== */
/* BUFFER CAPACITIES & AUDIT RECORD DEPTHS                                    */
/* ========================================================================== */

#define STRUCTURED_FAULT_REGISTRY_CAPACITY       32U
#define STATE_TRANSITION_LOG_CAPACITY            64U
#define OPERATOR_RECOMMENDATION_CAPACITY         16U
#define HISTORICAL_TREND_SAMPLE_DEPTH            120U

/* ========================================================================== */
/* CONTACTOR TIMING & WEAR THRESHOLDS                                         */
/* ========================================================================== */

#define CONTACTOR_TRANSITION_TIMEOUT_MS          80U
#define CONTACTOR_MAX_NOMINAL_CYCLES             10000U

#ifdef __cplusplus
}
#endif

#endif /* BLYNK_ANALYTICS_CONFIG_H */
