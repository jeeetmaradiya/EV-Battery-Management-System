# Engineering Bug Log & Defect Tracking Matrix

**Project:** Modular Battery Management System (BMS) & Industrial Safety Telemetry Architecture  
**Domain Target:** Automotive / Industrial Energy Storage Systems (ISO 26262 ASIL-C / IEC 61508 SIL-2)  
**Tracking Period:** Task 1 through Task 6  
**Status:** All Identified Defects Fully Remediated & Verified (100% Pass Rate)

---

## 1. Overview & Defect Lifecycle Policy

This document provides a comprehensive audit trail of all anomalies, boundary condition failures, and algorithmic edge cases discovered during the design, implementation, and simulation of Tasks 1 through 6. 

Each entry documents:
* **Defect Identifier (BUG-ID)** and associated subsystem/task.
* **Severity Classification**:
  - `CRITICAL`: Immediate safety violation, state machine deadlock, or false contactor trip under nominal conditions.
  - `HIGH`: Measurement inaccuracy, bus saturation, or data loss during transient spikes.
  - `MEDIUM`: Sub-optimal resource utilization, visual artifact (flickering), or telemetry jitter.
  - `LOW`: Formatting discrepancies or minor documentation drifts.
* **Discovery Method**: Unit testing, automated simulation, edge case stress test, or statistical analysis.
* **Root Cause Analysis (RCA)**: Deep technical breakdown of the underlying physical, numerical, or architectural failure.
* **Remediation & Fix**: Concrete algorithmic or structural code modification applied.
* **Verification & Regression**: Evidence of resolution across deterministic test suites and 10,000-tick continuous stress benchmarks.

---

## 2. Comprehensive Defect Tracking Matrix

| Bug ID | Task / Subsystem | Severity | Discovery Method | Failure Summary | Resolution Status |
| :--- | :--- | :---: | :--- | :--- | :---: |
| **BUG-001** | Task 1: BMS Engine | `CRITICAL` | Corner-case dynamic discharge simulation | False cell imbalance trip during high-current acceleration bursts | **RESOLVED & VERIFIED** |
| **BUG-002** | Task 1: BMS Engine | `HIGH` | Extreme SoC knee stress tests (SoC < 10%) | Inappropriate static voltage deviation alarm at non-linear OCV knee | **RESOLVED & VERIFIED** |
| **BUG-003** | Task 2: Safety Relay | `CRITICAL` | Step-load inrush simulation (0A to 40A) | Variance inflation in sliding window triggered false anomaly trip | **RESOLVED & VERIFIED** |
| **BUG-004** | Task 2: Safety Relay | `HIGH` | Rapid boundary fluctuation testing | Contactor contact chatter near over-current threshold | **RESOLVED & VERIFIED** |
| **BUG-005** | Task 3: LCD Engine | `MEDIUM` | Hardware bench & canvas rendering audit | Full-screen strobe flicker and 32ms I2C bus lockup from `lcd.clear()` | **RESOLVED & VERIFIED** |
| **BUG-006** | Task 3: LCD Engine | `HIGH` | Preemptive fault injection under active page rotation | Critical safety alert delayed by active page dwell timer | **RESOLVED & VERIFIED** |
| **BUG-007** | Task 4: Fault State Machine | `CRITICAL` | Sensor wire disconnect & stuck bus simulation | Silent system freeze due to undetected frozen/locked ADC rail | **RESOLVED & VERIFIED** |
| **BUG-008** | Task 4: Fault State Machine | `CRITICAL` | Contactor welding & mechanical relay fault injection | State machine commanded contacts open while hardware remained closed | **RESOLVED & VERIFIED** |
| **BUG-009** | Task 4: Fault State Machine | `HIGH` | Repeated fault clearance stress test | Infinite oscillatory recovery loop between FAILSAFE and NORMAL | **RESOLVED & VERIFIED** |
| **BUG-010** | Task 5: Telemetry Engine | `HIGH` | Extended Wi-Fi outage simulation (>60 seconds) | Telemetry ring buffer overflow dropped critical safety alarms | **RESOLVED & VERIFIED** |
| **BUG-011** | Task 5: Telemetry Engine | `MEDIUM` | Post-reconnection burst transmission | Socket choking and packet loss during offline queue drain | **RESOLVED & VERIFIED** |
| **BUG-012** | Task 6: Blynk Analytics | `HIGH` | Multi-factor risk synthesis under transient current | Single-variable spike caused extreme jump in Composite Risk Score | **RESOLVED & VERIFIED** |

