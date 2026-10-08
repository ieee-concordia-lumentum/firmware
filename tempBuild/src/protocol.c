#include "protocol.h"
#include "hardware.h"
#include <string.h>

#if defined(SENDER) && defined(RECEIVER)
#error "Define only one device role"
#elif !defined(SENDER) && !defined(RECEIVER)
#error "Define SENDER or RECEIVER"
#endif

/* How a message travels over the link (highest bit of each byte first):
 * First send 16 alternating 1/0 bits so the receiver can synchronize to bit
 * timing.  Next send D3 to confirm the start, then a 7-byte header and data
 * packets.
 * Header: 4-bit A marker | 16-bit message length | 4-bit 5 marker, followed by
 * 4 extra bytes for error correction.
 * Each data packet is: AA D3 | 6 message bytes | 4 error correction bytes | 55
 * 2C.
 * Reed-Solomon uses those extra bytes to repair up to 2 damaged bytes in each
 * header or data block. The packet markers have no error protection.  A
 * codeword means the original bytes together with their correction bytes.
 * The timer interrupt handles individual bits; the main loop does the slower
 * error correction work so it does not delay receiver sampling.
 */
#define PREAMBLE 0xAAAAU
#define SYNCWORD 0xD3U
#define START_PATTERN 0xAU
#define END_PATTERN 0x5U
#define HEADER_BYTES 7U
#define RS_ECC_SYMBOLS 4U
#define DATA_CODEWORD_BYTES 10U
#define GF_PRIMITIVE_POLYNOMIAL 0x011DU
#define MAX_PACKETS ((PROTOCOL_MAX_MESSAGE_BYTES + 5U) / 6U)
#define MAX_WIRE_BYTES (MAX_PACKETS * PROTOCOL_PACKET_BYTES)
static uint8_t gfExp[512];
static uint8_t gfLog[256];
static uint8_t rsGenerator[RS_ECC_SYMBOLS + 1];


// Error correction works over GF(256) fields.
// Multiplication uses lookup tables for faster computation: add the two
// log table entries, then look up the result in the exponent table. Handle
// zero separately.
static uint8_t gfMultiply(uint8_t a, uint8_t b){
    if ((a == 0) || (b == 0)){
        return 0;
    }

    return gfExp[(uint16_t)gfLog[a] + gfLog[b]];
}


// Find the value that multiplies with this byte to give 1 in GF(256).
// Multiplying by this inverse performs division in the correction formulas.
// Zero has no inverse; return 0 as a fallback.
static uint8_t gfInverse(uint8_t value){
    if (value == 0){
        return 0;
    }

    return gfExp[255 - gfLog[value]];
}


// Prepare the lookup tables used by both encoding and decoding.
// 0x11D defines how values wrap back into one byte in this arithmetic.
// Then build the generator: the shared polynomial used to calculate the
// four correction bytes. Its roots are the first four exponent table values.
static void rsInit(void){
    uint16_t value = 1;
    uint8_t degree;

    memset(gfLog, 0, sizeof(gfLog));

    for (uint16_t index = 0; index < 255; index++){
        gfExp[index] = (uint8_t)value;
        gfLog[(uint8_t)value] = (uint8_t)index;

        value <<= 1;
        if ((value & UINT16_C(0x0100)) != 0){
            value ^= GF_PRIMITIVE_POLYNOMIAL;
        }
    }

    for (uint16_t index = 255; index < 512; index++){
        gfExp[index] = gfExp[index - 255];
    }

    memset(rsGenerator, 0, sizeof(rsGenerator));
    rsGenerator[0] = 1;

    for (degree = 0; degree < RS_ECC_SYMBOLS; degree++){
        uint8_t root = gfExp[degree];

        rsGenerator[degree + 1] = 0;
        for (int8_t index = (int8_t)degree; index >= 0; index--){
            rsGenerator[index + 1] ^=
                gfMultiply(rsGenerator[index], root);
        }
    }
}


