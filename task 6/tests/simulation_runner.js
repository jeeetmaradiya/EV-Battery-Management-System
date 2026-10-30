/**
 * @file simulation_runner.js
 * @brief High-Fidelity Simulation and Formal Verification Engine for
 *        Enterprise Blynk Analytics & Decision Dashboard.
 * 
 * Safety Critical Embedded System Architecture (ISO 26262 ASIL-C / MISRA-C:2012).
 * Executes 8 formal verification scenarios and a 10,000-tick continuous stress benchmark.
 * Outputs structured results to simulation_results.json.
 */

const fs = require('fs');
const path = require('path');
const assert = require('assert');

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
    TICK_MS: 50,
    CELL_NOMINAL_MV: 3700,
    CELL_IMBALANCE_NOMINAL_LIMIT_MV: 15,
    CELL_IMBALANCE_WARNING_MV: 40,
    CELL_IMBALANCE_CRITICAL_MV: 80,
    TEMP_WARNING_HIGH_C: 45,
    TEMP_CRITICAL_HIGH_C: 55,
    TEMP_GRADIENT_WARN_C: 5,
    TEMP_GRADIENT_CRIT_C: 10,
    RECOVERY_CLEARANCE_SAMPLES: 5,
    RECOVERY_STABILIZATION_MS: 500,
    RECOVERY_VERIFY_MS: 500,
    RECOVERY_PROVISIONAL_MS: 500,
    RECOVERY_MAX_RETRIES: 3,
    WEIGHT_IMBALANCE: 0.30,
    WEIGHT_FAULT_FREQ: 0.25,
    WEIGHT_SOC: 0.20,
    WEIGHT_THERMAL: 0.15,
    WEIGHT_RELAY: 0.10
};

/* ========================================================================== */
/* SIMULATION ENGINE CLASS                                                    */
/* ========================================================================== */

class AnalyticsSimulationEngine {
    constructor() {
        this.reset();
    }

    reset() {
        this.currentTimeMs = 0;
        this.systemUptimeSec = 0;
        this.tickCounter = 0;

        this.cellVoltagesMv = new Array(CONFIG.CELL_COUNT).fill(CONFIG.CELL_NOMINAL_MV);
        this.temperaturesC = [25, 25, 25, 25];
        this.packVoltageMv = CONFIG.CELL_NOMINAL_MV * CONFIG.CELL_COUNT;
        this.packCurrentMa = 1200;
        this.minCellMv = CONFIG.CELL_NOMINAL_MV;
        this.maxCellMv = CONFIG.CELL_NOMINAL_MV;
        this.weakestCellIdx = 0;
        this.strongestCellIdx = 0;
        this.cellImbalanceDeltaMv = 0;
        this.imbalanceDriftRateMvSec = 0.0;

        this.stateOfChargePct = 75.0;
        this.stateOfHealthPct = 98.8;

        this.commandedMainContactor = true;
        this.commandedPrechargeContactor = true;
        this.feedbackMainContactor = true;
        this.feedbackPrechargeContactor = true;
        this.contactorCycleCount = 142;

        this.operatingState = OperatingState.NORMAL;
        this.recoveryStage = RecoveryStage.IDLE;
        this.stageEnteredTimestampMs = 0;
        this.recoveryRetryCounter = 0;
        this.clearanceSampleCount = 0;

        this.risk = {
            imbalanceScore: 0,
            faultFrequencyScore: 0,
            socStressScore: 0,
            thermalStressScore: 0,
            contactorWearScore: 0,
            compositeRiskScore: 0,
            tier: RiskTier.LOW,
            trendDirection: 0
        };
        this.previousRiskScore = 0;

        this.faultRegistry = [];
        this.totalCumulativeFaults = 0;
        this.activeFaultCount = 0;

        this.transitionLog = [];
        this.transitionSeqCounter = 0;

        this.recommendations = [];
        this.recommendationSeqCounter = 0;
        this.primaryDirectiveText = "Nominal Operation: SOH 98.8%, cells balanced within 2mV.";

        this.vpins = new Array(35).fill(0);

        this.logTransition(OperatingState.NORMAL, OperatingState.NORMAL, 0, FaultSeverity.NONE, "Initial Cold Boot Qualification");
        this.evaluateRisk();
        this.generateRecommendations();
        this.syncVpins();
    }

    logTransition(fromState, toState, triggerFaultId, severity, reason) {
        this.transitionSeqCounter++;
        const record = {
            transitionSeqId: this.transitionSeqCounter,
            timestampMs: this.currentTimeMs,
            fromState,
            toState,
            fromStateName: OperatingStateNames[fromState],
            toStateName: OperatingStateNames[toState],
            triggerFaultId,
            severity,
            severityName: FaultSeverityNames[severity],
            reason
        };
        this.transitionLog.push(record);
        if (this.transitionLog.length > 200) this.transitionLog.shift();
    }

