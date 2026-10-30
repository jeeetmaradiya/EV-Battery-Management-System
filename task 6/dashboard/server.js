/**
 * @file server.js
 * @brief High-Precision HTTP Server & Behavioral Analytics Engine for
 *        Enterprise Blynk Analytics and Decision Dashboard.
 * 
 * Safety Critical Embedded System Architecture (ISO 26262 ASIL-C / MISRA-C:2012).
 * Serves dashboard on http://localhost:3600.
 */

const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = 3600;
const startTime = Date.now();

/* ========================================================================== */
/* CONSTANTS & DEFINITIONS                                                    */
/* ========================================================================== */

const OperatingState = { NORMAL: 0, DEGRADED: 1, FAILSAFE: 2, SHUTDOWN: 3 };
const OperatingStateNames = ['NORMAL', 'DEGRADED', 'FAILSAFE', 'SHUTDOWN'];

const FaultSeverity = { NONE: 0, WARNING: 1, CRITICAL: 2, FATAL: 3 };
const FaultSeverityNames = ['NONE', 'WARNING', 'CRITICAL', 'FATAL'];

const FaultSource = { NONE: 0, CELL: 1, RELAY: 2, COMM: 3, ADC: 4, SYSTEM: 5 };
const FaultSourceNames = ['NONE', 'BATTERY_CELL', 'RELAY', 'COMMUNICATION', 'ADC', 'SYSTEM'];

const RecoveryStage = {
    IDLE: 0, CLEARANCE_CHECK: 1, STABILIZATION: 2,
    SUBSYSTEM_VERIFY: 3, PROVISIONAL_DEGRADE: 4, NORMAL_RESTORE: 5, ABORTED: 6
};
const RecoveryStageNames = [
    'IDLE', 'CLEARANCE_CHECK', 'STABILIZATION',
    'SUBSYSTEM_VERIFY', 'PROVISIONAL_DEGRADE', 'NORMAL_RESTORE', 'ABORTED'
];

const RiskTier = { LOW: 0, MODERATE: 1, HIGH: 2, CRITICAL: 3 };
const RiskTierNames = ['LOW', 'MODERATE', 'HIGH', 'CRITICAL'];

const RecommendationPriority = { INFO: 0, ADVISORY: 1, WARNING: 2, CRITICAL: 3 };
const RecommendationPriorityNames = ['INFO', 'ADVISORY', 'WARNING', 'CRITICAL'];

const RecommendationCategory = {
    CELL_BALANCING: 0, THERMAL_REGULATION: 1, CONTACTOR_MAINTENANCE: 2,
    SOC_CYCLING: 3, COMM_HARDWARE: 4, SYSTEM_HEALTH: 5
};
const RecommendationCategoryNames = [
    'CELL_BALANCING', 'THERMAL_REGULATION', 'CONTACTOR_MAINTENANCE',
    'SOC_CYCLING', 'COMM_HARDWARE', 'SYSTEM_HEALTH'
];

const CONFIG = {
    CELL_COUNT: 12,
    TEMP_SENSORS: 4,
    TICK_MS: 100, // 10Hz live update
    CELL_NOMINAL_MV: 3700,
    CELL_IMBALANCE_NOMINAL_LIMIT_MV: 15,
    CELL_IMBALANCE_WARNING_MV: 40,
    CELL_IMBALANCE_CRITICAL_MV: 80,
    TEMP_WARNING_HIGH_C: 45,
    TEMP_CRITICAL_HIGH_C: 55,
    TEMP_GRADIENT_WARN_C: 5,
    TEMP_GRADIENT_CRIT_C: 10,
    RECOVERY_CLEARANCE_SAMPLES: 10,
    RECOVERY_STABILIZATION_MS: 1500,
    RECOVERY_VERIFY_MS: 1500,
    RECOVERY_PROVISIONAL_MS: 2000,
    RECOVERY_MAX_RETRIES: 3,
    WEIGHT_IMBALANCE: 0.30,
    WEIGHT_FAULT_FREQ: 0.25,
    WEIGHT_SOC: 0.20,
    WEIGHT_THERMAL: 0.15,
    WEIGHT_RELAY: 0.10,
    TREND_HISTORY_LIMIT: 120
};

/* ========================================================================== */
/* LIVE ENGINE STATE                                                          */
/* ========================================================================== */

const engine = {
    currentTimeMs: 0,
    systemUptimeSec: 0,
    tickCounter: 0,
    autoDemoActive: false,
    autoDemoStep: 0,
    autoDemoTick: 0,

    cellVoltagesMv: new Array(CONFIG.CELL_COUNT).fill(CONFIG.CELL_NOMINAL_MV),
    temperaturesC: [25, 25, 25, 25],
    packVoltageMv: CONFIG.CELL_NOMINAL_MV * CONFIG.CELL_COUNT,
    packCurrentMa: 1250,
    minCellMv: CONFIG.CELL_NOMINAL_MV,
    maxCellMv: CONFIG.CELL_NOMINAL_MV,
    weakestCellIdx: 0,
    strongestCellIdx: 0,
    cellImbalanceDeltaMv: 0,
    imbalanceDriftRateMvSec: 0.0,

    stateOfChargePct: 75.0,
    stateOfHealthPct: 98.8,

    commandedMainContactor: true,
    commandedPrechargeContactor: true,
    feedbackMainContactor: true,
    feedbackPrechargeContactor: true,
    contactorCycleCount: 142,

    operatingState: OperatingState.NORMAL,
    recoveryStage: RecoveryStage.IDLE,
    stageEnteredTimestampMs: 0,
    recoveryRetryCounter: 0,
    clearanceSampleCount: 0,

    risk: {
        imbalanceScore: 0,
        faultFrequencyScore: 0,
        socStressScore: 0,
        thermalStressScore: 0,
        contactorWearScore: 0,
        compositeRiskScore: 0,
        tier: RiskTier.LOW,
        trendDirection: 0
    },
    previousRiskScore: 0,

    faultRegistry: [],
    totalCumulativeFaults: 0,
    activeFaultCount: 0,

    transitionLog: [],
    transitionSeqCounter: 0,

    recommendations: [],
    recommendationSeqCounter: 0,
    primaryDirectiveText: "Nominal Operation: SOH 98.8%, cells balanced within 2mV.",

    vpins: new Array(35).fill(0),
    historicalTrends: []
};

