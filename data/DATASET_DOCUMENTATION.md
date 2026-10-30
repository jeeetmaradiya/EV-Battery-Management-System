# BMS Telemetry, Load Profiles & Risk Datasets

**Project:** Modular Battery Management System (BMS)  
**Track:** Elevance Skills — Embedded Systems, IoT & Functional Safety  
**Domain:** Energy Storage Systems / Automotive ASIL-C Telemetry  
**Sampling Frequency:** 100 Hz (10 ms tick resolution)  

---

## 1. Overview of Included Datasets

In safety-critical embedded systems and BMS firmware engineering, **datasets** represent dynamic physical telemetry streams, empirical drive cycles, fault injection scenarios, and multi-factor analytical evaluation logs. 

This directory contains three formal datasets synthesized and ingested by the firmware verification suites and simulation runners:

| Dataset File | Records | Sampling Rate | Description |
| :--- | :---: | :---: | :--- |
| **`cell_voltages_16s_telemetry.csv`** | 1,000 rows | 100 Hz (10 ms) | Time-series individual cell voltages ($V_1$ to $V_{16}$), pack voltage, pack current, State of Charge (SoC %), cell delta imbalance, and balancing bitmask. Includes simulated cell degradation divergence on Cell 4. |
| **`drive_cycle_dynamic_current.csv`** | 1,000 rows | 100 Hz (10 ms) | Dynamic drive cycle with nominal current, inductive load inrush step (38.5A), over-current fault trip (56A at 120ms debounce), slew rate $|dI/dt|$, contactor state, and thermal profiles. |
| **`composite_risk_evaluation_log.csv`** | 1,000 rows | 100 Hz (10 ms) | Mathematical multi-factor risk decomposition tracking Imbalance Factor, Thermal Spread, SoC Stress, Contactor Wear, instantaneous Composite Risk Score (CRS %), FSM State, and active operator directives. |

---

## 2. Parameter Specifications & Data Dictionary

### `cell_voltages_16s_telemetry.csv`
* `Timestamp_ms`: Discrete millisecond counter ($10\,\text{ms}$ increments).
* `V_Pack_V`: Total pack voltage in Volts ($48\text{V}$ nominal).
* `Current_A`: Instantaneous pack current (Amperes).
* `SoC_pct`: Estimated State of Charge ($0\text{--}100\%$).
* `Imbalance_mV`: Instantaneous divergence $\Delta V = V_{\max} - V_{\min}$ (millivolts).
* `Cell_1_mV` to `Cell_16_mV`: Individual tap voltages for each of the 16 series-connected cells.
* `Balancing_Bitmask`: Active 16-bit passive bleed balancing register in hexadecimal format.

### `drive_cycle_dynamic_current.csv`
* `Current_A`: Pack shunt ammeter reading ($0\text{ to }60\,\text{A}$).
* `Slew_Rate_A_per_s`: Discrete first numerical derivative of current ($dI/dt$).
* `Hysteresis_Trip_A`: Protection threshold boundary ($50.0\,\text{A}$).
* `Contactor_State`: Main high-voltage safety contactor state (`1 = CLOSED`, `0 = OPEN / TRIPPED`).
* `Temp_Max_C`: Hottest cell NTC thermistor measurement ($^\circ\text{C}$).
* `Aux_Feedback`: Mechanically linked microswitch verification signal.

### `composite_risk_evaluation_log.csv`
* `Composite_Risk_Score_pct`: Instantaneous normalized risk index $\text{CRS}(t) \in [0, 100]\%$.
* `FSM_State`: Active deterministic state (`NORMAL`, `DEGRADED`, `FAILSAFE`, `SHUTDOWN`).
* `Directive_Code`: Automated operator advisory directive (`DIR_SCHEDULE_CELL_BALANCING`, `DIR_EMERGENCY_HV_ISOLATION`).

---

## 3. How These Datasets Are Used

1. **Embedded C Unit Tests:** Ingested in `tests/test_bms_engine.c` and `tests/test_safety_relay.c` to verify deterministic algorithms under boundary conditions.
2. **Automated JavaScript Simulation Suites:** Processed by `task X/tests/simulation_runner.js` to evaluate assertion compliance and stress benchmarks.
3. **Web Dashboards:** Streamed to `dashboard/index.html` via REST APIs to visualize real-time canvas traces and oscilloscope waveforms.