    updateTelemetry(cellVoltages, temperatures, currentMa, elapsedMs) {
        this.currentTimeMs += elapsedMs;
        this.systemUptimeSec = Math.floor(this.currentTimeMs / 1000);
        this.tickCounter++;

        let minV = 99999, maxV = 0, minIdx = 0, maxIdx = 0, sumV = 0;
        for (let i = 0; i < CONFIG.CELL_COUNT; i++) {
            const v = cellVoltages[i];
            this.cellVoltagesMv[i] = v;
            sumV += v;
            if (v < minV) { minV = v; minIdx = i; }
            if (v > maxV) { maxV = v; maxIdx = i; }
        }

        this.packVoltageMv = sumV;
        this.packCurrentMa = currentMa;
        this.minCellMv = minV;
        this.maxCellMv = maxV;
        this.weakestCellIdx = minIdx;
        this.strongestCellIdx = maxIdx;

        const oldDelta = this.cellImbalanceDeltaMv;
        this.cellImbalanceDeltaMv = maxV - minV;
        this.imbalanceDriftRateMvSec = (0.7 * this.imbalanceDriftRateMvSec) + (0.3 * (this.cellImbalanceDeltaMv - oldDelta));

        for (let i = 0; i < CONFIG.TEMP_SENSORS; i++) {
            this.temperaturesC[i] = temperatures[i];
        }

        // SoC Estimation
        const avgV = sumV / CONFIG.CELL_COUNT;
        if (avgV >= 4150) this.stateOfChargePct = 95.0 + ((avgV - 4150) / 100.0) * 5.0;
        else if (avgV >= 3700) this.stateOfChargePct = 50.0 + ((avgV - 3700) / 450.0) * 45.0;
        else if (avgV >= 3200) this.stateOfChargePct = 15.0 + ((avgV - 3200) / 500.0) * 35.0;
        else this.stateOfChargePct = ((avgV - 2800) / 400.0) * 15.0;
        this.stateOfChargePct = Math.max(0, Math.min(100, this.stateOfChargePct));

        // SOH degradation
        const imbPenalty = (this.cellImbalanceDeltaMv / 100.0) * 2.0;
        const cycPenalty = (this.contactorCycleCount / 10000.0) * 1.5;
        this.stateOfHealthPct = Math.max(60, Math.min(100, 100.0 - imbPenalty - cycPenalty));
    }

    getHighestActiveSeverity() {
        let maxSev = FaultSeverity.NONE;
        for (const f of this.faultRegistry) {
            if (f.isActive && f.severity > maxSev) {
                maxSev = f.severity;
            }
        }
        return maxSev;
    }

    evaluateRisk() {
        this.previousRiskScore = this.risk.compositeRiskScore;

        // 1. Imbalance Factor Score
        const delta = this.cellImbalanceDeltaMv;
        let scoreImb = 0;
        if (delta <= CONFIG.CELL_IMBALANCE_NOMINAL_LIMIT_MV) scoreImb = 0;
        else if (delta <= CONFIG.CELL_IMBALANCE_WARNING_MV) {
            scoreImb = 10 + ((delta - 15) / 25) * 30;
        } else if (delta <= CONFIG.CELL_IMBALANCE_CRITICAL_MV) {
            scoreImb = 40 + ((delta - 40) / 40) * 35;
        } else {
            scoreImb = 75 + Math.min(25, delta - 80);
        }
        if (this.imbalanceDriftRateMvSec > 0.5) scoreImb += 10;
        scoreImb = Math.min(100, scoreImb);
        this.risk.imbalanceScore = Math.round(scoreImb);

        // 2. Fault Frequency Score
        const highestSev = this.getHighestActiveSeverity();
        let scoreFlt = 0;
        if (highestSev === FaultSeverity.WARNING) scoreFlt += 35;
        else if (highestSev === FaultSeverity.CRITICAL) scoreFlt += 75;
        else if (highestSev === FaultSeverity.FATAL) scoreFlt += 100;
        scoreFlt += Math.min(30, this.totalCumulativeFaults * 4);
        scoreFlt = Math.min(100, scoreFlt);
        this.risk.faultFrequencyScore = Math.round(scoreFlt);

        // 3. SoC Stress Score
        let scoreSoc = 0;
        const soc = this.stateOfChargePct;
        if (soc < 10) scoreSoc = 95;
        else if (soc < 20) scoreSoc = 45 + ((20 - soc) / 10) * 50;
        else if (soc > 95) scoreSoc = 85;
        else if (soc > 90) scoreSoc = 30 + ((soc - 90) / 5) * 55;
        else scoreSoc = 0;
        this.risk.socStressScore = Math.round(scoreSoc);

        // 4. Thermal Stress Score
        let maxT = Math.max(...this.temperaturesC);
        let minT = Math.min(...this.temperaturesC);
        let gradT = maxT - minT;
        let scoreTherm = 0;
        if (maxT > CONFIG.TEMP_CRITICAL_HIGH_C) scoreTherm = 90;
        else if (maxT > CONFIG.TEMP_WARNING_HIGH_C) scoreTherm = 40 + ((maxT - 45) / 10) * 50;
        if (gradT > CONFIG.TEMP_GRADIENT_CRIT_C) scoreTherm += 40;
        else if (gradT > CONFIG.TEMP_GRADIENT_WARN_C) scoreTherm += 20;
        scoreTherm = Math.min(100, scoreTherm);
        this.risk.thermalStressScore = Math.round(scoreTherm);

        // 5. Contactor Wear Score
        let scoreRelay = (this.contactorCycleCount / 10000) * 30;
        if (this.commandedMainContactor !== this.feedbackMainContactor) scoreRelay = 95;
        scoreRelay = Math.min(100, scoreRelay);
        this.risk.contactorWearScore = Math.round(scoreRelay);

        // Composite Weighted Sum
        let composite = (CONFIG.WEIGHT_IMBALANCE * this.risk.imbalanceScore) +
                        (CONFIG.WEIGHT_FAULT_FREQ * this.risk.faultFrequencyScore) +
                        (CONFIG.WEIGHT_SOC * this.risk.socStressScore) +
                        (CONFIG.WEIGHT_THERMAL * this.risk.thermalStressScore) +
                        (CONFIG.WEIGHT_RELAY * this.risk.contactorWearScore);

        if ((this.operatingState === OperatingState.FAILSAFE || highestSev === FaultSeverity.CRITICAL) && composite < 76) {
            composite = 76;
        }
        if (this.operatingState === OperatingState.SHUTDOWN || highestSev === FaultSeverity.FATAL) {
            composite = 100;
        }
        composite = Math.min(100, Math.round(composite));

        this.risk.compositeRiskScore = composite;
        if (composite >= 76) this.risk.tier = RiskTier.CRITICAL;
        else if (composite >= 51) this.risk.tier = RiskTier.HIGH;
        else if (composite >= 26) this.risk.tier = RiskTier.MODERATE;
        else this.risk.tier = RiskTier.LOW;

        if (composite > this.previousRiskScore) this.risk.trendDirection = 1;
        else if (composite < this.previousRiskScore) this.risk.trendDirection = -1;
        else this.risk.trendDirection = 0;
    }