// Treat the byte array as polynomial coefficients, highest power first.
// Evaluate it by repeatedly multiplying the running result and adding the
// next byte. In GF(256), addition and subtraction both use XOR.
static uint8_t polynomialEvaluate(const uint8_t *polynomial, uint8_t length, uint8_t value){
    uint8_t result = polynomial[0];

    for (uint8_t index = 1; index < length; index++){
        result = gfMultiply(result, value) ^ polynomial[index];
    }

    return result;
}


// Keep the original bytes and calculate four extra error correction bytes.
// Divide a temporary copy, with four zeros appended, by the generator.
// The last four remaining bytes are the correction bytes to append.
// A 3-byte header becomes 7 bytes; a 6-byte message block becomes 10 bytes.
static void rsEncode(const uint8_t *header, uint8_t dataLength, uint8_t *codeword){
    uint8_t remainder[DATA_CODEWORD_BYTES];

    memcpy(remainder, header, dataLength);
    memset(&remainder[dataLength], 0, RS_ECC_SYMBOLS);

    for (uint8_t index = 0; index < dataLength; index++){
        uint8_t coefficient = remainder[index];

        if (coefficient != 0)
            for (uint8_t generatorIndex = 1; generatorIndex <= RS_ECC_SYMBOLS; generatorIndex++)
                remainder[index + generatorIndex] ^= gfMultiply(coefficient, rsGenerator[generatorIndex]);
    }

    memcpy(codeword, header, dataLength);
    memcpy(&codeword[dataLength], &remainder[dataLength], RS_ECC_SYMBOLS);
}


