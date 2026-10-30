# Demonstration Video Recording Guide & Checklist

**Internship Program:** Elevance Skills  
**Target Duration:** 2 to 5 minutes (Mandatory limit)  
**File Size Limit:** Maximum 100 MB  
**Accepted Formats:** MP4, WebM, or MOV  
**Location:** Store locally in `media/video/` and upload via the official internship submission form or host as a public unlisted link (e.g., YouTube Unlisted or Google Drive Public Link).

---

## 1. Video Requirements Summary

Per the Elevance Skills evaluation criteria:
* **Live Running System:** Show the project actually running (live dashboards, running simulation suites, and active terminal outputs), not merely static source code files.
* **Audio & Screen Clarity:** Speak clearly with a good microphone. Ensure the screen resolution is crisp so text and numbers are readable.
* **Timing:** Strictly between **2:00 and 5:00 minutes**. Videos under 2 minutes or exceeding 5 minutes may incur evaluation penalties.
* **Review Before Submitting:** Watch the video once in full after recording to verify screen clarity and audible voiceover.

---

## 2. Recommended 3-to-4 Minute Presentation Script

| Section | Target Duration | What to Show on Screen | What to Say (Speaking Script Outline) |
| :--- | :---: | :--- | :--- |
| **1. Introduction** | ~30 seconds | Title slide or root `README.md` | *"Hello mentors, my name is [Your Name], and this is my final internship submission for the Embedded Systems and IoT track at Elevance Skills. My project delivers a 6-stage Modular Battery Management System (BMS) with functional safety, anti-chatter protection, flicker-free telemetry, deterministic fault recovery, and an enterprise cloud analytics dashboard."* |
| **2. Architecture & Tasks 1–3** | ~60 seconds | Browser with Task 1 BMS Dashboard & Task 2 Relay Oscilloscope | *"In Task 1, we implemented a compile-time scalable BMS engine with dynamic IR compensation and adaptive SoC knee models to eliminate false imbalance alarms. In Task 2, we engineered a non-blocking safety relay with dual-threshold hysteresis ($50\text{A}/42\text{A}$) and rolling median filtering to eliminate contact chatter. In Task 3, our differential shadow-buffer LCD engine eliminates full-screen blackouts, slashing I2C bus traffic by $94.44\%$."* |
| **3. Tasks 4 & 5 Demonstration** | ~60 seconds | Terminal running `simulation_runner.js` & Task 5 Blynk console | *"Task 4 features a deterministic 4-state machine (`NORMAL`, `DEGRADED`, `FAILSAFE`, `SHUTDOWN`) with frozen ADC rail detection, welded contactor sensing, and a 5-stage verified recovery pipeline. Here you can see Task 5's event-driven telemetry engine: it filters out sub-deadband noise and preserves critical trip packets in an offline ring buffer during simulated Wi-Fi dropouts."* |
| **4. Task 6 Enterprise Analytics & Live Run** | ~60 seconds | Task 6 Enterprise Blynk Dashboard (`dashboard/index.html`) | *"Finally, in Task 6, we synthesize all telemetry into a real-time Composite Risk Score (CRS) based on 5 mathematical risk vectors. Here on the live dashboard, you can see real-time canvas graphs, battery State of Health, and intelligent operator directives. Notice how injecting an over-temperature fault immediately elevates risk and commands contactor isolation."* |
| **5. Verification & Conclusion** | ~30 seconds | Terminal showing 100% test pass rate across test suites | *"All 6 tasks have been empirically verified across deterministic unit test suites and 10,000-tick continuous stress benchmarks with a 100% pass rate. All code adheres to MISRA-C guidelines with zero dynamic heap allocation. Thank you to the Elevance Skills mentors for their guidance throughout this internship."* |

---

## 3. Recommended Tools for Recording

* **OBS Studio (Free & Open Source):** Excellent for recording screen and microphone simultaneously.
* **Windows Game Bar (`Win + G`):** Quick built-in Windows screen recorder.
* **Handbrake (Free):** If your recorded video file is larger than 100 MB, open Handbrake, select the "Web 1080p30" or "Web 720p30" preset, and re-encode to compress below 50 MB with zero visual loss.

---

## 4. Pre-Upload Sanity Checklist

- [ ] Duration is between 2:00 and 5:00 minutes.
- [ ] File size is $< 100\text{ MB}$.
- [ ] Audio is loud, clear, and free from excessive background noise.
- [ ] Screen resolution is sharp (at least 720p or 1080p).
- [ ] Dashboards and simulations are demonstrated running live.
- [ ] Video playback tested before final submission.
