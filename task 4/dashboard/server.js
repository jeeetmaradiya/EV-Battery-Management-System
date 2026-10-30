const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = 3400;

// ─── State Machine Constants ───────────────────────────────────────────────────
const STATES = { NORMAL: 'NORMAL', DEGRADED: 'DEGRADED', FAILSAFE: 'FAILSAFE', SHUTDOWN: 'SHUTDOWN' };
const SEVERITY = { LOW: 1, MEDIUM: 2, HIGH: 3, CRITICAL: 4 };
const SEVERITY_NAMES = { 1: 'LOW', 2: 'MEDIUM', 3: 'HIGH', 4: 'CRITICAL' };

const RECOVERY_STAGES = [
  { id: 0, name: 'FAULT_DETECTED', label: 'Fault Detected' },
  { id: 1, name: 'ISOLATION', label: 'Isolation' },
  { id: 2, name: 'MITIGATION', label: 'Mitigation' },
  { id: 3, name: 'VERIFICATION', label: 'Verification' },
  { id: 4, name: 'RESTORATION', label: 'Restoration' },
  { id: 5, name: 'MONITORING', label: 'Monitoring' }
];

// Allowed transitions
const TRANSITION_TABLE = [
  { from: STATES.NORMAL,   to: STATES.DEGRADED, condition: 'Low/Medium severity fault detected' },
  { from: STATES.NORMAL,   to: STATES.FAILSAFE, condition: 'High/Critical severity fault detected' },
  { from: STATES.DEGRADED, to: STATES.NORMAL,    condition: 'All faults cleared, recovery complete' },
  { from: STATES.DEGRADED, to: STATES.FAILSAFE,  condition: 'Fault escalation or new critical fault' },
  { from: STATES.FAILSAFE, to: STATES.DEGRADED,  condition: 'Recovery pipeline success' },
  { from: STATES.FAILSAFE, to: STATES.SHUTDOWN,  condition: 'Recovery failed or ESTOP' },
  { from: STATES.SHUTDOWN, to: STATES.NORMAL,     condition: 'Service reset by operator' }
];

// ─── Engine State ──────────────────────────────────────────────────────────────
let faultIdCounter = 0;
const startTime = Date.now();

const engine = {
  currentState: STATES.NORMAL,
  previousState: STATES.NORMAL,
  transitionSeq: 0,
  transitionLog: [],
  faults: [],
  recoveryPipeline: {
    active: false,
    currentStage: -1,
    progress: 0,
    startedAt: null,
    targetFaultId: null,
    verificationSamples: 0,
    verificationTarget: 10
  },
  telemetry: {
    cellVoltages: Array(12).fill(0).map(() => 3.6 + Math.random() * 0.2),
    adcChannels: [2048, 2048, 2048, 2048],
    adcFrozenCounters: [0, 0, 0, 0],
    commHeartbeatAge: 0,
    commLastHeartbeat: Date.now(),
    relayCommanded: [true, true, true, true],
    relayActual: [true, true, true, true],
    temperature: 25 + Math.random() * 5,
    current: 10 + Math.random() * 2,
    busPower: true
  },
  subsystems: {
    battery: { status: 'OK', detail: 'All cells nominal' },
    relay: { status: 'OK', detail: 'All relays matched' },
    communication: { status: 'OK', detail: 'Heartbeat active' },
    adc: { status: 'OK', detail: 'All channels active' }
  },
  metrics: {
    totalFaults: 0,
    totalTransitions: 0,
    recoveryAttempts: 0
  },
  scenarioTimer: 0,
  scenarioPhase: 0
};

// ─── Fault Management ──────────────────────────────────────────────────────────
function generateFaultId() {
  faultIdCounter++;
  return '0x' + faultIdCounter.toString(16).toUpperCase().padStart(4, '0');
}

function registerFault(name, source, severity, reason) {
  const existing = engine.faults.find(f => f.name === name && f.status === 'Active');
  if (existing) {
    existing.occurrences++;
    return existing;
  }

  const fault = {
    id: generateFaultId(),
    name,
    source,
    severity,
    severityName: SEVERITY_NAMES[severity],
    status: 'Active',
    firstDetected: new Date().toISOString(),
    lastSeen: new Date().toISOString(),
    occurrences: 1,
    reason
  };
  engine.faults.push(fault);
  engine.metrics.totalFaults++;
  return fault;
}