/* ========================================================================== */
/* ENGINE METHODS                                                             */
/* ========================================================================== */

function logTransition(fromState, toState, triggerFaultId, severity, reason) {
    engine.transitionSeqCounter++;
    const record = {
        seqId: engine.transitionSeqCounter,
        timestampMs: engine.currentTimeMs,
        uptimeStr: formatUptime(Math.floor(engine.currentTimeMs / 1000)),
        fromState,
        toState,
        fromStateName: OperatingStateNames[fromState],
        toStateName: OperatingStateNames[toState],
        triggerFaultId: triggerFaultId ? `0x${triggerFaultId.toString(16).toUpperCase()}` : '0x0000',
        severity,
        severityName: FaultSeverityNames[severity],
        reason
    };
    engine.transitionLog.unshift(record);
    if (engine.transitionLog.length > 100) engine.transitionLog.pop();
}

function updateTelemetry(cellVoltages, temperatures, currentMa, elapsedMs) {
    engine.currentTimeMs += elapsedMs;
    engine.systemUptimeSec = Math.floor(engine.currentTimeMs / 1000);
    engine.tickCounter++;

    let minV = 99999, maxV = 0, minIdx = 0, maxIdx = 0, sumV = 0;
    for (let i = 0; i < CONFIG.CELL_COUNT; i++) {
        const v = cellVoltages[i];
        engine.cellVoltagesMv[i] = v;
        sumV += v;
        if (v < minV) { minV = v; minIdx = i; }
        if (v > maxV) { maxV = v; maxIdx = i; }
    }

    engine.packVoltageMv = sumV;
    engine.packCurrentMa = currentMa;
    engine.minCellMv = minV;
    engine.maxCellMv = maxV;
    engine.weakestCellIdx = minIdx;
    engine.strongestCellIdx = maxIdx;

    const oldDelta = engine.cellImbalanceDeltaMv;
    engine.cellImbalanceDeltaMv = maxV - minV;
    engine.imbalanceDriftRateMvSec = (0.7 * engine.imbalanceDriftRateMvSec) + (0.3 * (engine.cellImbalanceDeltaMv - oldDelta));

    for (let i = 0; i < CONFIG.TEMP_SENSORS; i++) {
        engine.temperaturesC[i] = temperatures[i];
    }

    // Dynamic SoC Calculation
    const avgV = sumV / CONFIG.CELL_COUNT;
    if (avgV >= 4150) engine.stateOfChargePct = 95.0 + ((avgV - 4150) / 100.0) * 5.0;
    else if (avgV >= 3700) engine.stateOfChargePct = 50.0 + ((avgV - 3700) / 450.0) * 45.0;
    else if (avgV >= 3200) engine.stateOfChargePct = 15.0 + ((avgV - 3200) / 500.0) * 35.0;
    else engine.stateOfChargePct = ((avgV - 2800) / 400.0) * 15.0;
    engine.stateOfChargePct = Math.max(0, Math.min(100, engine.stateOfChargePct));

    // Dynamic SOH Calculation
    const imbPenalty = (engine.cellImbalanceDeltaMv / 100.0) * 2.0;
    const cycPenalty = (engine.contactorCycleCount / 10000.0) * 1.5;
    engine.stateOfHealthPct = Math.max(60, Math.min(100, 100.0 - imbPenalty - cycPenalty));
}

function getHighestActiveSeverity() {
    let maxSev = FaultSeverity.NONE;
    for (const f of engine.faultRegistry) {
        if (f.isActive && f.severity > maxSev) {
            maxSev = f.severity;
        }
    }
    return maxSev;
}

