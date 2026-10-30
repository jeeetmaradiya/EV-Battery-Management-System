# Mandatory Screenshots Guide & Placement Directory

This directory is designated for all mandatory project, simulation, and dashboard screenshots required by the Elevance Skills evaluation criteria.

---

## 1. Directory Structure

Place your captured screenshots into their respective folders:

```
media/
└── screenshots/
    ├── task1/   # BMS Engine, Multi-Cell Voltage Scans & Thresholds
    ├── task2/   # Safety Relay Oscilloscope & Anti-Chatter Hysteresis
    ├── task3/   # Flicker-Free LCD Differential Rendering Comparison
    ├── task4/   # Deterministic Fault FSM & 5-Stage Recovery
    ├── task5/   # Event Telemetry Deadband & Offline Queue
    └── task6/   # Enterprise Blynk Analytics & Multi-Factor Risk
```

---

## 2. Screenshot Capture Checklist & Recommended Filenames

### Task 1: BMS Engine
1. `bms_simulation_terminal_pass.png`: Terminal output showing `node test_runner.js` passing 100% of scenarios.
2. `bms_telemetry_dashboard.png`: Browser view of `dashboard/index.html` displaying cell voltages, SoC slider, and balancing bitmask.
3. `bms_adaptive_threshold_graph.png`: Chart showing dynamic allowable voltage delta compensating during 40A burst.

### Task 2: Safety Relay Engine
1. `relay_simulation_results.png`: Terminal output showing `node simulation_runner.js` passing all 6 scenarios.
2. `relay_dashboard_oscilloscope.png`: Live dual-trace oscilloscope on `dashboard/index.html` showing current waveform and contactor trip boundary.
3. `relay_anti_chatter_hysteresis.png`: Visualization of dual threshold ($I_{\text{trip}} = 50\text{A}$, $I_{\text{reset}} = 42\text{A}$).

### Task 3: Flicker-Free LCD Engine
1. `lcd_benchmark_results.png`: Terminal output showing differential diff metrics and $94.44\%$ I2C bandwidth reduction.
2. `lcd_side_by_side_comparison.png`: Dashboard showing conventional blinking LCD vs smooth differential shadow-buffer rendering.
3. `lcd_preemptive_alarm_screen.png`: Preemptive high-priority safety override screen with active blinking header.

### Task 4: Fault State Machine
1. `fsm_simulation_pass.png`: Terminal output showing 7/7 scenarios passing in `simulation_runner.js`.
2. `fsm_state_matrix_view.png`: Dashboard tracking transitions between `NORMAL`, `DEGRADED`, `FAILSAFE`, and `SHUTDOWN`.
3. `fsm_5stage_recovery_log.png`: Visual trace of the 5-stage recovery pipeline executing after fault clearance.

### Task 5: Event Telemetry & Blynk Dashboard
1. `telemetry_test_pass.png`: Terminal output showing 8/8 simulation scenarios passed.
2. `telemetry_deadband_suppression.png`: Console showing sub-deadband noise suppression efficiency ($|\Delta V| < 25\text{mV}$).
3. `telemetry_offline_buffer_drain.png`: Visual demonstration of the 64-packet offline ring buffer flushing in FIFO order upon Wi-Fi reconnection.

### Task 6: Enterprise Blynk Analytics
1. `analytics_benchmark_100pass.png`: Terminal output showing 39/39 assertions passing.
2. `analytics_executive_dashboard.png`: Dark-mode console showing Composite Risk Score (CRS %), SOH meter, and live canvas charts.
3. `analytics_operator_directives.png`: Real-time diagnostic directive cards (`CRITICAL`, `WARNING`, `ADVISORY`).

---

## 3. Formatting & Resolution Rules

* **Format:** PNG or JPEG.
* **Resolution:** Minimum 1280x720 (720p), prefer 1920x1080 (1080p).
* **Clarity:** Ensure all signal names, voltage numbers, states, and text are crisp and clearly legible.
