/**
 * @file server.js
 * @brief Real-Time Simulation and Telemetry Server for Flicker-Free LCD Display Engine.
 * 
 * Provides HTTP endpoints and REST API for:
 *  - Live LCD VRAM and Front/Back buffer streams
 *  - Dirty cell heatmaps and bus bandwidth statistics
 *  - Naive vs Differential side-by-side comparison state
 *  - Interactive fault injection and page navigation controls
 */

const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = 3002;

/* ========================================================================== */
/* SIMULATION CORE INTEGRATION                                                */
/* ========================================================================== */

const LCD_COLS = 20;
const LCD_ROWS = 4;

class LiveLcdServerEngine {
    constructor() {
        this.frontBuffer = Array(LCD_ROWS).fill(null).map(() => Array(LCD_COLS).fill(' '));
        this.backBuffer = Array(LCD_ROWS).fill(null).map(() => Array(LCD_COLS).fill(' '));
        this.dirtyMap = Array(LCD_ROWS).fill(null).map(() => Array(LCD_COLS).fill(false));

        // Naive display state (for flicker simulation)
        this.naiveBuffer = Array(LCD_ROWS).fill(null).map(() => Array(LCD_COLS).fill(' '));
        this.naiveFlickerPhase = false; // Simulates the 1.5ms blanked state

        this.state = 'NORMAL_ROTATION';
        this.activePage = 0; // 0=Battery, 1=System, 2=Telemetry
        this.previousPage = 0;
        this.refreshIntervalMs = 100;
        this.pageDwellMs = 4000;
        this.blinkPeriodMs = 500;

        this.lastRefreshMs = Date.now();
        this.lastPageRotateMs = Date.now();
        this.lastBlinkToggleMs = Date.now();
        this.blinkState = false;
        this.forceFullRedraw = true;

        // Telemetry models
        this.battery = {
            voltageMv: 52400,
            currentMa: 14200,
            socPct: 88,
            tempDegC: 32,
            state: 'CHG ',
            cycles: 142
        };

        this.system = {
            mode: 'AUTO',
            uptimeSec: 0,
            cpuLoadPct: 18,
            ramUsedPct: 42,
            loopFreqHz: 100,
            watchdogOk: true
        };

        this.telemetry = {
            rssiDbm: -68,
            snrDb: 19,
            packetsTx: 12480,
            packetsRx: 12474,
            packetLossX100: 4,
            crcErrors: 0
        };

        this.fault = {
            active: false,
            code: 0,
            title: '',
            valueStr: '',
            limitStr: '',
            action: '',
            timestampMs: 0
        };

        // Counters
        this.stats = {
            framesRendered: 0,
            charsWritten: 0,
            charsSkipped: 0,
            cursorCommands: 0,
            clearsAvoided: 0,
            pageRotations: 0,
            faultPreemptions: 0,
            busBytesTransmitted: 0,
            busBytesNaive: 0,
            busBytesSaved: 0,
            lastDirtyCount: 0,
            savingsPct: 0
        };

        this.bootTimeMs = Date.now();
    }

    padLine(str, len = LCD_COLS) {
        if (str.length >= len) return str.substring(0, len);
        return str + ' '.repeat(len - str.length);
    }