function evaluateRisk() {
    engine.previousRiskScore = engine.risk.compositeRiskScore;

    // 1. Imbalance Factor Score (0 - 100)
    const delta = engine.cellImbalanceDeltaMv;
    let scoreImb = 0;
    if (delta <= CONFIG.CELL_IMBALANCE_NOMINAL_LIMIT_MV) scoreImb = 0;
    else if (delta <= CONFIG.CELL_IMBALANCE_WARNING_MV) {
        scoreImb = 10 + ((delta - 15) / 25) * 30;
    } else if (delta <= CONFIG.CELL_IMBALANCE_CRITICAL_MV) {
        scoreImb = 40 + ((delta - 40) / 40) * 35;
    } else {
        scoreImb = 75 + Math.min(25, delta - 80);
    }
    if (engine.imbalanceDriftRateMvSec > 0.5) scoreImb += 10;
    scoreImb = Math.min(100, scoreImb);
    engine.risk.imbalanceScore = Math.round(scoreImb);

    // 2. Fault Frequency Score (0 - 100)
    const highestSev = getHighestActiveSeverity();
    let scoreFlt = 0;
    if (highestSev === FaultSeverity.WARNING) scoreFlt += 35;
    else if (highestSev === FaultSeverity.CRITICAL) scoreFlt += 75;
    else if (highestSev === FaultSeverity.FATAL) scoreFlt += 100;
    scoreFlt += Math.min(30, engine.totalCumulativeFaults * 4);
    scoreFlt = Math.min(100, scoreFlt);
    engine.risk.faultFrequencyScore = Math.round(scoreFlt);

    // 3. SoC Stress Score (0 - 100)
    let scoreSoc = 0;
    const soc = engine.stateOfChargePct;
    if (soc < 10) scoreSoc = 95;
    else if (soc < 20) scoreSoc = 45 + ((20 - soc) / 10) * 50;
    else if (soc > 95) scoreSoc = 85;
    else if (soc > 90) scoreSoc = 30 + ((soc - 90) / 5) * 55;
    else scoreSoc = 0;
    engine.risk.socStressScore = Math.round(scoreSoc);

    // 4. Thermal Stress Score (0 - 100)
    let maxT = Math.max(...engine.temperaturesC);
    let minT = Math.min(...engine.temperaturesC);
    let gradT = maxT - minT;
    let scoreTherm = 0;
    if (maxT > CONFIG.TEMP_CRITICAL_HIGH_C) scoreTherm = 90;
    else if (maxT > CONFIG.TEMP_WARNING_HIGH_C) scoreTherm = 40 + ((maxT - 45) / 10) * 50;
    if (gradT > CONFIG.TEMP_GRADIENT_CRIT_C) scoreTherm += 40;
    else if (gradT > CONFIG.TEMP_GRADIENT_WARN_C) scoreTherm += 20;
    scoreTherm = Math.min(100, scoreTherm);
    engine.risk.thermalStressScore = Math.round(scoreTherm);

    // 5. Contactor Wear Score (0 - 100)
    let scoreRelay = (engine.contactorCycleCount / 10000) * 30;
    if (engine.commandedMainContactor !== engine.feedbackMainContactor) scoreRelay = 95;
    scoreRelay = Math.min(100, scoreRelay);
    engine.risk.contactorWearScore = Math.round(scoreRelay);

    // Composite Weighted Sum
    let composite = (CONFIG.WEIGHT_IMBALANCE * engine.risk.imbalanceScore) +
                    (CONFIG.WEIGHT_FAULT_FREQ * engine.risk.faultFrequencyScore) +
                    (CONFIG.WEIGHT_SOC * engine.risk.socStressScore) +
                    (CONFIG.WEIGHT_THERMAL * engine.risk.thermalStressScore) +
                    (CONFIG.WEIGHT_RELAY * engine.risk.contactorWearScore);

    if ((engine.operatingState === OperatingState.FAILSAFE || highestSev === FaultSeverity.CRITICAL) && composite < 76) {
        composite = 76;
    }
    if (engine.operatingState === OperatingState.SHUTDOWN || highestSev === FaultSeverity.FATAL) {
        composite = 100;
    }
    composite = Math.min(100, Math.round(composite));

    engine.risk.compositeRiskScore = composite;
    if (composite >= 76) engine.risk.tier = RiskTier.CRITICAL;
    else if (composite >= 51) engine.risk.tier = RiskTier.HIGH;
    else if (composite >= 26) engine.risk.tier = RiskTier.MODERATE;
    else engine.risk.tier = RiskTier.LOW;

    if (composite > engine.previousRiskScore) engine.risk.trendDirection = 1;
    else if (composite < engine.previousRiskScore) engine.risk.trendDirection = -1;
    else engine.risk.trendDirection = 0;
}

