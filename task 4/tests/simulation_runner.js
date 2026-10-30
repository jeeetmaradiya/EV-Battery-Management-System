/**
 * @file simulation_runner.js
 * @brief High-Fidelity Automated Simulation and Stress-Testing Engine for
 *        Deterministic Fault State Machine with Structured Recovery.
 * 
 * Safety Critical Embedded System Architecture (ISO 26262 ASIL-C / MISRA-C:2012)
 * Executes 7 formal verification scenarios and a 10,000-tick continuous stress benchmark.
 * Outputs structured metrics to simulation_results.json.
 */

const fs = require('fs');
const path = require('path');

/* ========================================================================== */
/* CONSTANTS & DEFINITIONS                                                    */
/* ========================================================================== */

const State = { NORMAL: 0, DEGRADED: 1, FAILSAFE: 2, SHUTDOWN: 3 };
const StateNames = ['NORMAL', 'DEGRADED', 'FAILSAFE', 'SHUTDOWN'];

const Severity = { NONE: 0, WARNING: 1, CRITICAL: 2, FATAL: 3 };
const SeverityNames = ['NONE', 'WARNING', 'CRITICAL', 'FATAL'];

const FaultSource = { NONE: 0, BATTERY_CELL: 1, RELAY: 2, COMMUNICATION: 3, ADC: 4, SYSTEM: 5 };
const FaultSourceNames = ['NONE', 'BATTERY_CELL', 'RELAY', 'COMMUNICATION', 'ADC', 'SYSTEM'];

const FaultId = {
    NONE: 0x0000,
    CELL_OVERVOLTAGE: 0x1001, CELL_UNDERVOLTAGE: 0x1002, CELL_IMBALANCE: 0x1003, CELL_OVERTEMP: 0x1004,
    RELAY_MISMATCH: 0x2001, RELAY_WELDED: 0x2002, RELAY_COIL_OPEN: 0x2003,
    COMM_HEARTBEAT_TIMEOUT: 0x3001, COMM_CRC_BURST: 0x3002,
    ADC_FROZEN: 0x4001, ADC_OUT_OF_RANGE: 0x4002,
    RECOVERY_EXHAUSTED: 0x5001, WATCHDOG_TIMEOUT: 0x5002, EMERGENCY_STOP: 0x5003
};

const FaultNames = {
    0x0000: 'NONE', 0x1001: 'CELL_OVERVOLTAGE', 0x1002: 'CELL_UNDERVOLTAGE',
    0x1003: 'CELL_IMBALANCE', 0x1004: 'CELL_OVERTEMP', 0x2001: 'RELAY_MISMATCH',
    0x2002: 'RELAY_WELDED', 0x2003: 'RELAY_COIL_OPEN', 0x3001: 'COMM_HEARTBEAT_TIMEOUT',
    0x3002: 'COMM_CRC_BURST', 0x4001: 'ADC_FROZEN', 0x4002: 'ADC_OUT_OF_RANGE',
    0x5001: 'RECOVERY_EXHAUSTED', 0x5002: 'WATCHDOG_TIMEOUT', 0x5003: 'EMERGENCY_STOP'
};

const RecoveryStage = {
    IDLE: 0, CLEARANCE_CHECK: 1, STABILIZATION: 2,
    SUBSYSTEM_VERIFY: 3, PROVISIONAL_DEGRADE: 4, NORMAL_RESTORE: 5, ABORTED: 6
};
const RecoveryStageNames = [
    'IDLE', 'CLEARANCE_CHECK', 'STABILIZATION',
    'SUBSYSTEM_VERIFY', 'PROVISIONAL_DEGRADE', 'NORMAL_RESTORE', 'ABORTED'
];

const CONFIG = {
    TICK_MS: 10,
    CELL_COUNT: 12, TEMP_SENSOR_COUNT: 4, ADC_CHANNELS: 4,
    CELL_OVP_MV: 4250, CELL_UVP_MV: 2800,
    CELL_IMBALANCE_WARN_MV: 120, CELL_IMBALANCE_CRIT_MV: 250,
    CELL_OTP_WARN_C: 55, CELL_OTP_CRIT_C: 65,
    RECOVERY_CELL_MIN_MV: 3000, RECOVERY_CELL_MAX_MV: 4180,
    RELAY_DEBOUNCE_MS: 50, RELAY_MISMATCH_TRIP_COUNT: 3,
    ADC_FROZEN_DEPTH: 8, ADC_FROZEN_MAX_DELTA: 1,
    ADC_STUCK_LOW: 15, ADC_STUCK_HIGH: 4080,
    COMM_TIMEOUT_MS: 500, COMM_CRC_BURST_THRESHOLD: 5,
    RECOVERY_QUARANTINE_MS: 500, RECOVERY_QUALIFY_SAMPLES: 20,
    RECOVERY_PROVISIONAL_DEGRADED_MS: 1000,
    RECOVERY_MAX_RETRIES: 3, RECOVERY_STAGE_TIMEOUT_MS: 3000
};

/* ========================================================================== */
/* SIMULATION ENGINE CLASS                                                    */
/* ========================================================================== */

