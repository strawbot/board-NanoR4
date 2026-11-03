// Board/limb_fw_test.c — a controller-side test driver for the limb-bus
// firmware-update mechanism (limb_board/SMA_Limb_Bus_Firmware_Update.md),
// streaming a small SYNTHETIC test image to the bench node
// (board-Nucleo446) over the real wire, via MAINT_FW_BEGIN/CHUNK/END.
//
// Deliberately NOT a real firmware image: just enough bytes with a
// valid limb_app_header_t to exercise Begin's module_type/size checks,
// Chunk's bounds-checked writes, and End's whole-image CRC32 re-check,
// against the real flash hooks (Board/limb_update_flash.c on the node),
// not a host-side mock. A real image would risk the node's boot-check
// stub copying a non-bootable blob into APP_LOWER the next time it
// resets for any unrelated reason -- see this file's own comment at the
// bottom on why this test never triggers that and leaves UPDATE_META
// alone afterward rather than resetting the node.
//
// Three CLI entry points, each its own small when()/after() state
// machine (CLAUDE.md: nothing here may busy-wait):
//   limb-fw-test     the happy path: Begin -> Chunk x N -> End.
//   limb-fw-redpath  a controller "reboot" mid-transfer: Begin, then a
//                     second Begin with no abort/reset message (must
//                     recover cleanly), then confirms the pre-reboot
//                     session's token is now stale/rejected while the
//                     new one still works.
//   limb-fw-timeout  Begin, then go quiet for longer than the node's
//                     idle timeout, then confirm a Chunk with the
//                     (still technically current) token is rejected.
// All three built in response to the question "what if the controller
// reboots and leaves the node hanging mid-transfer, is there a timeout
// or reset/restart command" -- see
// limb_board/SMA_Limb_Bus_Firmware_Update.md and
// Robot/io/limb_bus/limb_bus_update_receiver.h's own notes on the
// session-token/idle-timeout design this exercises.
//
// Compiles in every environment (same reason as limb_uart.c): the body
// is behind LIMB_UART_DEV so this can never be what breaks the normal
// firmware build.

#include "limb_fw_test.h"

#ifdef LIMB_UART_DEV

#include <stdint.h>
#include <stdbool.h>

#include "tea.h"
#include "printers.h"
#include "limb_bus.h"
#include "limb_bus_update.h" // LIMB_APP_HEADER_OFFSET/MAGIC, header shape

// Must match board-Nucleo446/Board/limb_update_flash.c's
// LIMB_MODULE_TYPE_BENCH -- no project-wide module-type enum exists yet
// (see that file's own comment); this is the bench stand-in on both
// ends of the wire.
#define TEST_MODULE_TYPE 0x01U

// Small and arbitrary -- just has to be bigger than the header offset
// and small enough that a handful of 128-byte chunks and one sector
// erase finish quickly on the bench. Nowhere near a real image's size.
#define TEST_IMAGE_SIZE 1024U

static uint8_t test_image[TEST_IMAGE_SIZE];
static uint32_t test_image_crc32;

static uint32_t crc32_raw(uint32_t crc, const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)(-(int32_t)(crc & 1U)));
        }
    }
    return crc;
}

// Deterministic filler + a valid limb_app_header_t at the fixed offset
// -- same shape patch_limb_app_header.py produces, computed here instead
// since this image never exists as a linked .bin to post-process.
static void build_test_image(void) {
    for (uint32_t i = 0; i < TEST_IMAGE_SIZE; i++) {
        test_image[i] = (uint8_t)(i * 31U + 7U);
    }
    limb_app_header_t hdr = { .magic = LIMB_APP_HEADER_MAGIC, .size = TEST_IMAGE_SIZE, .crc32 = 0, .reserved = 0 };
    for (uint32_t i = 0; i < sizeof(hdr); i++) {
        test_image[LIMB_APP_HEADER_OFFSET + i] = ((const uint8_t *)&hdr)[i];
    }
    uint32_t crc = crc32_raw(0xFFFFFFFFU, test_image, TEST_IMAGE_SIZE);
    test_image_crc32 = crc ^ 0xFFFFFFFFU;
    for (uint32_t i = 0; i < 4U; i++) {
        test_image[LIMB_APP_HEADER_OFFSET + 8U + i] = (uint8_t)(test_image_crc32 >> (8U * i));
    }
}

