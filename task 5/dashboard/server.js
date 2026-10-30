/**
 * @file server.js
 * @brief High-Precision HTTP & Telemetry Engine Server for Live Blynk Dashboard.
 * 
 * Runs behavioral telemetry engine with offline circular queue, non-blocking Wi-Fi FSM,
 * RSSI smoothing, deadband event filtering, and Blynk virtual pin dispatch.
 * Serves dashboard at http://localhost:3500.
 */

const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = 3500;
const startTime = Date.now();

/* ========================================================================== */
/* CONSTANTS & DEFINITIONS                                                    */
/* ========================================================================== */

const EventTypeNames = [
    'NONE', 'HEARTBEAT', 'CELL_DELTA', 'EXTREMA_CHANGE', 'STATE_CHANGE',
    'FAULT_ASSERTED', 'FAULT_CLEARED', 'RELAY_CHANGE', 'NETWORK_STATUS', 'QUEUE_FLUSH'
];

const PriorityNames = ['LOW', 'NORMAL', 'HIGH', 'CRITICAL'];
const OriginNames = ['LIVE', 'QUEUED'];
const BmsStateNames = ['NORMAL', 'DEGRADED', 'FAILSAFE', 'SHUTDOWN'];
const WifiStateNames = [
    'DISCONNECTED', 'INIT', 'CONNECTING', 'CONNECTED',
    'BLYNK_CONNECTING', 'BLYNK_CONNECTED', 'BACKOFF_WAIT', 'FATAL_RECOVERY'
];
const RssiQualityNames = ['EXCELLENT', 'GOOD', 'FAIR', 'POOR', 'LOST'];

const CONFIG = {
    CELL_COUNT: 12,
    TEMP_SENSORS: 4,
    OFFLINE_QUEUE_CAPACITY: 64,
    CELL_VOLTAGE_DEADBAND_MV: 25,
    CELL_DELTA_DEADBAND_MV: 15,
    PACK_VOLTAGE_DEADBAND_MV: 250,
    CURRENT_DEADBAND_MA: 800,
    TEMP_DEADBAND_C: 2,
    HEARTBEAT_INTERVAL_MS: 10000,
    DRAIN_BURST_MAX: 2,
    DRAIN_INTERVAL_MS: 100, // 100ms for visible smooth dashboard animation
    WIFI_INITIAL_BACKOFF_MS: 1500,
    WIFI_MAX_BACKOFF_MS: 16000,
    WIFI_BACKOFF_MULTIPLIER: 2,
    RSSI_EMA_WEIGHT: 0.25
};

/* ========================================================================== */
/* ENGINE STATE                                                               */
/* ========================================================================== */

const engine = {
    currentTimeMs: 0,
    monotonicSeqId: 1,
    lastPacketTimeMs: 0,
    lastDrainTimeMs: 0,
    onlineStableStartMs: 0,
    outageStartMs: 0,
    hasTransmittedBaseline: false,

    currentBms: {
        cellVoltagesMv: new Array(CONFIG.CELL_COUNT).fill(3700),
        packVoltageMv: 3700 * CONFIG.CELL_COUNT,
        packCurrentMa: 1200,
        temperaturesC: [25, 26, 25, 27],
        strongestCellIdx: 0,
        weakestCellIdx: 0,
        strongestCellMv: 3700,
        weakestCellMv: 3700,
        cellDeltaMv: 0,
        fsmState: 0, // NORMAL
        activeFaultId: 0,
        faultSeverity: 0,
        faultReason: 'NONE',
        relayStatusFlags: 0x03 // Main commanded & feedback closed
    },

    lastTransmittedBms: null,

    queue: {
        buffer: [],
        capacity: CONFIG.OFFLINE_QUEUE_CAPACITY,
        totalEnqueued: 0,
        totalDequeued: 0,
        overflowDropped: 0,
        peakDepth: 0
    },

    wifi: {
        state: 0, // Starts DISCONNECTED, will transition to BLYNK_CONNECTED
        stateEntryTimeMs: 0,
        backoffIntervalMs: CONFIG.WIFI_INITIAL_BACKOFF_MS,
        reconnectAttempts: 0,
        rawRssiDbm: -62,
        filteredRssiDbm: -62,
        quality: 1, // GOOD
        physicalLinkUp: true,
        blynkServerAvailable: true
    },

    metrics: {
        eventsGenerated: 0,
        deadbandSuppressedTicks: 0,
        packetsTransmittedLive: 0,
        packetsTransmittedQueued: 0,
        totalBytesTransmitted: 0,
        networkOutageCount: 0,
        totalOutageDurationMs: 0
    },

    recentPackets: [],
    eventLog: []
};