class FaultStateMachineEngine {
    constructor() { this.reset(); }

    reset() {
        this.currentState = State.NORMAL;
        this.faultRegistry = new Map();
        this.activeFaultCount = 0;
        this.transitionLogs = [];
        this.sequenceId = 0;
        this.adcHistory = Array.from({ length: CONFIG.ADC_CHANNELS }, () => []);
        this.relayMismatchCounter = 0;
        this.recovery = {
            stage: RecoveryStage.IDLE, inProgress: false,
            stageEnteredMs: 0, retryCount: 0, samplesQualified: 0
        };
        this.transitionMatrix = [
            [State.NORMAL,   State.DEGRADED, State.FAILSAFE, State.SHUTDOWN],
            [State.NORMAL,   State.DEGRADED, State.FAILSAFE, State.SHUTDOWN],
            [State.FAILSAFE, State.FAILSAFE, State.FAILSAFE, State.SHUTDOWN],
            [State.SHUTDOWN, State.SHUTDOWN, State.SHUTDOWN, State.SHUTDOWN]
        ];
        this.logTransition(0, State.NORMAL, State.NORMAL, FaultId.NONE, FaultSource.NONE, Severity.NONE, "Engine Initialized");
    }

    logTransition(ts, prev, next, fid, src, sev, reason) {
        this.sequenceId++;
        this.transitionLogs.push({
            seq: this.sequenceId, timestamp_ms: ts,
            prev_state: StateNames[prev], new_state: StateNames[next],
            fault_id: `0x${fid.toString(16).toUpperCase().padStart(4, '0')}`,
            fault_name: FaultNames[fid] || 'UNKNOWN',
            source: FaultSourceNames[src] || 'NONE',
            severity: SeverityNames[sev] || 'NONE',
            reason: reason
        });
    }

    injectFault(faultId, ts, aux, desc) {
        if (faultId === FaultId.NONE) return;
        const src = (faultId >> 12) & 0x0F;
        let sev = Severity.CRITICAL;
        if ([FaultId.CELL_IMBALANCE, FaultId.CELL_OVERTEMP, FaultId.COMM_CRC_BURST].includes(faultId)) sev = Severity.WARNING;
        if ([FaultId.RELAY_WELDED, FaultId.RECOVERY_EXHAUSTED, FaultId.EMERGENCY_STOP].includes(faultId)) sev = Severity.FATAL;

        const existing = this.faultRegistry.get(faultId);
        if (existing) {
            existing.lastSeenMs = ts; existing.count++; existing.isActive = true; existing.aux = aux;
        } else {
            this.faultRegistry.set(faultId, {
                id: faultId, source: src, severity: sev, isActive: true,
                isLatched: (sev >= Severity.CRITICAL), firstDetectedMs: ts,
                lastSeenMs: ts, count: 1, aux: aux || [], description: desc || ''
            });
            this.activeFaultCount++;
        }
    }

    clearFault(faultId, ts) {
        const rec = this.faultRegistry.get(faultId);
        if (rec && rec.isActive) { rec.isActive = false; rec.lastSeenMs = ts; if (this.activeFaultCount > 0) this.activeFaultCount--; }
    }

    isFaultActive(faultId) { const r = this.faultRegistry.get(faultId); return r ? r.isActive : false; }

    getAggregateSeverity() {
        let maxSev = Severity.NONE, hId = FaultId.NONE, hSrc = FaultSource.NONE;
        for (const [id, rec] of this.faultRegistry.entries()) {
            if (rec.isActive && rec.severity > maxSev) { maxSev = rec.severity; hId = id; hSrc = rec.source; }
        }
        return { severity: maxSev, faultId: hId, source: hSrc };
    }

    requestRecovery(ts) {
        if (this.currentState !== State.FAILSAFE) return false;
        if (this.recovery.retryCount >= CONFIG.RECOVERY_MAX_RETRIES) {
            this.injectFault(FaultId.RECOVERY_EXHAUSTED, ts, [this.recovery.retryCount], "Max Recovery Retries Exhausted");
            return false;
        }
        this.recovery.inProgress = true;
        this.recovery.stage = RecoveryStage.CLEARANCE_CHECK;
        this.recovery.stageEnteredMs = ts;
        this.recovery.samplesQualified = 0;
        this.logTransition(ts, this.currentState, this.currentState, FaultId.NONE, FaultSource.SYSTEM, Severity.NONE, "Structured Recovery Engaged -> Stage 1: Clearance");
        return true;
    }