---

## 3. Detailed Root Cause Analysis & Resolutions

### BUG-001: False Cell Imbalance Alarm During High-Current Acceleration
* **Subsystem:** `task 1/src/bms_engine.c`
* **Severity:** `CRITICAL`
* **Symptom:** Under a 40A discharge step, cell delta voltage ($\Delta V$) spiked from $12\text{ mV}$ to $48\text{ mV}$ for 200 ms, triggering a balancing alarm despite healthy cell chemistry.
* **Root Cause Analysis:** Real lithium cells exhibit microscopic differences in internal resistance ($\Delta R_i \approx 0.8\text{ m}\Omega$). Under high current $I$, Ohm's law produces an instantaneous voltage depression difference $\Delta V_{IR} = I \cdot \Delta R_i$. The initial baseline algorithm used a rigid static threshold ($\Delta V_{\text{limit}} = 30\text{ mV}$) that failed to account for dynamic IR drops.
* **Remediation:** Implemented dynamic IR compensation in `bms_engine.c`:
  $$\Delta V_{\text{allowable}} = \Delta V_{\text{base}} + (I_{\text{pack}} \cdot K_{IR})$$
  The dynamic threshold expands proportionately during acceleration transients and contracts during low-current coasting.
* **Verification:** Scenario 3 of `test_bms_engine.c` verified that a 40A transient passes cleanly without triggering false balancing or alarm flags.

---

### BUG-002: False Imbalance Alarms at Electrochemical OCV Knees
* **Subsystem:** `task 1/src/bms_engine.c`
* **Severity:** `HIGH`
* **Symptom:** In the extreme low State of Charge region ($\text{SoC} < 10\%$) or high saturation region ($\text{SoC} > 95\%$), healthy cells naturally diverge due to steep Open Circuit Voltage (OCV) curves, causing nuisance fault trips.
* **Root Cause Analysis:** The voltage vs. SoC curve of LiFePO4 / NMC chemistry is highly non-linear. The flat plateau ($20\%\text{--}80\%$) exhibits low $dV/d(\text{SoC})$, but the knees exhibit extreme steepness. A fixed threshold is overly sensitive at the knees.
* **Remediation:** Derived a continuous quadratic knee expansion model:
  $$\Delta V_{\text{knee}} = K_{\text{knee}} \cdot (1.0 - \text{SoC})^2$$
  This dynamically increases tolerance as SoC drops towards the knee while maintaining rigorous tight bounds ($\le 25\text{ mV}$) along the plateau.
* **Verification:** Scenario 1 and 2 pass with zero nuisance alarms across the entire 0% to 100% SoC discharge envelope.

---

### BUG-003: Variance Inflation in Sliding Window Triggering False Safety Trip
* **Subsystem:** `task 2/src/safety_relay_engine.c`
* **Severity:** `CRITICAL`
* **Symptom:** During legitimate load step additions (e.g., motor inrush of 35A), the rolling variance metric inflated dramatically, erroneously flagging the load step as sensor noise/anomaly and tripping the safety relay.
* **Root Cause Analysis:** When a step transition from 10A to 35A occurs, the circular FIFO buffer contains a mix of old low samples and new high samples. Calculating variance $\sigma^2$ across this transitional window inherently produces a massive mathematical variance spike, even though the current reading is completely legitimate.
* **Remediation:** Decoupled the anomaly noise detector from rolling variance during detected step transitions. Established a dual-criterion discriminator: if $|dI/dt|$ matches an expected physical step profile and subsequent samples remain stable, the noise variance threshold is clamped to nominal baseline ($4\sigma_{\text{nominal}}$).
* **Verification:** Verified in Scenario 4 of `simulation_runner.js`; 35A step addition processed with 100% continuity and zero contactor false trips.

---

### BUG-004: Contact Chatter Near Over-Current Boundary
* **Subsystem:** `task 2/src/safety_relay_engine.c`
* **Severity:** `HIGH`
* **Symptom:** When current hovered between 49.5A and 50.5A, the relay contacts opened and closed repeatedly at high frequency, risking electrical arc damage and relay contact welding.
* **Root Cause Analysis:** The trip logic initially relied on a single scalar threshold with inadequate hysteresis deadband and zero debounce integration.
* **Remediation:** Implemented dual-threshold hysteresis:
  - Trip Threshold: $I_{\text{trip}} = 50.0\text{ A}$
  - Reset Threshold: $I_{\text{reset}} = 42.0\text{ A}$ ($\Delta H = 8.0\text{ A}$)
  Paired with non-blocking debounce timers: $120\text{ ms}$ trip persistence qualification and $300\text{ ms}$ recovery qualification.