function clearFault(name) {
  const fault = engine.faults.find(f => f.name === name && f.status === 'Active');
  if (fault) {
    fault.status = 'Cleared';
    fault.clearedAt = new Date().toISOString();
  }
  return fault;
}

function getActiveFaults() {
  return engine.faults.filter(f => f.status === 'Active');
}

function getMaxActiveSeverity() {
  const active = getActiveFaults();
  if (active.length === 0) return 0;
  return Math.max(...active.map(f => f.severity));
}

// ─── State Transitions ─────────────────────────────────────────────────────────
function transitionTo(newState, faultId, source, severity, reason) {
  if (engine.currentState === newState) return;

  const allowed = TRANSITION_TABLE.find(t => t.from === engine.currentState && t.to === newState);
  if (!allowed) return;

  engine.previousState = engine.currentState;
  engine.currentState = newState;
  engine.transitionSeq++;
  engine.metrics.totalTransitions++;

  const logEntry = {
    seq: engine.transitionSeq,
    timestamp: new Date().toISOString(),
    prevState: engine.previousState,
    newState: engine.currentState,
    faultId: faultId || 'N/A',
    source: source || 'SYSTEM',
    severity: severity ? SEVERITY_NAMES[severity] : 'N/A',
    reason: reason || allowed.condition
  };

  engine.transitionLog.unshift(logEntry);
  if (engine.transitionLog.length > 50) engine.transitionLog.pop();
}

// ─── Recovery Pipeline ─────────────────────────────────────────────────────────
function startRecovery() {
  if (engine.currentState !== STATES.FAILSAFE) return false;
  if (engine.recoveryPipeline.active) return false;

  engine.recoveryPipeline = {
    active: true,
    currentStage: 0,
    progress: 0,
    startedAt: Date.now(),
    targetFaultId: getActiveFaults()[0]?.id || 'N/A',
    verificationSamples: 0,
    verificationTarget: 10
  };
  engine.metrics.recoveryAttempts++;
  return true;
}

function tickRecovery() {
  const rp = engine.recoveryPipeline;
  if (!rp.active) return;

  const elapsed = Date.now() - rp.startedAt;
  const stageDuration = 2000; // 2 seconds per stage

  if (rp.currentStage < 5) {
    rp.progress += 5 + Math.random() * 10;
    if (rp.progress >= 100) {
      rp.currentStage++;
      rp.progress = 0;
      if (rp.currentStage === 3) {
        rp.verificationSamples = 0;
      }
    }

    if (rp.currentStage === 3) {
      rp.verificationSamples = Math.min(rp.verificationTarget, rp.verificationSamples + 1);
    }
  } else {
    // Recovery complete
    rp.active = false;
    rp.currentStage = 5;
    rp.progress = 100;

    // Clear all active faults
    getActiveFaults().forEach(f => {
      f.status = 'Cleared';
      f.clearedAt = new Date().toISOString();
    });

    // Transition to DEGRADED first
    transitionTo(STATES.DEGRADED, rp.targetFaultId, 'RECOVERY', null, 'Recovery pipeline completed successfully');

    // Then if no faults, go to NORMAL
    setTimeout(() => {
      if (getActiveFaults().length === 0 && engine.currentState === STATES.DEGRADED) {
        transitionTo(STATES.NORMAL, 'N/A', 'RECOVERY', null, 'All faults cleared, system nominal');
        resetTelemetry();
      }
    }, 1500);
  }
}

// ─── Telemetry Simulation ──────────────────────────────────────────────────────
function resetTelemetry() {
  engine.telemetry.cellVoltages = Array(12).fill(0).map(() => 3.6 + Math.random() * 0.2);
  engine.telemetry.adcChannels = [2048 + Math.random() * 100, 2048 + Math.random() * 100, 2048 + Math.random() * 100, 2048 + Math.random() * 100];
  engine.telemetry.adcFrozenCounters = [0, 0, 0, 0];
  engine.telemetry.commHeartbeatAge = 0;
  engine.telemetry.commLastHeartbeat = Date.now();
  engine.telemetry.relayCommanded = [true, true, true, true];
  engine.telemetry.relayActual = [true, true, true, true];
  engine.telemetry.temperature = 25 + Math.random() * 5;
  engine.telemetry.current = 10 + Math.random() * 2;
  engine.telemetry.busPower = true;

  engine.subsystems.battery = { status: 'OK', detail: 'All cells nominal' };
  engine.subsystems.relay = { status: 'OK', detail: 'All relays matched' };
  engine.subsystems.communication = { status: 'OK', detail: 'Heartbeat active' };
  engine.subsystems.adc = { status: 'OK', detail: 'All channels active' };
}