// Shared MAINT_FW_ACK helpers -- see limb_bus.h: status is always
// payload[0]; a *successful BEGIN* ack has four more bytes, the session
// token, at payload[1..4] little-endian.
static bool ack_ok(const limb_bus_maint_frame_t *f) {
    return f->payload_len >= 1U && f->payload[0] == 0x00U;
}
static uint32_t ack_token(const limb_bus_maint_frame_t *f) {
    return (uint32_t)f->payload[1] | ((uint32_t)f->payload[2] << 8)
         | ((uint32_t)f->payload[3] << 16) | ((uint32_t)f->payload[4] << 24);
}

// ── limb-fw-test: the happy path ──────────────────────────────────────
typedef enum { TEST_IDLE, TEST_WAIT_BEGIN, TEST_WAIT_CHUNK, TEST_WAIT_END } test_state_t;
static test_state_t test_state;
static uint32_t test_token;
static uint32_t next_offset;

static void test_timeout(void);
static void test_ack(void);

static void test_fail(const char *why) {
    test_state = TEST_IDLE;
    never(limb_bus_maint_frame_event);
    stop_te(test_timeout);
    print("limb-fw-test: FAIL -- ");
    print(why);
    printCr();
}

static void send_next_chunk(void) {
    uint32_t len = TEST_IMAGE_SIZE - next_offset;
    if (len > LIMB_MAINT_CHUNK_MAX) len = LIMB_MAINT_CHUNK_MAX;
    test_state = TEST_WAIT_CHUNK;
    when(limb_bus_maint_frame_event, test_ack);
    after(msec(200), test_timeout);
    limb_bus_send_fw_chunk(test_token, next_offset, &test_image[next_offset], (uint16_t)len);
}

static void test_timeout(void) {
    test_fail("timeout");
}

static void test_ack(void) {
    const limb_bus_maint_frame_t *f = limb_bus_last_maint_frame();
    if (f->type != LIMB_TYPE_MAINT_FW_ACK) {
        return; // not our reply -- keep waiting
    }
    stop_te(test_timeout);
    never(limb_bus_maint_frame_event);
    if (!ack_ok(f)) {
        test_fail("node acked failure");
        return;
    }

    switch (test_state) {
    case TEST_WAIT_BEGIN:
        if (f->payload_len < 5U) { test_fail("BEGIN ack too short -- no token"); return; }
        test_token = ack_token(f);
        next_offset = 0;
        print("limb-fw-test: BEGIN ok (token=0x"); printHex(test_token); print("), sending chunks...\n");
        send_next_chunk();
        break;

    case TEST_WAIT_CHUNK: {
        uint32_t sent_len = TEST_IMAGE_SIZE - next_offset;
        if (sent_len > LIMB_MAINT_CHUNK_MAX) sent_len = LIMB_MAINT_CHUNK_MAX;
        next_offset += sent_len;
        if (next_offset < TEST_IMAGE_SIZE) {
            send_next_chunk();
        } else {
            print("limb-fw-test: all chunks ok, sending END...\n");
            test_state = TEST_WAIT_END;
            when(limb_bus_maint_frame_event, test_ack);
            after(msec(200), test_timeout);
            limb_bus_send_fw_end(test_token);
        }
        break;
    }

    case TEST_WAIT_END:
        test_state = TEST_IDLE;
        print("limb-fw-test: PASS -- node staged and committed the test image "
              "(size="); printDec0(TEST_IMAGE_SIZE);
        print(" crc32=0x"); printHex(test_image_crc32);
        print("). UPDATE_META is now pending on the node -- left alone on\n"
              "purpose (this image isn't a real bootable app); clear it over\n"
              "SWD before the node's next reset, or leave it until a real\n"
              "update test replaces it.\n");
        break;

    default:
        break;
    }
}

void limb_fw_test_cli(void) {
    if (test_state != TEST_IDLE) {
        print("limb-fw-test: already running\n");
        return;
    }
    build_test_image();
    print("limb-fw-test: sending BEGIN (module_type=1 size=");
    printDec0(TEST_IMAGE_SIZE);
    print(" crc32=0x"); printHex(test_image_crc32);
    print(")...\n");
    test_state = TEST_WAIT_BEGIN;
    when(limb_bus_maint_frame_event, test_ack);
    // Begin's reply is gated on the node's own erase of enough of
    // APP_UPPER to hold the image -- a real (if small, here) flash
    // sector erase, not just a protocol ack, so this gets a longer
    // timeout than the chunk/end steps' pure-software turnaround.
    after(msec(3000), test_timeout);
    limb_bus_send_fw_begin(TEST_MODULE_TYPE, 1U, TEST_IMAGE_SIZE, test_image_crc32);
}

