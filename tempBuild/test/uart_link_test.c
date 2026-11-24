// Run the UART framing and desktop ECC code on a PC without a board.
#include "uart_link.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static unsigned checks = 0;
#define CHECK(test) do { ++checks; if (!(test)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); exit(1); } } while (0)
static UartLinkReceiver receiver;
static uint8_t frame[PROTOCOL_FRAME_BYTES], record[UART_LINK_MAX_RECORD];
static unsigned feed(const uint8_t *bytes, size_t length) {
    unsigned completed = 0;
    for (size_t i = 0; i < length; ++i) if (uartLinkFeed(&receiver, bytes[i])) ++completed;
    return completed;
}
static void roundTrip(uint32_t seq) {
    size_t n = uartLinkEncode(frame, seq, record, sizeof(record));
    CHECK(n > 0 && n <= sizeof(record));
    CHECK(record[n - 1] == 0);
    for (size_t i = 0; i < n - 1; ++i) CHECK(record[i] != 0);
    CHECK(feed(record, n) == 1);
    CHECK(receiver.ready && memcmp(frame, uartLinkFrame(&receiver), sizeof(frame)) == 0);
    uartLinkRelease(&receiver);
}
int main(void) {
    // Start with edge cases, then use repeatable random data to test whole frames.
    uint32_t random = 1;
    uartLinkInit(&receiver);
    memset(frame, 0, sizeof(frame));
    roundTrip(0);
    memset(frame, 255, sizeof(frame));
    roundTrip(1);
    for (unsigned run = 2; run < 2000; ++run) {
        for (size_t i = 0; i < sizeof(frame); ++i) {
            random = random * UINT32_C(1664525) + UINT32_C(1013904223);
            frame[i] = (uint8_t)(random >> 24);
        }
        roundTrip(run);
    }
    CHECK(receiver.stats.records == 2000 && receiver.stats.crc_errors == 0);
    CHECK(receiver.stats.framing_errors == 0 && receiver.stats.sequence_gaps == 0);
    roundTrip(2001);
    CHECK(receiver.stats.sequence_gaps == 1);
    uartLinkInit(&receiver);
    roundTrip(UINT32_MAX); roundTrip(0);
    CHECK(receiver.stats.sequence_gaps == 0);
    CHECK(uartLinkEncode(NULL, 0, record, sizeof(record)) == 0);
    CHECK(uartLinkEncode(frame, 0, record, sizeof(record) - 1) == 0);

    // Oversized or truncated records must recover at a subsequent delimiter.
    uartLinkInit(&receiver);
    for (unsigned i = 0; i < UART_LINK_MAX_ENCODED + 50; ++i) CHECK(!uartLinkFeed(&receiver, 1));
    CHECK(receiver.stats.framing_errors == 1 && receiver.discarding);
    CHECK(!uartLinkFeed(&receiver, 0));
    roundTrip(1);
    size_t n = uartLinkEncode(frame, 2, record, sizeof(record));
    CHECK(feed(record, n / 2) == 0);
    CHECK(!uartLinkFeed(&receiver, 0));
    CHECK(receiver.stats.framing_errors == 2);
    roundTrip(3);
    CHECK(feed(record, 5) == 0);
    uartLinkAbort(&receiver);
    CHECK(receiver.discarding);
    CHECK(!uartLinkFeed(&receiver, 0)); roundTrip(4);

    // A dropped byte or malformed COBS record must not poison the next record.
    uartLinkInit(&receiver); n = uartLinkEncode(frame, 5, record, sizeof(record));
    CHECK(feed(record, 10) == 0);
    CHECK(feed(record + 11, n - 11) == 0);
    CHECK(receiver.stats.framing_errors > 0);
    roundTrip(6);
    uartLinkInit(&receiver);
    CHECK(!uartLinkFeed(&receiver, 255));
    CHECK(!uartLinkFeed(&receiver, 0));
    CHECK(receiver.stats.framing_errors == 1); roundTrip(7);

    // Preserve a decodable but CRC-damaged wire record for downstream ECC.
    bool foundCorruption = false;
    for (size_t pos = 1; pos < n - 1 && !foundCorruption; ++pos) {
        n = uartLinkEncode(frame, 8, record, sizeof(record));
        uint8_t original = record[pos];
        record[pos] = original == 1 ? 2 : 1;
        uartLinkInit(&receiver);
        if (feed(record, n) == 1 && receiver.stats.crc_errors == 1) {
            foundCorruption = true;
            CHECK(receiver.ready && !receiver.have_sequence);
        }
    }
    CHECK(foundCorruption);
    // Ready frame must stay immutable until explicitly released.
    uint8_t saved[PROTOCOL_FRAME_BYTES];
    memcpy(saved, uartLinkFrame(&receiver), sizeof(saved));
    CHECK(uartLinkFeed(&receiver, 42));
    CHECK(memcmp(saved, uartLinkFrame(&receiver), sizeof(saved)) == 0);
    uartLinkRelease(&receiver);
    roundTrip(9);

    // Real desktop ECC/CRC framing survives the new UART wrapper, including a repair.
    uint8_t payload[PROTOCOL_MAX_MESSAGE_BYTES], decoded[PROTOCOL_MAX_MESSAGE_BYTES];
    for (unsigned i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)i;
    protocolInit();
    CHECK(protocolEncode(payload, sizeof(payload), frame));
    frame[9] ^= 0x5A;
    uartLinkInit(&receiver); n = uartLinkEncode(frame, 10, record, sizeof(record));
    CHECK(feed(record, n) == 1);
    uint16_t length = 0; int16_t corrected = 0;
    CHECK(protocolDecode(uartLinkFrame(&receiver), PROTOCOL_FRAME_BYTES, decoded, &length, &corrected));
    CHECK(length == sizeof(payload) && corrected == 1 && memcmp(payload, decoded, sizeof(payload)) == 0);
    printf("PASS: %u UART framing/recovery checks, including real ECC round trip.\n", checks);
    return 0;
}