// Check a received block and try to repair up to two damaged bytes in place.
// Returns 0 if all checks already pass, or 1/2 for the repaired byte count,
// and -1 if no valid repair is found. More than two damaged bytes can lead
// to an incorrect repair or pass unnoticed; this is not a checksum guarantee.
static int8_t rsDecode(uint8_t *codeword, uint8_t codewordLength){
    uint8_t syndrome[4];

    // These four results (syndromes) are error checks: a valid block gives
    // zero at every generator root. Nonzero results guide the repair.
    for (uint8_t index = 0; index < sizeof(syndrome); index++)
        syndrome[index] = polynomialEvaluate(codeword, codewordLength, gfExp[index]);

    if ((syndrome[0] == 0) && (syndrome[1] == 0) &&
        (syndrome[2] == 0) && (syndrome[3] == 0)){
        return 0;
    }

    // First try a one-byte repair. S0 gives the bits to flip in that byte;
    // S1 divided by S0 gives its position in polynomial form.
    // Check S2 and S3 agree before trying the repair and checking it again.
    if (syndrome[0] != 0){
        uint8_t location = gfMultiply(syndrome[1], gfInverse(syndrome[0]));

        if (location != 0){
            uint8_t locationSquared = gfMultiply(location, location);
            uint8_t locationCubed = gfMultiply(locationSquared, location);

            if ((gfMultiply(syndrome[0], locationSquared) == syndrome[2]) &&
                (gfMultiply(syndrome[0], locationCubed) == syndrome[3])){
                uint8_t degree = gfLog[location];

                if (degree < codewordLength){
                    uint8_t position =
                        (uint8_t)(codewordLength - 1 - degree);

                    codeword[position] ^= syndrome[0];

                    for (uint8_t index = 0; index < RS_ECC_SYMBOLS; index++){
                        if (polynomialEvaluate(codeword, codewordLength, gfExp[index]) != 0){
                            codeword[position] ^= syndrome[0];
                            break;
                        }

                        if (index == (RS_ECC_SYMBOLS - 1))
                            return 1;
                    }
                }
            }
        }
    }

    {
        // If one-byte repair failed, use all four checks to locate two errors.
        // sigma1 and sigma2 describe a polynomial that identifies the positions.
        // If the formula denominator is zero, these equations cannot locate them.
        uint8_t determinant =
            gfMultiply(syndrome[1], syndrome[1]) ^
            gfMultiply(syndrome[0], syndrome[2]);
        uint8_t sigma1, sigma2;
        uint8_t position1 = 0, position2 = 0;
        uint8_t location1 = 0, location2 = 0;
        uint8_t found = 0;

        if (determinant == 0){
            return -1;
        }

        sigma1 = gfMultiply(
            gfMultiply(syndrome[2], syndrome[1]) ^
                gfMultiply(syndrome[3], syndrome[0]),
            gfInverse(determinant));
        sigma2 = gfMultiply(
            gfMultiply(syndrome[1], syndrome[3]) ^
                gfMultiply(syndrome[2], syndrome[2]),
            gfInverse(determinant));

        // Try each byte position. A zero polynomial result marks an error.
        // Degree 0 refers to the last byte, so convert it back to an array index.
        for (uint8_t degree = 0; degree < codewordLength; degree++){
            uint8_t location = gfExp[degree];
            uint8_t inverseLocation = gfInverse(location);
            uint8_t locatorValue =
                1 ^ gfMultiply(sigma1, inverseLocation) ^
                gfMultiply(sigma2,
                           gfMultiply(inverseLocation, inverseLocation));

            if (locatorValue == 0){
                if (found == 0){
                    position1 = (uint8_t)(codewordLength - 1 - degree);
                    location1 = location;
                }
                else if (found == 1){
                    position2 = (uint8_t)(codewordLength - 1 - degree);
                    location2 = location;
                }

                found++;
            }
        }

        if ((found != 2) || (location1 == location2)){
            return -1;
        }

        {
            // Use the two positions and the first two checks to find which
            // bits to flip in each byte. XOR applies these changes.
            // Repeat all four checks; undo the changes if any check still fails.
            uint8_t denominator = location2 ^ location1;
            uint8_t error1 = gfMultiply(
                gfMultiply(syndrome[0], location2) ^ syndrome[1],
                gfInverse(denominator));
            uint8_t error2 = gfMultiply(
                syndrome[1] ^ gfMultiply(syndrome[0], location1),
                gfInverse(denominator));

            codeword[position1] ^= error1;
            codeword[position2] ^= error2;

            for (uint8_t index = 0; index < RS_ECC_SYMBOLS; index++){
                if (polynomialEvaluate(codeword,
                                       codewordLength,
                                       gfExp[index]) != 0){
                    codeword[position1] ^= error1;
                    codeword[position2] ^= error2;
                    return -1;
                }
            }
        }
    }

    return 2;
}


#ifdef SENDER
typedef enum{
    TX_IDLE,
    TX_PREAMBLE,
    TX_SYNC,
    TX_HEADER,
    TX_DATA,
    TX_FINISH
} TxState;
// The main loop builds the bytes before setting inTransmission to true.
// The timer interrupt sends one bit per tick; these bytes stay unchanged.
static volatile TxState txState;
static volatile bool inTransmission;
static uint8_t encodedHeader[HEADER_BYTES];
static uint8_t txWire[MAX_WIRE_BYTES];
static uint16_t txWireLength;
static volatile uint16_t txBitIndex;
#else
typedef enum{
    RX_WAITING,
    RX_PREAMBLE,
    RX_SYNC,
    RX_HEADER,
    RX_DATA,
    RX_READY
} RxState;
// The timer interrupt collects bits into bytes; the main loop repairs them.
// volatile tells the compiler these values can change outside the current
// code path, for example when a timer interrupt updates reception progress.
static volatile RxState rxState;
static volatile bool resetRequested;
static volatile uint8_t previousSample;
static volatile uint8_t samplePhase;
static volatile uint8_t highSampleCount;
static volatile uint8_t preambleCount;
static volatile uint8_t syncCount;
static volatile uint8_t syncValue;
static volatile uint8_t receivedHeader[HEADER_BYTES];
static volatile uint8_t headerBitCount;
static volatile uint8_t rxWire[MAX_WIRE_BYTES];
static volatile uint16_t rxWireBytes;
static uint8_t incomingByte;
static uint8_t incomingBits;
static uint16_t receivedLength;
static uint16_t decodedPackets;
static uint16_t messageBytes;
static uint8_t rxMessage[PROTOCOL_MAX_MESSAGE_BYTES];
static bool headerDecoded;
static int16_t correctedSymbols;