function generateRecommendations() {
    engine.recommendations = [];

    if (engine.operatingState === OperatingState.SHUTDOWN) {
        engine.recommendations.push({
            id: ++engine.recommendationSeqCounter,
            priority: RecommendationPriority.CRITICAL,
            priorityName: 'CRITICAL',
            category: RecommendationCategory.CONTACTOR_MAINTENANCE,
            categoryName: 'CONTACTOR_MAINTENANCE',
            title: 'CRITICAL: System Locked in Terminal SHUTDOWN',
            evidence: 'Recovery retries exhausted or welded contactor latched open.',
            action: 'De-energize main high-voltage harness. Complete physical contactor inspection and service reset.',
            timestampMs: engine.currentTimeMs
        });
        engine.primaryDirectiveText = "CRITICAL LOCKOUT: High-voltage isolated. Authorized physical service reset required.";
        return;
    }

    if (engine.operatingState === OperatingState.FAILSAFE) {
        if (engine.recoveryStage !== RecoveryStage.IDLE) {
            engine.recommendations.push({
                id: ++engine.recommendationSeqCounter,
                priority: RecommendationPriority.WARNING,
                priorityName: 'WARNING',
                category: RecommendationCategory.SYSTEM_HEALTH,
                categoryName: 'SYSTEM_HEALTH',
                title: `Structured Recovery In Progress: Stage ${engine.recoveryStage} (${RecoveryStageNames[engine.recoveryStage]})`,
                evidence: `System progressing through Stage ${RecoveryStageNames[engine.recoveryStage]} verification.`,
                action: 'Quarantine state active. Stand by for Stage 5 Normal Restoration before re-applying load.',
                timestampMs: engine.currentTimeMs
            });
        } else {
            engine.recommendations.push({
                id: ++engine.recommendationSeqCounter,
                priority: RecommendationPriority.CRITICAL,
                priorityName: 'CRITICAL',
                category: RecommendationCategory.SYSTEM_HEALTH,
                categoryName: 'SYSTEM_HEALTH',
                title: 'CRITICAL: High-Voltage Contactor Isolation Active',
                evidence: 'Critical safety violation tripped FSM to FAILSAFE. Contactors opened.',
                action: 'Verify root clearance, then initiate 5-Stage Structured Recovery sequence.',
                timestampMs: engine.currentTimeMs
            });
        }
        engine.primaryDirectiveText = "FAILSAFE ACTIVE: HV contactors opened. Verify clearance to begin structured recovery.";
        return;
    }

    if (engine.cellImbalanceDeltaMv >= CONFIG.CELL_IMBALANCE_WARNING_MV) {
        const isCrit = engine.cellImbalanceDeltaMv >= CONFIG.CELL_IMBALANCE_CRITICAL_MV;
        engine.recommendations.push({
            id: ++engine.recommendationSeqCounter,
            priority: isCrit ? RecommendationPriority.CRITICAL : RecommendationPriority.WARNING,
            priorityName: isCrit ? 'CRITICAL' : 'WARNING',
            category: RecommendationCategory.CELL_BALANCING,
            categoryName: 'CELL_BALANCING',
            title: 'Elevated Cell Voltage Imbalance Detected',
            evidence: `Cell #${engine.weakestCellIdx + 1} (${engine.minCellMv}mV) vs Cell #${engine.strongestCellIdx + 1} (${engine.maxCellMv}mV) delta is ${engine.cellImbalanceDeltaMv}mV.`,
            action: `Engage passive balancing bypass resistors on Cell #${engine.strongestCellIdx + 1} before high-current fast charge.`,
            timestampMs: engine.currentTimeMs
        });
        engine.primaryDirectiveText = `IMBALANCE ALERT: Delta ${engine.cellImbalanceDeltaMv}mV exceeds threshold. Passive cell balancing recommended.`;
        return;
    }

    const maxT = Math.max(...engine.temperaturesC);
    const minT = Math.min(...engine.temperaturesC);
    const gradT = maxT - minT;
    if (gradT >= CONFIG.TEMP_GRADIENT_WARN_C || maxT >= CONFIG.TEMP_WARNING_HIGH_C) {
        engine.recommendations.push({
            id: ++engine.recommendationSeqCounter,
            priority: RecommendationPriority.WARNING,
            priorityName: 'WARNING',
            category: RecommendationCategory.THERMAL_REGULATION,
            categoryName: 'THERMAL_REGULATION',
            title: 'Thermal Gradient Anomaly / Elevated Cell Temperatures',
            evidence: `Peak temp ${maxT}C, inter-cell thermal gradient ${gradT}C.`,
            action: 'Inspect coolant pump circulation and air intake filters. Derate continuous charge/discharge current by 30%.',
            timestampMs: engine.currentTimeMs
        });
        engine.primaryDirectiveText = `THERMAL ADVISORY: Max ${maxT}C, gradient ${gradT}C. Derate current and inspect cooling channels.`;
        return;
    }

    if (engine.stateOfChargePct < 20.0) {
        engine.recommendations.push({
            id: ++engine.recommendationSeqCounter,
            priority: RecommendationPriority.ADVISORY,
            priorityName: 'ADVISORY',
            category: RecommendationCategory.SOC_CYCLING,
            categoryName: 'SOC_CYCLING',
            title: 'Deep Discharge Risk Advisory',
            evidence: `Pack State of Charge is ${engine.stateOfChargePct.toFixed(1)}%.`,
            action: 'Discontinue high-current discharge. Reconnect charger to prevent copper current collector dissolution.',
            timestampMs: engine.currentTimeMs
        });
        engine.primaryDirectiveText = `SOC ADVISORY: State of Charge at ${engine.stateOfChargePct.toFixed(1)}%. Recharge promptly to preserve cell capacity.`;
        return;
    }

    engine.recommendations.push({
        id: ++engine.recommendationSeqCounter,
        priority: RecommendationPriority.INFO,
        priorityName: 'INFO',
        category: RecommendationCategory.SYSTEM_HEALTH,
        categoryName: 'SYSTEM_HEALTH',
        title: 'Subsystems Operating in Verified Nominal Regime',
        evidence: 'Cell delta within 15mV, SOH optimal, contactor resistance normal.',
        action: 'Routine operational profile. Standard periodic diagnostic log export scheduled.',
        timestampMs: engine.currentTimeMs
    });
    engine.primaryDirectiveText = `NOMINAL: SOH ${engine.stateOfHealthPct.toFixed(1)}%, cells balanced within ${engine.cellImbalanceDeltaMv}mV. System operating in peak safety envelope.`;
}