function tickTelemetry() {
  const t = engine.telemetry;

  // Normal jitter on cell voltages
  t.cellVoltages = t.cellVoltages.map(v => {
    let nv = v + (Math.random() - 0.5) * 0.02;
    return Math.max(2.5, Math.min(4.3, nv));
  });

  // ADC jitter
  t.adcChannels = t.adcChannels.map((v, i) => {
    if (t.adcFrozenCounters[i] > 0) return v; // frozen
    return v + (Math.random() - 0.5) * 20;
  });

  // Comm heartbeat age
  t.commHeartbeatAge = (Date.now() - t.commLastHeartbeat) / 1000;

  // Temperature drift
  t.temperature += (Math.random() - 0.5) * 0.3;
  t.temperature = Math.max(15, Math.min(60, t.temperature));

  // Current drift
  t.current += (Math.random() - 0.5) * 0.5;
  t.current = Math.max(0, Math.min(30, t.current));
}

// ─── Fault Detectors ───────────────────────────────────────────────────────────
function runFaultDetectors() {
  const t = engine.telemetry;

  // ADC Frozen Detector
  t.adcFrozenCounters.forEach((count, i) => {
    if (count > 5) {
      const fault = registerFault('ADC_FROZEN_CH' + i, 'ADC_DETECTOR', SEVERITY.HIGH, `ADC Channel ${i} frozen for ${count} cycles`);
      engine.subsystems.adc = { status: 'FAULT', detail: `Channel ${i} frozen` };
      if (engine.currentState === STATES.NORMAL) {
        transitionTo(STATES.FAILSAFE, fault.id, fault.source, fault.severity, fault.reason);
      } else if (engine.currentState === STATES.DEGRADED) {
        transitionTo(STATES.FAILSAFE, fault.id, fault.source, fault.severity, fault.reason);
      }
    }
  });

  // Relay Mismatch Detector
  for (let i = 0; i < 4; i++) {
    if (t.relayCommanded[i] !== t.relayActual[i]) {
      const fault = registerFault('RELAY_MISMATCH_' + i, 'RELAY_DETECTOR', SEVERITY.CRITICAL, `Relay ${i} commanded=${t.relayCommanded[i]} actual=${t.relayActual[i]}`);
      engine.subsystems.relay = { status: 'FAULT', detail: `Relay ${i} mismatch` };
      if (engine.currentState === STATES.NORMAL) {
        transitionTo(STATES.FAILSAFE, fault.id, fault.source, fault.severity, fault.reason);
      } else if (engine.currentState === STATES.DEGRADED) {
        transitionTo(STATES.FAILSAFE, fault.id, fault.source, fault.severity, fault.reason);
      }
    }
  }

  // Cell Voltage Detector
  for (let i = 0; i < 12; i++) {
    if (t.cellVoltages[i] > 4.25) {
      const fault = registerFault('CELL_OV_' + i, 'CELL_DETECTOR', SEVERITY.HIGH, `Cell ${i} overvoltage: ${t.cellVoltages[i].toFixed(3)}V`);
      engine.subsystems.battery = { status: 'FAULT', detail: `Cell ${i} overvoltage` };
      if (engine.currentState === STATES.NORMAL) {
        transitionTo(STATES.FAILSAFE, fault.id, fault.source, fault.severity, fault.reason);
      } else if (engine.currentState === STATES.DEGRADED) {
        transitionTo(STATES.FAILSAFE, fault.id, fault.source, fault.severity, fault.reason);
      }
    }
    if (t.cellVoltages[i] < 2.8) {
      const fault = registerFault('CELL_UV_' + i, 'CELL_DETECTOR', SEVERITY.MEDIUM, `Cell ${i} undervoltage: ${t.cellVoltages[i].toFixed(3)}V`);
      engine.subsystems.battery = { status: 'WARNING', detail: `Cell ${i} undervoltage` };
      if (engine.currentState === STATES.NORMAL) {
        transitionTo(STATES.DEGRADED, fault.id, fault.source, fault.severity, fault.reason);
      }
    }
  }

  // Comm Heartbeat Detector
  if (t.commHeartbeatAge > 5) {
    const fault = registerFault('COMM_HEARTBEAT_LOSS', 'COMM_DETECTOR', SEVERITY.HIGH, `Heartbeat age: ${t.commHeartbeatAge.toFixed(1)}s`);
    engine.subsystems.communication = { status: 'FAULT', detail: `Heartbeat lost (${t.commHeartbeatAge.toFixed(1)}s)` };
    if (engine.currentState === STATES.NORMAL) {
      transitionTo(STATES.FAILSAFE, fault.id, fault.source, fault.severity, fault.reason);
    } else if (engine.currentState === STATES.DEGRADED) {
      transitionTo(STATES.FAILSAFE, fault.id, fault.source, fault.severity, fault.reason);
    }
  } else if (t.commHeartbeatAge > 2) {
    engine.subsystems.communication = { status: 'WARNING', detail: `Heartbeat delayed (${t.commHeartbeatAge.toFixed(1)}s)` };
  } else {
    if (engine.subsystems.communication.status !== 'OK') {
      const cleared = clearFault('COMM_HEARTBEAT_LOSS');
      engine.subsystems.communication = { status: 'OK', detail: 'Heartbeat active' };
    }
  }
}

