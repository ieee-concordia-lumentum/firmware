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
// 1010101010101010, 11010011, then one 24-bit header packet.
#define PREAMBLE          UINT16_C(0xAAAA)
#define PREAMBLE_LENGTH   UINT8_C(16)
#define SYNCWORD          UINT8_C(0xD3)
#define SYNCWORD_LENGTH   UINT8_C(8)

// First 24-bit header packet:
// [packet type: 8 bits][message length in bytes: 16 bits]
#define HEADER_TYPE       UINT8_C(0x01)
#define HEADER_LENGTH     UINT8_C(24)

#define SAMPLES_PER_BIT   UINT8_C(4)
#define VOTE_SAMPLES      UINT8_C(3)


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
    static volatile uint32_t headerPacket = 0;
    static volatile bool inTransmission = false;


#elif defined(RECEIVER)
    typedef enum{
        RX_WAITING,
        RX_PREAMBLE,
        RX_SYNC,
        RX_HEADER,
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

    static volatile uint32_t receivedHeader = 0;
    static volatile uint8_t headerCount = 0;
    static volatile uint8_t receivedHeaderType = 0;
    static volatile uint16_t receivedMessageLength = 0;

    static volatile uint8_t receiverProgress = 0;


    static void PROTOCOL_ISR_ATTR resetReceiver(void){
        rxState = RX_WAITING;
        samplePhase = 0;
        highSampleCount = 0;

        preambleCount = 0;
        expectedPreambleBit = 1;

        receivedSyncword = 0;
        syncwordCount = 0;

        receivedHeader = 0;
        headerCount = 0;
        receivedHeaderType = 0;
        receivedMessageLength = 0;
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
                        receivedHeader = 0;
                        headerCount = 0;
                        rxState = RX_HEADER;
                    }
                    else{
                        resetReceiver();
                    }
                }
                break;


            case RX_HEADER:
                receivedHeader =
                    (receivedHeader << 1) | bitValue;
                headerCount++;

                if ((25 + headerCount) > receiverProgress){
                    receiverProgress = 25 + headerCount;
                }

                if (headerCount >= HEADER_LENGTH){
                    receivedHeaderType =
                        (uint8_t)(receivedHeader >> 16);
                    receivedMessageLength =
                        (uint16_t)(receivedHeader & UINT16_C(0xFFFF));

                    if (receivedHeaderType == HEADER_TYPE){
                        rxState = RX_READY;
                    }
                    else{
                        resetReceiver();
                    }
                }
                break;


            default:
                break;
        }
    }
#endif


void protocolInit(void){
#ifdef SENDER
    txState = TX_IDLE;
    txBitIndex = 0;
    headerPacket = 0;
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

    if (inTransmission || (message == NULL)){
        return;
    }

    messageLength = strlen(message);

    if (messageLength > UINT16_MAX){
        messageLength = UINT16_MAX;
    }

    headerPacket =
        ((uint32_t)HEADER_TYPE << 16) |
        (uint16_t)messageLength;

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
            bitValue = (uint8_t)((headerPacket >>
                (HEADER_LENGTH - 1 - txBitIndex)) & 1);

            transmitBit(bitValue ? '1' : '0');
            txBitIndex++;

            if (txBitIndex >= HEADER_LENGTH){
                txBitIndex = 0;
                txState = TX_FINISH;
            }
            break;


        case TX_FINISH:
            // End the last sync bit and return the line to idle LOW.
            transmitBit('0');
            txState = TX_IDLE;
            inTransmission = false;
            break;
    }
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


uint8_t protocolGetHeaderType(void){
#ifdef RECEIVER
    return receivedHeaderType;
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


void protocolResetReceiver(void){
#ifdef RECEIVER
    previousSample = 0;
    receiverProgress = 0;
    resetReceiver();
#endif
}