* **Verification:** 10,000-tick continuous noise-boundary simulation confirmed zero chatter cycles.

---

### BUG-005: Full-Screen LCD Strobe Flicker & I2C Bus Saturation
* **Subsystem:** `task 3/src/flicker_free_lcd_engine.c`
* **Severity:** `MEDIUM`
* **Symptom:** Periodic flashing of the 20x4 alphanumeric display every 500 ms, accompanied by high I2C bus latency blocking other sensor acquisitions for ~32 ms.
* **Root Cause Analysis:** Traditional rendering routines invoked `lcd.clear()` before redrawing characters. `lcd.clear()` turns off all DDRAM pixels for $1.52\text{ ms}$ to $2.0\text{ ms}$ (visible flash) and forces an 80-byte rewrite across the 100 kHz I2C bus.
* **Remediation:** Engineered differential shadow-buffer rendering:
  - Maintained `front_buffer` (mirroring physical display) and `back_buffer` (staging target frame).
  - Executed differential character comparison; clustered modified glyphs into run-length spans.
  - Banned `lcd.clear()`; padded blank areas with space characters (`0x20`).
  - Slashed I2C bus traffic by **$94.44\%$** and reduced per-tick execution time to $< 50\,\mu\text{s}$.
* **Verification:** Optical simulation and test harness confirmed zero luminance drops and verified differential updates across 10,000 continuous frames.

---

### BUG-006: Critical Preemptive Fault Alert Dwell Delay
* **Subsystem:** `task 3/src/flicker_free_lcd_engine.c`
* **Severity:** `HIGH`
* **Symptom:** When an over-temperature safety fault occurred, the LCD display continued showing the non-critical Telemetry Page for up to 3 seconds until the standard page rotation timer expired.
* **Root Cause Analysis:** The page scheduler evaluated the fault flag only inside the periodic page-switching branch of the cooperative tick function.
* **Remediation:** Added an instantaneous high-priority preemptive check at the top of the tick loop. If `fault_active` is asserted, normal rotation is bypassed immediately ($0\text{ ms}$ latency), the display locks onto the Safety Alarm screen, and a $1.0\text{ Hz}$ header blinker activates.
* **Verification:** Scenario 4 of `test_flicker_free_lcd.c` proved immediate frame transition on the exact tick of fault injection.

---

### BUG-007: Silent System Freeze Due to Locked / Stuck ADC Rail
* **Subsystem:** `task 4/src/fault_state_machine_engine.c`
* **Severity:** `CRITICAL`
* **Symptom:** When a current sensor ADC crashed and output a fixed static voltage (2.500V), the controller assumed current was completely constant and failed to detect an over-current event on another channel.
* **Root Cause Analysis:** Baseline firmware only checked for minimum/maximum boundary crossings ($V_{\text{min}} < V < V_{\text{max}}$). It lacked temporal invariance detection.
* **Remediation:** Implemented an invariant sample depth detector using a per-channel ring buffer:
  $$\Delta_{\text{span}} = \max(V_{k}) - \min(V_{k}) \quad \text{for } k \in [1 \dots N]$$
  If $\Delta_{\text{span}} < \epsilon$ ($0.015\text{ A}$) over 16 consecutive samples while the system is under load, the ADC channel is flagged as `FAULT_SOURCE_ADC_FROZEN` and the system transitions to `FAILSAFE`.
* **Verification:** Scenario 2 of `test_fault_state_machine.c` reliably detects frozen rails and isolates the affected channel within $160\text{ ms}$.

---

### BUG-008: Undetected Welded Mechanical Relay Contacts
* **Subsystem:** `task 4/src/fault_state_machine_engine.c`
* **Severity:** `CRITICAL`
* **Symptom:** Under contact degradation, the contactor command was set to OPEN (`0`), but the physical relay contacts remained electrically welded shut (`1`), maintaining live battery pack voltage to the output bus.
* **Root Cause Analysis:** The software assumed command execution was synonymous with physical contact state, lacking feedback loop reconciliation.
* **Remediation:** Integrated auxiliary contact feedback monitoring (`aux_feedback`). Implemented debounce-qualified discrepancy detection:
  If `command != aux_feedback` for $> 50\text{ ms}$, the system immediately declares `FAULT_SOURCE_RELAY_WELDED` and transitions to irreversible `SHUTDOWN`.