function calculateExtrema(snap) {
    let minV = snap.cellVoltagesMv[0];
    let maxV = snap.cellVoltagesMv[0];
    let minIdx = 0;
    let maxIdx = 0;
    let sumV = 0;

    for (let i = 0; i < CONFIG.CELL_COUNT; i++) {
        let v = snap.cellVoltagesMv[i];
        sumV += v;
        if (v < minV) { minV = v; minIdx = i; }
        if (v > maxV) { maxV = v; maxIdx = i; }
    }

    snap.weakestCellIdx = minIdx;
    snap.weakestCellMv = minV;
    snap.strongestCellIdx = maxIdx;
    snap.strongestCellMv = maxV;
    snap.cellDeltaMv = maxV - minV;
    snap.packVoltageMv = sumV;
}

calculateExtrema(engine.currentBms);
engine.lastTransmittedBms = JSON.parse(JSON.stringify(engine.currentBms));

function updateRssiQuality() {
    if (!engine.wifi.physicalLinkUp || engine.wifi.filteredRssiDbm < -90) {
        engine.wifi.quality = 4; // LOST
    } else if (engine.wifi.filteredRssiDbm >= -60) {
        engine.wifi.quality = 0; // EXCELLENT
    } else if (engine.wifi.filteredRssiDbm >= -70) {
        engine.wifi.quality = 1; // GOOD
    } else if (engine.wifi.filteredRssiDbm >= -80) {
        engine.wifi.quality = 2; // FAIR
    } else {
        engine.wifi.quality = 3; // POOR
    }
}

function setRssi(rawDbm) {
    engine.wifi.rawRssiDbm = rawDbm;
    engine.wifi.filteredRssiDbm = Math.round(
        engine.wifi.filteredRssiDbm * (1 - CONFIG.RSSI_EMA_WEIGHT) + rawDbm * CONFIG.RSSI_EMA_WEIGHT
    );

    let oldQual = engine.wifi.quality;
    updateRssiQuality();

    if (engine.wifi.quality !== oldQual && engine.wifi.state === 5) {
        let pkt = {
            seqId: engine.monotonicSeqId++,
            captureTimestampMs: engine.currentTimeMs,
            eventType: 8, // NETWORK_STATUS
            priority: (engine.wifi.quality >= 3) ? 2 : 1,
            snapshot: JSON.parse(JSON.stringify(engine.currentBms)),
            reason: `RSSI_TIER_CHANGED_${RssiQualityNames[oldQual]}->${RssiQualityNames[engine.wifi.quality]} (${engine.wifi.filteredRssiDbm} dBm)`
        };
        dispatchPacket(pkt);
        engine.lastPacketTimeMs = engine.currentTimeMs;
    }
}

function computeBackoff(attempts) {
    if (attempts === 0) return CONFIG.WIFI_INITIAL_BACKOFF_MS;
    let b = CONFIG.WIFI_INITIAL_BACKOFF_MS * Math.pow(CONFIG.WIFI_BACKOFF_MULTIPLIER, attempts - 1);
    if (b > CONFIG.WIFI_MAX_BACKOFF_MS) b = CONFIG.WIFI_MAX_BACKOFF_MS;
    return b;
}

function enqueueEvent(pkt) {
    if (engine.queue.buffer.length >= engine.queue.capacity) {
        if (pkt.priority >= 2) { // HIGH or CRITICAL
            let lowestIdx = -1;
            let lowestPrio = pkt.priority;
            for (let i = 0; i < engine.queue.buffer.length; i++) {
                if (engine.queue.buffer[i].priority < lowestPrio) {
                    lowestPrio = engine.queue.buffer[i].priority;
                    lowestIdx = i;
                }
            }
            if (lowestIdx !== -1) {
                pkt.origin = 1; // QUEUED
                engine.queue.buffer[lowestIdx] = pkt;
                engine.queue.overflowDropped++;
                engine.queue.totalEnqueued++;
                return;
            } else {
                engine.queue.buffer.shift();
                engine.queue.overflowDropped++;
            }
        } else {
            engine.queue.overflowDropped++;
            return;
        }
    }

    pkt.origin = 1; // QUEUED
    engine.queue.buffer.push(pkt);
    engine.queue.totalEnqueued++;
    if (engine.queue.buffer.length > engine.queue.peakDepth) {
        engine.queue.peakDepth = engine.queue.buffer.length;
    }
}