// ── limb-fw-redpath: controller reboot mid-transfer ──────────────────
// Begin -> (simulated reboot: no abort/reset, just a second Begin) ->
// confirm the node accepted the second Begin with a fresh token, then
// confirm the FIRST (now stale) token is rejected while the second
// still works. This is the live-hardware version of the scenario
// exercised on a mock in the host-side integration test -- see
// Robot/io/limb_bus/limb_bus_update_receiver.h's note on why a Begin
// always wins (so a reboot-and-restart needs no special message) and
// what the session token guards against instead (a stale frame from
// the abandoned attempt landing in the new session).
typedef enum { RP_IDLE, RP_WAIT_BEGIN1, RP_WAIT_BEGIN2, RP_WAIT_STALE, RP_WAIT_FRESH } rp_state_t;
static rp_state_t rp_state;
static uint32_t rp_token_a, rp_token_b;

static void rp_timeout(void);
static void rp_ack(void);

static void rp_fail(const char *why) {
    rp_state = RP_IDLE;
    never(limb_bus_maint_frame_event);
    stop_te(rp_timeout);
    print("limb-fw-redpath: FAIL -- ");
    print(why);
    printCr();
}

static void rp_timeout(void) {
    rp_fail("timeout");
}

static void rp_ack(void) {
    const limb_bus_maint_frame_t *f = limb_bus_last_maint_frame();
    if (f->type != LIMB_TYPE_MAINT_FW_ACK) return;
    stop_te(rp_timeout);
    never(limb_bus_maint_frame_event);
    bool ok = ack_ok(f);

    switch (rp_state) {
    case RP_WAIT_BEGIN1:
        if (!ok || f->payload_len < 5U) { rp_fail("first BEGIN refused"); return; }
        rp_token_a = ack_token(f);
        print("limb-fw-redpath: first BEGIN ok, token_a=0x"); printHex(rp_token_a); printCr();
        print("limb-fw-redpath: simulating a controller reboot -- second BEGIN, no abort/reset sent...\n");
        rp_state = RP_WAIT_BEGIN2;
        when(limb_bus_maint_frame_event, rp_ack);
        after(msec(3000), rp_timeout);
        limb_bus_send_fw_begin(TEST_MODULE_TYPE, 1U, TEST_IMAGE_SIZE, test_image_crc32);
        break;

    case RP_WAIT_BEGIN2:
        if (!ok || f->payload_len < 5U) { rp_fail("second BEGIN (post-reboot) was refused -- should recover cleanly"); return; }
        rp_token_b = ack_token(f);
        if (rp_token_b == rp_token_a) { rp_fail("second BEGIN returned the SAME token -- not a fresh session"); return; }
        print("limb-fw-redpath: second BEGIN ok, token_b=0x"); printHex(rp_token_b);
        print(" -- recovered with no abort/reset message needed\n");
        print("limb-fw-redpath: sending a CHUNK with the STALE pre-reboot token (expect rejection)...\n");
        rp_state = RP_WAIT_STALE;
        when(limb_bus_maint_frame_event, rp_ack);
        after(msec(200), rp_timeout);
        limb_bus_send_fw_chunk(rp_token_a, 0, test_image, 16);
        break;

    case RP_WAIT_STALE:
        if (ok) { rp_fail("stale pre-reboot token was WRONGLY accepted"); return; }
        print("limb-fw-redpath: stale token correctly rejected\n");
        print("limb-fw-redpath: sending a CHUNK with the CURRENT token (expect success)...\n");
        rp_state = RP_WAIT_FRESH;
        when(limb_bus_maint_frame_event, rp_ack);
        after(msec(200), rp_timeout);
        limb_bus_send_fw_chunk(rp_token_b, 0, test_image, 16);
        break;

    case RP_WAIT_FRESH:
        rp_state = RP_IDLE;
        if (!ok) { print("limb-fw-redpath: FAIL -- current session's token was wrongly rejected\n"); return; }
        print("limb-fw-redpath: PASS -- reboot-and-restart recovered cleanly, "
              "stale token rejected, current session still works\n");
        break;

    default:
        break;
    }
}