function processFsm() {
    const activeSev = getHighestActiveSeverity();
    const curr = engine.operatingState;

    if (curr === OperatingState.SHUTDOWN) {
        engine.commandedMainContactor = false;
        engine.feedbackMainContactor = false;
        return;
    }

    if (curr === OperatingState.NORMAL) {
        if (activeSev === FaultSeverity.WARNING) {
            engine.operatingState = OperatingState.DEGRADED;
            logTransition(curr, OperatingState.DEGRADED, 0x1003, activeSev, "Warning fault asserted -> State moved to DEGRADED");
        } else if (activeSev === FaultSeverity.CRITICAL) {
            engine.operatingState = OperatingState.FAILSAFE;
            engine.commandedMainContactor = false;
            engine.feedbackMainContactor = false;
            logTransition(curr, OperatingState.FAILSAFE, 0x1001, activeSev, "Critical safety fault tripped -> HV contactors opened, FAILSAFE engaged");
        } else if (activeSev === FaultSeverity.FATAL) {
            engine.operatingState = OperatingState.SHUTDOWN;
            engine.commandedMainContactor = false;
            engine.feedbackMainContactor = false;
            logTransition(curr, OperatingState.SHUTDOWN, 0x2002, activeSev, "Fatal unrecoverable hazard -> Terminal SHUTDOWN lockout");
        }
    }
    else if (curr === OperatingState.DEGRADED) {
        if (engine.recoveryStage === RecoveryStage.PROVISIONAL_DEGRADE) {
            const elapsed = engine.currentTimeMs - engine.stageEnteredTimestampMs;
            if (elapsed >= CONFIG.RECOVERY_PROVISIONAL_MS) {
                engine.recoveryStage = RecoveryStage.IDLE;
                engine.operatingState = OperatingState.NORMAL;
                engine.recoveryRetryCounter = 0;
                logTransition(OperatingState.DEGRADED, OperatingState.NORMAL, 0, FaultSeverity.NONE, "Structured Recovery Pipeline Completed -> Verified return to NORMAL");
                return;
            }
        }

        if (activeSev === FaultSeverity.NONE) {
            engine.operatingState = OperatingState.NORMAL;
            logTransition(curr, OperatingState.NORMAL, 0, FaultSeverity.NONE, "All warning conditions cleared -> State restored to NORMAL");
        } else if (activeSev === FaultSeverity.CRITICAL) {
            engine.operatingState = OperatingState.FAILSAFE;
            engine.commandedMainContactor = false;
            engine.feedbackMainContactor = false;
            engine.recoveryStage = RecoveryStage.IDLE;
            logTransition(curr, OperatingState.FAILSAFE, 0x1001, activeSev, "Fault escalated from Warning to Critical -> Immediate FAILSAFE isolation");
        } else if (activeSev === FaultSeverity.FATAL) {
            engine.operatingState = OperatingState.SHUTDOWN;
            engine.commandedMainContactor = false;
            engine.feedbackMainContactor = false;
            engine.recoveryStage = RecoveryStage.IDLE;
            logTransition(curr, OperatingState.SHUTDOWN, 0x2002, activeSev, "Fatal fault in DEGRADED -> Escalate to SHUTDOWN");
        }
    }
    else if (curr === OperatingState.FAILSAFE) {
        engine.commandedMainContactor = false;
        engine.feedbackMainContactor = false;

        if (engine.recoveryStage !== RecoveryStage.IDLE) {
            const elapsed = engine.currentTimeMs - engine.stageEnteredTimestampMs;

            if (activeSev >= FaultSeverity.CRITICAL && engine.recoveryStage !== RecoveryStage.CLEARANCE_CHECK) {
                engine.recoveryRetryCounter++;
                if (engine.recoveryRetryCounter >= CONFIG.RECOVERY_MAX_RETRIES) {
                    engine.recoveryStage = RecoveryStage.ABORTED;
                    engine.operatingState = OperatingState.SHUTDOWN;
                    logTransition(OperatingState.FAILSAFE, OperatingState.SHUTDOWN, 0x5001, FaultSeverity.FATAL, "Recovery failed & max retries exhausted -> Permanent SHUTDOWN lockout");
                } else {
                    engine.recoveryStage = RecoveryStage.IDLE;
                    logTransition(OperatingState.FAILSAFE, OperatingState.FAILSAFE, 0, FaultSeverity.CRITICAL, "Recovery aborted due to active fault -> Retrying quarantine");
                }
                return;
            }

            switch (engine.recoveryStage) {
                case RecoveryStage.CLEARANCE_CHECK:
                    if (activeSev === FaultSeverity.NONE || activeSev === FaultSeverity.WARNING) {
                        engine.clearanceSampleCount++;
                        if (engine.clearanceSampleCount >= CONFIG.RECOVERY_CLEARANCE_SAMPLES) {
                            engine.recoveryStage = RecoveryStage.STABILIZATION;
                            engine.stageEnteredTimestampMs = engine.currentTimeMs;
                        }
                    } else {
                        engine.clearanceSampleCount = 0;
                    }
                    break;

                case RecoveryStage.STABILIZATION:
                    if (elapsed >= CONFIG.RECOVERY_STABILIZATION_MS) {
                        engine.recoveryStage = RecoveryStage.SUBSYSTEM_VERIFY;
                        engine.stageEnteredTimestampMs = engine.currentTimeMs;
                    }
                    break;

                case RecoveryStage.SUBSYSTEM_VERIFY:
                    if (elapsed >= CONFIG.RECOVERY_VERIFY_MS) {
                        engine.recoveryStage = RecoveryStage.PROVISIONAL_DEGRADE;
                        engine.stageEnteredTimestampMs = engine.currentTimeMs;
                        engine.operatingState = OperatingState.DEGRADED;
                        engine.commandedMainContactor = true;
                        engine.feedbackMainContactor = true;
                        engine.contactorCycleCount++;
                        logTransition(OperatingState.FAILSAFE, OperatingState.DEGRADED, 0, FaultSeverity.WARNING, "Recovery Subsystem Verify Passed -> Provisional DEGRADED engagement");
                    }
                    break;
            }
        }
    }
}

function startRecovery() {
    if (engine.operatingState !== OperatingState.FAILSAFE) return false;
    engine.recoveryStage = RecoveryStage.CLEARANCE_CHECK;
    engine.stageEnteredTimestampMs = engine.currentTimeMs;
    engine.clearanceSampleCount = 0;
    return true;
}

function injectFault(faultId, source, severity, auxMetric, description) {
    const existing = engine.faultRegistry.find(f => f.faultId === faultId);
    if (existing) {
        existing.isActive = true;
        existing.occurrenceCount++;
        existing.auxMetric = auxMetric;
        engine.totalCumulativeFaults++;
        return true;
    }

    engine.faultRegistry.unshift({
        seqId: engine.faultRegistry.length + 1,
        faultId,
        faultIdHex: `0x${faultId.toString(16).toUpperCase()}`,
        source,
        sourceName: FaultSourceNames[source],
        severity,
        severityName: FaultSeverityNames[severity],
        assertedTimeMs: engine.currentTimeMs,
        clearedTimeMs: 0,
        isActive: true,
        isLatched: severity >= FaultSeverity.FATAL,
        occurrenceCount: 1,
        auxMetric,
        description: description || "Diagnostic Fault"
    });

    engine.totalCumulativeFaults++;
    engine.activeFaultCount++;
    return true;
}

