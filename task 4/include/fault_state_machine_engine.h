/**
 * @file fault_state_machine_engine.h
 * @brief Public API for Deterministic Fault State Machine & Structured Recovery.
 * 
 * Safety Critical Embedded System Architecture (ISO 26262 ASIL-C / MISRA-C:2012)
 */

#ifndef FAULT_STATE_MACHINE_ENGINE_H
#define FAULT_STATE_MACHINE_ENGINE_H

#include "fault_state_machine_types.h"
#include "fault_state_machine_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* ENGINE LIFECYCLE                                                           */
/* ========================================================================== */

/**
 * @brief Initializes the Fault State Machine, Transition Matrix, and Registries.
 * Starts in FAULT_STATE_NORMAL with zero active faults.
 */
void FaultFSM_Init(void);

/**
 * @brief Periodic execution cycle (typically invoked every 10 ms).
 * Evaluates all sensor inputs, runs anomaly detectors, updates recovery pipeline,
 * and executes state transitions according to the deterministic transition table.
 * 
 * @param current_timestamp_ms Monotonic millisecond system time.
 * @param telemetry Pointer to latest hardware sensor readings.
 */
void FaultFSM_Tick(uint32_t current_timestamp_ms, const SubsystemTelemetry_t *telemetry);

/* ========================================================================== */
/* STATE & FAULT STATUS QUERIES                                               */
/* ========================================================================== */

/**
 * @brief Returns current operational state (NORMAL, DEGRADED, FAILSAFE, SHUTDOWN).
 */
FaultOperatingState_t FaultFSM_GetCurrentState(void);

/**
 * @brief Converts operating state enum to human-readable string.
 */
const char *FaultFSM_StateToString(FaultOperatingState_t state);

/**
 * @brief Converts fault source enum to human-readable string.
 */
const char *FaultFSM_SourceToString(FaultSource_t source);

/**
 * @brief Converts fault ID code to standardized mnemonic string.
 */
const char *FaultFSM_FaultIdToString(FaultId_t fault_id);

/**
 * @brief Returns true if a specific fault ID is currently active in registry.
 */
bool FaultFSM_IsFaultActive(FaultId_t fault_id);

/**
 * @brief Returns count of currently active faults.
 */
uint32_t FaultFSM_GetActiveFaultCount(void);

/**
 * @brief Returns pointer to a fault record by registry index (0 to count-1).
 */
const FaultRecord_t *FaultFSM_GetFaultRecord(uint32_t index);

/**
 * @brief Returns total registry capacity.
 */
uint32_t FaultFSM_GetFaultRegistryCapacity(void);

/* ========================================================================== */
/* STRUCTURED RECOVERY INTERFACE                                              */
/* ========================================================================== */

/**
 * @brief Initiates structured recovery sequence from FAILSAFE state.
 * Returns true if recovery sequence successfully engaged, false if rejected
 * (e.g., active un-cleared faults, max retries exceeded, or not in FAILSAFE).
 * 
 * @param current_timestamp_ms Monotonic system time.
 */
bool FaultFSM_RequestRecovery(uint32_t current_timestamp_ms);

/**
 * @brief Returns current recovery verification stage.
 */
RecoveryStage_t FaultFSM_GetRecoveryStage(void);

/**
 * @brief Converts recovery stage enum to descriptive string.
 */
const char *FaultFSM_RecoveryStageToString(RecoveryStage_t stage);

/**
 * @brief Returns total number of recovery attempts since entering FAILSAFE.
 */
uint32_t FaultFSM_GetRecoveryRetryCount(void);

/**
 * @brief Returns qualified verification samples accumulated in Stage 3.
 */
uint32_t FaultFSM_GetRecoverySampleProgress(void);

/* ========================================================================== */
/* STRUCTURED TRANSITION AUDIT LOG                                            */
/* ========================================================================== */

/**
 * @brief Returns total number of logged state transitions since initialization.
 */
uint32_t FaultFSM_GetTransitionLogCount(void);

/**
 * @brief Retrieves transition log entry by index (0 is oldest available).
 */
const StateTransitionLog_t *FaultFSM_GetTransitionLog(uint32_t index);

/**
 * @brief Formats transition log entry into a standardized ISO-style text string.
 */
void FaultFSM_FormatStructuredLog(const StateTransitionLog_t *entry, char *out_buffer, uint32_t buffer_len);

/**
 * @brief Formats transition log entry into structured JSON format.
 */
void FaultFSM_FormatJsonLog(const StateTransitionLog_t *entry, char *out_buffer, uint32_t buffer_len);

/* ========================================================================== */
/* FAULT INJECTION & DIAGNOSTICS (TEST HARNESS)                               */
/* ========================================================================== */

/**
 * @brief Injects an isolated fault into the registry with auxiliary telemetry.
 */
void FaultFSM_InjectFault(FaultId_t fault_id, uint32_t current_timestamp_ms, const uint32_t aux[4], const char *desc);

/**
 * @brief Clears an active fault from the registry (simulating hardware repair/clearance).
 */
void FaultFSM_ClearFault(FaultId_t fault_id, uint32_t current_timestamp_ms);

/**
 * @brief Resets all fault latches and restores FSM to NORMAL (service test use only).
 */
void FaultFSM_ServiceReset(uint32_t current_timestamp_ms);

#ifdef __cplusplus
}
#endif

#endif /* FAULT_STATE_MACHINE_ENGINE_H */