static volatile enum ReceiveProgress receiverProgress;

// Forget the previous frame and wait for the start of a new message.
static void PROTOCOL_ISR_ATTR resetReceiver(void){
    rxState = RX_WAITING;
    samplePhase = highSampleCount = 0;
    preambleCount = syncCount = syncValue = 0;
    headerBitCount = 0;
    rxWireBytes = 0;
    incomingByte = incomingBits = 0;
    headerDecoded = false;
    receivedLength = decodedPackets = messageBytes = 0;
    correctedSymbols = 0;
    receiverProgress = PROG_WAITING;
}

// Handle one decided bit: check the alternating start pattern and D3,
// then collect the header and packet bytes. A bad start returns to waiting.
// Error correction happens later in the main loop to keep this interrupt short.
static void PROTOCOL_ISR_ATTR processReceivedBit(uint8_t bit){
    switch (rxState){
        case RX_PREAMBLE:
            if (bit != ((preambleCount & 1) ? 0 : 1)){
                resetReceiver();
                return;
            }
            if (++preambleCount == 16){
                rxState = RX_SYNC;
                receiverProgress = PROG_SYNC;
            }
            break;
        case RX_SYNC:
            syncValue = (uint8_t)((syncValue << 1) | bit);
            if (++syncCount == 8){
                if (syncValue != SYNCWORD){
                    resetReceiver();
                    return;
                }
                rxState = RX_HEADER;
                receiverProgress = PROG_HEADER;
            }
            break;
        case RX_HEADER: {
            uint8_t index = headerBitCount / 8;
            if ((headerBitCount % 8) == 0){
                receivedHeader[index] = 0;
            }
            receivedHeader[index] = (uint8_t)((receivedHeader[index] << 1) | bit);
            if (++headerBitCount == 56){
                // Keep collecting immediately; header ECC runs in the main loop.
                rxState = RX_DATA;
                receiverProgress = PROG_DATA;
            }
            break;
    }
    case RX_DATA:
        if (rxWireBytes >= MAX_WIRE_BYTES){
            break;
        }
        incomingByte = (uint8_t)((incomingByte << 1) | bit);
        if (++incomingBits == 8){
            rxWire[rxWireBytes] = incomingByte;
            rxWireBytes++; // Publish only complete bytes.
            incomingByte = incomingBits = 0;
        }
        break;
    default:
        break;
    }
}
#endif

void protocolInit(void){
    rsInit();
#ifdef SENDER
    txState = TX_IDLE;
    inTransmission = false;
    txBitIndex = 0;
#else
    previousSample = 0;
    resetReceiver();
#endif
}

// Reject invalid requests or a request made while another message is sending.
// Build the header and split the message into groups of six bytes.
// Fill unused bytes of the final group with zeros. The header length tells
// the receiver which bytes are real data. Start sending only when ready.
bool protocolTransmit(const uint8_t *data, uint16_t length){
#ifdef SENDER
    if (inTransmission || data == NULL || length == 0 ||
        length > PROTOCOL_MAX_MESSAGE_BYTES)
        return false;

    uint32_t header = ((uint32_t)START_PATTERN << 20) |
                      ((uint32_t)length << 4) | END_PATTERN;
    uint8_t rawHeader[3] = {
        (uint8_t)(header >> 16), (uint8_t)(header >> 8), (uint8_t)header
    };
    rsEncode(rawHeader, 3, encodedHeader);
    txWireLength = 0;
    for (uint16_t offset = 0; offset < length; offset += 6){
        uint8_t payload[6] = {0};
        uint16_t remaining = length - offset;
        uint8_t count = remaining > 6 ? 6 : (uint8_t)remaining;
        memcpy(payload, data + offset, count);
        uint8_t *packet = txWire + txWireLength;
        packet[0] = 0xAA;
        packet[1] = 0xD3;
        rsEncode(payload, 6, packet + 2);
        packet[12] = 0x55;
        packet[13] = 0x2C;
        txWireLength += PROTOCOL_PACKET_BYTES;
    }
    txBitIndex = 0;
    txState = TX_PREAMBLE;
    inTransmission = true;
    return true;
#else
    (void)data;
    (void)length;
    return false;
#endif
}

