/**
 * @file server.js
 * @brief Lightweight HTTP server to host the Modular BMS Telemetry Dashboard.
 */

const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = 3000;
const HTML_FILE = path.join(__dirname, 'index.html');

const server = http.createServer((req, res) => {
    fs.readFile(HTML_FILE, (err, data) => {
        if (err) {
            res.writeHead(500, { 'Content-Type': 'text/plain' });
            res.end('Error loading dashboard file');
            return;
        }
        res.writeHead(200, { 'Content-Type': 'text/html' });
        res.end(data);
    });
});

server.listen(PORT, () => {
    console.log(`======================================================================`);
    console.log(`  BMS DASHBOARD ONLINE: http://localhost:${PORT}`);
    console.log(`  (Or directly double-click: d:\\New folder\\dashboard\\index.html)`);
    console.log(`======================================================================`);
});