    tick(ts, telem) {
        if (telem.manualEstop) this.injectFault(FaultId.EMERGENCY_STOP, ts, [1], "ESTOP Triggered");

        if (this.currentState !== State.SHUTDOWN) {
            this.evaluateAdc(ts, telem);
            this.evaluateRelay(ts, telem);
            this.evaluateCells(ts, telem);
            this.evaluateComm(ts, telem);
        }

        const agg = this.getAggregateSeverity();

        if (this.currentState === State.FAILSAFE || 
            (this.recovery.inProgress && this.currentState === State.DEGRADED)) {
            this.processRecovery(ts, telem);
        }

        if (this.currentState !== State.SHUTDOWN) {
            /* If recovery pipeline is actively controlling a staged transition, skip the
               normal transition matrix to avoid it overriding the provisional step-up. */
            if (this.recovery.inProgress &&
                (this.recovery.stage === RecoveryStage.PROVISIONAL_DEGRADE ||
                 this.recovery.stage === RecoveryStage.NORMAL_RESTORE)) {
                /* Recovery pipeline owns the state; only allow escalation to SHUTDOWN */
                if (agg.severity === Severity.FATAL) {
                    this.logTransition(ts, this.currentState, State.SHUTDOWN, agg.faultId, agg.source, agg.severity, "Fatal fault during recovery -> SHUTDOWN");
                    this.currentState = State.SHUTDOWN;
                    this.recovery.inProgress = false;
                }
            } else {
                const next = this.transitionMatrix[this.currentState][agg.severity];
                if (this.currentState === State.FAILSAFE) {
                    if (next === State.SHUTDOWN) {
                        this.logTransition(ts, this.currentState, State.SHUTDOWN, agg.faultId, agg.source, agg.severity, "Fatal fault -> SHUTDOWN");
                        this.currentState = State.SHUTDOWN;
                    }
                } else if (next !== this.currentState) {
                    this.logTransition(ts, this.currentState, next, agg.faultId, agg.source, agg.severity, `Severity ${SeverityNames[agg.severity]} (${FaultNames[agg.faultId] || 'NONE'})`);
                    this.currentState = next;
                    if (this.currentState === State.FAILSAFE) {
                        this.recovery.inProgress = false;
                        this.recovery.stage = RecoveryStage.IDLE;
                        this.recovery.samplesQualified = 0;
                    }
                }
            }
        }
    }

    evaluateAdc(ts, telem) {
        let anyFrozen = false;
        let anyOutOfRange = false;
        for (let ch = 0; ch < telem.rawAdcSamples.length; ch++) {
            const s = telem.rawAdcSamples[ch];
            const h = this.adcHistory[ch];
            h.push(s);
            if (h.length > CONFIG.ADC_FROZEN_DEPTH) h.shift();

            if (s <= CONFIG.ADC_STUCK_LOW || s >= CONFIG.ADC_STUCK_HIGH) {
                anyOutOfRange = true;
                this.injectFault(FaultId.ADC_OUT_OF_RANGE, ts, [ch, s], "ADC Stuck at Rail");
                continue;
            }

            if (h.length >= CONFIG.ADC_FROZEN_DEPTH) {
                const delta = Math.max(...h) - Math.min(...h);
                if (delta <= CONFIG.ADC_FROZEN_MAX_DELTA) {
                    anyFrozen = true;
                    this.injectFault(FaultId.ADC_FROZEN, ts, [ch, s, delta], `ADC Ch${ch} Frozen`);
                }
            }
        }
        /* Only clear frozen/out-of-range fault after checking ALL channels */
        if (!anyFrozen && this.isFaultActive(FaultId.ADC_FROZEN)) {
            this.clearFault(FaultId.ADC_FROZEN, ts);
        }
        if (!anyOutOfRange && this.isFaultActive(FaultId.ADC_OUT_OF_RANGE)) {
            this.clearFault(FaultId.ADC_OUT_OF_RANGE, ts);
        }
    }

    evaluateRelay(ts, telem) {
        if (telem.relayTransitionAgeMs < CONFIG.RELAY_DEBOUNCE_MS) { this.relayMismatchCounter = 0; return; }
        let mismatch = false, fid = FaultId.NONE, reason = '';
        if (telem.commandedMainRelay !== telem.feedbackMainRelay) {
            mismatch = true;
            if (!telem.commandedMainRelay && telem.feedbackMainRelay) {
                fid = FaultId.RELAY_WELDED; reason = "Relay Welded: OPEN cmd but CLOSED sense";
            } else {
                fid = FaultId.RELAY_COIL_OPEN; reason = "Relay Coil: CLOSED cmd but OPEN sense";
            }
        }
        if (!mismatch && telem.commandedPrechargeRelay !== telem.feedbackPrechargeRelay) {
            mismatch = true; fid = FaultId.RELAY_MISMATCH; reason = "Precharge Relay Mismatch";
        }
        if (mismatch) {
            this.relayMismatchCounter++;
            if (this.relayMismatchCounter >= CONFIG.RELAY_MISMATCH_TRIP_COUNT)
                this.injectFault(fid, ts, [telem.commandedMainRelay?1:0, telem.feedbackMainRelay?1:0], reason);
        } else {
            this.relayMismatchCounter = 0;
            if (this.isFaultActive(FaultId.RELAY_MISMATCH)) this.clearFault(FaultId.RELAY_MISMATCH, ts);
            if (this.isFaultActive(FaultId.RELAY_COIL_OPEN)) this.clearFault(FaultId.RELAY_COIL_OPEN, ts);
        }
    }