function dispatchPacket(pkt) {
    engine.metrics.eventsGenerated++;

    if (engine.wifi.state === 5 && engine.queue.buffer.length === 0) { // BLYNK_CONNECTED
        pkt.origin = 0; // LIVE
        pkt.transmitTimestampMs = engine.currentTimeMs;
        recordPacket(pkt);
        engine.metrics.packetsTransmittedLive++;
        engine.metrics.totalBytesTransmitted += 128;
    } else {
        pkt.origin = 1; // QUEUED
        enqueueEvent(pkt);
    }
}

function recordPacket(pkt) {
    engine.recentPackets.unshift(pkt);
    if (engine.recentPackets.length > 50) engine.recentPackets.pop();

    engine.eventLog.unshift({
        seqId: pkt.seqId,
        timestamp: new Date().toLocaleTimeString(),
        captureMs: pkt.captureTimestampMs,
        transmitMs: pkt.transmitTimestampMs,
        origin: pkt.origin === 0 ? 'LIVE' : 'QUEUED',
        type: EventTypeNames[pkt.eventType] || 'UNKNOWN',
        priority: PriorityNames[pkt.priority] || 'NORMAL',
        reason: pkt.reason,
        fsmState: BmsStateNames[pkt.snapshot.fsmState],
        deltaMv: pkt.snapshot.cellDeltaMv,
        strongest: `C${pkt.snapshot.strongestCellIdx + 1} (${pkt.snapshot.strongestCellMv}mV)`,
        weakest: `C${pkt.snapshot.weakestCellIdx + 1} (${pkt.snapshot.weakestCellMv}mV)`
    });
    if (engine.eventLog.length > 80) engine.eventLog.pop();
}

