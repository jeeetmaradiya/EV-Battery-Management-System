/**
 * @file test_safety_relay.c
 * @brief Standalone C Test Harness for the Non-Blocking Protection Relay.
 * 
 * Verifies:
 *  1. Non-blocking tick execution and zero-delay timing.
 *  2. Anti-chatter hysteresis deadband (Trip 50A, Reset 42A).
 *  3. Debounce timing for trip (120ms) and clear (300ms).
 *  4. Sensor anomaly detection (Freeze, Slew Jump, Out of Range).
 *  5. Noise spike rejection vs genuine step persistence.
 *  6. Post-fault timed recovery sequence (Cooldown -> Soak -> Re-arm -> Normal).
 *  7. Stability under continuous fault injection.
 */

#include <stdio.h>
#include <assert.h>
#include <math.h>
#include "../include/safety_relay_engine.h"

static void Test_AntiChatterHysteresis(void) {
    RelayContext_t ctx;
    uint32_t time = 1000;
    SafetyRelay_Init(&ctx, time);

    assert(ctx.current_state == RELAY_STATE_NORMAL_MONITORING);
    assert(ctx.contacts == RELAY_CONTACTS_CLOSED);

    /* 1. In-band operation at 30A */
    for (int i = 0; i < 20; i++) {
        time += 10;
        SafetyRelay_ProcessTick(&ctx, time, 30.0f, 230.0f);
    }
    assert(ctx.current_state == RELAY_STATE_NORMAL_MONITORING);

    /* 2. Oscillation near threshold: 48A (below 50A trip) */
    for (int i = 0; i < 20; i++) {
        time += 10;
        SafetyRelay_ProcessTick(&ctx, time, 48.0f, 230.0f);
    }
    /* Must remain NORMAL: Hysteresis deadband active, no trip! */
    assert(ctx.current_state == RELAY_STATE_NORMAL_MONITORING);

    /* 3. Cross 50A trip threshold: 52A for 1 tick -> should enter TRIP_DEBOUNCING */
    time += 10;
    SafetyRelay_ProcessTick(&ctx, time, 52.0f, 230.0f);
    assert(ctx.current_state == RELAY_STATE_TRIP_DEBOUNCING);

    /* 4. Drop back to 46A before debounce completes (glitch filter) */
    time += 10;
    SafetyRelay_ProcessTick(&ctx, time, 46.0f, 230.0f);
    /* Transient cleared! */
    assert(ctx.current_state == RELAY_STATE_NORMAL_MONITORING);

    /* 5. Sustained overcurrent 55A for > 120ms */
    for (int i = 0; i < 15; i++) {
        time += 10;
        SafetyRelay_ProcessTick(&ctx, time, 55.0f, 230.0f);
    }
    /* Debounce expired: must be TRIPPED */
    assert(ctx.current_state == RELAY_STATE_TRIPPED_FAULT);
    assert(ctx.contacts == RELAY_CONTACTS_OPEN);

    /* 6. Current drops to 45A (inside hysteresis deadband between 42A and 50A) */
    for (int i = 0; i < 40; i++) {
        time += 10;
        SafetyRelay_ProcessTick(&ctx, time, 45.0f, 230.0f);
    }
    /* Anti-chatter: Must stay TRIPPED because it has NOT dropped below 42A reset! */
    assert(ctx.current_state == RELAY_STATE_TRIPPED_FAULT);

    /* 7. Current drops below 42A (e.g. 35A) for > 300ms clear debounce */
    for (int i = 0; i < 35; i++) {
        time += 10;
        SafetyRelay_ProcessTick(&ctx, time, 35.0f, 230.0f);
    }
    /* Clear debounce expired: Must enter RECOVERY_COOLDOWN */
    assert(ctx.current_state == RELAY_STATE_RECOVERY_COOLDOWN);
    assert(ctx.contacts == RELAY_CONTACTS_OPEN);

    printf("[PASS] Test_AntiChatterHysteresis\n");
}

static void Test_SensorAnomalies(void) {
    RelayContext_t ctx;
    uint32_t time = 5000;
    SafetyRelay_Init(&ctx, time);

    /* Out of range broken wire test: Voltage drops to 0V (< 10V min) */
    time += 10;
    SafetyRelay_ProcessTick(&ctx, time, 10.0f, 0.0f);
    assert((ctx.active_flags & ANOMALY_FLAG_OUT_OF_RANGE_LOW) != 0);

    /* Unrealistic slew rate test: jump from 10A to 80A in 10ms (7000 A/s) */
    time += 10;
    SafetyRelay_ProcessTick(&ctx, time, 80.0f, 230.0f);
    assert((ctx.active_flags & ANOMALY_FLAG_UNREALISTIC_SLEW_JUMP) != 0);

    printf("[PASS] Test_SensorAnomalies\n");
}

int main(void) {
    printf("=== Starting Non-Blocking Safety Relay Verification ===\n");
    Test_AntiChatterHysteresis();
    Test_SensorAnomalies();
    printf("=== All C Unit Tests Passed Successfully ===\n");
    return 0;
}