    renderLines() {
        if (this.fault.active) {
            const header = this.blinkState ? '*** SYSTEM FAULT ***' : '! EMERGENCY ALARM ! ';
            const codeHex = '0x' + this.fault.code.toString(16).toUpperCase().padStart(2, '0');
            return [
                this.padLine(header),
                this.padLine(`${codeHex} ${this.fault.title}`),
                this.padLine(`${this.fault.valueStr.padEnd(9, ' ')} ${this.fault.limitStr.padEnd(9, ' ')}`),
                this.padLine(`ACT: ${this.fault.action}`)
            ];
        }

        if (this.activePage === 0) {
            const v = (this.battery.voltageMv / 1000).toFixed(2);
            const sign = this.battery.currentMa >= 0 ? '+' : '';
            const i = sign + (this.battery.currentMa / 1000).toFixed(1);
            const soc = String(this.battery.socPct).padStart(3, ' ');
            const temp = String(this.battery.tempDegC).padStart(3, ' ');
            const cyc = String(this.battery.cycles).padStart(4, ' ');
            return [
                this.padLine('[1/3] BATTERY PACK  '),
                this.padLine(`V:${v.padStart(5, ' ')}V I:${i.padStart(6, ' ')}A`),
                this.padLine(`SOC:${soc}%  T:${temp}C`),
                this.padLine(`ST:${this.battery.state} CYC:${cyc}`)
            ];
        } else if (this.activePage === 1) {
            const hours = String(Math.floor(this.system.uptimeSec / 3600)).padStart(2, '0');
            const mins = String(Math.floor((this.system.uptimeSec % 3600) / 60)).padStart(2, '0');
            const secs = String(this.system.uptimeSec % 60).padStart(2, '0');
            const cpu = String(this.system.cpuLoadPct).padStart(2, ' ');
            const ram = String(this.system.ramUsedPct).padStart(2, ' ');
            const wdg = this.system.watchdogOk ? 'OK ' : 'FLT';
            return [
                this.padLine('[2/3] SYSTEM HEALTH '),
                this.padLine(`MODE: ${this.system.mode.padEnd(13, ' ')}`),
                this.padLine(`CPU:${cpu}%   RAM:${ram}%`),
                this.padLine(`UP:${hours}:${mins}:${secs} WDG:${wdg}`)
            ];
        } else {
            const rssi = String(this.telemetry.rssiDbm).padStart(3, ' ');
            const snr = String(this.telemetry.snrDb).padStart(2, ' ');
            const tx = String(this.telemetry.packetsTx).padEnd(6, ' ');
            const rx = String(this.telemetry.packetsRx).padEnd(6, ' ');
            const loss = (this.telemetry.packetLossX100 / 100).toFixed(2).padStart(4, ' ');
            const crc = String(this.telemetry.crcErrors).padEnd(3, ' ');
            return [
                this.padLine('[3/3] RF TELEMETRY  '),
                this.padLine(`SIG:${rssi}dBm SNR:${snr}dB`),
                this.padLine(`TX:${tx} RX:${rx}`),
                this.padLine(`LOSS:${loss}% CRC:${crc}`)
            ];
        }
    }

    tick(now) {
        // Blink timer
        if (now - this.lastBlinkToggleMs >= this.blinkPeriodMs) {
            this.blinkState = !this.blinkState;
            this.lastBlinkToggleMs = now;
        }

        // Page rotation timer
        if (this.state === 'NORMAL_ROTATION' && !this.fault.active) {
            if (now - this.lastPageRotateMs >= this.pageDwellMs) {
                this.activePage = (this.activePage + 1) % 3;
                this.lastPageRotateMs = now;
                this.stats.pageRotations++;
            }
        }

        // Refresh interval throttle
        if (now - this.lastRefreshMs < this.refreshIntervalMs) {
            return;
        }
        this.lastRefreshMs = now;
        this.stats.framesRendered++;
        this.stats.clearsAvoided++;

        // Render to back buffer
        const lines = this.renderLines();
        for (let r = 0; r < LCD_ROWS; r++) {
            for (let c = 0; c < LCD_COLS; c++) {
                this.backBuffer[r][c] = lines[r][c] || ' ';
            }
        }

        // Differential Flush
        let dirtyCount = 0;
        let charsWritten = 0;
        let cursorsSent = 0;

        for (let r = 0; r < LCD_ROWS; r++) {
            let c = 0;
            while (c < LCD_COLS) {
                const isDirty = this.forceFullRedraw || (this.backBuffer[r][c] !== this.frontBuffer[r][c]);
                this.dirtyMap[r][c] = isDirty;

                if (!isDirty) {
                    this.stats.charsSkipped++;
                    c++;
                    continue;
                }

                const spanStart = c;
                while (c < LCD_COLS && (this.forceFullRedraw || (this.backBuffer[r][c] !== this.frontBuffer[r][c]))) {
                    this.dirtyMap[r][c] = true;
                    c++;
                }
                const spanLen = c - spanStart;
                dirtyCount += spanLen;
                cursorsSent++;

                for (let i = 0; i < spanLen; i++) {
                    const ch = this.backBuffer[r][spanStart + i];
                    this.frontBuffer[r][spanStart + i] = ch;
                    charsWritten++;
                }
            }
        }

        this.forceFullRedraw = false;
        this.stats.lastDirtyCount = dirtyCount;
        this.stats.charsWritten += charsWritten;
        this.stats.cursorCommands += cursorsSent;

        // Bus accounting
        const actualBytes = (charsWritten * 4) + (cursorsSent * 4);
        const naiveBytes = (80 * 4) + (4 * 4) + 4; // 340 bytes
        this.stats.busBytesTransmitted += actualBytes;
        this.stats.busBytesNaive += naiveBytes;
        this.stats.busBytesSaved += Math.max(0, naiveBytes - actualBytes);
        this.stats.savingsPct = ((this.stats.busBytesSaved / this.stats.busBytesNaive) * 100).toFixed(1);

        // Update Naive buffer with simulated screen clear
        this.naiveBuffer = lines.map(line => line.split(''));
    }