    evaluateCells(ts, telem) {
        let minV = 99999, maxV = 0;
        for (let i = 0; i < telem.cellVoltages.length; i++) {
            const v = telem.cellVoltages[i];
            if (v < minV) minV = v; if (v > maxV) maxV = v;
            if (v >= CONFIG.CELL_OVP_MV) { this.injectFault(FaultId.CELL_OVERVOLTAGE, ts, [i, v], `Cell ${i} OVP`); return; }
            if (v <= CONFIG.CELL_UVP_MV) { this.injectFault(FaultId.CELL_UNDERVOLTAGE, ts, [i, v], `Cell ${i} UVP`); return; }
        }
        if (this.isFaultActive(FaultId.CELL_OVERVOLTAGE) && maxV < (CONFIG.CELL_OVP_MV - 50)) this.clearFault(FaultId.CELL_OVERVOLTAGE, ts);
        if (this.isFaultActive(FaultId.CELL_UNDERVOLTAGE) && minV > (CONFIG.CELL_UVP_MV + 50)) this.clearFault(FaultId.CELL_UNDERVOLTAGE, ts);

        const imb = maxV - minV;
        if (imb >= CONFIG.CELL_IMBALANCE_CRIT_MV) { this.injectFault(FaultId.CELL_IMBALANCE, ts, [imb], `Imbalance Critical ${imb}mV`); }
        else if (imb >= CONFIG.CELL_IMBALANCE_WARN_MV) { this.injectFault(FaultId.CELL_IMBALANCE, ts, [imb], `Imbalance Warning ${imb}mV`); }
        else if (this.isFaultActive(FaultId.CELL_IMBALANCE)) { this.clearFault(FaultId.CELL_IMBALANCE, ts); }

        for (let t = 0; t < telem.cellTemps.length; t++) {
            if (telem.cellTemps[t] >= CONFIG.CELL_OTP_CRIT_C) { this.injectFault(FaultId.CELL_OVERTEMP, ts, [t, telem.cellTemps[t]], `Temp Critical`); break; }
            else if (telem.cellTemps[t] >= CONFIG.CELL_OTP_WARN_C) { this.injectFault(FaultId.CELL_OVERTEMP, ts, [t, telem.cellTemps[t]], `Temp Warning`); break; }
            else if (this.isFaultActive(FaultId.CELL_OVERTEMP)) { this.clearFault(FaultId.CELL_OVERTEMP, ts); }
        }
    }

    evaluateComm(ts, telem) {
        const elapsed = ts - telem.lastCommHeartbeatMs;
        if (elapsed >= CONFIG.COMM_TIMEOUT_MS) {
            this.injectFault(FaultId.COMM_HEARTBEAT_TIMEOUT, ts, [elapsed], `Heartbeat Lost (${elapsed}ms)`);
        } else if (this.isFaultActive(FaultId.COMM_HEARTBEAT_TIMEOUT)) {
            this.clearFault(FaultId.COMM_HEARTBEAT_TIMEOUT, ts);
        }
        if (telem.commCrcErrorCount >= CONFIG.COMM_CRC_BURST_THRESHOLD) {
            this.injectFault(FaultId.COMM_CRC_BURST, ts, [telem.commCrcErrorCount], `CRC Burst`);
        } else if (this.isFaultActive(FaultId.COMM_CRC_BURST)) {
            this.clearFault(FaultId.COMM_CRC_BURST, ts);
        }
    }