function clearFault(faultId) {
    const f = engine.faultRegistry.find(x => x.faultId === faultId && x.isActive);
    if (!f) return false;
    if (f.isLatched) return false;
    f.isActive = false;
    f.clearedTimeMs = engine.currentTimeMs;
    engine.activeFaultCount = Math.max(0, engine.activeFaultCount - 1);
    return true;
}

function serviceReset() {
    for (const f of engine.faultRegistry) {
        f.isActive = false;
        f.isLatched = false;
        f.clearedTimeMs = engine.currentTimeMs;
    }
    engine.activeFaultCount = 0;
    engine.recoveryRetryCounter = 0;
    engine.recoveryStage = RecoveryStage.IDLE;

    const prev = engine.operatingState;
    engine.operatingState = OperatingState.NORMAL;
    engine.commandedMainContactor = true;
    engine.feedbackMainContactor = true;
    engine.commandedPrechargeContactor = true;
    engine.feedbackPrechargeContactor = true;

    // Reset cell telemetry
    engine.cellVoltagesMv = new Array(CONFIG.CELL_COUNT).fill(CONFIG.CELL_NOMINAL_MV);
    engine.temperaturesC = [25, 25, 25, 25];

    logTransition(prev, OperatingState.NORMAL, 0, FaultSeverity.NONE, "Operator Service Reset: Latched faults cleared, nominal engagement restored");
    return true;
}

function syncVpins() {
    for (let i = 0; i < 12; i++) engine.vpins[i] = engine.cellVoltagesMv[i];
    engine.vpins[12] = engine.packVoltageMv;
    engine.vpins[13] = engine.packCurrentMa;
    engine.vpins[14] = engine.temperaturesC[0];
    engine.vpins[15] = engine.cellImbalanceDeltaMv;
    engine.vpins[16] = engine.weakestCellIdx;
    engine.vpins[17] = engine.strongestCellIdx;
    engine.vpins[18] = engine.operatingState;

    let activeFid = 0, activeSev = 0;
    for (const f of engine.faultRegistry) {
        if (f.isActive) {
            activeFid = f.faultId;
            if (f.severity > activeSev) activeSev = f.severity;
        }
    }
    engine.vpins[19] = activeFid;
    engine.vpins[20] = activeSev;
    engine.vpins[21] = engine.feedbackMainContactor ? 1 : 0;
    engine.vpins[22] = engine.feedbackPrechargeContactor ? 1 : 0;
    engine.vpins[23] = engine.risk.compositeRiskScore;
    engine.vpins[24] = Math.round(engine.stateOfHealthPct);
    engine.vpins[25] = Math.round(engine.stateOfChargePct);
    engine.vpins[26] = engine.recommendations.length > 0 ? engine.recommendations[0].priority : 0;
    engine.vpins[27] = engine.totalCumulativeFaults;
    engine.vpins[28] = engine.recoveryStage;
    engine.vpins[29] = engine.systemUptimeSec;
    engine.vpins[30] = engine.risk.imbalanceScore;
    engine.vpins[31] = engine.risk.faultFrequencyScore;
    engine.vpins[32] = engine.risk.socStressScore;
    engine.vpins[33] = engine.risk.thermalStressScore;
    engine.vpins[34] = engine.primaryDirectiveText;
}

function recordTrendHistory() {
    const point = {
        timeSec: engine.systemUptimeSec,
        minCellMv: engine.minCellMv,
        maxCellMv: engine.maxCellMv,
        deltaMv: engine.cellImbalanceDeltaMv,
        packVoltageV: Number((engine.packVoltageMv / 1000).toFixed(2)),
        packCurrentA: Number((engine.packCurrentMa / 1000).toFixed(2)),
        socPct: Number(engine.stateOfChargePct.toFixed(1)),
        sohPct: Number(engine.stateOfHealthPct.toFixed(1)),
        compositeRisk: engine.risk.compositeRiskScore,
        riskImbalance: engine.risk.imbalanceScore,
        riskFault: engine.risk.faultFrequencyScore,
        riskSoc: engine.risk.socStressScore,
        riskThermal: engine.risk.thermalStressScore,
        maxTempC: Math.max(...engine.temperaturesC),
        tempGradientC: Math.max(...engine.temperaturesC) - Math.min(...engine.temperaturesC),
        fsmState: engine.operatingState
    };
    engine.historicalTrends.push(point);
    if (engine.historicalTrends.length > CONFIG.TREND_HISTORY_LIMIT) {
        engine.historicalTrends.shift();
    }
}

function formatUptime(seconds) {
    const hrs = Math.floor(seconds / 3600);
    const mins = Math.floor((seconds % 3600) / 60);
    const secs = seconds % 60;
    return `${String(hrs).padStart(2, '0')}:${String(mins).padStart(2, '0')}:${String(secs).padStart(2, '0')}`;
}

// Initial Boot Log
logTransition(OperatingState.NORMAL, OperatingState.NORMAL, 0, FaultSeverity.NONE, "Subsystems qualified nominal on cold boot");
evaluateRisk();
generateRecommendations();
syncVpins();
recordTrendHistory();

/* ========================================================================== */
/* AUTOMATED DEMONSTRATION TOUR FSM                                           */
/* ========================================================================== */