// Send one bit whenever the sender timer fires. The current state chooses
// the start pattern, D3, header, or packet data. Reset the bit counter when
// changing stages. After the final bit has had its full interval, set the
// output LOW and mark the sender as available for the next message.
void PROTOCOL_ISR_ATTR protocolTransmitTick(void){
#ifdef SENDER
    if (!inTransmission){
        return;
    }
    uint8_t bit = 0;
    switch (txState){
        case TX_PREAMBLE:
            bit = (PREAMBLE >> (15 - txBitIndex)) & 1;
            break;
        case TX_SYNC:
            bit = (SYNCWORD >> (7 - txBitIndex)) & 1;
            break;
        case TX_HEADER:
            bit = (encodedHeader[txBitIndex / 8] >>
                   (7 - txBitIndex % 8)) & 1;
            break;
        case TX_DATA:
            bit = (txWire[txBitIndex / 8] >>
                   (7 - txBitIndex % 8)) & 1;
            break;
        case TX_FINISH:
            transmitBit(Low);
            txState = TX_IDLE;
            inTransmission = false;
            return;
        default:
            return;
    }
    transmitBit(bit ? High : Low);
    txBitIndex++;
    if (txState == TX_PREAMBLE && txBitIndex == 16){
        txBitIndex = 0;
    txState = TX_SYNC;
    }
    else if (txState == TX_SYNC && txBitIndex == 8){
        txBitIndex = 0;
    txState = TX_HEADER;
    }
    else if (txState == TX_HEADER && txBitIndex == 56){
        txBitIndex = 0;
    txState = TX_DATA;
    }
    else if (txState == TX_DATA && txBitIndex == txWireLength * 8){
        txBitIndex = 0;
    txState = TX_FINISH;
    }
#endif
}

// Read the input four times per transmitted bit. A LOW-to-HIGH change
// starts the timing count. For each bit, use the next three readings:
// two or more HIGH readings mean 1; otherwise the bit is 0.
// The fourth reading finishes that bit interval without adding a vote.
// Stop collecting once the message is ready, until the application resets.
void PROTOCOL_ISR_ATTR protocolReceiveTick(bool sample){
#ifdef RECEIVER
    uint8_t current = sample ? 1 : 0;
    if (resetRequested){
        resetReceiver();
        resetRequested = false;
        previousSample = current;
        return;
    }
    if (rxState == RX_READY){
        previousSample = current;
        return;
    }
    if (rxState == RX_WAITING){
        if (!previousSample && current){
            rxState = RX_PREAMBLE;
            receiverProgress = PROG_START;
            samplePhase = highSampleCount = 0;
        }
        previousSample = current;
        return;
    }
    samplePhase++;
    if (samplePhase <= 3 && current)
        highSampleCount++;
    if (samplePhase == 3)
        processReceivedBit(highSampleCount >= 2);
    if (samplePhase >= 4)
        samplePhase = highSampleCount = 0;
    previousSample = current;
#else
    (void)sample;
#endif
}

