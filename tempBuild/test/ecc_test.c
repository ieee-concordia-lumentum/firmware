/* Run the actual desktop packet/ECC functions on a PC, without any board hardware.
 * Including protocol.c here makes its private ECC helpers available to this
 * test only. DESKTOP excludes all MCU timing and GPIO code. */
#define SENDER
#define DESKTOP
#include "../src/protocol.c"
#include <stdio.h>

/* Host builds of the actual MCU timing code use this capture instead of GPIO. */
void Sender_protocolInit(void);
bool Sender_protocolTransmit(const uint8_t *data, uint16_t length);
void Sender_protocolTransmitTick(void);
bool Sender_protocolIsTransmitting(void);
void Receiver_protocolInit(void);
void Receiver_protocolReceiveTick(bool sample);
bool Receiver_protocolIsReady(void);
const uint8_t *Receiver_protocolGetMessageData(void);
void Receiver_protocolResetReceiver(void);

static uint8_t wire[PROTOCOL_FRAME_BYTES * 8U + 25U];
static size_t wireBits = 0;

void transmitBit(GpioState bit) {
    if (wireBits < sizeof(wire)) wire[wireBits++] = bit == High;
}

static unsigned tests = 0;
static unsigned failures = 0;

/* XOR changes selected bits in a byte. Distinct positions mean distinct
 * damaged byte symbols, even when a mask changes several bits at once. */
static void checkRepair(const uint8_t *original, uint8_t length,
                        int first, int second, uint8_t mask1, uint8_t mask2) {
    uint8_t damaged[DATA_CODEWORD_BYTES];
    memcpy(damaged, original, length);
    if (first >= 0) damaged[first] ^= mask1;
    if (second >= 0) damaged[second] ^= mask2;

    int expected = (first >= 0) + (second >= 0);
    int actual = rsDecode(damaged, length);
    tests++;
    /* Check the whole block, including parity: a count alone is not proof. */
    if (actual != expected || memcmp(damaged, original, length) != 0) {
        if (failures < 10)
            printf("FAIL: block=%u positions=%d,%d masks=%02X,%02X expected=%d got=%d\n",
                   (unsigned)length, first, second, mask1, mask2, expected, actual);
        failures++;
    }
}

static void testBlock(const uint8_t *payload, uint8_t payloadLength) {
    uint8_t original[DATA_CODEWORD_BYTES];
    uint8_t length = payloadLength + RS_ECC_SYMBOLS;
    rsEncode(payload, payloadLength, original);
    checkRepair(original, length, -1, -1, 0, 0);

    /* Every byte position, with every possible nonzero corruption mask. */
    for (int position = 0; position < length; position++)
        for (unsigned mask = 1; mask <= 255; mask++)
            checkRepair(original, length, position, -1, (uint8_t)mask, 0);

    /* Every pair of positions and every pair of nonzero masks. This also
     * covers payload/parity combinations and errors with equal magnitudes. */
    for (int first = 0; first < length; first++)
        for (int second = first + 1; second < length; second++)
            for (unsigned mask1 = 1; mask1 <= 255; mask1++)
                for (unsigned mask2 = 1; mask2 <= 255; mask2++)
                    checkRepair(original, length, first, second, mask1, mask2);
}

static void testFrames(void) {
    uint8_t payload[PROTOCOL_MAX_MESSAGE_BYTES];
    uint8_t frame[PROTOCOL_FRAME_BYTES];
    uint8_t message[PROTOCOL_MAX_MESSAGE_BYTES];

    for (unsigned length = 1; length <= sizeof(payload); length++) {
        for (unsigned index = 0; index < length; index++)
            payload[index] = (uint8_t)(index + length);

        if (!protocolEncode(payload, length, frame)) {
            failures++;
            continue;
        }

        for (unsigned errors = 0; errors <= 2; errors++) {
            uint16_t decodedLength = 0;
            int16_t correctedSymbols = 0;
            unsigned packets = (length + 5U) / 6U;

            // Add one new byte error to each codeword on each iteration.
            if (errors != 0) {
                frame[errors == 1 ? 0 : 6] ^= 0xA5;
                for (unsigned packet = 0; packet < packets; packet++)
                    frame[7 + packet * 14 + (errors == 1 ? 2 : 11)] ^= 0x5A;
            }

            tests++;
            Sender_protocolInit();
            Receiver_protocolInit();
            wireBits = 0;
            if (!Sender_protocolTransmit(frame, sizeof(frame)) ||
                Sender_protocolTransmit(frame, sizeof(frame))) {
                failures++;
                continue;
            }
            while (Sender_protocolIsTransmitting()) Sender_protocolTransmitTick();
            Receiver_protocolReceiveTick(false);
            for (size_t bit = 0; bit < wireBits; bit++)
                for (unsigned sample = 0; sample < 4; sample++)
                    Receiver_protocolReceiveTick(wire[bit] != 0);

            if (!Receiver_protocolIsReady() ||
                memcmp(Receiver_protocolGetMessageData(), frame, sizeof(frame)) != 0 ||
                !protocolDecode(Receiver_protocolGetMessageData(), sizeof(frame), message, &decodedLength, &correctedSymbols) ||
                decodedLength != length || memcmp(payload, message, length) != 0 ||
                correctedSymbols != (int)(errors * (packets + 1U))) {
                failures++;
                printf("FAIL: packet length=%u errors=%u\n", length, errors);
            }

            Receiver_protocolResetReceiver();
            Receiver_protocolReceiveTick(false);
            tests++;
            if (Receiver_protocolIsReady()) failures++;
        }
    }

    uint16_t length = 0;
    int16_t corrections = 0;
    tests += 5;
    if (protocolEncode(NULL, 1, frame)) failures++;
    if (protocolEncode(payload, 0, frame)) failures++;
    if (protocolEncode(payload, PROTOCOL_MAX_MESSAGE_BYTES + 1, frame)) failures++;
    if (protocolDecode(frame, sizeof(frame) - 1, message, &length, &corrections)) failures++;
    protocolEncode(payload, 1, frame);
    frame[7] ^= 1; // Packet markers are deliberately outside ECC.
    if (protocolDecode(frame, sizeof(frame), message, &length, &corrections)) failures++;
}

int main(void) {
    const uint8_t payloads[][6] = {
        {0, 0, 0, 0, 0, 0},
        {255, 255, 255, 255, 255, 255},
        {'H', 'e', 'l', 'l', 'o', '!'},
        {0xA0, 0x03, 0x05, 0x55, 0xAA, 0xD3}
    };
    rsInit();
    puts("Testing desktop ECC/packets and the MCU bit functions (no board needed)...");
    for (unsigned i = 0; i < sizeof(payloads) / sizeof(payloads[0]); i++) {
        printf("Payload %u: testing 7-byte header and 10-byte data block...\n", i + 1);
        testBlock(payloads[i], 3);
        testBlock(payloads[i], 6);
    }
    testFrames();
    printf("%s: %u cases, %u failures\n", failures ? "FAIL" : "PASS", tests, failures);
    puts("This checks ECC, packets, and simulated bit reception, not real hardware timing.");
    puts("Three or more damaged bytes are outside the correction guarantee.");
    return failures ? 1 : 0;
}