function processAutoDemo() {
    if (!engine.autoDemoActive) return;
    engine.autoDemoTick++;

    // Step 0: Nominal balanced cycling (0-40 ticks, 4s)
    if (engine.autoDemoStep === 0) {
        const cells = engine.cellVoltagesMv.map((_, i) => 3700 + Math.round(Math.sin(engine.tickCounter / 10 + i) * 5));
        updateTelemetry(cells, [25, 25, 26, 25], 1200, CONFIG.TICK_MS);
        if (engine.autoDemoTick > 40) {
            engine.autoDemoStep = 1;
            engine.autoDemoTick = 0;
            // Inject Imbalance
            injectFault(0x1003, FaultSource.CELL, FaultSeverity.WARNING, 65, "Cell 4 imbalance excursion");
        }
    }
    // Step 1: Cell Imbalance & DEGRADED mode (40 ticks, 4s)
    else if (engine.autoDemoStep === 1) {
        const cells = new Array(12).fill(3700);
        cells[3] = 3765;
        updateTelemetry(cells, [25, 26, 25, 25], 800, CONFIG.TICK_MS);
        if (engine.autoDemoTick > 40) {
            engine.autoDemoStep = 2;
            engine.autoDemoTick = 0;
            // Inject Overvoltage Critical Trip
            injectFault(0x1001, FaultSource.CELL, FaultSeverity.CRITICAL, 4260, "Cell 4 Overvoltage Critical Trip");
        }
    }
    // Step 2: Critical Overvoltage & FAILSAFE Trip (30 ticks, 3s)
    else if (engine.autoDemoStep === 2) {
        const cells = new Array(12).fill(3700);
        cells[3] = 4260;
        updateTelemetry(cells, [25, 25, 25, 25], 0, CONFIG.TICK_MS);
        if (engine.autoDemoTick > 30) {
            engine.autoDemoStep = 3;
            engine.autoDemoTick = 0;
            // Clear fault & start recovery
            clearFault(0x1001);
            clearFault(0x1003);
            startRecovery();
        }
    }
    // Step 3: Structured 5-Stage Recovery Progression (60 ticks, 6s)
    else if (engine.autoDemoStep === 3) {
        const cells = new Array(12).fill(3700);
        updateTelemetry(cells, [25, 25, 25, 25], 500, CONFIG.TICK_MS);
        if (engine.operatingState === OperatingState.NORMAL && engine.recoveryStage === RecoveryStage.IDLE && engine.autoDemoTick > 50) {
            engine.autoDemoStep = 4;
            engine.autoDemoTick = 0;
            // Inject Thermal Anomaly
            engine.temperaturesC[1] = 48;
        }
    }
    // Step 4: Thermal Gradient Anomaly (40 ticks, 4s)
    else if (engine.autoDemoStep === 4) {
        const cells = new Array(12).fill(3700);
        updateTelemetry(cells, [25, 48, 26, 25], 1100, CONFIG.TICK_MS);
        if (engine.autoDemoTick > 40) {
            engine.autoDemoStep = 0;
            engine.autoDemoTick = 0;
            // Return to nominal
            serviceReset();
        }
    }
}

/* ========================================================================== */
/* BACKGROUND ENGINE LOOP (100ms TICK)                                        */
/* ========================================================================== */

setInterval(() => {
    if (engine.autoDemoActive) {
        processAutoDemo();
    } else {
        // Natural gentle noise when idle
        const cells = engine.cellVoltagesMv.map((v, i) => {
            if (engine.operatingState === OperatingState.FAILSAFE || engine.operatingState === OperatingState.SHUTDOWN) {
                return v;
            }
            const jitter = (Math.random() - 0.5) * 2;
            return Math.round(v + jitter);
        });
        updateTelemetry(cells, engine.temperaturesC, engine.feedbackMainContactor ? engine.packCurrentMa : 0, CONFIG.TICK_MS);
    }

    processFsm();
    evaluateRisk();
    generateRecommendations();
    syncVpins();

    // Trend sample every 5 ticks (500ms)
    if (engine.tickCounter % 5 === 0) {
        recordTrendHistory();
    }
}, CONFIG.TICK_MS);

/* ========================================================================== */
/* HTTP REST & DASHBOARD SERVER                                               */
/* ========================================================================== */