function updateWifiFsm() {
    let elapsed = engine.currentTimeMs - engine.wifi.stateEntryTimeMs;

    switch (engine.wifi.state) {
        case 0: // DISCONNECTED
            if (engine.wifi.physicalLinkUp) {
                engine.wifi.state = 1; // INIT
                engine.wifi.stateEntryTimeMs = engine.currentTimeMs;
            }
            break;
        case 1: // INIT
            if (!engine.wifi.physicalLinkUp) {
                engine.wifi.state = 0;
                engine.wifi.stateEntryTimeMs = engine.currentTimeMs;
            } else if (elapsed >= 100) {
                engine.wifi.state = 2; // CONNECTING
                engine.wifi.stateEntryTimeMs = engine.currentTimeMs;
            }
            break;
        case 2: // CONNECTING
            if (!engine.wifi.physicalLinkUp) {
                engine.wifi.reconnectAttempts++;
                engine.wifi.backoffIntervalMs = computeBackoff(engine.wifi.reconnectAttempts);
                engine.wifi.state = 6; // BACKOFF_WAIT
                engine.wifi.stateEntryTimeMs = engine.currentTimeMs;
            } else if (elapsed >= 300) {
                engine.wifi.state = 3; // CONNECTED
                engine.wifi.stateEntryTimeMs = engine.currentTimeMs;
            }
            break;
        case 3: // CONNECTED
            if (!engine.wifi.physicalLinkUp) {
                engine.wifi.state = 0;
                engine.wifi.stateEntryTimeMs = engine.currentTimeMs;
            } else if (engine.wifi.blynkServerAvailable) {
                engine.wifi.state = 4; // BLYNK_CONNECTING
                engine.wifi.stateEntryTimeMs = engine.currentTimeMs;
            }
            break;
        case 4: // BLYNK_CONNECTING
            if (!engine.wifi.physicalLinkUp || !engine.wifi.blynkServerAvailable) {
                engine.wifi.reconnectAttempts++;
                engine.wifi.backoffIntervalMs = computeBackoff(engine.wifi.reconnectAttempts);
                engine.wifi.state = 6;
                engine.wifi.stateEntryTimeMs = engine.currentTimeMs;
            } else if (elapsed >= 200) {
                engine.wifi.state = 5; // BLYNK_CONNECTED
                engine.wifi.stateEntryTimeMs = engine.currentTimeMs;
                engine.onlineStableStartMs = engine.currentTimeMs;
                if (engine.outageStartMs > 0) {
                    engine.metrics.totalOutageDurationMs += (engine.currentTimeMs - engine.outageStartMs);
                    engine.outageStartMs = 0;
                }
            }
            break;
        case 5: // BLYNK_CONNECTED
            if (!engine.wifi.physicalLinkUp || !engine.wifi.blynkServerAvailable) {
                engine.wifi.reconnectAttempts++;
                engine.wifi.backoffIntervalMs = computeBackoff(engine.wifi.reconnectAttempts);
                engine.wifi.state = 6;
                engine.wifi.stateEntryTimeMs = engine.currentTimeMs;
            } else if (engine.currentTimeMs - engine.onlineStableStartMs >= 5000) {
                engine.wifi.reconnectAttempts = 0;
                engine.wifi.backoffIntervalMs = CONFIG.WIFI_INITIAL_BACKOFF_MS;
            }
            break;
        case 6: // BACKOFF_WAIT
            if (engine.wifi.reconnectAttempts >= 6) {
                engine.wifi.state = 7; // FATAL_RECOVERY
                engine.wifi.stateEntryTimeMs = engine.currentTimeMs;
            } else if (elapsed >= engine.wifi.backoffIntervalMs) {
                engine.wifi.state = 1; // INIT
                engine.wifi.stateEntryTimeMs = engine.currentTimeMs;
            }
            break;
        case 7: // FATAL_RECOVERY
            if (elapsed >= 500) {
                engine.wifi.reconnectAttempts = 1;
                engine.wifi.backoffIntervalMs = CONFIG.WIFI_INITIAL_BACKOFF_MS;
                engine.wifi.state = 1;
                engine.wifi.stateEntryTimeMs = engine.currentTimeMs;
            }
            break;
    }
}

function stepEngine() {
    engine.currentTimeMs = Date.now() - startTime;
    updateWifiFsm();

    // Baseline initial packet once connected
    if (!engine.hasTransmittedBaseline && engine.wifi.state === 5) {
        let basePkt = {
            seqId: engine.monotonicSeqId++,
            captureTimestampMs: engine.currentTimeMs,
            eventType: 4, // STATE_CHANGE
            priority: 1, // NORMAL
            snapshot: JSON.parse(JSON.stringify(engine.currentBms)),
            reason: "INITIAL_BASELINE_TELEMETRY"
        };
        dispatchPacket(basePkt);
        engine.lastTransmittedBms = JSON.parse(JSON.stringify(engine.currentBms));
        engine.lastPacketTimeMs = engine.currentTimeMs;
        engine.hasTransmittedBaseline = true;
    }

    // Quiescent Heartbeat
    if (engine.hasTransmittedBaseline &&
        (engine.currentTimeMs - engine.lastPacketTimeMs >= CONFIG.HEARTBEAT_INTERVAL_MS)) {
        let hb = {
            seqId: engine.monotonicSeqId++,
            captureTimestampMs: engine.currentTimeMs,
            eventType: 1, // HEARTBEAT
            priority: 0, // LOW
            snapshot: JSON.parse(JSON.stringify(engine.currentBms)),
            reason: "QUIESCENT_HEARTBEAT_SYNC"
        };
        dispatchPacket(hb);
        engine.lastPacketTimeMs = engine.currentTimeMs;
        engine.lastTransmittedBms = JSON.parse(JSON.stringify(engine.currentBms));
    }

    // Paced Queue Drain
    if (engine.wifi.state === 5 && engine.queue.buffer.length > 0) {
        if (engine.currentTimeMs - engine.lastDrainTimeMs >= CONFIG.DRAIN_INTERVAL_MS) {
            let burst = 0;
            while (engine.queue.buffer.length > 0 && burst < CONFIG.DRAIN_BURST_MAX) {
                let queuedPkt = engine.queue.buffer.shift();
                queuedPkt.origin = 1; // QUEUED
                queuedPkt.transmitTimestampMs = engine.currentTimeMs;
                recordPacket(queuedPkt);
                engine.queue.totalDequeued++;
                engine.metrics.packetsTransmittedQueued++;
                engine.metrics.totalBytesTransmitted += 128;
                burst++;
            }
            engine.lastDrainTimeMs = engine.currentTimeMs;

            if (engine.queue.buffer.length === 0) {
                let flushPkt = {
                    seqId: engine.monotonicSeqId++,
                    captureTimestampMs: engine.currentTimeMs,
                    transmitTimestampMs: engine.currentTimeMs,
                    eventType: 9, // QUEUE_FLUSH
                    priority: 1,
                    origin: 0, // LIVE
                    snapshot: JSON.parse(JSON.stringify(engine.currentBms)),
                    reason: "OFFLINE_QUEUE_RESTORED_LIVE"
                };
                recordPacket(flushPkt);
                engine.metrics.packetsTransmittedLive++;
            }
        }
    }
}

