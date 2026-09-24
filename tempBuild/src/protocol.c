#include "protocol.h"
#include "hardware.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char *messageToSend = NULL;

static unsigned int messageLength = 0;

static volatile unsigned int currentBit = 0;
static volatile bool inTransmission = false;


static char getBinary(void){
    if (currentBit < messageLength){
        uint8_t currentByte =
            (uint8_t)messageToSend[currentBit / 8];

        uint8_t bitValue =
            (currentByte >> (7 - (currentBit % 8))) & 1;

        currentBit++;

        return bitValue ? '1' : '0';
    }

    return '\0';
}


void protocolInit(void){
    messageToSend = NULL;
    messageLength = 0;
    currentBit = 0;
    inTransmission = false;
}


void protocolTransmit(const char *message){
    if (inTransmission){
        return;
    }

    messageToSend = message;
    messageLength = strlen(message) * 8;

    currentBit = 0;
    inTransmission = true;
}


void protocolTimerTick(void){
    if (!inTransmission){
        return;
    }

    char bitValue = getBinary();

    if (bitValue != '\0'){
        transmitBit(bitValue);
    }
    else{
        inTransmission = false;
    }
}