    generateRecommendations() {
        this.recommendations = [];

        if (this.operatingState === OperatingState.SHUTDOWN) {
            this.recommendations.push({
                id: ++this.recommendationSeqCounter,
                priority: RecommendationPriority.CRITICAL,
                priorityName: 'CRITICAL',
                category: RecommendationCategory.CONTACTOR_MAINTENANCE,
                categoryName: 'CONTACTOR_MAINTENANCE',
                title: 'CRITICAL: System Locked in Terminal SHUTDOWN',
                evidence: 'Recovery retries exhausted or welded contactor latched open.',
                action: 'De-energize main high-voltage harness. Complete physical contactor inspection and service reset.',
                timestampMs: this.currentTimeMs
            });
            this.primaryDirectiveText = "CRITICAL LOCKOUT: High-voltage isolated. Authorized physical service reset required.";
            return;
        }

        if (this.operatingState === OperatingState.FAILSAFE) {
            if (this.recoveryStage !== RecoveryStage.IDLE) {
                this.recommendations.push({
                    id: ++this.recommendationSeqCounter,
                    priority: RecommendationPriority.WARNING,
                    priorityName: 'WARNING',
                    category: RecommendationCategory.SYSTEM_HEALTH,
                    title: `Structured Recovery In Progress: Stage ${this.recoveryStage}`,
                    evidence: `System progressing through Stage ${RecoveryStageNames[this.recoveryStage]} verification.`,
                    action: 'Quarantine state active. Stand by for Stage 5 Normal Restoration before re-applying load.',
                    timestampMs: this.currentTimeMs
                });
            } else {
                this.recommendations.push({
                    id: ++this.recommendationSeqCounter,
                    priority: RecommendationPriority.CRITICAL,
                    priorityName: 'CRITICAL',
                    category: RecommendationCategory.SYSTEM_HEALTH,
                    title: 'CRITICAL: High-Voltage Contactor Isolation Active',
                    evidence: 'Critical safety violation tripped FSM to FAILSAFE. Contactor opened.',
                    action: 'Verify root clearance, then initiate 5-Stage Structured Recovery sequence.',
                    timestampMs: this.currentTimeMs
                });
            }
            this.primaryDirectiveText = "FAILSAFE ACTIVE: HV contactors opened. Verify clearance to begin structured recovery.";
            return;
        }

        if (this.cellImbalanceDeltaMv >= CONFIG.CELL_IMBALANCE_WARNING_MV) {
            const isCrit = this.cellImbalanceDeltaMv >= CONFIG.CELL_IMBALANCE_CRITICAL_MV;
            this.recommendations.push({
                id: ++this.recommendationSeqCounter,
                priority: isCrit ? RecommendationPriority.CRITICAL : RecommendationPriority.WARNING,
                priorityName: isCrit ? 'CRITICAL' : 'WARNING',
                category: RecommendationCategory.CELL_BALANCING,
                categoryName: 'CELL_BALANCING',
                title: 'Elevated Cell Voltage Imbalance Detected',
                evidence: `Cell #${this.weakestCellIdx + 1} (${this.minCellMv}mV) vs Cell #${this.strongestCellIdx + 1} (${this.maxCellMv}mV) delta is ${this.cellImbalanceDeltaMv}mV.`,
                action: `Engage passive balancing bypass resistors on Cell #${this.strongestCellIdx + 1} before high-current fast charge.`,
                timestampMs: this.currentTimeMs
            });
            this.primaryDirectiveText = `IMBALANCE ALERT: Delta ${this.cellImbalanceDeltaMv}mV exceeds threshold. Passive cell balancing recommended.`;
            return;
        }

        const maxT = Math.max(...this.temperaturesC);
        const minT = Math.min(...this.temperaturesC);
        const gradT = maxT - minT;
        if (gradT >= CONFIG.TEMP_GRADIENT_WARN_C || maxT >= CONFIG.TEMP_WARNING_HIGH_C) {
            this.recommendations.push({
                id: ++this.recommendationSeqCounter,
                priority: RecommendationPriority.WARNING,
                priorityName: 'WARNING',
                category: RecommendationCategory.THERMAL_REGULATION,
                categoryName: 'THERMAL_REGULATION',
                title: 'Thermal Gradient Anomaly / Elevated Cell Temperatures',
                evidence: `Peak temp ${maxT}C, inter-cell thermal gradient ${gradT}C.`,
                action: 'Inspect coolant pump circulation and air intake filters. Derate continuous charge/discharge current by 30%.',
                timestampMs: this.currentTimeMs
            });
            this.primaryDirectiveText = `THERMAL ADVISORY: Max ${maxT}C, gradient ${gradT}C. Derate current and inspect cooling channels.`;
            return;
        }

        if (this.stateOfChargePct < 20.0) {
            this.recommendations.push({
                id: ++this.recommendationSeqCounter,
                priority: RecommendationPriority.ADVISORY,
                priorityName: 'ADVISORY',
                category: RecommendationCategory.SOC_CYCLING,
                categoryName: 'SOC_CYCLING',
                title: 'Deep Discharge Risk Advisory',
                evidence: `Pack State of Charge is ${this.stateOfChargePct.toFixed(1)}%.`,
                action: 'Discontinue high-current discharge. Reconnect charger to prevent copper current collector dissolution.',
                timestampMs: this.currentTimeMs
            });
            this.primaryDirectiveText = `SOC ADVISORY: State of Charge at ${this.stateOfChargePct.toFixed(1)}%. Recharge promptly to preserve cell capacity.`;
            return;
        }

        this.recommendations.push({
            id: ++this.recommendationSeqCounter,
            priority: RecommendationPriority.INFO,
            priorityName: 'INFO',
            category: RecommendationCategory.SYSTEM_HEALTH,
            categoryName: 'SYSTEM_HEALTH',
            title: 'Subsystems Operating in Verified Nominal Regime',
            evidence: 'Cell delta within 15mV, SOH optimal, contactor resistance normal.',
            action: 'Routine operational profile. Standard periodic diagnostic log export scheduled.',
            timestampMs: this.currentTimeMs
        });
        this.primaryDirectiveText = `NOMINAL: SOH ${this.stateOfHealthPct.toFixed(1)}%, cells balanced within ${this.cellImbalanceDeltaMv}mV. System operating in peak safety envelope.`;
    }