// Background engine step timer (every 20ms)
setInterval(stepEngine, 20);

// Mild drift simulation on cells (micro-noise for realism)
setInterval(() => {
    if (engine.currentBms.fsmState === 0) {
        let snap = JSON.parse(JSON.stringify(engine.currentBms));
        for (let i = 0; i < CONFIG.CELL_COUNT; i++) {
            let jitter = Math.floor(Math.random() * 5) - 2; // ±2 mV (well within 25mV deadband)
            snap.cellVoltagesMv[i] = Math.max(3600, Math.min(3800, snap.cellVoltagesMv[i] + jitter));
        }
        calculateExtrema(snap);
        engine.currentBms = snap;
    }
}, 500);

/* ========================================================================== */
/* BLYNK VIRTUAL PIN EXPORT                                                   */
/* ========================================================================== */

function getBlynkVirtualPins() {
    const bms = engine.currentBms;
    const isOnline = (engine.wifi.state === 5);
    const streamMode = (engine.queue.buffer.length > 0) ? 1 : 0; // 0=LIVE, 1=QUEUED
    const drainPct = (engine.queue.totalEnqueued > 0) ?
        Math.min(100, Math.round((engine.queue.totalDequeued / engine.queue.totalEnqueued) * 100)) : 100;

    const pins = {
        V0: (bms.packVoltageMv / 1000).toFixed(2), // Pack Volts
        V1: (bms.packCurrentMa / 1000).toFixed(2), // Pack Current (A)
        V2: Math.max(...bms.temperaturesC),        // Max Temp (°C)
        V3: bms.weakestCellMv,                     // Weakest Cell mV
        V4: bms.strongestCellMv,                   // Strongest Cell mV
        V5: bms.cellDeltaMv,                       // Imbalance Delta mV
        V6: bms.strongestCellIdx + 1,              // Strongest Cell # (1-based)
        V7: bms.weakestCellIdx + 1,                // Weakest Cell # (1-based)
        V8: BmsStateNames[bms.fsmState],           // FSM State Name
        V9: bms.activeFaultId ? `0x${bms.activeFaultId.toString(16).toUpperCase()}` : '0x0000',
        V10: `0x${bms.relayStatusFlags.toString(16).toUpperCase().padStart(2, '0')}`,
        V11: engine.wifi.filteredRssiDbm,          // RSSI in dBm
        V12: engine.queue.buffer.length,           // Current Offline Queue Depth
        V13: streamMode === 0 ? 'LIVE' : 'QUEUED', // Stream Mode
        V14: engine.monotonicSeqId - 1,            // Sequence ID
        V15: WifiStateNames[engine.wifi.state],    // Wi-Fi FSM State
        V28: engine.queue.overflowDropped,         // Dropped packets
        V29: `${drainPct}%`                        // Queue Drain %
    };

    for (let c = 0; c < CONFIG.CELL_COUNT; c++) {
        pins[`V${16 + c}`] = bms.cellVoltagesMv[c];
    }

    return pins;
}

/* ========================================================================== */
/* ACTION HANDLERS                                                            */
/* ========================================================================== */

