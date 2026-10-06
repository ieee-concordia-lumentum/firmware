#include "protocol.h"
#include "hardware.h"
#include <string.h>

#if defined(SENDER) && defined(RECEIVER)
#error "Define only one device role"
#elif !defined(SENDER) && !defined(RECEIVER)
#error "Define SENDER or RECEIVER"
#endif

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


static uint8_t gfMultiply(uint8_t a, uint8_t b){
    if ((a == 0) || (b == 0)){
        return 0;
    }

    return gfExp[(uint16_t)gfLog[a] + gfLog[b]];
}


static uint8_t gfInverse(uint8_t value){
    if (value == 0){
        return 0;
    }

    return gfExp[255 - gfLog[value]];
}


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


static uint8_t polynomialEvaluate(const uint8_t *polynomial,
                                  uint8_t length,
                                  uint8_t value){
    uint8_t result = polynomial[0];

    for (uint8_t index = 1; index < length; index++){
        result = gfMultiply(result, value) ^ polynomial[index];
    }

    return result;
}


static void rsEncode(const uint8_t *header, uint8_t dataLength, uint8_t *codeword){
    uint8_t remainder[DATA_CODEWORD_BYTES];

    memcpy(remainder, header, dataLength);
    memset(&remainder[dataLength], 0, RS_ECC_SYMBOLS);

    for (uint8_t index = 0; index < dataLength; index++){
        uint8_t coefficient = remainder[index];

        if (coefficient != 0){
            for (uint8_t generatorIndex = 1;
                 generatorIndex <= RS_ECC_SYMBOLS;
                 generatorIndex++){
                remainder[index + generatorIndex] ^=
                    gfMultiply(coefficient, rsGenerator[generatorIndex]);
            }
        }
    }

    memcpy(codeword, header, dataLength);
    memcpy(&codeword[dataLength],
           &remainder[dataLength],
           RS_ECC_SYMBOLS);
}