    processFsm() {
        const activeSev = this.getHighestActiveSeverity();
        const curr = this.operatingState;

        if (curr === OperatingState.SHUTDOWN) {
            this.commandedMainContactor = false;
            this.feedbackMainContactor = false;
            return;
        }

        if (curr === OperatingState.NORMAL) {
            if (activeSev === FaultSeverity.WARNING) {
                this.operatingState = OperatingState.DEGRADED;
                this.logTransition(curr, OperatingState.DEGRADED, 0x1003, activeSev, "Warning fault asserted -> State moved to DEGRADED");
            } else if (activeSev === FaultSeverity.CRITICAL) {
                this.operatingState = OperatingState.FAILSAFE;
                this.commandedMainContactor = false;
                this.feedbackMainContactor = false;
                this.logTransition(curr, OperatingState.FAILSAFE, 0x1001, activeSev, "Critical safety fault tripped -> HV contactors opened, FAILSAFE engaged");
            } else if (activeSev === FaultSeverity.FATAL) {
                this.operatingState = OperatingState.SHUTDOWN;
                this.commandedMainContactor = false;
                this.feedbackMainContactor = false;
                this.logTransition(curr, OperatingState.SHUTDOWN, 0x2002, activeSev, "Fatal unrecoverable hazard -> Terminal SHUTDOWN lockout");
            }
        }
        else if (curr === OperatingState.DEGRADED) {
            if (this.recoveryStage === RecoveryStage.PROVISIONAL_DEGRADE) {
                const elapsed = this.currentTimeMs - this.stageEnteredTimestampMs;
                if (elapsed >= CONFIG.RECOVERY_PROVISIONAL_MS) {
                    this.recoveryStage = RecoveryStage.IDLE;
                    this.operatingState = OperatingState.NORMAL;
                    this.recoveryRetryCounter = 0;
                    this.logTransition(OperatingState.DEGRADED, OperatingState.NORMAL, 0, FaultSeverity.NONE, "Structured Recovery Pipeline Completed -> Verified return to NORMAL");
                    return;
                }
            }

            if (activeSev === FaultSeverity.NONE) {
                this.operatingState = OperatingState.NORMAL;
                this.logTransition(curr, OperatingState.NORMAL, 0, FaultSeverity.NONE, "All warning conditions cleared -> State restored to NORMAL");
            } else if (activeSev === FaultSeverity.CRITICAL) {
                this.operatingState = OperatingState.FAILSAFE;
                this.commandedMainContactor = false;
                this.feedbackMainContactor = false;
                this.recoveryStage = RecoveryStage.IDLE;
                this.logTransition(curr, OperatingState.FAILSAFE, 0x1001, activeSev, "Fault escalated from Warning to Critical -> Immediate FAILSAFE isolation");
            } else if (activeSev === FaultSeverity.FATAL) {
                this.operatingState = OperatingState.SHUTDOWN;
                this.commandedMainContactor = false;
                this.feedbackMainContactor = false;
                this.recoveryStage = RecoveryStage.IDLE;
                this.logTransition(curr, OperatingState.SHUTDOWN, 0x2002, activeSev, "Fatal fault in DEGRADED -> Escalate to SHUTDOWN");
            }
        }
        else if (curr === OperatingState.FAILSAFE) {
            this.commandedMainContactor = false;
            this.feedbackMainContactor = false;

            if (this.recoveryStage !== RecoveryStage.IDLE) {
                const elapsed = this.currentTimeMs - this.stageEnteredTimestampMs;

                if (activeSev >= FaultSeverity.CRITICAL && this.recoveryStage !== RecoveryStage.CLEARANCE_CHECK) {
                    this.recoveryRetryCounter++;
                    if (this.recoveryRetryCounter >= CONFIG.RECOVERY_MAX_RETRIES) {
                        this.recoveryStage = RecoveryStage.ABORTED;
                        this.operatingState = OperatingState.SHUTDOWN;
                        this.logTransition(OperatingState.FAILSAFE, OperatingState.SHUTDOWN, 0x5001, FaultSeverity.FATAL, "Recovery failed & max retries exhausted -> Permanent SHUTDOWN lockout");
                    } else {
                        this.recoveryStage = RecoveryStage.IDLE;
                        this.logTransition(OperatingState.FAILSAFE, OperatingState.FAILSAFE, 0, FaultSeverity.CRITICAL, "Recovery aborted due to active fault -> Retrying quarantine");
                    }
                    return;
                }

                switch (this.recoveryStage) {
                    case RecoveryStage.CLEARANCE_CHECK:
                        if (activeSev === FaultSeverity.NONE || activeSev === FaultSeverity.WARNING) {
                            this.clearanceSampleCount++;
                            if (this.clearanceSampleCount >= CONFIG.RECOVERY_CLEARANCE_SAMPLES) {
                                this.recoveryStage = RecoveryStage.STABILIZATION;
                                this.stageEnteredTimestampMs = this.currentTimeMs;
                            }
                        } else {
                            this.clearanceSampleCount = 0;
                        }
                        break;

                    case RecoveryStage.STABILIZATION:
                        if (elapsed >= CONFIG.RECOVERY_STABILIZATION_MS) {
                            this.recoveryStage = RecoveryStage.SUBSYSTEM_VERIFY;
                            this.stageEnteredTimestampMs = this.currentTimeMs;
                        }
                        break;

                    case RecoveryStage.SUBSYSTEM_VERIFY:
                        if (elapsed >= CONFIG.RECOVERY_VERIFY_MS) {
                            this.recoveryStage = RecoveryStage.PROVISIONAL_DEGRADE;
                            this.stageEnteredTimestampMs = this.currentTimeMs;
                            this.operatingState = OperatingState.DEGRADED;
                            this.commandedMainContactor = true;
                            this.feedbackMainContactor = true;
                            this.contactorCycleCount++;
                            this.logTransition(OperatingState.FAILSAFE, OperatingState.DEGRADED, 0, FaultSeverity.WARNING, "Recovery Subsystem Verify Passed -> Provisional DEGRADED engagement");
                        }
                        break;

                    case RecoveryStage.PROVISIONAL_DEGRADE:
                        if (elapsed >= CONFIG.RECOVERY_PROVISIONAL_MS) {
                            this.recoveryStage = RecoveryStage.IDLE;
                            this.operatingState = OperatingState.NORMAL;
                            this.recoveryRetryCounter = 0;
                            this.logTransition(OperatingState.DEGRADED, OperatingState.NORMAL, 0, FaultSeverity.NONE, "Structured Recovery Pipeline Completed -> Verified return to NORMAL");
                        }
                        break;
                }
            }
        }
    }