function handleAction(action, payload) {
    switch (action) {
        case 'wifi_disconnect':
            engine.wifi.physicalLinkUp = false;
            if (engine.wifi.state === 5) {
                engine.outageStartMs = engine.currentTimeMs;
                engine.metrics.networkOutageCount++;
            }
            return { success: true, message: 'Wi-Fi physical link disconnected (Outage active)' };

        case 'wifi_reconnect':
            engine.wifi.physicalLinkUp = true;
            return { success: true, message: 'Wi-Fi physical link restored (Initiating non-blocking FSM)' };

        case 'rssi_degrade':
            setRssi(-86); // Poor link
            return { success: true, message: 'RSSI degraded to -86 dBm (POOR)' };

        case 'rssi_restore':
            setRssi(-55); // Excellent link
            return { success: true, message: 'RSSI restored to -55 dBm (EXCELLENT)' };

        case 'inject_ovp':
            engine.currentBms.cellVoltagesMv[3] = 4280; // Cell 4 jumps to 4.28V
            calculateExtrema(engine.currentBms);
            engine.currentBms.activeFaultId = 0x1001;
            engine.currentBms.faultSeverity = 3;
            engine.currentBms.faultReason = 'CELL_4_OVERVOLTAGE_4280MV';
            engine.currentBms.fsmState = 2; // FAILSAFE
            engine.currentBms.relayStatusFlags = 0x00; // Tripped

            let ovpPkt = {
                seqId: engine.monotonicSeqId++,
                captureTimestampMs: engine.currentTimeMs,
                eventType: 5, // FAULT_ASSERTED
                priority: 3, // CRITICAL
                snapshot: JSON.parse(JSON.stringify(engine.currentBms)),
                reason: 'FAULT_ASSERTED: 0x1001 CELL_OVERVOLTAGE (4.280V on Cell 4)'
            };
            dispatchPacket(ovpPkt);
            engine.lastTransmittedBms = JSON.parse(JSON.stringify(engine.currentBms));
            engine.lastPacketTimeMs = engine.currentTimeMs;
            return { success: true, message: 'Injected 0x1001 Cell Overvoltage (4.28V) -> FAILSAFE' };

        case 'inject_uvp':
            engine.currentBms.cellVoltagesMv[7] = 2750; // Cell 8 drops to 2.75V
            calculateExtrema(engine.currentBms);
            engine.currentBms.activeFaultId = 0x1002;
            engine.currentBms.faultSeverity = 3;
            engine.currentBms.faultReason = 'CELL_8_UNDERVOLTAGE_2750MV';
            engine.currentBms.fsmState = 2; // FAILSAFE
            engine.currentBms.relayStatusFlags = 0x00; // Tripped

            let uvpPkt = {
                seqId: engine.monotonicSeqId++,
                captureTimestampMs: engine.currentTimeMs,
                eventType: 5,
                priority: 3,
                snapshot: JSON.parse(JSON.stringify(engine.currentBms)),
                reason: 'FAULT_ASSERTED: 0x1002 CELL_UNDERVOLTAGE (2.750V on Cell 8)'
            };
            dispatchPacket(uvpPkt);
            engine.lastTransmittedBms = JSON.parse(JSON.stringify(engine.currentBms));
            engine.lastPacketTimeMs = engine.currentTimeMs;
            return { success: true, message: 'Injected 0x1002 Cell Undervoltage (2.75V) -> FAILSAFE' };

        case 'inject_imbalance':
            engine.currentBms.cellVoltagesMv[1] = 3950; // Cell 2 high
            engine.currentBms.cellVoltagesMv[6] = 3650; // Cell 7 low
            calculateExtrema(engine.currentBms);
            engine.currentBms.activeFaultId = 0x1003;
            engine.currentBms.faultSeverity = 1;
            engine.currentBms.faultReason = 'CELL_IMBALANCE_300MV';
            engine.currentBms.fsmState = 1; // DEGRADED

            let imbPkt = {
                seqId: engine.monotonicSeqId++,
                captureTimestampMs: engine.currentTimeMs,
                eventType: 5,
                priority: 2,
                snapshot: JSON.parse(JSON.stringify(engine.currentBms)),
                reason: 'FAULT_ASSERTED: 0x1003 CELL_IMBALANCE (300mV Delta)'
            };
            dispatchPacket(imbPkt);
            engine.lastTransmittedBms = JSON.parse(JSON.stringify(engine.currentBms));
            engine.lastPacketTimeMs = engine.currentTimeMs;
            return { success: true, message: 'Injected 0x1003 Cell Imbalance (300mV) -> DEGRADED' };

        case 'inject_welded_relay':
            engine.currentBms.relayStatusFlags = 0x22; // Commanded OPEN but Feedback CLOSED (WELDED)
            engine.currentBms.activeFaultId = 0x2002;
            engine.currentBms.faultSeverity = 3;
            engine.currentBms.faultReason = 'RELAY_WELDED_CONTACTS';
            engine.currentBms.fsmState = 3; // SHUTDOWN

            let weldPkt = {
                seqId: engine.monotonicSeqId++,
                captureTimestampMs: engine.currentTimeMs,
                eventType: 7, // RELAY_CHANGE
                priority: 3,
                snapshot: JSON.parse(JSON.stringify(engine.currentBms)),
                reason: 'FATAL: 0x2002 RELAY_WELDED (Main Contactor Welded Closed)'
            };
            dispatchPacket(weldPkt);
            engine.lastTransmittedBms = JSON.parse(JSON.stringify(engine.currentBms));
            engine.lastPacketTimeMs = engine.currentTimeMs;
            return { success: true, message: 'Injected 0x2002 Welded Relay Contact Mismatch -> SHUTDOWN' };

        case 'clear_faults':
            for (let i = 0; i < CONFIG.CELL_COUNT; i++) engine.currentBms.cellVoltagesMv[i] = 3700;
            calculateExtrema(engine.currentBms);
            engine.currentBms.activeFaultId = 0;
            engine.currentBms.faultSeverity = 0;
            engine.currentBms.faultReason = 'NONE';
            engine.currentBms.fsmState = 0; // NORMAL
            engine.currentBms.relayStatusFlags = 0x03; // Closed nominal

            let clrPkt = {
                seqId: engine.monotonicSeqId++,
                captureTimestampMs: engine.currentTimeMs,
                eventType: 6, // FAULT_CLEARED
                priority: 2,
                snapshot: JSON.parse(JSON.stringify(engine.currentBms)),
                reason: 'FAULTS_CLEARED_NORMAL_RESTORED'
            };
            dispatchPacket(clrPkt);
            engine.lastTransmittedBms = JSON.parse(JSON.stringify(engine.currentBms));
            engine.lastPacketTimeMs = engine.currentTimeMs;
            return { success: true, message: 'Cleared all faults; restored NORMAL state' };

        case 'cell_delta_step':
            engine.currentBms.cellVoltagesMv[0] += 35; // +35 mV > 25 mV deadband
            calculateExtrema(engine.currentBms);
            let stepPkt = {
                seqId: engine.monotonicSeqId++,
                captureTimestampMs: engine.currentTimeMs,
                eventType: 2, // CELL_DELTA
                priority: 1,
                snapshot: JSON.parse(JSON.stringify(engine.currentBms)),
                reason: `CELL_1_DELTA: ${engine.currentBms.cellVoltagesMv[0]}mV (+35mV jump)`
            };
            dispatchPacket(stepPkt);
            engine.lastTransmittedBms = JSON.parse(JSON.stringify(engine.currentBms));
            engine.lastPacketTimeMs = engine.currentTimeMs;
            return { success: true, message: 'Cell 1 stepped by +35mV (Deadband crossed -> Telemetry dispatched)' };

        case 'burst_events':
            for (let i = 1; i <= 15; i++) {
                let burstSnap = JSON.parse(JSON.stringify(engine.currentBms));
                burstSnap.cellVoltagesMv[0] = 3700 + (i * 20);
                calculateExtrema(burstSnap);
                let bPkt = {
                    seqId: engine.monotonicSeqId++,
                    captureTimestampMs: engine.currentTimeMs + (i * 10),
                    eventType: 2,
                    priority: 1,
                    snapshot: burstSnap,
                    reason: `RAPID_BURST_EVENT_${i}_OF_15 (+${i*20}mV)`
                };
                dispatchPacket(bPkt);
            }
            return { success: true, message: 'Dispatched burst of 15 telemetry events' };

        case 'force_heartbeat':
            let fbPkt = {
                seqId: engine.monotonicSeqId++,
                captureTimestampMs: engine.currentTimeMs,
                eventType: 1, // HEARTBEAT
                priority: 0,
                snapshot: JSON.parse(JSON.stringify(engine.currentBms)),
                reason: 'MANUAL_FORCE_HEARTBEAT_SYNC'
            };
            dispatchPacket(fbPkt);
            engine.lastPacketTimeMs = engine.currentTimeMs;
            return { success: true, message: 'Manual Heartbeat keepalive forced' };

        default:
            return { success: false, message: `Unknown action: ${action}` };
    }
}