// ─── Scenario Simulation ───────────────────────────────────────────────────────
function tickScenario() {
  engine.scenarioTimer++;

  // Auto-cycle through scenarios every ~30 seconds
  const cycleLength = 60; // ticks (at 500ms = 30s)

  if (engine.currentState === STATES.NORMAL && !engine.recoveryPipeline.active) {
    // Randomly inject a fault scenario
    if (engine.scenarioTimer % cycleLength === 0) {
      engine.scenarioPhase = (engine.scenarioPhase + 1) % 5;
      switch (engine.scenarioPhase) {
        case 0:
          injectFault('adc_frozen');
          break;
        case 1:
          injectFault('relay_welded');
          break;
        case 2:
          injectFault('comm_loss');
          break;
        case 3:
          injectFault('cell_overvoltage');
          break;
        case 4:
          // No auto-inject, just let it run normal
          break;
      }
    }
  }

  // Auto-recovery in FAILSAFE after a delay
  if (engine.currentState === STATES.FAILSAFE && !engine.recoveryPipeline.active) {
    if (engine.scenarioTimer % 20 === 0) {
      startRecovery();
    }
  }
}

// ─── Fault Injection ───────────────────────────────────────────────────────────
function injectFault(type) {
  const t = engine.telemetry;
  switch (type) {
    case 'adc_frozen': {
      const ch = Math.floor(Math.random() * 4);
      t.adcFrozenCounters[ch] = 10;
      t.adcChannels[ch] = 2048; // Freeze at fixed value
      engine.subsystems.adc = { status: 'FAULT', detail: `Channel ${ch} frozen (injected)` };
      return { success: true, message: `ADC Channel ${ch} frozen` };
    }
    case 'relay_welded': {
      const r = Math.floor(Math.random() * 4);
      t.relayActual[r] = !t.relayCommanded[r];
      engine.subsystems.relay = { status: 'FAULT', detail: `Relay ${r} welded (injected)` };
      return { success: true, message: `Relay ${r} welded` };
    }
    case 'comm_loss': {
      t.commLastHeartbeat = Date.now() - 10000;
      engine.subsystems.communication = { status: 'FAULT', detail: 'Heartbeat lost (injected)' };
      return { success: true, message: 'Communication heartbeat lost' };
    }
    case 'cell_overvoltage': {
      const cell = Math.floor(Math.random() * 12);
      t.cellVoltages[cell] = 4.35 + Math.random() * 0.1;
      engine.subsystems.battery = { status: 'FAULT', detail: `Cell ${cell} overvoltage (injected)` };
      return { success: true, message: `Cell ${cell} overvoltage` };
    }
    case 'estop': {
      const fault = registerFault('ESTOP_ACTIVATED', 'OPERATOR', SEVERITY.CRITICAL, 'Emergency stop activated by operator');
      if (engine.currentState === STATES.NORMAL) {
        transitionTo(STATES.FAILSAFE, fault.id, 'OPERATOR', SEVERITY.CRITICAL, 'ESTOP activated');
      }
      if (engine.currentState === STATES.DEGRADED) {
        transitionTo(STATES.FAILSAFE, fault.id, 'OPERATOR', SEVERITY.CRITICAL, 'ESTOP activated');
      }
      if (engine.currentState === STATES.FAILSAFE) {
        transitionTo(STATES.SHUTDOWN, fault.id, 'OPERATOR', SEVERITY.CRITICAL, 'ESTOP - forced shutdown');
      }
      return { success: true, message: 'ESTOP activated' };
    }
    case 'request_recovery': {
      const ok = startRecovery();
      return { success: ok, message: ok ? 'Recovery pipeline started' : 'Cannot start recovery (not in FAILSAFE or already running)' };
    }
    case 'service_reset': {
      if (engine.currentState !== STATES.SHUTDOWN) {
        return { success: false, message: 'Service reset only available in SHUTDOWN state' };
      }
      // Clear all faults
      engine.faults.forEach(f => {
        if (f.status === 'Active') {
          f.status = 'Cleared';
          f.clearedAt = new Date().toISOString();
        }
      });
      engine.recoveryPipeline = {
        active: false, currentStage: -1, progress: 0, startedAt: null,
        targetFaultId: null, verificationSamples: 0, verificationTarget: 10
      };
      resetTelemetry();
      transitionTo(STATES.NORMAL, 'N/A', 'OPERATOR', null, 'Service reset by operator');
      return { success: true, message: 'Service reset complete - system NORMAL' };
    }
    default:
      return { success: false, message: 'Unknown fault type' };
  }
}