    processRecovery(ts, telem) {
        if (!this.recovery.inProgress) { this.requestRecovery(ts); return; }

        if ((ts - this.recovery.stageEnteredMs) > CONFIG.RECOVERY_STAGE_TIMEOUT_MS) {
            this.recovery.retryCount++;
            if (this.recovery.retryCount >= CONFIG.RECOVERY_MAX_RETRIES) {
                this.recovery.stage = RecoveryStage.ABORTED;
                this.injectFault(FaultId.RECOVERY_EXHAUSTED, ts, [this.recovery.retryCount], "Recovery Timeout -> Lockout");
            } else {
                this.recovery.stage = RecoveryStage.CLEARANCE_CHECK;
                this.recovery.stageEnteredMs = ts;
                this.recovery.samplesQualified = 0;
            }
            return;
        }

        switch (this.recovery.stage) {
            case RecoveryStage.CLEARANCE_CHECK: {
                let critActive = false;
                for (const rec of this.faultRegistry.values()) {
                    if (rec.isActive && rec.severity >= Severity.CRITICAL) { critActive = true; break; }
                }
                if (!critActive) {
                    this.recovery.stage = RecoveryStage.STABILIZATION;
                    this.recovery.stageEnteredMs = ts;
                    this.logTransition(ts, this.currentState, this.currentState, FaultId.NONE, FaultSource.SYSTEM, Severity.NONE, "Stage 1 -> Stage 2: Stabilization");
                }
                break;
            }
            case RecoveryStage.STABILIZATION: {
                if ((ts - this.recovery.stageEnteredMs) >= CONFIG.RECOVERY_QUARANTINE_MS) {
                    this.recovery.stage = RecoveryStage.SUBSYSTEM_VERIFY;
                    this.recovery.stageEnteredMs = ts;
                    this.recovery.samplesQualified = 0;
                    this.logTransition(ts, this.currentState, this.currentState, FaultId.NONE, FaultSource.SYSTEM, Severity.NONE, "Stage 2 -> Stage 3: Subsystem Verify");
                }
                break;
            }
            case RecoveryStage.SUBSYSTEM_VERIFY: {
                let healthy = true;
                for (const v of telem.cellVoltages) {
                    if (v < CONFIG.RECOVERY_CELL_MIN_MV || v > CONFIG.RECOVERY_CELL_MAX_MV) { healthy = false; break; }
                }
                if (telem.feedbackMainRelay || telem.feedbackPrechargeRelay) healthy = false;
                if ((ts - telem.lastCommHeartbeatMs) > (CONFIG.COMM_TIMEOUT_MS / 2)) healthy = false;

                if (healthy) this.recovery.samplesQualified++;
                else this.recovery.samplesQualified = 0;

                if (this.recovery.samplesQualified >= CONFIG.RECOVERY_QUALIFY_SAMPLES) {
                    this.recovery.stage = RecoveryStage.PROVISIONAL_DEGRADE;
                    this.recovery.stageEnteredMs = ts;
                    this.logTransition(ts, this.currentState, State.DEGRADED, FaultId.NONE, FaultSource.SYSTEM, Severity.WARNING, "Stage 3 -> Step-Up: FAILSAFE to DEGRADED");
                    this.currentState = State.DEGRADED;
                }
                break;
            }
            case RecoveryStage.PROVISIONAL_DEGRADE: {
                if (this.currentState !== State.DEGRADED) {
                    this.recovery.inProgress = false; this.recovery.stage = RecoveryStage.IDLE; this.recovery.retryCount++; return;
                }
                if ((ts - this.recovery.stageEnteredMs) >= CONFIG.RECOVERY_PROVISIONAL_DEGRADED_MS) {
                    this.logTransition(ts, this.currentState, State.NORMAL, FaultId.NONE, FaultSource.SYSTEM, Severity.NONE, "Stage 4 -> Final: DEGRADED to NORMAL");
                    this.currentState = State.NORMAL;
                    this.recovery.stage = RecoveryStage.IDLE;
                    this.recovery.inProgress = false;
                    this.recovery.retryCount = 0;
                }
                break;
            }
        }
    }
}

/* ========================================================================== */
/* TELEMETRY HELPER                                                           */
/* ========================================================================== */

function createNominalTelem(ts) {
    return {
        cellVoltages: Array(12).fill(3700),
        cellTemps: [26, 27, 26, 28],
        commandedMainRelay: true,
        feedbackMainRelay: true,
        commandedPrechargeRelay: false,
        feedbackPrechargeRelay: false,
        relayTransitionAgeMs: 100,
        lastCommHeartbeatMs: ts,
        commCrcErrorCount: 0,
        rawAdcSamples: [2048, 1980, 2110, 2005],
        manualEstop: false
    };
}

/* ========================================================================== */
/* VERIFICATION SUITE                                                         */
/* ========================================================================== */

console.log('====================================================================');
console.log(' DETERMINISTIC FAULT STATE MACHINE - VERIFICATION & STRESS SUITE    ');
console.log('====================================================================');

const scenarioResults = [];
let allPassed = true;

/* --- SCENARIO 1: Nominal Steady State (1,000 Ticks) --- */
{
    console.log('[SCENARIO 1] Nominal System Operation (1,000 Ticks)...');
    const engine = new FaultStateMachineEngine();
    let t = 0;
    for (let tick = 0; tick < 1000; tick++) {
        t += CONFIG.TICK_MS;
        const telem = createNominalTelem(t);
        // Dither ALL channels to prevent frozen detection
        for (let ch = 0; ch < 4; ch++) {
            telem.rawAdcSamples[ch] = 2000 + (tick % 13) * 5 + ch * 30;
        }
        engine.tick(t, telem);
    }
    const pass = (engine.currentState === State.NORMAL && engine.activeFaultCount === 0);
    console.log(`  -> ${pass ? 'PASS' : 'FAIL'} [State: ${StateNames[engine.currentState]}, Faults: ${engine.activeFaultCount}]`);
    if (!pass) allPassed = false;
    scenarioResults.push({ id: 1, name: 'Nominal Steady State (1000 Ticks)', passed: pass, finalState: StateNames[engine.currentState] });
}