// Returns 0 for no error, 1 or 2 for the number of corrected byte symbols,
// and -1 when the codeword cannot be corrected.
static int8_t rsDecode(uint8_t *codeword, uint8_t codewordLength){
    uint8_t syndrome[RS_ECC_SYMBOLS];
    uint8_t syndrome0;
    uint8_t syndrome1;
    uint8_t syndrome2;
    uint8_t syndrome3;

    for (uint8_t index = 0; index < RS_ECC_SYMBOLS; index++){
        syndrome[index] = polynomialEvaluate(
            codeword, codewordLength, gfExp[index]);
    }

    if ((syndrome[0] == 0) && (syndrome[1] == 0) &&
        (syndrome[2] == 0) && (syndrome[3] == 0)){
        return 0;
    }

    syndrome0 = syndrome[0];
    syndrome1 = syndrome[1];
    syndrome2 = syndrome[2];
    syndrome3 = syndrome[3];

    if (syndrome0 != 0){
        uint8_t location = gfMultiply(syndrome1, gfInverse(syndrome0));

        if (location != 0){
            uint8_t locationSquared = gfMultiply(location, location);
            uint8_t locationCubed = gfMultiply(locationSquared, location);

            if ((gfMultiply(syndrome0, locationSquared) == syndrome2) &&
                (gfMultiply(syndrome0, locationCubed) == syndrome3)){
                uint8_t degree = gfLog[location];

                if (degree < codewordLength){
                    uint8_t position =
                        (uint8_t)(codewordLength - 1 - degree);

                    codeword[position] ^= syndrome0;

                    for (uint8_t index = 0;
                         index < RS_ECC_SYMBOLS;
                         index++){
                        if (polynomialEvaluate(codeword,
                                               codewordLength,
                                               gfExp[index]) != 0){
                            codeword[position] ^= syndrome0;
                            break;
                        }

                        if (index == (RS_ECC_SYMBOLS - 1)){
                            return 1;
                        }
                    }
                }
            }
        }
    }

    {
        uint8_t determinant =
            gfMultiply(syndrome1, syndrome1) ^
            gfMultiply(syndrome0, syndrome2);
        uint8_t sigma1;
        uint8_t sigma2;
        uint8_t found = 0;
        uint8_t position1 = 0;
        uint8_t position2 = 0;
        uint8_t location1 = 0;
        uint8_t location2 = 0;

        if (determinant == 0){
            return -1;
        }

        sigma1 = gfMultiply(
            gfMultiply(syndrome2, syndrome1) ^
                gfMultiply(syndrome3, syndrome0),
            gfInverse(determinant));
        sigma2 = gfMultiply(
            gfMultiply(syndrome1, syndrome3) ^
                gfMultiply(syndrome2, syndrome2),
            gfInverse(determinant));

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
            uint8_t denominator = location2 ^ location1;
            uint8_t error1 = gfMultiply(
                gfMultiply(syndrome0, location2) ^ syndrome1,
                gfInverse(denominator));
            uint8_t error2 = gfMultiply(
                syndrome1 ^ gfMultiply(syndrome0, location1),
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
static volatile uint8_t receiverProgress;

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
    receiverProgress = 0;
}

static void PROTOCOL_ISR_ATTR processReceivedBit(uint8_t bit){
    switch (rxState){
        case RX_PREAMBLE:
            if (bit != ((preambleCount & 1U) ? 0U : 1U)){
                resetReceiver();
                return;
            }
            if (++preambleCount == 16U){
                rxState = RX_SYNC;
                receiverProgress = 2;
            }
            break;
        case RX_SYNC:
            syncValue = (uint8_t)((syncValue << 1) | bit);
            if (++syncCount == 8U){
                if (syncValue != SYNCWORD){
                    resetReceiver();
                    return;
                }
                rxState = RX_HEADER;
                receiverProgress = 3;
            }
            break;
        case RX_HEADER: {
            uint8_t index = headerBitCount / 8U;
            if ((headerBitCount % 8U) == 0U){
                receivedHeader[index] = 0;
            }
            receivedHeader[index] = (uint8_t)((receivedHeader[index] << 1) | bit);
            if (++headerBitCount == 56U){
                // Keep collecting immediately; header ECC runs in the main loop.
                rxState = RX_DATA;
                receiverProgress = 4;
            }
            break;
    }
    case RX_DATA:
        if (rxWireBytes >= MAX_WIRE_BYTES){
            break;
        }
        incomingByte = (uint8_t)((incomingByte << 1) | bit);
        if (++incomingBits == 8U){
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

bool protocolTransmit(const uint8_t *data, uint16_t length){
#ifdef SENDER
    if (inTransmission || data == NULL || length == 0 ||
        length > PROTOCOL_MAX_MESSAGE_BYTES) return false;

    uint32_t header = ((uint32_t)START_PATTERN << 20) |
                      ((uint32_t)length << 4) | END_PATTERN;
    uint8_t rawHeader[3] = {
        (uint8_t)(header >> 16), (uint8_t)(header >> 8), (uint8_t)header
    };
    rsEncode(rawHeader, 3, encodedHeader);
    txWireLength = 0;
    for (uint16_t offset = 0; offset < length; offset += 6U){
        uint8_t payload[6] = {0};
        uint16_t remaining = length - offset;
        uint8_t count = remaining > 6U ? 6U : (uint8_t)remaining;
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

void PROTOCOL_ISR_ATTR protocolTransmitTick(void){
#ifdef SENDER
    if (!inTransmission){
        return;
    }
    uint8_t bit = 0;
    switch (txState){
        case TX_PREAMBLE:
            bit = (PREAMBLE >> (15U - txBitIndex)) & 1U;
            break;
        case TX_SYNC:
            bit = (SYNCWORD >> (7U - txBitIndex)) & 1U;
            break;
        case TX_HEADER:
            bit = (encodedHeader[txBitIndex / 8U] >>
                   (7U - txBitIndex % 8U)) & 1U;
            break;
        case TX_DATA:
            bit = (txWire[txBitIndex / 8U] >>
                   (7U - txBitIndex % 8U)) & 1U;
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
    if (txState == TX_PREAMBLE && txBitIndex == 16U){
        txBitIndex = 0;
    txState = TX_SYNC;
    }
    else if (txState == TX_SYNC && txBitIndex == 8U){
        txBitIndex = 0;
    txState = TX_HEADER;
    }
    else if (txState == TX_HEADER && txBitIndex == 56U){
        txBitIndex = 0;
    txState = TX_DATA;
    }
    else if (txState == TX_DATA && txBitIndex == txWireLength * 8U){
        txBitIndex = 0;
    txState = TX_FINISH;
    }
#endif
}

void PROTOCOL_ISR_ATTR protocolReceiveTick(bool sample){
#ifdef RECEIVER
    uint8_t current = sample ? 1U : 0U;
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
            receiverProgress = 1;
            samplePhase = highSampleCount = 0;
        }
        previousSample = current;
        return;
    }
    samplePhase++;
    if (samplePhase <= 3U && current){
        highSampleCount++;
    }
    if (samplePhase == 3U){
        processReceivedBit(highSampleCount >= 2U);
    }
    if (samplePhase >= 4U){
        samplePhase = highSampleCount = 0;
    }
    previousSample = current;
#else
    (void)sample;
#endif
}

// Complete wire bytes are immutable while main-loop ECC runs.
void protocolProcess(void){
#ifdef RECEIVER
    if (resetRequested || rxState != RX_DATA){
        return;
    }
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
    uint16_t expectedPackets = (receivedLength + 5U) / 6U;
    while (decodedPackets < expectedPackets &&
           rxWireBytes >= (decodedPackets + 1U) * PROTOCOL_PACKET_BYTES){
        uint16_t offset = decodedPackets * PROTOCOL_PACKET_BYTES;
        if (rxWire[offset] != 0xAA || rxWire[offset + 1U] != 0xD3 ||
            rxWire[offset + 12U] != 0x55 || rxWire[offset + 13U] != 0x2C){
            protocolResetReceiver();
            return;
        }
        uint8_t codeword[DATA_CODEWORD_BYTES];
        for (uint8_t i = 0; i < DATA_CODEWORD_BYTES; i++)
            codeword[i] = rxWire[offset + 2U + i];
        int8_t result = rsDecode(codeword, DATA_CODEWORD_BYTES);
        if (result < 0){
            protocolResetReceiver();
            return;
        }
        correctedSymbols += result;
        uint16_t remaining = receivedLength - messageBytes;
        uint8_t count = remaining > 6U ? 6U : (uint8_t)remaining;
        memcpy(rxMessage + messageBytes, codeword, count);
        messageBytes += count;
        decodedPackets++;
    }
    if (decodedPackets == expectedPackets){
        rxState = RX_READY;
        receiverProgress = 5;
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


uint8_t protocolGetReceiverProgress(void){
#ifdef RECEIVER
    return resetRequested ? 0 : receiverProgress;
#else
    return 0;
#endif
}


void protocolResetReceiver(void){
#ifdef RECEIVER
    resetRequested = true;
#endif
}