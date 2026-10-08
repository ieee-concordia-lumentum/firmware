/* Run the actual firmware ECC functions on a PC, without any board hardware.
 * Including protocol.c here makes its private ECC helpers available to this
 * test only. The firmware itself and its build settings are unchanged. */
#define SENDER
#include "../src/protocol.c"
#include <stdio.h>

/* The PC test never sends GPIO bits; satisfy the protocol's hardware hook. */
void transmitBit(GpioState bit) { (void)bit; }

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
                    checkRepair(original, length, first, second,
                                (uint8_t)mask1, (uint8_t)mask2);
}

int main(void) {
    const uint8_t payloads[][6] = {
        {0, 0, 0, 0, 0, 0},
        {255, 255, 255, 255, 255, 255},
        {'H', 'e', 'l', 'l', 'o', '!'},
        {0xA0, 0x03, 0x05, 0x55, 0xAA, 0xD3}
    };
    rsInit();
    puts("Testing the firmware ECC (no board needed)...");
    for (unsigned i = 0; i < sizeof(payloads) / sizeof(payloads[0]); i++) {
        printf("Payload %u: testing 7-byte header and 10-byte data block...\n", i + 1);
        fflush(stdout);
        testBlock(payloads[i], 3);
        testBlock(payloads[i], 6);
    }
    printf("%s: %u cases, %u failures\n", failures ? "FAIL" : "PASS", tests, failures);
    puts("This checks ECC repair only, not optical timing or packet reception.");
    puts("Three or more damaged bytes are outside the correction guarantee.");
    return failures ? 1 : 0;
}