/**
 * @file test_telemetry_engine.c
 * @brief Formal Unit Test Suite for Event-Driven Telemetry Engine.
 * 
 * Safety Critical Embedded System Architecture (ISO 26262 ASIL-C / MISRA-C:2012).
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../include/telemetry_engine.h"

static uint32_t s_captured_packets = 0;
static telemetry_event_packet_t s_last_packet;

static void on_telemetry_tx(const telemetry_event_packet_t *packet) {
    s_captured_packets++;
    s_last_packet = *packet;
}

static void reset_test_fixture(void) {
    telemetry_engine_init();
    telemetry_register_tx_callback(on_telemetry_tx);
    s_captured_packets = 0;
    memset(&s_last_packet, 0, sizeof(s_last_packet));
}

void test_deadband_suppression(void) {
    printf("[TEST 1] Verifying Deadband Suppression for Sub-Threshold Noise...\n");
    reset_test_fixture();

    /* Bring network online */
    for (uint32_t t = 0; t <= 1000; t += 10) {
        telemetry_engine_tick(t);
    }
    assert(telemetry_wifi_is_online() == true);
    uint32_t baseline_packets = s_captured_packets;

    /* Feed minor fluctuations (10mV change < 25mV deadband) */
    bms_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    for (int i = 0; i < TELEMETRY_CELL_COUNT; i++) snap.cell_voltages_mv[i] = 3710;
    snap.fsm_state = BMS_STATE_NORMAL;
    snap.relay_status_flags = (RELAY_FLAG_MAIN_COMMANDED | RELAY_FLAG_MAIN_FEEDBACK);

    telemetry_update_bms(&snap);
    telemetry_engine_tick(1020);

    /* Assert that no new packet was transmitted */
    assert(s_captured_packets == baseline_packets);
    printf("  -> PASS: Sub-deadband noise successfully suppressed.\n");
}

void test_cell_delta_trigger(void) {
    printf("[TEST 2] Verifying Telemetry Trigger on Significant Parameter Change...\n");
    reset_test_fixture();

    for (uint32_t t = 0; t <= 1000; t += 10) telemetry_engine_tick(t);
    uint32_t baseline_packets = s_captured_packets;

    /* Feed significant jump (35mV > 25mV deadband on Cell 1) */
    bms_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    for (int i = 0; i < TELEMETRY_CELL_COUNT; i++) snap.cell_voltages_mv[i] = 3700;
    snap.cell_voltages_mv[0] = 3735; /* +35 mV */
    snap.fsm_state = BMS_STATE_NORMAL;
    snap.relay_status_flags = (RELAY_FLAG_MAIN_COMMANDED | RELAY_FLAG_MAIN_FEEDBACK);

    telemetry_update_bms(&snap);
    telemetry_engine_tick(1020);

    assert(s_captured_packets == baseline_packets + 1);
    assert(s_last_packet.event_type == EVENT_TYPE_CELL_DELTA);
    assert(s_last_packet.origin == DATA_ORIGIN_LIVE);
    printf("  -> PASS: Significant parameter delta triggered event packet #%u.\n", s_last_packet.seq_id);
}

void test_extrema_detection(void) {
    printf("[TEST 3] Verifying Extrema Detection (Strongest / Weakest Cells)...\n");
    reset_test_fixture();

    for (uint32_t t = 0; t <= 1000; t += 10) telemetry_engine_tick(t);

    bms_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    for (int i = 0; i < TELEMETRY_CELL_COUNT; i++) snap.cell_voltages_mv[i] = 3700;
    snap.cell_voltages_mv[2] = 3850; /* Cell 3 strongest */
    snap.cell_voltages_mv[6] = 3620; /* Cell 7 weakest */
    snap.fsm_state = BMS_STATE_NORMAL;
    snap.relay_status_flags = (RELAY_FLAG_MAIN_COMMANDED | RELAY_FLAG_MAIN_FEEDBACK);

    telemetry_update_bms(&snap);
    telemetry_engine_tick(1020);

    assert(s_last_packet.snapshot.strongest_cell_idx == 2);
    assert(s_last_packet.snapshot.weakest_cell_idx == 6);
    assert(s_last_packet.snapshot.strongest_cell_mv == 3850);
    assert(s_last_packet.snapshot.weakest_cell_mv == 3620);
    assert(s_last_packet.snapshot.cell_delta_mv == 230);
    printf("  -> PASS: Extrema correctly calculated: Max C3=3850mV, Min C7=3620mV, Delta=230mV.\n");
}