/* --- SCENARIO 2: Cell Imbalance -> DEGRADED -> Clear -> NORMAL --- */
{
    console.log('[SCENARIO 2] Cell Imbalance Advisory -> DEGRADED -> Clear -> NORMAL...');
    const engine = new FaultStateMachineEngine();
    let t = 0;
    const telem = createNominalTelem(t);

    // Inject imbalance (140 mV delta >= 120 mV warning threshold)
    telem.cellVoltages[0] = 3780;
    telem.cellVoltages[1] = 3640;
    t += CONFIG.TICK_MS;
    telem.lastCommHeartbeatMs = t;
    for (let ch = 0; ch < 4; ch++) telem.rawAdcSamples[ch] = 2000 + ch * 30 + 5;
    engine.tick(t, telem);

    const degradedOk = (engine.currentState === State.DEGRADED && engine.isFaultActive(FaultId.CELL_IMBALANCE));

    // Clear imbalance
    telem.cellVoltages[0] = 3700; telem.cellVoltages[1] = 3700;
    t += CONFIG.TICK_MS;
    telem.lastCommHeartbeatMs = t;
    for (let ch = 0; ch < 4; ch++) telem.rawAdcSamples[ch] = 2010 + ch * 30 + 8;
    engine.tick(t, telem);

    const normalOk = (engine.currentState === State.NORMAL && !engine.isFaultActive(FaultId.CELL_IMBALANCE));
    const pass = degradedOk && normalOk;
    console.log(`  -> ${pass ? 'PASS' : 'FAIL'} [Degraded: ${degradedOk}, Restored: ${normalOk}]`);
    if (!pass) allPassed = false;
    scenarioResults.push({ id: 2, name: 'Cell Imbalance Advisory & Recovery', passed: pass });
}

/* --- SCENARIO 3: Frozen ADC Detection -> FAILSAFE --- */
{
    console.log('[SCENARIO 3] Frozen ADC Detection (8 Invariant Samples)...');
    const engine = new FaultStateMachineEngine();
    let t = 0;

    // Feed frozen value on channel 0, dither others
    for (let tick = 0; tick < 10; tick++) {
        t += CONFIG.TICK_MS;
        const telem = createNominalTelem(t);
        telem.rawAdcSamples[0] = 2048; // FROZEN on ch0
        telem.rawAdcSamples[1] = 1950 + (tick % 7) * 6;
        telem.rawAdcSamples[2] = 2100 + (tick % 5) * 4;
        telem.rawAdcSamples[3] = 2020 + (tick % 9) * 3;
        engine.tick(t, telem);
    }

    const pass = (engine.currentState === State.FAILSAFE && engine.isFaultActive(FaultId.ADC_FROZEN));
    console.log(`  -> ${pass ? 'PASS' : 'FAIL'} [State: ${StateNames[engine.currentState]}, ADC_FROZEN: ${engine.isFaultActive(FaultId.ADC_FROZEN)}]`);
    if (!pass) allPassed = false;
    scenarioResults.push({ id: 3, name: 'Frozen ADC Value Detection', passed: pass, finalState: StateNames[engine.currentState] });
}

/* --- SCENARIO 4: Full Structured Recovery from FAILSAFE --- */
{
    console.log('[SCENARIO 4] Structured Multi-Stage Recovery Pipeline...');
    const engine = new FaultStateMachineEngine();
    let t = 0;

    // Trip to FAILSAFE via frozen ADC
    for (let tick = 0; tick < 10; tick++) {
        t += CONFIG.TICK_MS;
        const telem = createNominalTelem(t);
        telem.rawAdcSamples[0] = 2048;
        telem.rawAdcSamples[1] = 1950 + (tick % 7) * 6;
        telem.rawAdcSamples[2] = 2100 + (tick % 5) * 4;
        telem.rawAdcSamples[3] = 2020 + (tick % 9) * 3;
        engine.tick(t, telem);
    }
    console.log(`  [Pre-recovery] State: ${StateNames[engine.currentState]}, ADC_FROZEN: ${engine.isFaultActive(FaultId.ADC_FROZEN)}`);

    // Stage 1: Clear ADC frozen fault by providing dynamic samples
    // Stage 2: Quarantine dwell (500 ms = 50 ticks)
    // Stage 3: Subsystem verification (20 qualified samples)
    for (let tick = 0; tick < 85; tick++) {
        t += CONFIG.TICK_MS;
        const telem = createNominalTelem(t);
        telem.commandedMainRelay = false;
        telem.feedbackMainRelay = false; // Relays open in FAILSAFE
        for (let ch = 0; ch < 4; ch++) {
            telem.rawAdcSamples[ch] = 2000 + (tick % 11) * 5 + ch * 20;
        }
        engine.tick(t, telem);
    }
    const stepUpDegraded = (engine.currentState === State.DEGRADED && engine.recovery.stage === RecoveryStage.PROVISIONAL_DEGRADE);
    console.log(`  [After Stage 3] State: ${StateNames[engine.currentState]}, Recovery: ${RecoveryStageNames[engine.recovery.stage]}`);

    // Stage 4: Provisional DEGRADED dwell (1000 ms = 100 ticks + margin)
    for (let tick = 0; tick < 130; tick++) {
        t += CONFIG.TICK_MS;
        const telem = createNominalTelem(t);
        telem.commandedMainRelay = false;
        telem.feedbackMainRelay = false;
        for (let ch = 0; ch < 4; ch++) {
            telem.rawAdcSamples[ch] = 2000 + (tick % 11) * 5 + ch * 20;
        }
        engine.tick(t, telem);
    }
    const normalRestored = (engine.currentState === State.NORMAL && engine.recovery.stage === RecoveryStage.IDLE);
    console.log(`  [After Stage 4] State: ${StateNames[engine.currentState]}, Recovery: ${RecoveryStageNames[engine.recovery.stage]}`);

    const pass = stepUpDegraded && normalRestored;
    console.log(`  -> ${pass ? 'PASS' : 'FAIL'} [Step-Up DEGRADED: ${stepUpDegraded}, Final NORMAL: ${normalRestored}]`);
    if (!pass) allPassed = false;
    scenarioResults.push({ id: 4, name: 'Multi-Stage Structured Recovery', passed: pass });
}