// Run from the main loop. Repair and check collected bytes while the timer
// continues receiving later bytes. The timer does not rewrite completed bytes
// until reception is reset.
void protocolProcess(void){
#ifdef RECEIVER
    if (resetRequested || rxState != RX_DATA)
        return;

    // Repair the header, check its markers, and reject an invalid length.
    if (!headerDecoded){
        uint8_t codeword[HEADER_BYTES];
        for (uint8_t i = 0; i < HEADER_BYTES; i++) codeword[i] = receivedHeader[i];
        int8_t result = rsDecode(codeword, HEADER_BYTES);
        uint32_t header = ((uint32_t)codeword[0] << 16) |
                          ((uint32_t)codeword[1] << 8) | codeword[2];
        uint16_t length = (header >> 4) & 0xFFFFU;
        if (result < 0 || (header >> 20) != START_PATTERN ||
            (header & 0xFU) != END_PATTERN || length == 0 ||
            length > PROTOCOL_MAX_MESSAGE_BYTES){
            protocolResetReceiver();
            return;
        }
        receivedLength = length;
        correctedSymbols = result;
        headerDecoded = true;
    }
    // Round the length up to groups of six and wait for each full packet.
    // Check packet markers, repair its data, and append it to the message.
    // Ignore padding in the final packet. A failed check discards this frame.
    uint16_t expectedPackets = (receivedLength + 5) / 6;
    while (decodedPackets < expectedPackets &&
           rxWireBytes >= (decodedPackets + 1) * PROTOCOL_PACKET_BYTES){
        uint16_t offset = decodedPackets * PROTOCOL_PACKET_BYTES;
        if (rxWire[offset] != 0xAA || rxWire[offset + 1] != 0xD3 ||
            rxWire[offset + 12U] != 0x55 || rxWire[offset + 13] != 0x2C){
            protocolResetReceiver();
            return;
        }
        uint8_t codeword[DATA_CODEWORD_BYTES];
        for (uint8_t i = 0; i < DATA_CODEWORD_BYTES; i++)
            codeword[i] = rxWire[offset + 2 + i];
        int8_t result = rsDecode(codeword, DATA_CODEWORD_BYTES);
        if (result < 0){
            protocolResetReceiver();
            return;
        }
        correctedSymbols += result;
        uint16_t remaining = receivedLength - messageBytes;
        uint8_t count = remaining > 6 ? 6 : (uint8_t)remaining;
        memcpy(rxMessage + messageBytes, codeword, count);
        messageBytes += count;
        decodedPackets++;
    }
    // All packets passed: make the completed message available to the caller.
    if (decodedPackets == expectedPackets){
        rxState = RX_READY;
        receiverProgress = PROG_READY;
    }
#endif
}

bool protocolIsReady(void){
#ifdef RECEIVER
    return !resetRequested && rxState == RX_READY;
#else
    return false;
#endif
}


bool protocolIsTransmitting(void){
#ifdef SENDER
    return inTransmission;
#else
    return false;
#endif
}


// Return the internal message buffer. Copy it before requesting a reset.
const uint8_t *protocolGetMessageData(void){
#ifdef RECEIVER
    return rxMessage;
#else
    return NULL;
#endif
}


uint16_t protocolGetMessageLength(void){
#ifdef RECEIVER
    return receivedLength;
#else
    return 0;
#endif
}


int16_t protocolGetCorrectedSymbolCount(void){
#ifdef RECEIVER
    return correctedSymbols;
#else
    return 0;
#endif
}


// Progress: 0 waiting, 1 start pattern, 2 sync, 3 header, 4 data, 5 ready.
enum ReceiveProgress protocolGetReceiverProgress(void){
#ifdef RECEIVER
    return resetRequested ? PROG_WAITING : receiverProgress;
#else
    return PROG_WAITING;
#endif
}


// Ask the timer interrupt to reset on its next reading. This avoids
// clearing collection state while the interrupt is working on a bit.
void protocolResetReceiver(void){
#ifdef RECEIVER
    resetRequested = true;
#endif
}