* **Verification:** Scenario 3 of `test_fault_state_machine.c` triggers welded contact detection and engages emergency contactor isolation within 5 ticks.

---

### BUG-009: Infinite Recovery Oscillation Between FAILSAFE and NORMAL
* **Subsystem:** `task 4/src/fault_state_machine_engine.c`
* **Severity:** `HIGH`
* **Symptom:** An intermittent wiring loose contact caused the system to cycle between `FAILSAFE` and `NORMAL` indefinitely, repeatedly energizing and de-energizing high-voltage contactors.
* **Root Cause Analysis:** The recovery mechanism lacked persistence memory and automatic escalation upon repeated failures.
* **Remediation:** Engineered a 5-stage structured recovery pipeline with a strict retry limit:
  1. Fault Clearance Verification
  2. Stabilization Quarantine ($3,000\text{ ms}$)
  3. Subsystem Self-Test
  4. Provisional Degraded Dwell ($2,000\text{ ms}$)
  5. Restoration to Normal
  If $\ge 3$ consecutive recovery attempts fail within a 60-second window, the system escalates directly to permanent locked `SHUTDOWN`.
* **Verification:** Scenario 6 and 7 verify deadlock-free operation and guaranteed latching into `SHUTDOWN` upon threshold exhaustion.

---

### BUG-010: Ring Buffer Saturation Dropping Critical Alarms During Wi-Fi Outage
* **Subsystem:** `task 5/src/telemetry_engine.c`
* **Severity:** `HIGH`
* **Symptom:** During a simulated 90-second wireless drop, high-frequency periodic telemetry filled the circular queue, causing an over-temperature trip packet to be discarded when the buffer wrapped around.
* **Root Cause Analysis:** Standard FIFO overwrite dropped the newest incoming packet when full, treating all telemetry packets with equal priority.
* **Remediation:** Implemented priority-aware queue retention in `telemetry_engine.c`:
  Packets are tagged with `PRIORITY_CRITICAL`, `PRIORITY_WARNING`, or `PRIORITY_NORMAL`. When the queue reaches capacity ($N = 64$), normal periodic telemetry packets are evicted first to ensure safety alarms and contactor trip events are guaranteed 100% persistence.
* **Verification:** Scenario 5 of `test_telemetry_engine.c` demonstrated 100% preservation of critical emergency packets across a 120-second connection loss.

---

### BUG-011: Socket Congestion During Post-Outage Backlog Flush
* **Subsystem:** `task 5/src/telemetry_engine.c`
* **Severity:** `MEDIUM`
* **Symptom:** Upon Wi-Fi link restoration, the engine flushed all 64 stored packets in a single tick, overloading the network stack and causing TCP buffer drops.
* **Root Cause Analysis:** Lack of egress rate limiting during queue drain.
* **Remediation:** Added a paced drain scheduler: upon link recovery, the engine transmits at most 1 queued packet every $50\text{ ms}$ while interleaving real-time live events. Packets are tagged with `ORIGIN_QUEUED` so the cloud dashboard distinguishes historical backfill from live telemetry.
* **Verification:** Scenario 6 confirms stable FIFO queue drainage without dropped packets or socket timeouts.

---

### BUG-012: Extreme Volatility in Composite Risk Score from Single-Sample Spikes
* **Subsystem:** `task 6/src/blynk_analytics_engine.c`
* **Severity:** `HIGH`
* **Symptom:** A single EMI noise glitch on cell 4 voltage caused the Composite Risk Score (CRS) to instantly jump from 12% to 78%, generating momentary operator panic warnings.
* **Root Cause Analysis:** The raw instantaneous imbalance acceleration ($\frac{d\Delta V}{dt}$) was multiplied directly into the composite risk equation without low-pass filtering.
* **Remediation:** Applied an Exponential Moving Average (EMA) low-pass filter ($\alpha = 0.25$) to both $\Delta V$ and its numerical derivative before feeding the weighted risk summation:
  $$\text{CRS}(t) = \min\left(100, \; \sum_{i=1}^{5} w_i \cdot R_i(t)\right)$$
* **Verification:** Scenario 4 of `simulation_runner.js` verified that isolated 1-sample EMI spikes are rejected, while genuine progressive cell degradation produces smooth, deterministic risk elevation.

---

## 4. Defect Closure & Sign-Off

All 12 defects have undergone complete root cause remediation, unit test verification, and automated regression testing. Zero open defects remain across the codebase.
