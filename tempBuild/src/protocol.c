#include "protocol.h"
#include "hardware.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(SENDER) && defined(RECEIVER)
    #error "Define only SENDER or RECEIVER, not both"
#elif !defined(SENDER) && !defined(RECEIVER)
    #error "Define SENDER or RECEIVER"
#endif


// Everything is sent MSB first:
// 1010101010101010, 11010011, then one Reed-Solomon protected header.
#define PREAMBLE          UINT16_C(0xAAAA)
#define PREAMBLE_LENGTH   UINT8_C(16)
#define SYNCWORD          UINT8_C(0xD3)
#define SYNCWORD_LENGTH   UINT8_C(8)

// The uncoded header is three bytes:
// [start pattern: 4 bits][message length: 16 bits][end pattern: 4 bits]
// Four RS parity bytes are appended, producing a seven-byte RS(7, 3)
// codeword. This corrects up to two corrupted bytes in the header.
#define START_PATTERN     UINT8_C(0x0A)
#define END_PATTERN       UINT8_C(0x05)
#define HEADER_DATA_BYTES UINT8_C(3)
#define RS_ECC_SYMBOLS    UINT8_C(4)
#define HEADER_BYTES      (HEADER_DATA_BYTES + RS_ECC_SYMBOLS)
#define HEADER_BIT_LENGTH (HEADER_BYTES * UINT8_C(8))

#define GF_PRIMITIVE_POLYNOMIAL UINT16_C(0x011D)

#define SAMPLES_PER_BIT   UINT8_C(4)
#define VOTE_SAMPLES      UINT8_C(3)


// GF(256) uses primitive polynomial 0x11D. The generator polynomial has
// roots alpha^0 through alpha^3 so the encoder and decoder use four
// consecutive syndromes.
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