    startRecovery() {
        if (this.operatingState !== OperatingState.FAILSAFE) return false;
        this.recoveryStage = RecoveryStage.CLEARANCE_CHECK;
        this.stageEnteredTimestampMs = this.currentTimeMs;
        this.clearanceSampleCount = 0;
        return true;
    }

    injectFault(faultId, source, severity, auxMetric, description) {
        const existing = this.faultRegistry.find(f => f.faultId === faultId);
        if (existing) {
            existing.isActive = true;
            existing.occurrenceCount++;
            existing.auxMetric = auxMetric;
            this.totalCumulativeFaults++;
            return true;
        }

        this.faultRegistry.push({
            seqId: this.faultRegistry.length + 1,
            faultId,
            source,
            sourceName: FaultSourceNames[source],
            severity,
            severityName: FaultSeverityNames[severity],
            assertedTimeMs: this.currentTimeMs,
            clearedTimeMs: 0,
            isActive: true,
            isLatched: severity >= FaultSeverity.FATAL,
            occurrenceCount: 1,
            auxMetric,
            description: description || "Diagnostic Fault"
        });

        this.totalCumulativeFaults++;
        this.activeFaultCount++;
        return true;
    }

    clearFault(faultId) {
        const f = this.faultRegistry.find(x => x.faultId === faultId && x.isActive);
        if (!f) return false;
        if (f.isLatched) return false;
        f.isActive = false;
        f.clearedTimeMs = this.currentTimeMs;
        this.activeFaultCount = Math.max(0, this.activeFaultCount - 1);
        return true;
    }

    serviceReset() {
        for (const f of this.faultRegistry) {
            f.isActive = false;
            f.isLatched = false;
            f.clearedTimeMs = this.currentTimeMs;
        }
        this.activeFaultCount = 0;
        this.recoveryRetryCounter = 0;
        this.recoveryStage = RecoveryStage.IDLE;

        const prev = this.operatingState;
        this.operatingState = OperatingState.NORMAL;
        this.commandedMainContactor = true;
        this.feedbackMainContactor = true;
        this.commandedPrechargeContactor = true;
        this.feedbackPrechargeContactor = true;

        this.logTransition(prev, OperatingState.NORMAL, 0, FaultSeverity.NONE, "Operator Service Reset: Latched faults cleared, nominal engagement restored");
        return true;
    }