    triggerFault(code, title, val, lim, act) {
        this.fault.active = true;
        this.fault.code = code;
        this.fault.title = title;
        this.fault.valueStr = val;
        this.fault.limitStr = lim;
        this.fault.action = act;
        this.fault.timestampMs = Date.now();

        this.previousPage = this.activePage;
        this.state = 'FAULT_OVERRIDE';
        this.blinkState = true;
        this.lastBlinkToggleMs = Date.now();
        this.lastRefreshMs = 0;
        this.stats.faultPreemptions++;
    }

    clearFault() {
        if (!this.fault.active) return false;
        this.fault.active = false;
        this.state = 'NORMAL_ROTATION';
        this.activePage = this.previousPage;
        this.lastPageRotateMs = Date.now();
        this.lastRefreshMs = 0;
        return true;
    }
}

const engine = new LiveLcdServerEngine();

// Background 10ms simulation tick
setInterval(() => {
    const now = Date.now();
    engine.system.uptimeSec = Math.floor((now - engine.bootTimeMs) / 1000);

    // Subtle sensor noise
    if (!engine.fault.active) {
        engine.battery.voltageMv = 52400 + Math.floor(Math.sin(now / 1500) * 150);
        engine.battery.currentMa = 14200 + Math.floor(Math.cos(now / 1200) * 300);
        engine.system.cpuLoadPct = 16 + Math.floor(Math.abs(Math.sin(now / 3000)) * 12);
        engine.telemetry.packetsTx += 1;
        engine.telemetry.packetsRx += 1;
    }

    engine.tick(now);
}, 10);

/* ========================================================================== */
/* HTTP SERVER & API ROUTES                                                   */
/* ========================================================================== */

const server = http.createServer((req, res) => {
    const parsedUrl = new URL(req.url, `http://${req.headers.host}`);
    const pathname = parsedUrl.pathname;

    // CORS Headers
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Origin, Content-Type');

    if (req.method === 'OPTIONS') {
        res.writeHead(204);
        res.end();
        return;
    }

    if (pathname === '/' || pathname === '/index.html') {
        const filePath = path.join(__dirname, 'index.html');
        fs.readFile(filePath, (err, data) => {
            if (err) {
                res.writeHead(500, { 'Content-Type': 'text/plain' });
                res.end('Dashboard index.html not found');
                return;
            }
            res.writeHead(200, { 'Content-Type': 'text/html' });
            res.end(data);
        });
    } else if (pathname === '/api/state') {
        const payload = {
            state: engine.state,
            activePage: engine.activePage,
            dwellRemainingMs: Math.max(0, engine.pageDwellMs - (Date.now() - engine.lastPageRotateMs)),
            frontBuffer: engine.frontBuffer,
            backBuffer: engine.backBuffer,
            naiveBuffer: engine.naiveBuffer,
            dirtyMap: engine.dirtyMap,
            stats: engine.stats,
            fault: engine.fault,
            battery: engine.battery,
            system: engine.system,
            telemetry: engine.telemetry,
            refreshIntervalMs: engine.refreshIntervalMs
        };
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(payload));
    } else if (pathname === '/api/control') {
        let body = '';
        req.on('data', chunk => { body += chunk; });
        req.on('end', () => {
            try {
                const data = JSON.parse(body || '{}');
                const action = data.action;

                if (action === 'TRIGGER_FAULT') {
                    const code = data.code || 0x11;
                    const title = data.title || 'OVER-VOLTAGE';
                    const val = data.val || '59.40V';
                    const lim = data.lim || 'MAX:58.40V';
                    const act = data.act || 'DISCONN CHG';
                    engine.triggerFault(code, title, val, lim, act);
                } else if (action === 'CLEAR_FAULT') {
                    engine.clearFault();
                } else if (action === 'NEXT_PAGE') {
                    engine.activePage = (engine.activePage + 1) % 3;
                    engine.lastPageRotateMs = Date.now();
                    engine.lastRefreshMs = 0;
                } else if (action === 'PREV_PAGE') {
                    engine.activePage = (engine.activePage + 2) % 3;
                    engine.lastPageRotateMs = Date.now();
                    engine.lastRefreshMs = 0;
                } else if (action === 'PAUSE_ROTATION') {
                    engine.state = (engine.state === 'PAGE_PAUSED') ? 'NORMAL_ROTATION' : 'PAGE_PAUSED';
                } else if (action === 'SET_REFRESH') {
                    if (data.interval >= 20 && data.interval <= 1000) {
                        engine.refreshIntervalMs = data.interval;
                    }
                }

                res.writeHead(200, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify({ success: true, state: engine.state }));
            } catch (err) {
                res.writeHead(400, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify({ error: err.message }));
            }
        });
    } else {
        res.writeHead(404, { 'Content-Type': 'text/plain' });
        res.end('Not Found');
    }
});

server.listen(PORT, () => {
    console.log(`[LCD Dashboard Server] Live at http://localhost:${PORT}/`);
});