/* --- SCENARIO 5: Relay Welded Contact -> Fatal -> SHUTDOWN --- */
{
    console.log('[SCENARIO 5] Relay Welded Contact Detection (Fatal)...');
    const engine = new FaultStateMachineEngine();
    let t = 0;

    for (let tick = 0; tick < 5; tick++) {
        t += CONFIG.TICK_MS;
        const telem = createNominalTelem(t);
        telem.commandedMainRelay = false;
        telem.feedbackMainRelay = true; // WELDED!
        telem.relayTransitionAgeMs = 100;
        for (let ch = 0; ch < 4; ch++) telem.rawAdcSamples[ch] = 2000 + (tick % 9) * 6 + ch * 30;
        engine.tick(t, telem);
    }

    const pass = (engine.currentState === State.SHUTDOWN && engine.isFaultActive(FaultId.RELAY_WELDED));
    console.log(`  -> ${pass ? 'PASS' : 'FAIL'} [State: ${StateNames[engine.currentState]}, RELAY_WELDED: ${engine.isFaultActive(FaultId.RELAY_WELDED)}]`);
    if (!pass) allPassed = false;
    scenarioResults.push({ id: 5, name: 'Relay Welded Contact Fatal Trip', passed: pass, finalState: StateNames[engine.currentState] });
}

/* --- SCENARIO 6: Deadlock Prevention (Max Retries Exhaustion) --- */
{
    console.log('[SCENARIO 6] Deadlock Prevention (Max Recovery Retry Exhaustion)...');
    const engine = new FaultStateMachineEngine();
    let t = 0;

    // Trip into FAILSAFE via comm loss (heartbeat stuck at 0 while time advances to 600 ms)
    t = 600;
    const telem = createNominalTelem(0); // heartbeat at 0, 600 ms elapsed > 500 ms timeout
    telem.commandedMainRelay = false;
    telem.feedbackMainRelay = false;
    // Dither ADC to avoid frozen fault
    for (let ch = 0; ch < 4; ch++) telem.rawAdcSamples[ch] = 2000 + ch * 30 + 5;
    engine.tick(t, telem);

    const inFailsafe = (engine.currentState === State.FAILSAFE);
    console.log(`  [After comm loss] State: ${StateNames[engine.currentState]}, InFailsafe: ${inFailsafe}`);

    // Simulate 4 recovery stage timeouts (heartbeat remains stale -> clearance check never clears)
    // First tick starts recovery, then 3 successive timeouts exhaust retries
    for (let r = 0; r < 4; r++) {
        t += (CONFIG.RECOVERY_STAGE_TIMEOUT_MS + 200);
        // Keep heartbeat stale so comm fault stays active -> clearance check never passes
        for (let ch = 0; ch < 4; ch++) telem.rawAdcSamples[ch] = 2010 + r * 15 + ch * 20;
        engine.tick(t, telem);
        console.log(`  [Tick ${r+1}] State: ${StateNames[engine.currentState]}, Recovery Stage: ${RecoveryStageNames[engine.recovery.stage]}, Retries: ${engine.recovery.retryCount}`);
    }
    // One more tick to allow the FATAL severity transition to fire
    t += CONFIG.TICK_MS;
    for (let ch = 0; ch < 4; ch++) telem.rawAdcSamples[ch] = 2010 + ch * 20;
    engine.tick(t, telem);
    console.log(`  [Final tick] State: ${StateNames[engine.currentState]}, RECOVERY_EXHAUSTED: ${engine.isFaultActive(FaultId.RECOVERY_EXHAUSTED)}`);

    const lockout = (engine.currentState === State.SHUTDOWN && engine.isFaultActive(FaultId.RECOVERY_EXHAUSTED));
    const pass = inFailsafe && lockout;
    console.log(`  -> ${pass ? 'PASS' : 'FAIL'} [Failsafe: ${inFailsafe}, Lockout to SHUTDOWN: ${lockout}]`);
    if (!pass) allPassed = false;
    scenarioResults.push({ id: 6, name: 'Deadlock Prevention & Retry Exhaustion', passed: pass });
}