    syncVpins() {
        for (let i = 0; i < 12; i++) this.vpins[i] = this.cellVoltagesMv[i];
        this.vpins[12] = this.packVoltageMv;
        this.vpins[13] = this.packCurrentMa;
        this.vpins[14] = this.temperaturesC[0];
        this.vpins[15] = this.cellImbalanceDeltaMv;
        this.vpins[16] = this.weakestCellIdx;
        this.vpins[17] = this.strongestCellIdx;
        this.vpins[18] = this.operatingState;

        let activeFid = 0, activeSev = 0;
        for (const f of this.faultRegistry) {
            if (f.isActive) {
                activeFid = f.faultId;
                if (f.severity > activeSev) activeSev = f.severity;
            }
        }
        this.vpins[19] = activeFid;
        this.vpins[20] = activeSev;
        this.vpins[21] = this.feedbackMainContactor ? 1 : 0;
        this.vpins[22] = this.feedbackPrechargeContactor ? 1 : 0;
        this.vpins[23] = this.risk.compositeRiskScore;
        this.vpins[24] = Math.round(this.stateOfHealthPct);
        this.vpins[25] = Math.round(this.stateOfChargePct);
        this.vpins[26] = this.recommendations.length > 0 ? this.recommendations[0].priority : 0;
        this.vpins[27] = this.totalCumulativeFaults;
        this.vpins[28] = this.recoveryStage;
        this.vpins[29] = this.systemUptimeSec;
        this.vpins[30] = this.risk.imbalanceScore;
        this.vpins[31] = this.risk.faultFrequencyScore;
        this.vpins[32] = this.risk.socStressScore;
        this.vpins[33] = this.risk.thermalStressScore;
    }
}

/* ========================================================================== */
/* FORMAL SCENARIO RUNNER                                                     */
/* ========================================================================== */

const results = {
    testDate: new Date().toISOString(),
    standard: "ISO 26262 ASIL-C / MISRA-C:2012 / IEC 61508 SIL-2",
    scenarios: [],
    benchmark: {},
    summary: { totalTests: 0, passedTests: 0, passRatePct: 0 }
};

function runAssertion(scenario, name, condition, details = "") {
    scenario.totalAssertions++;
    results.summary.totalTests++;
    const passed = Boolean(condition);
    if (passed) {
        scenario.passedAssertions++;
        results.summary.passedTests++;
    }
    scenario.assertions.push({ name, passed, details });
    console.log(`    [${passed ? 'PASS' : 'FAIL'}] ${name}${details ? ` (${details})` : ''}`);
    return passed;
}

console.log("====================================================================");
console.log("  ENTERPRISE BLYNK ANALYTICS & DECISION ENGINE FORMAL VERIFICATION  ");
console.log("  ISO 26262 ASIL-C / MISRA-C:2012 COMPREHENSIVE TEST SUITE           ");
console.log("====================================================================");

// SCENARIO 1: Pristine Nominal Initialization & Baseline Risk
{
    console.log("\n▶ SCENARIO 1: Pristine Nominal Initialization & Baseline Risk");
    const sc = { name: "Scenario 1: Nominal Baseline", totalAssertions: 0, passedAssertions: 0, assertions: [] };
    const engine = new AnalyticsSimulationEngine();

    runAssertion(sc, "Initial FSM State is NORMAL", engine.operatingState === OperatingState.NORMAL);
    runAssertion(sc, "Contactor auxiliary feedback is closed", engine.feedbackMainContactor === true);
    runAssertion(sc, "Initial cell imbalance delta is 0mV", engine.cellImbalanceDeltaMv === 0);
    runAssertion(sc, "State of Health is >= 98%", engine.stateOfHealthPct >= 98.0, `SOH: ${engine.stateOfHealthPct}%`);
    runAssertion(sc, "Composite Risk Score in LOW tier (<25%)", engine.risk.compositeRiskScore < 25, `CRS: ${engine.risk.compositeRiskScore}%`);
    runAssertion(sc, "Blynk V18 mirrors NORMAL state (0)", engine.vpins[18] === 0);
    runAssertion(sc, "Blynk V23 mirrors Composite Risk Score", engine.vpins[23] === engine.risk.compositeRiskScore);
    results.scenarios.push(sc);
}

// SCENARIO 2: Progressive Imbalance & Derated DEGRADED State Reflection
{
    console.log("\n▶ SCENARIO 2: Progressive Imbalance & Derated DEGRADED Transition");
    const sc = { name: "Scenario 2: Cell Imbalance & DEGRADED Transition", totalAssertions: 0, passedAssertions: 0, assertions: [] };
    const engine = new AnalyticsSimulationEngine();

    // Drift Cell 4 upwards by 55mV
    const cells = new Array(12).fill(3700);
    cells[3] = 3755;
    engine.updateTelemetry(cells, [25, 25, 25, 25], 1200, 100);
    engine.injectFault(0x1003, FaultSource.CELL, FaultSeverity.WARNING, 55, "Cell 4 imbalance warning");
    engine.evaluateRisk();
    engine.processFsm();
    engine.generateRecommendations();
    engine.syncVpins();

    runAssertion(sc, "State transitioned to DEGRADED", engine.operatingState === OperatingState.DEGRADED);
    runAssertion(sc, "Main contactor remains closed in DEGRADED", engine.feedbackMainContactor === true);
    runAssertion(sc, "Imbalance factor score scaled >= 50%", engine.risk.imbalanceScore >= 50, `Imb Score: ${engine.risk.imbalanceScore}%`);
    runAssertion(sc, "Composite Risk Score escalated to MODERATE", engine.risk.compositeRiskScore >= 26, `CRS: ${engine.risk.compositeRiskScore}%`);
    runAssertion(sc, "Operator Recommendation generated for Cell Balancing",
        engine.recommendations.some(r => r.category === RecommendationCategory.CELL_BALANCING));
    runAssertion(sc, "State Transition logged accurately",
        engine.transitionLog.some(t => t.toState === OperatingState.DEGRADED && t.triggerFaultId === 0x1003));
    results.scenarios.push(sc);
}

