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
// State machine, not a blocking loop (CLAUDE.md: nothing here may
// busy-wait): Begin -> wait for ack -> Chunk x N (one at a time, per
// protocol §8.4's "body holds at most one chunk in flight") -> wait for
// ack each -> End -> wait for ack -> report. Same when()/after() timeout
// shape as limb_bus_cli_ping(), just with more than one step.
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

typedef enum { TEST_IDLE, TEST_WAIT_BEGIN, TEST_WAIT_CHUNK, TEST_WAIT_END } test_state_t;
static test_state_t test_state;
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
    limb_bus_send_fw_chunk(next_offset, &test_image[next_offset], (uint16_t)len);
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
    if (f->payload_len < 1U || f->payload[0] != 0x00U) {
        test_fail("node acked failure");
        return;
    }

    switch (test_state) {
    case TEST_WAIT_BEGIN:
        next_offset = 0;
        print("limb-fw-test: BEGIN ok, sending chunks...\n");
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
            limb_bus_send_fw_end();
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

#else // !LIMB_UART_DEV

#include "printers.h"

void limb_fw_test_cli(void) {
    print("limb-fw-test: not built in this firmware (needs env:nano_r4_limb)\n");
}

#endif // LIMB_UART_DEV
