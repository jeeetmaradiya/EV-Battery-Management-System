/**
 * @file server.js
 * @brief High-Speed WebSocket & HTTP Telemetry Server for the Safety Relay Dashboard.
 */

const http = require('http');
const fs = require('fs');
const path = require('path');
const { SafetyRelaySimulator, CONFIG, RelayState, Contacts } = require('../tests/simulation_runner');

const PORT = 3001;
const sim = new SafetyRelaySimulator(Date.now());

let targetCurrent = 28.0;
let targetVoltage = 230.0;
let injectMode = 'NONE';
let injectTicksRemaining = 0;

/* Periodic 10ms tick loop */
setInterval(() => {
    const now = Date.now();
    let current = targetCurrent;
    let voltage = targetVoltage;

    /* Base transducer thermal noise */
    current += (Math.random() - 0.5) * 0.15;
    voltage += (Math.random() - 0.5) * 0.5;

    /* Dynamic injection handling */
    if (injectTicksRemaining > 0) {
        injectTicksRemaining--;
        if (injectMode === 'OVERCURRENT') {
            current = 58.0 + (Math.random() - 0.5) * 0.2;
        } else if (injectMode === 'TRANSIENT_SPIKE') {
            current = 52.0;
        } else if (injectMode === 'FREEZE') {
            current = 28.000; /* Exactly constant, 0 span */
        } else if (injectMode === 'SLEW_JUMP') {
            current = 95.0;
        } else if (injectMode === 'WIRE_BREAK') {
            voltage = 0.0;
        } else if (injectMode === 'EMI_BURST') {
            current = 68.0;
        } else if (injectMode === 'GENUINE_STEP') {
            current = 45.0 + (Math.random() - 0.5) * 0.15;
        }
    } else {
        if (injectMode === 'GENUINE_STEP') {
            targetCurrent = 45.0; /* Step persists */
        }
        injectMode = 'NONE';
    }

    sim.processTick(now, current, voltage);
}, CONFIG.TICK_INTERVAL_MS);

const server = http.createServer((req, res) => {
    const url = req.url.split('?')[0];

    if (url === '/' || url === '/index.html') {
        const filePath = path.join(__dirname, 'index.html');
        fs.readFile(filePath, (err, data) => {
            if (err) {
                res.writeHead(500, { 'Content-Type': 'text/plain' });
                res.end('Error loading dashboard');
                return;
            }
            res.writeHead(200, { 'Content-Type': 'text/html' });
            res.end(data);
        });
    } else if (url === '/api/telemetry') {
        const data = {
            timestamp: sim.currentTimeMs,
            state: sim.state,
            contacts: sim.contacts,
            current: sim.window.prevSample,
            filteredCurrent: sim.filteredCurrent,
            voltage: sim.filteredVoltage,
            activeFlags: sim.activeFlags,
            signalClass: sim.window.classification,
            variance: sim.window.variance,
            stdDev: sim.window.stdDev,
            slewRate: sim.window.instantaneousSlewRate,
            span: sim.window.span,
            dwellTime: sim.currentTimeMs - sim.stateEntryTimeMs,
            tripDebounce: sim.tripDebounceCounterMs,
            clearDebounce: sim.clearDebounceCounterMs,
            reFaultCount: sim.reFaultCount,
            tripCount: sim.tripCount,
            noiseSpikesRejected: sim.noiseSpikesRejected,
            sensorAnomaliesCount: sim.sensorAnomaliesCount,
            recentEvents: sim.eventRing.slice(-10).reverse()
        };
        res.writeHead(200, { 'Content-Type': 'application/json', 'Access-Control-Allow-Origin': '*' });
        res.end(JSON.stringify(data));
    } else if (url === '/api/inject') {
        const parsedUrl = new URL(req.url, `http://${req.headers.host}`);
        const type = parsedUrl.searchParams.get('type') || 'NONE';
        const duration = parseInt(parsedUrl.searchParams.get('duration') || '15', 10);

        injectMode = type;
        injectTicksRemaining = duration;

        if (type === 'RESET') {
            sim.manualReset(Date.now());
            targetCurrent = 28.0;
            targetVoltage = 230.0;
            injectMode = 'NONE';
            injectTicksRemaining = 0;
        } else if (type === 'SET_PARAMS') {
            const cur = parseFloat(parsedUrl.searchParams.get('current'));
            const volt = parseFloat(parsedUrl.searchParams.get('voltage'));
            if (!isNaN(cur)) targetCurrent = cur;
            if (!isNaN(volt)) targetVoltage = volt;
        }

        res.writeHead(200, { 'Content-Type': 'application/json', 'Access-Control-Allow-Origin': '*' });
        res.end(JSON.stringify({ status: 'ok', injectMode, injectTicksRemaining }));
    } else {
        res.writeHead(404, { 'Content-Type': 'text/plain' });
        res.end('Not Found');
    }
});

server.listen(PORT, () => {
    console.log(`Safety Relay Live Telemetry Server running at: http://localhost:${PORT}`);
});