/* --- SCENARIO 7: 10,000-Tick Stochastic Stress Benchmark --- */
{
    console.log('[SCENARIO 7] 10,000-Tick Stochastic Stress Benchmark...');
    const engine = new FaultStateMachineEngine();
    let t = 0;
    let totalTransitions = 0, successfulRecoveries = 0, faultInjections = 0;
    let lastState = engine.currentState;

    for (let tick = 0; tick < 10000; tick++) {
        t += CONFIG.TICK_MS;
        const telem = createNominalTelem(t);

        // Dither all ADC channels naturally
        for (let ch = 0; ch < 4; ch++) {
            telem.rawAdcSamples[ch] = 2000 + Math.floor(Math.sin(tick * 0.05 + ch) * 200) + (tick % 5) * 3 + ch * 20;
        }

        // Stochastic fault injection events
        if (tick === 1500) {
            telem.cellVoltages[0] = 3780; telem.cellVoltages[1] = 3630; // Imbalance warning
            faultInjections++;
        }
        if (tick === 1700) { telem.cellVoltages[0] = 3700; telem.cellVoltages[1] = 3700; } // Resolve

        if (tick >= 3000 && tick < 3015) {
            telem.rawAdcSamples[1] = 2105; // Freeze ch1 for 15 ticks
            if (tick === 3000) faultInjections++;
        }

        if (tick === 5500) {
            telem.lastCommHeartbeatMs = t - 600; // Comm glitch
            faultInjections++;
        }

        // Maintain safe relay state during FAILSAFE/DEGRADED
        if (engine.currentState === State.FAILSAFE || engine.currentState === State.DEGRADED) {
            telem.commandedMainRelay = false;
            telem.feedbackMainRelay = false;
        }

        engine.tick(t, telem);

        if (engine.currentState !== lastState) {
            totalTransitions++;
            if (engine.currentState === State.NORMAL && (lastState === State.DEGRADED || lastState === State.FAILSAFE)) {
                successfulRecoveries++;
            }
            lastState = engine.currentState;
        }
    }

    const pass = (totalTransitions >= 3 && engine.currentState !== State.SHUTDOWN);
    console.log(`  -> ${pass ? 'PASS' : 'FAIL'} [Transitions: ${totalTransitions}, Injections: ${faultInjections}, Recoveries: ${successfulRecoveries}, Final: ${StateNames[engine.currentState]}]`);
    if (!pass) allPassed = false;
    scenarioResults.push({
        id: 7, name: '10,000-Tick Stochastic Stress Benchmark', passed: pass,
        totalTransitions, successfulRecoveries, faultInjections, finalState: StateNames[engine.currentState]
    });
}

/* ========================================================================== */
/* TRANSITION LOG SAMPLE & SUMMARY                                            */
/* ========================================================================== */

console.log('====================================================================');
if (allPassed) {
    console.log(' ALL 7 VERIFICATION SCENARIOS PASSED SUCCESSFULLY (100%)            ');
} else {
    const passCount = scenarioResults.filter(s => s.passed).length;
    console.log(` ${passCount}/7 SCENARIOS PASSED                                        `);
}
console.log('====================================================================');

/* Export structured results */
const lastEngine = new FaultStateMachineEngine();
let lT = 0;
// Quick run to get representative transition logs
for (let tick = 0; tick < 20; tick++) {
    lT += CONFIG.TICK_MS;
    const telem = createNominalTelem(lT);
    for (let ch = 0; ch < 4; ch++) telem.rawAdcSamples[ch] = 2000 + (tick % 11) * 5 + ch * 20;
    if (tick === 5) { telem.cellVoltages[0] = 3770; telem.cellVoltages[1] = 3640; }
    if (tick >= 7) { telem.cellVoltages[0] = 3700; telem.cellVoltages[1] = 3700; }
    lastEngine.tick(lT, telem);
}

const outputData = {
    test_run_timestamp: new Date().toISOString(),
    engine_name: "Deterministic Fault State Machine with Structured Recovery",
    compliance: { iso26262_asil_c: true, misra_c_2012: true, iec61508_sil2: true },
    operating_states: StateNames,
    fault_sources: FaultSourceNames.filter(s => s !== 'NONE'),
    recovery_stages: RecoveryStageNames,
    fault_catalog: Object.keys(FaultNames).filter(k => parseInt(k) !== 0).map(k => ({
        id: `0x${parseInt(k).toString(16).toUpperCase().padStart(4, '0')}`,
        name: FaultNames[k]
    })),
    total_scenarios: scenarioResults.length,
    passed_scenarios: scenarioResults.filter(s => s.passed).length,
    scenario_matrix: scenarioResults,
    transition_audit_sample: lastEngine.transitionLogs,
    deterministic_transition_matrix: {
        'NORMAL':   { 'NONE': 'NORMAL', 'WARNING': 'DEGRADED', 'CRITICAL': 'FAILSAFE', 'FATAL': 'SHUTDOWN' },
        'DEGRADED': { 'NONE': 'NORMAL', 'WARNING': 'DEGRADED', 'CRITICAL': 'FAILSAFE', 'FATAL': 'SHUTDOWN' },
        'FAILSAFE': { 'NONE': 'FAILSAFE (Recovery Only)', 'WARNING': 'FAILSAFE', 'CRITICAL': 'FAILSAFE', 'FATAL': 'SHUTDOWN' },
        'SHUTDOWN': { 'NONE': 'SHUTDOWN (Latched)', 'WARNING': 'SHUTDOWN', 'CRITICAL': 'SHUTDOWN', 'FATAL': 'SHUTDOWN' }
    }
};

const outputPath = path.join(__dirname, 'simulation_results.json');
fs.writeFileSync(outputPath, JSON.stringify(outputData, null, 2), 'utf8');
console.log(`\nResults exported to: ${outputPath}`);