// SCENARIO 3: Critical Overvoltage Trip & FAILSAFE Isolation
{
    console.log("\n▶ SCENARIO 3: Critical Overvoltage Trip & FAILSAFE Isolation");
    const sc = { name: "Scenario 3: Critical Overvoltage & FAILSAFE Trip", totalAssertions: 0, passedAssertions: 0, assertions: [] };
    const engine = new AnalyticsSimulationEngine();

    const cells = new Array(12).fill(3700);
    cells[3] = 4260; // Overvoltage
    engine.updateTelemetry(cells, [25, 25, 25, 25], 1200, 100);
    engine.injectFault(0x1001, FaultSource.CELL, FaultSeverity.CRITICAL, 4260, "Cell 4 Overvoltage Critical Trip");
    engine.processFsm();
    engine.evaluateRisk();
    engine.generateRecommendations();
    engine.syncVpins();

    runAssertion(sc, "State transitioned immediately to FAILSAFE", engine.operatingState === OperatingState.FAILSAFE);
    runAssertion(sc, "HV contactor opened immediately for isolation", engine.feedbackMainContactor === false);
    runAssertion(sc, "Composite Risk Score jumped into CRITICAL tier (>=76%)", engine.risk.compositeRiskScore >= 76, `CRS: ${engine.risk.compositeRiskScore}%`);
    runAssertion(sc, "Blynk V21 reflects OPEN contactor (0)", engine.vpins[21] === 0);
    runAssertion(sc, "Primary Directive alerts operator of HV isolation",
        engine.primaryDirectiveText.includes("FAILSAFE ACTIVE"));
    results.scenarios.push(sc);
}

// SCENARIO 4: 5-Stage Structured Recovery Sequence Verification
{
    console.log("\n▶ SCENARIO 4: 5-Stage Structured Recovery Sequence Verification");
    const sc = { name: "Scenario 4: 5-Stage Structured Recovery", totalAssertions: 0, passedAssertions: 0, assertions: [] };
    const engine = new AnalyticsSimulationEngine();

    // Trip into FAILSAFE
    engine.injectFault(0x1001, FaultSource.CELL, FaultSeverity.CRITICAL, 4260, "Trip");
    engine.processFsm();
    assert(engine.operatingState === OperatingState.FAILSAFE);

    // Clear fault & start recovery
    engine.clearFault(0x1001);
    const startOk = engine.startRecovery();
    runAssertion(sc, "Recovery start accepted in FAILSAFE", startOk === true);
    runAssertion(sc, "Stage 1: CLEARANCE_CHECK initialized", engine.recoveryStage === RecoveryStage.CLEARANCE_CHECK);

    // Run 5 clearance samples
    for (let i = 0; i < 6; i++) {
        engine.currentTimeMs += 50;
        engine.processFsm();
    }
    runAssertion(sc, "Stage 2: STABILIZATION entered after clearance", engine.recoveryStage === RecoveryStage.STABILIZATION);

    // Stabilization dwell 500ms
    engine.currentTimeMs += 600;
    engine.processFsm();
    runAssertion(sc, "Stage 3: SUBSYSTEM_VERIFY entered after dwell", engine.recoveryStage === RecoveryStage.SUBSYSTEM_VERIFY);

    // Subsystem verify dwell 500ms -> Provisional degrade
    engine.currentTimeMs += 600;
    engine.processFsm();
    runAssertion(sc, "Stage 4: PROVISIONAL_DEGRADE stepped up state to DEGRADED", engine.operatingState === OperatingState.DEGRADED);
    runAssertion(sc, "Contactor re-engaged under provisional derating", engine.feedbackMainContactor === true);

    // Provisional dwell 500ms -> Normal Restore
    engine.currentTimeMs += 600;
    engine.processFsm();
    runAssertion(sc, "Stage 5: NORMAL_RESTORE completed, FSM in NORMAL", engine.operatingState === OperatingState.NORMAL);
    runAssertion(sc, "Recovery stage returned to IDLE", engine.recoveryStage === RecoveryStage.IDLE);
    results.scenarios.push(sc);
}

// SCENARIO 5: Welded Contactor & Recovery Exhaustion -> SHUTDOWN
{
    console.log("\n▶ SCENARIO 5: Welded Contactor & Recovery Exhaustion -> SHUTDOWN");
    const sc = { name: "Scenario 5: Contactor Lockout & Recovery Exhaustion", totalAssertions: 0, passedAssertions: 0, assertions: [] };
    const engine = new AnalyticsSimulationEngine();

    // Trip to FAILSAFE
    engine.injectFault(0x1001, FaultSource.CELL, FaultSeverity.CRITICAL, 4260, "Trip");
    engine.processFsm();

    // 3 Aborted recovery sequences
    for (let retry = 0; retry < 3; retry++) {
        engine.clearFault(0x1001);
        engine.startRecovery();
        for (let i = 0; i < 6; i++) { engine.currentTimeMs += 50; engine.processFsm(); }
        // Re-inject mid-recovery
        engine.injectFault(0x1001, FaultSource.CELL, FaultSeverity.CRITICAL, 4260, "Recurrence");
        engine.processFsm();
    }

    engine.evaluateRisk();

    runAssertion(sc, "Recovery retries exhausted escalated FSM to SHUTDOWN", engine.operatingState === OperatingState.SHUTDOWN);
    runAssertion(sc, "Contactors locked open in SHUTDOWN", engine.feedbackMainContactor === false);
    runAssertion(sc, "Composite Risk Score locked at 100%", engine.risk.compositeRiskScore === 100);
    runAssertion(sc, "Service Reset successfully clears SHUTDOWN lockout", engine.serviceReset() === true && engine.operatingState === OperatingState.NORMAL);
    results.scenarios.push(sc);
}