void limb_fw_redpath_cli(void) {
    if (rp_state != RP_IDLE) {
        print("limb-fw-redpath: already running\n");
        return;
    }
    build_test_image();
    print("limb-fw-redpath: sending first BEGIN...\n");
    rp_state = RP_WAIT_BEGIN1;
    when(limb_bus_maint_frame_event, rp_ack);
    after(msec(3000), rp_timeout);
    limb_bus_send_fw_begin(TEST_MODULE_TYPE, 1U, TEST_IMAGE_SIZE, test_image_crc32);
}

// ── limb-fw-timeout: idle session is abandoned ────────────────────────
// Begin, then go quiet for longer than the node's own idle timeout
// (Robot/io/limb_bus/limb_bus_update_receiver.h's
// LIMB_BUS_UPDATE_IDLE_TIMEOUT_MSEC, 2000ms by default) with no Chunk/
// End traffic at all, then confirm a Chunk carrying the still-technically-
// current token is rejected anyway, because the node gave up on the
// session after the idle window passed.
typedef enum { TO_IDLE, TO_WAIT_BEGIN, TO_WAITING, TO_WAIT_CHUNK } to_state_t;
static to_state_t to_state;
static uint32_t to_token;

static void to_timeout(void);
static void to_ack(void);
static void to_wait_done(void);

static void to_fail(const char *why) {
    to_state = TO_IDLE;
    never(limb_bus_maint_frame_event);
    stop_te(to_timeout);
    stop_te(to_wait_done);
    print("limb-fw-timeout: FAIL -- ");
    print(why);
    printCr();
}

static void to_timeout(void) {
    to_fail("timeout waiting for ack");
}

static void to_wait_done(void) {
    print("limb-fw-timeout: wait done -- sending a CHUNK with the original token "
          "(expect rejection: session should have idled out)...\n");
    to_state = TO_WAIT_CHUNK;
    when(limb_bus_maint_frame_event, to_ack);
    after(msec(200), to_timeout);
    limb_bus_send_fw_chunk(to_token, 0, test_image, 16);
}

static void to_ack(void) {
    const limb_bus_maint_frame_t *f = limb_bus_last_maint_frame();
    if (f->type != LIMB_TYPE_MAINT_FW_ACK) return;
    stop_te(to_timeout);
    never(limb_bus_maint_frame_event);
    bool ok = ack_ok(f);

    switch (to_state) {
    case TO_WAIT_BEGIN:
        if (!ok || f->payload_len < 5U) { to_fail("BEGIN refused"); return; }
        to_token = ack_token(f);
        print("limb-fw-timeout: BEGIN ok (token=0x"); printHex(to_token);
        print("), now waiting 2.5s with no further traffic (node's idle timeout is 2s)...\n");
        to_state = TO_WAITING;
        after(msec(2500), to_wait_done);
        break;

    case TO_WAIT_CHUNK:
        to_state = TO_IDLE;
        if (ok) { print("limb-fw-timeout: FAIL -- chunk accepted; session should have idled out\n"); return; }
        print("limb-fw-timeout: PASS -- idle session was correctly abandoned by the node\n");
        break;

    default:
        break;
    }
}

void limb_fw_timeout_cli(void) {
    if (to_state != TO_IDLE) {
        print("limb-fw-timeout: already running\n");
        return;
    }
    build_test_image();
    print("limb-fw-timeout: sending BEGIN...\n");
    to_state = TO_WAIT_BEGIN;
    when(limb_bus_maint_frame_event, to_ack);
    after(msec(3000), to_timeout);
    limb_bus_send_fw_begin(TEST_MODULE_TYPE, 1U, TEST_IMAGE_SIZE, test_image_crc32);
}

#else // !LIMB_UART_DEV

#include "printers.h"

void limb_fw_test_cli(void) {
    print("limb-fw-test: not built in this firmware (needs env:nano_r4_limb)\n");
}
void limb_fw_redpath_cli(void) {
    print("limb-fw-redpath: not built in this firmware (needs env:nano_r4_limb)\n");
}
void limb_fw_timeout_cli(void) {
    print("limb-fw-timeout: not built in this firmware (needs env:nano_r4_limb)\n");
}

#endif // LIMB_UART_DEV