// ─── Main Tick ─────────────────────────────────────────────────────────────────
function tick() {
  tickTelemetry();
  runFaultDetectors();
  tickRecovery();
  tickScenario();
}

setInterval(tick, 500);

// ─── Build API Response ────────────────────────────────────────────────────────
function getApiData() {
  const now = Date.now();
  const uptimeMs = now - startTime;
  const uptimeSec = Math.floor(uptimeMs / 1000);
  const hours = Math.floor(uptimeSec / 3600);
  const minutes = Math.floor((uptimeSec % 3600) / 60);
  const seconds = uptimeSec % 60;

  return {
    timestamp: new Date().toISOString(),
    uptime: `${String(hours).padStart(2, '0')}:${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}`,
    uptimeMs,
    currentState: engine.currentState,
    previousState: engine.previousState,
    transitionSeq: engine.transitionSeq,
    transitionLog: engine.transitionLog.slice(0, 20),
    transitionTable: TRANSITION_TABLE,
    faults: engine.faults.slice(-30).reverse(),
    activeFaults: getActiveFaults(),
    recoveryPipeline: {
      ...engine.recoveryPipeline,
      stages: RECOVERY_STAGES
    },
    telemetry: engine.telemetry,
    subsystems: engine.subsystems,
    metrics: engine.metrics
  };
}

// ─── HTTP Server ───────────────────────────────────────────────────────────────
const server = http.createServer((req, res) => {
  const url = new URL(req.url, `http://${req.headers.host}`);

  // CORS headers
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');

  if (req.method === 'OPTIONS') {
    res.writeHead(204);
    res.end();
    return;
  }

  if (url.pathname === '/api/data') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(getApiData()));
    return;
  }

  if (url.pathname === '/api/inject' && req.method === 'POST') {
    let body = '';
    req.on('data', chunk => { body += chunk; });
    req.on('end', () => {
      try {
        const { type } = JSON.parse(body);
        const result = injectFault(type);
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(result));
      } catch (e) {
        res.writeHead(400, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ success: false, message: 'Invalid request' }));
      }
    });
    return;
  }

  if (url.pathname === '/' || url.pathname === '/index.html') {
    const htmlPath = path.join(__dirname, 'index.html');
    fs.readFile(htmlPath, 'utf8', (err, data) => {
      if (err) {
        res.writeHead(500, { 'Content-Type': 'text/plain' });
        res.end('Error loading dashboard');
        return;
      }
      res.writeHead(200, { 'Content-Type': 'text/html' });
      res.end(data);
    });
    return;
  }

  res.writeHead(404, { 'Content-Type': 'text/plain' });
  res.end('Not Found');
});

server.listen(PORT, () => {
  console.log(`\n  ╔══════════════════════════════════════════════════════╗`);
  console.log(`  ║  DETERMINISTIC FAULT STATE MACHINE DASHBOARD        ║`);
  console.log(`  ║  Server running on http://localhost:${PORT}            ║`);
  console.log(`  ║  API endpoint:   http://localhost:${PORT}/api/data     ║`);
  console.log(`  ╚══════════════════════════════════════════════════════╝\n`);
});