static void rsEncodeHeader(const uint8_t *header, uint8_t *codeword){
    uint8_t remainder[HEADER_BYTES];

    memcpy(remainder, header, HEADER_DATA_BYTES);
    memset(&remainder[HEADER_DATA_BYTES], 0, RS_ECC_SYMBOLS);

    for (uint8_t index = 0; index < HEADER_DATA_BYTES; index++){
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

    memcpy(codeword, header, HEADER_DATA_BYTES);
    memcpy(&codeword[HEADER_DATA_BYTES],
           &remainder[HEADER_DATA_BYTES],
           RS_ECC_SYMBOLS);
}


// Returns 0 for no error, 1 or 2 for the number of corrected byte symbols,
// and -1 when the codeword cannot be corrected.
static int8_t rsDecodeHeader(uint8_t *codeword){
    uint8_t syndrome[RS_ECC_SYMBOLS];
    uint8_t syndrome0;
    uint8_t syndrome1;
    uint8_t syndrome2;
    uint8_t syndrome3;

    for (uint8_t index = 0; index < RS_ECC_SYMBOLS; index++){
        syndrome[index] = polynomialEvaluate(
            codeword, HEADER_BYTES, gfExp[index]);
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

                if (degree < HEADER_BYTES){
                    uint8_t position =
                        (uint8_t)(HEADER_BYTES - 1 - degree);

                    codeword[position] ^= syndrome0;

                    for (uint8_t index = 0;
                         index < RS_ECC_SYMBOLS;
                         index++){
                        if (polynomialEvaluate(codeword,
                                               HEADER_BYTES,
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

        for (uint8_t degree = 0; degree < HEADER_BYTES; degree++){
            uint8_t location = gfExp[degree];
            uint8_t inverseLocation = gfInverse(location);
            uint8_t locatorValue =
                1 ^ gfMultiply(sigma1, inverseLocation) ^
                gfMultiply(sigma2,
                           gfMultiply(inverseLocation, inverseLocation));

            if (locatorValue == 0){
                if (found == 0){
                    position1 = (uint8_t)(HEADER_BYTES - 1 - degree);
                    location1 = location;
                }
                else if (found == 1){
                    position2 = (uint8_t)(HEADER_BYTES - 1 - degree);
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
                                       HEADER_BYTES,
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
        TX_FINISH
    } TxState;

    static volatile TxState txState = TX_IDLE;
    static volatile uint8_t txBitIndex = 0;
    static volatile uint8_t encodedHeader[HEADER_BYTES];
    static volatile bool inTransmission = false;


#elif defined(RECEIVER)
    typedef enum{
        RX_WAITING,
        RX_PREAMBLE,
        RX_SYNC,
        RX_HEADER,
        RX_ECC_PENDING,
        RX_READY
    } RxState;

    static volatile RxState rxState = RX_WAITING;

    static volatile uint8_t previousSample = 0;
    static volatile uint8_t samplePhase = 0;
    static volatile uint8_t highSampleCount = 0;

    static volatile uint8_t preambleCount = 0;
    static volatile uint8_t expectedPreambleBit = 1;

    static volatile uint8_t receivedSyncword = 0;
    static volatile uint8_t syncwordCount = 0;

    static volatile uint8_t receivedHeader[HEADER_BYTES];
    static volatile uint8_t headerBitCount = 0;
    static volatile uint16_t receivedMessageLength = 0;
    static volatile int8_t correctedSymbolCount = -1;

    static volatile uint8_t receiverProgress = 0;


    static void PROTOCOL_ISR_ATTR clearReceivedHeader(void){
        for (uint8_t index = 0; index < HEADER_BYTES; index++){
            receivedHeader[index] = 0;
        }
    }


    static void PROTOCOL_ISR_ATTR resetReceiver(void){
        rxState = RX_WAITING;
        samplePhase = 0;
        highSampleCount = 0;

        preambleCount = 0;
        expectedPreambleBit = 1;

        receivedSyncword = 0;
        syncwordCount = 0;

        clearReceivedHeader();
        headerBitCount = 0;
        receivedMessageLength = 0;
        correctedSymbolCount = -1;
    }


    static void PROTOCOL_ISR_ATTR processReceivedBit(uint8_t bitValue){
        switch (rxState){
            case RX_PREAMBLE:
                if (bitValue != expectedPreambleBit){
                    resetReceiver();
                    return;
                }

                preambleCount++;
                expectedPreambleBit = !expectedPreambleBit;

                if ((1 + preambleCount) > receiverProgress){
                    receiverProgress = 1 + preambleCount;
                }

                if (preambleCount >= PREAMBLE_LENGTH){
                    receivedSyncword = 0;
                    syncwordCount = 0;
                    rxState = RX_SYNC;
                }
                break;


            case RX_SYNC:
                receivedSyncword =
                    (uint8_t)((receivedSyncword << 1) | bitValue);
                syncwordCount++;

                if ((17 + syncwordCount) > receiverProgress){
                    receiverProgress = 17 + syncwordCount;
                }

                if (syncwordCount >= SYNCWORD_LENGTH){
                    if (receivedSyncword == SYNCWORD){
                        clearReceivedHeader();
                        headerBitCount = 0;
                        rxState = RX_HEADER;
                    }
                    else{
                        resetReceiver();
                    }
                }
                break;


            case RX_HEADER:
                {
                    uint8_t byteIndex = headerBitCount / 8;

                    receivedHeader[byteIndex] = (uint8_t)
                        ((receivedHeader[byteIndex] << 1) | bitValue);
                    headerBitCount++;
                }

                if ((25 + headerBitCount) > receiverProgress){
                    receiverProgress = 25 + headerBitCount;
                }

                if (headerBitCount >= HEADER_BIT_LENGTH){
                    rxState = RX_ECC_PENDING;
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
    txBitIndex = 0;
    memset((void *)encodedHeader, 0, sizeof(encodedHeader));
    inTransmission = false;

#elif defined(RECEIVER)
    previousSample = 0;
    receiverProgress = 0;
    resetReceiver();
#endif
}


void protocolTransmit(const char *message){
#ifdef SENDER
    size_t messageLength;
    uint8_t header[HEADER_DATA_BYTES];
    uint32_t headerPacket;

    if (inTransmission || (message == NULL)){
        return;
    }

    messageLength = strlen(message);

    if (messageLength > UINT16_MAX){
        messageLength = UINT16_MAX;
    }

    headerPacket =
        ((uint32_t)START_PATTERN << 20) |
        ((uint32_t)(uint16_t)messageLength << 4) |
        (uint32_t)END_PATTERN;

    header[0] = (uint8_t)(headerPacket >> 16);
    header[1] = (uint8_t)(headerPacket >> 8);
    header[2] = (uint8_t)headerPacket;
    rsEncodeHeader(header, (uint8_t *)encodedHeader);

    txBitIndex = 0;
    txState = TX_PREAMBLE;
    inTransmission = true;
#else
    (void)message;
#endif
}


void PROTOCOL_ISR_ATTR protocolTransmitTick(void){
#ifdef SENDER
    uint8_t bitValue;

    if (!inTransmission){
        return;
    }

    switch (txState){
        case TX_IDLE:
            return;


        case TX_PREAMBLE:
            bitValue = (uint8_t)((PREAMBLE >>
                (PREAMBLE_LENGTH - 1 - txBitIndex)) & 1);

            transmitBit(bitValue ? '1' : '0');
            txBitIndex++;

            if (txBitIndex >= PREAMBLE_LENGTH){
                txBitIndex = 0;
                txState = TX_SYNC;
            }
            break;


        case TX_SYNC:
            bitValue = (uint8_t)((SYNCWORD >>
                (SYNCWORD_LENGTH - 1 - txBitIndex)) & 1);

            transmitBit(bitValue ? '1' : '0');
            txBitIndex++;

            if (txBitIndex >= SYNCWORD_LENGTH){
                txBitIndex = 0;
                txState = TX_HEADER;
            }
            break;


        case TX_HEADER:
            bitValue = (uint8_t)((encodedHeader[txBitIndex / 8] >>
                (7 - (txBitIndex % 8))) & 1);

            transmitBit(bitValue ? '1' : '0');
            txBitIndex++;

            if (txBitIndex >= HEADER_BIT_LENGTH){
                txBitIndex = 0;
                txState = TX_FINISH;
            }
            break;


        case TX_FINISH:
            // End the last header bit and return the line to idle LOW.
            transmitBit('0');
            txState = TX_IDLE;
            inTransmission = false;
            break;
    }
#endif
}


void protocolProcess(void){
#ifdef RECEIVER
    uint8_t codeword[HEADER_BYTES];
    uint32_t headerPacket;
    int8_t decodeResult;

    if (rxState != RX_ECC_PENDING){
        return;
    }

    memcpy(codeword, (const void *)receivedHeader, sizeof(codeword));
    decodeResult = rsDecodeHeader(codeword);

    if (decodeResult < 0){
        resetReceiver();
        return;
    }

    headerPacket =
        ((uint32_t)codeword[0] << 16) |
        ((uint32_t)codeword[1] << 8) |
        codeword[2];

    if ((((headerPacket >> 20) & UINT8_C(0x0F)) != START_PATTERN) ||
        ((headerPacket & UINT8_C(0x0F)) != END_PATTERN)){
        resetReceiver();
        return;
    }

    memcpy((void *)receivedHeader, codeword, sizeof(codeword));
    receivedMessageLength =
        (uint16_t)((headerPacket >> 4) & UINT16_C(0xFFFF));
    correctedSymbolCount = decodeResult;
    rxState = RX_READY;
#endif
}


void PROTOCOL_ISR_ATTR protocolReceiveTick(bool sample){
#ifdef RECEIVER
    uint8_t currentSample = sample ? 1 : 0;

    if (rxState == RX_READY){
        previousSample = currentSample;
        return;
    }

    if (rxState == RX_WAITING){
        if ((previousSample == 0) && (currentSample == 1)){
            rxState = RX_PREAMBLE;

            preambleCount = 0;
            expectedPreambleBit = 1;
            receivedSyncword = 0;
            syncwordCount = 0;

            samplePhase = 0;
            highSampleCount = 0;

            if (receiverProgress == 0){
                receiverProgress = 1;
            }
        }

        previousSample = currentSample;
        return;
    }

    samplePhase++;

    if ((samplePhase <= VOTE_SAMPLES) && currentSample){
        highSampleCount++;
    }

    // Vote on three samples from inside the bit. Two matching samples win.
    if (samplePhase == VOTE_SAMPLES){
        uint8_t bitValue = (highSampleCount >= 2) ? 1 : 0;
        processReceivedBit(bitValue);
    }

    if (samplePhase >= SAMPLES_PER_BIT){
        samplePhase = 0;
        highSampleCount = 0;
    }

    previousSample = currentSample;
#else
    (void)sample;
#endif
}


bool protocolIsReady(void){
#ifdef RECEIVER
    return rxState == RX_READY;
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


uint8_t protocolGetReceiverProgress(void){
#ifdef RECEIVER
    return receiverProgress;
#else
    return 0;
#endif
}


uint16_t protocolGetMessageLength(void){
#ifdef RECEIVER
    return receivedMessageLength;
#else
    return 0;
#endif
}


int8_t protocolGetCorrectedSymbolCount(void){
#ifdef RECEIVER
    return correctedSymbolCount;
#else
    return -1;
#endif
}


void protocolResetReceiver(void){
#ifdef RECEIVER
    previousSample = 0;
    receiverProgress = 0;
    resetReceiver();
#endif
}