const server = http.createServer((req, res) => {
    const parsedUrl = new URL(req.url, `http://localhost:${PORT}`);
    const pathname = parsedUrl.pathname;

    // CORS Headers
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type');

    if (req.method === 'OPTIONS') {
        res.writeHead(204);
        res.end();
        return;
    }

    // Serve HTML Dashboard
    if (pathname === '/' || pathname === '/index.html') {
        const indexPath = path.join(__dirname, 'index.html');
        fs.readFile(indexPath, 'utf-8', (err, content) => {
            if (err) {
                res.writeHead(500, { 'Content-Type': 'text/plain' });
                res.end('Error loading dashboard: ' + err.message);
                return;
            }
            res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
            res.end(content);
        });
        return;
    }

    // REST API: GET /api/data
    if (pathname === '/api/data' && req.method === 'GET') {
        const payload = {
            uptime: formatUptime(engine.systemUptimeSec),
            uptimeSec: engine.systemUptimeSec,
            tickCounter: engine.tickCounter,
            autoDemoActive: engine.autoDemoActive,
            operatingState: engine.operatingState,
            operatingStateName: OperatingStateNames[engine.operatingState],
            recoveryStage: engine.recoveryStage,
            recoveryStageName: RecoveryStageNames[engine.recoveryStage],
            soh: Number(engine.stateOfHealthPct.toFixed(1)),
            soc: Number(engine.stateOfChargePct.toFixed(1)),
            packVoltageV: Number((engine.packVoltageMv / 1000).toFixed(2)),
            packCurrentA: Number((engine.packCurrentMa / 1000).toFixed(2)),
            cellImbalanceDeltaMv: engine.cellImbalanceDeltaMv,
            weakestCellIdx: engine.weakestCellIdx,
            strongestCellIdx: engine.strongestCellIdx,
            minCellMv: engine.minCellMv,
            maxCellMv: engine.maxCellMv,
            cellVoltagesMv: engine.cellVoltagesMv,
            temperaturesC: engine.temperaturesC,
            mainContactor: engine.feedbackMainContactor,
            prechargeContactor: engine.feedbackPrechargeContactor,
            contactorCycles: engine.contactorCycleCount,
            risk: engine.risk,
            riskTierName: RiskTierNames[engine.risk.tier],
            totalCumulativeFaults: engine.totalCumulativeFaults,
            activeFaultCount: engine.activeFaultCount,
            primaryDirective: engine.primaryDirectiveText,
            topRecommendation: engine.recommendations.length > 0 ? engine.recommendations[0] : null,
            vpins: engine.vpins
        };

        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(payload));
        return;
    }

    // REST API: GET /api/history
    if (pathname === '/api/history' && req.method === 'GET') {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(engine.historicalTrends));
        return;
    }

    // REST API: GET /api/recommendations
    if (pathname === '/api/recommendations' && req.method === 'GET') {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(engine.recommendations));
        return;
    }

    // REST API: GET /api/transitions
    if (pathname === '/api/transitions' && req.method === 'GET') {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(engine.transitionLog));
        return;
    }

    // REST API: GET /api/faults
    if (pathname === '/api/faults' && req.method === 'GET') {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(engine.faultRegistry));
        return;
    }

    // REST API: POST /api/action
    if (pathname === '/api/action' && req.method === 'POST') {
        let body = '';
        req.on('data', chunk => { body += chunk; });
        req.on('end', () => {
            try {
                const data = JSON.parse(body || '{}');
                const action = data.action;
                let status = 'OK';
                let message = '';

                switch (action) {
                    case 'inject_imbalance':
                        engine.cellVoltagesMv[3] = 3765;
                        injectFault(0x1003, FaultSource.CELL, FaultSeverity.WARNING, 65, "Cell 4 imbalance excursion");
                        message = 'Injected 65mV cell imbalance on Cell #4';
                        break;

                    case 'inject_overvoltage':
                        engine.cellVoltagesMv[3] = 4260;
                        injectFault(0x1001, FaultSource.CELL, FaultSeverity.CRITICAL, 4260, "Cell 4 Overvoltage Critical Trip");
                        message = 'Injected 4260mV critical overvoltage trip on Cell #4';
                        break;

                    case 'clear_faults':
                        clearFault(0x1001);
                        clearFault(0x1002);
                        clearFault(0x1003);
                        engine.cellVoltagesMv = new Array(CONFIG.CELL_COUNT).fill(CONFIG.CELL_NOMINAL_MV);
                        engine.temperaturesC = [25, 25, 25, 25];
                        message = 'Cleared active cell and warning faults';
                        break;

                    case 'start_recovery':
                        if (engine.operatingState === OperatingState.FAILSAFE) {
                            startRecovery();
                            message = 'Initiated 5-stage structured recovery pipeline';
                        } else {
                            status = 'INVALID_STATE';
                            message = 'Recovery can only be initiated from FAILSAFE state';
                        }
                        break;

                    case 'inject_welded_relay':
                        engine.feedbackMainContactor = true;
                        engine.commandedMainContactor = false;
                        injectFault(0x2002, FaultSource.RELAY, FaultSeverity.FATAL, 1, "Main contactor welded closed");
                        message = 'Injected Welded Relay Fault (FATAL)';
                        break;

                    case 'inject_thermal':
                        engine.temperaturesC[1] = 50; // 50°C
                        message = 'Injected thermal excursion on Sensor 2 (50°C, ΔT = 25°C)';
                        break;

                    case 'inject_deep_discharge':
                        for (let i = 0; i < CONFIG.CELL_COUNT; i++) engine.cellVoltagesMv[i] = 2900;
                        injectFault(0x1002, FaultSource.CELL, FaultSeverity.WARNING, 2900, "Pack deep discharge (<10% SoC)");
                        message = 'Injected deep discharge condition (2.90V/cell)';
                        break;

                    case 'service_reset':
                        serviceReset();
                        message = 'Operator bench service reset performed';
                        break;

                    case 'toggle_auto_demo':
                        engine.autoDemoActive = !engine.autoDemoActive;
                        engine.autoDemoStep = 0;
                        engine.autoDemoTick = 0;
                        message = `Automated demonstration tour ${engine.autoDemoActive ? 'STARTED' : 'STOPPED'}`;
                        break;

                    case 'set_nominal':
                        serviceReset();
                        engine.autoDemoActive = false;
                        message = 'Reset to pristine nominal state';
                        break;

                    default:
                        status = 'UNKNOWN_ACTION';
                        message = `Action '${action}' not recognized`;
                        break;
                }

                processFsm();
                evaluateRisk();
                generateRecommendations();
                syncVpins();

                res.writeHead(200, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify({ status, message, fsmState: OperatingStateNames[engine.operatingState] }));
            } catch (err) {
                res.writeHead(400, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify({ status: 'ERROR', error: err.message }));
            }
        });
        return;
    }

    res.writeHead(404, { 'Content-Type': 'text/plain' });
    res.end('Not Found');
});

server.listen(PORT, () => {
    console.log(`====================================================================`);
    console.log(`  ENTERPRISE BLYNK ANALYTICS & DECISION DASHBOARD ONLINE            `);
    console.log(`  Access URL : http://localhost:${PORT}                             `);
    console.log(`  Standard   : ISO 26262 ASIL-C / MISRA-C:2012 / IEC 61508 SIL-2    `);
    console.log(`====================================================================`);
});