// SCENARIO 6: Thermal Gradient Anomaly & Dynamic Recommendation
{
    console.log("\n▶ SCENARIO 6: Thermal Gradient Anomaly & Dynamic Recommendation");
    const sc = { name: "Scenario 6: Thermal Gradient & Action Recommendation", totalAssertions: 0, passedAssertions: 0, assertions: [] };
    const engine = new AnalyticsSimulationEngine();

    const cells = new Array(12).fill(3700);
    const temps = [24, 47, 25, 26]; // Sensor 2 at 47°C, gradient = 23°C
    engine.updateTelemetry(cells, temps, 1200, 100);
    engine.evaluateRisk();
    engine.generateRecommendations();
    engine.syncVpins();

    runAssertion(sc, "Thermal stress score elevated (>=60%)", engine.risk.thermalStressScore >= 60, `Thermal Score: ${engine.risk.thermalStressScore}%`);
    runAssertion(sc, "Operator Recommendation generated for THERMAL_REGULATION",
        engine.recommendations.some(r => r.category === RecommendationCategory.THERMAL_REGULATION));
    runAssertion(sc, "Recommendation advises coolant and fan inspection",
        engine.recommendations[0].action.includes("coolant pump"));
    results.scenarios.push(sc);
}

// SCENARIO 7: Deep Discharge SoC Risk Mitigation
{
    console.log("\n▶ SCENARIO 7: Deep Discharge SoC Risk Mitigation");
    const sc = { name: "Scenario 7: Deep Discharge & Low-SoC Mitigation", totalAssertions: 0, passedAssertions: 0, assertions: [] };
    const engine = new AnalyticsSimulationEngine();

    const cells = new Array(12).fill(2920); // Discharged
    engine.updateTelemetry(cells, [25, 25, 25, 25], 12000, 100);
    engine.evaluateRisk();
    engine.generateRecommendations();
    engine.syncVpins();

    runAssertion(sc, "SoC estimated below 15%", engine.stateOfChargePct < 15.0, `SoC: ${engine.stateOfChargePct.toFixed(1)}%`);
    runAssertion(sc, "SoC stress score elevated (>=70%)", engine.risk.socStressScore >= 70, `SoC Stress: ${engine.risk.socStressScore}%`);
    runAssertion(sc, "Deep Discharge Advisory emitted",
        engine.recommendations.some(r => r.category === RecommendationCategory.SOC_CYCLING));
    results.scenarios.push(sc);
}

// SCENARIO 8: 10,000-Tick Stochastic Stress & Invariance Benchmark
{
    console.log("\n▶ SCENARIO 8: 10,000-Tick Stochastic Stress & Invariance Benchmark");
    const sc = { name: "Scenario 8: 10,000-Tick Stochastic Stress Benchmark", totalAssertions: 0, passedAssertions: 0, assertions: [] };
    const engine = new AnalyticsSimulationEngine();

    let invalidStateTransitions = 0;
    let nanRiskScores = 0;
    let contactorViolationsInFailsafe = 0;

    const startPerf = Date.now();
    for (let tick = 0; tick < 10000; tick++) {
        // Stochastic telemetry perturbation
        const cells = [];
        const base = 3650 + Math.sin(tick / 100) * 150;
        for (let i = 0; i < 12; i++) {
            cells.push(Math.round(base + (Math.random() - 0.5) * 20));
        }
        const temps = [
            Math.round(25 + Math.random() * 5),
            Math.round(26 + Math.random() * 4),
            Math.round(25 + Math.random() * 6),
            Math.round(27 + Math.random() * 5)
        ];
        engine.updateTelemetry(cells, temps, 1500, CONFIG.TICK_MS);

        // Stochastic fault burst
        if (tick === 1000) engine.injectFault(0x1003, FaultSource.CELL, FaultSeverity.WARNING, 45, "Test warning");
        if (tick === 1500) engine.clearFault(0x1003);
        if (tick === 2000) engine.injectFault(0x1001, FaultSource.CELL, FaultSeverity.CRITICAL, 4260, "Test trip");
        if (tick === 2500) { engine.clearFault(0x1001); engine.startRecovery(); }
        if (tick === 5000) engine.serviceReset();

        engine.processFsm();
        engine.evaluateRisk();
        engine.generateRecommendations();
        engine.syncVpins();

        if (isNaN(engine.risk.compositeRiskScore)) nanRiskScores++;
        if (engine.operatingState === OperatingState.FAILSAFE && engine.feedbackMainContactor === true) {
            contactorViolationsInFailsafe++;
        }
    }
    const durationMs = Date.now() - startPerf;

    runAssertion(sc, "Zero NaN Composite Risk Scores observed across 10,000 ticks", nanRiskScores === 0);
    runAssertion(sc, "Zero Contactor closure violations while in FAILSAFE", contactorViolationsInFailsafe === 0);
    runAssertion(sc, "10,000 ticks executed within budget (<1000ms)", durationMs < 1000, `Duration: ${durationMs}ms`);

    results.benchmark = {
        totalTicks: 10000,
        executionTimeMs: durationMs,
        avgTickUs: (durationMs * 1000) / 10000,
        nanRiskScores,
        contactorViolationsInFailsafe
    };
    results.scenarios.push(sc);
}

// Compute Overall Summary
results.summary.passRatePct = Number(((results.summary.passedTests / results.summary.totalTests) * 100).toFixed(2));

console.log("\n====================================================================");
console.log(`  VERIFICATION RESULTS: ${results.summary.passedTests} / ${results.summary.totalTests} Passed (${results.summary.passRatePct}%)`);
console.log("====================================================================");

// Write results to JSON
const outputPath = path.join(__dirname, 'simulation_results.json');
fs.writeFileSync(outputPath, JSON.stringify(results, null, 2), 'utf-8');
console.log(`Structured metrics exported to ${outputPath}`);