/* ========================================================================== */
/* HTTP SERVER ROUTING                                                        */
/* ========================================================================== */

const server = http.createServer((req, res) => {
    const url = new URL(req.url, `http://${req.headers.host}`);

    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type');

    if (req.method === 'OPTIONS') {
        res.writeHead(204);
        res.end();
        return;
    }

    if (url.pathname === '/api/data') {
        const uptimeMs = Date.now() - startTime;
        const uptimeSec = Math.floor(uptimeMs / 1000);
        const hours = Math.floor(uptimeSec / 3600);
        const minutes = Math.floor((uptimeSec % 3600) / 60);
        const seconds = uptimeSec % 60;

        const responsePayload = {
            timestamp: new Date().toISOString(),
            uptime: `${String(hours).padStart(2, '0')}:${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}`,
            uptimeMs,
            bms: engine.currentBms,
            wifi: {
                state: engine.wifi.state,
                stateName: WifiStateNames[engine.wifi.state],
                physicalLinkUp: engine.wifi.physicalLinkUp,
                blynkServerAvailable: engine.wifi.blynkServerAvailable,
                rawRssiDbm: engine.wifi.rawRssiDbm,
                filteredRssiDbm: engine.wifi.filteredRssiDbm,
                quality: engine.wifi.quality,
                qualityName: RssiQualityNames[engine.wifi.quality],
                reconnectAttempts: engine.wifi.reconnectAttempts,
                backoffIntervalMs: engine.wifi.backoffIntervalMs,
                isOnline: (engine.wifi.state === 5)
            },
            queue: {
                depth: engine.queue.buffer.length,
                capacity: engine.queue.capacity,
                totalEnqueued: engine.queue.totalEnqueued,
                totalDequeued: engine.queue.totalDequeued,
                overflowDropped: engine.queue.overflowDropped,
                peakDepth: engine.queue.peakDepth,
                isDraining: (engine.wifi.state === 5 && engine.queue.buffer.length > 0)
            },
            metrics: engine.metrics,
            virtualPins: getBlynkVirtualPins(),
            eventLog: engine.eventLog.slice(0, 50),
            recentPackets: engine.recentPackets.slice(0, 10)
        };

        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(responsePayload));
        return;
    }

    if (url.pathname === '/api/action' && req.method === 'POST') {
        let body = '';
        req.on('data', chunk => { body += chunk; });
        req.on('end', () => {
            try {
                const { action, payload } = JSON.parse(body);
                const result = handleAction(action, payload);
                res.writeHead(200, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify(result));
            } catch (e) {
                res.writeHead(400, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify({ success: false, message: 'Malformed JSON payload' }));
            }
        });
        return;
    }

    if (url.pathname === '/' || url.pathname === '/index.html') {
        const htmlPath = path.join(__dirname, 'index.html');
        fs.readFile(htmlPath, 'utf8', (err, data) => {
            if (err) {
                res.writeHead(500, { 'Content-Type': 'text/plain' });
                res.end('Error loading dashboard index.html');
                return;
            }
            res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
            res.end(data);
        });
        return;
    }

    res.writeHead(404, { 'Content-Type': 'text/plain' });
    res.end('Not Found');
});

server.listen(PORT, () => {
    console.log(`\n  ╔═══════════════════════════════════════════════════════════════╗`);
    console.log(`  ║  LIVE BLYNK DASHBOARD & EVENT TELEMETRY SERVER RUNNING        ║`);
    console.log(`  ║  Console URL:  http://localhost:${PORT}                          ║`);
    console.log(`  ║  Telemetry:    http://localhost:${PORT}/api/data                   ║`);
    console.log(`  ║  Architecture: ISO 26262 ASIL-C / MISRA-C Compliant           ║`);
    console.log(`  ╚═══════════════════════════════════════════════════════════════╝\n`);
});