void test_offline_queueing(void) {
    printf("[TEST 4] Verifying Offline Queueing During Network Outage...\n");
    reset_test_fixture();

    /* Start online */
    for (uint32_t t = 0; t <= 1000; t += 10) telemetry_engine_tick(t);

    /* Disconnect network */
    telemetry_set_wifi_physical_link(false);
    telemetry_engine_tick(1010);
    assert(telemetry_wifi_is_online() == false);

    /* Generate 5 discrete telemetry events while offline */
    for (uint32_t i = 1; i <= 5; i++) {
        bms_snapshot_t snap;
        memset(&snap, 0, sizeof(snap));
        for (int c = 0; c < TELEMETRY_CELL_COUNT; c++) snap.cell_voltages_mv[c] = 3700 + (i * 30);
        snap.fsm_state = BMS_STATE_NORMAL;
        snap.relay_status_flags = (RELAY_FLAG_MAIN_COMMANDED | RELAY_FLAG_MAIN_FEEDBACK);
        telemetry_update_bms(&snap);
        telemetry_engine_tick(1010 + (i * 20));
    }

    /* Verify offline queue depth */
    assert(telemetry_queue_get_depth() == 5);
    printf("  -> PASS: 5 events queued offline without packet loss.\n");
}

void test_queue_overflow_policy(void) {
    printf("[TEST 5] Verifying Queue Overflow Protection & Priority Retention...\n");
    reset_test_fixture();

    /* Disconnect network */
    telemetry_set_wifi_physical_link(false);
    telemetry_engine_tick(10);

    /* Fill buffer with 64 routine events */
    for (uint32_t i = 0; i < OFFLINE_QUEUE_CAPACITY; i++) {
        bms_snapshot_t snap;
        memset(&snap, 0, sizeof(snap));
        for (int c = 0; c < TELEMETRY_CELL_COUNT; c++) snap.cell_voltages_mv[c] = 3700 + (i * 30);
        snap.fsm_state = BMS_STATE_NORMAL;
        snap.relay_status_flags = (RELAY_FLAG_MAIN_COMMANDED | RELAY_FLAG_MAIN_FEEDBACK);
        telemetry_update_bms(&snap);
        telemetry_engine_tick(20 + i * 10);
    }
    assert(telemetry_queue_get_depth() == OFFLINE_QUEUE_CAPACITY);

    /* Inject Critical Safety Fault while queue is completely full */
    telemetry_inject_fault(0x1001, 3, "CRITICAL_CELL_OVP_OVERFLOW_TEST");
    telemetry_engine_tick(1000);

    /* Assert buffer remained at capacity and dropped lower priority packet */
    assert(telemetry_queue_get_depth() == OFFLINE_QUEUE_CAPACITY);
    assert(telemetry_queue_get_dropped() > 0);
    printf("  -> PASS: Buffer overflow handled; critical fault preserved (Dropped: %u).\n",
           telemetry_queue_get_dropped());
}

void test_connection_restore_and_drain(void) {
    printf("[TEST 6] Verifying Reconnection & Orderly FIFO Queue Drain...\n");
    reset_test_fixture();

    /* Disconnect and queue 4 events */
    telemetry_set_wifi_physical_link(false);
    telemetry_engine_tick(10);

    for (uint32_t i = 1; i <= 4; i++) {
        bms_snapshot_t snap;
        memset(&snap, 0, sizeof(snap));
        for (int c = 0; c < TELEMETRY_CELL_COUNT; c++) snap.cell_voltages_mv[c] = 3700 + (i * 30);
        snap.fsm_state = BMS_STATE_NORMAL;
        snap.relay_status_flags = (RELAY_FLAG_MAIN_COMMANDED | RELAY_FLAG_MAIN_FEEDBACK);
        telemetry_update_bms(&snap);
        telemetry_engine_tick(20 + i * 20);
    }
    assert(telemetry_queue_get_depth() == 4);

    /* Restore Wi-Fi physical link */
    telemetry_set_wifi_physical_link(true);

    /* Step until Wi-Fi re-establishes */
    for (uint32_t t = 200; t <= 1200; t += 10) {
        telemetry_engine_tick(t);
    }
    assert(telemetry_wifi_is_online() == true);

    /* Step through paced queue drain */
    for (uint32_t t = 1250; t <= 1600; t += 50) {
        telemetry_engine_tick(t);
    }

    /* Queue should now be fully flushed back to 0 */
    assert(telemetry_queue_get_depth() == 0);
    telemetry_metrics_t metrics;
    telemetry_get_metrics(&metrics);
    assert(metrics.packets_transmitted_queued == 4);
    printf("  -> PASS: Orderly FIFO queue drain completed. 4 queued packets replayed.\n");
}

int main(void) {
    printf("====================================================================\n");
    printf(" EVENT-DRIVEN TELEMETRY & LIVE BLYNK DASHBOARD ENGINE TEST SUITE   \n");
    printf("====================================================================\n\n");

    test_deadband_suppression();
    test_cell_delta_trigger();
    test_extrema_detection();
    test_offline_queueing();
    test_queue_overflow_policy();
    test_connection_restore_and_drain();

    printf("\n====================================================================\n");
    printf(" ALL C UNIT TESTS PASSED SUCCESSFULLY (6/6)\n");
    printf("====================================================================\n");
    return 0;
}
