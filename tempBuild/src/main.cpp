#include <Arduino.h>
#include <esp_system.h>
#include <stdio.h>
#include <string.h>

#include "hardware.h"
#include "protocol.h"


constexpr unsigned long testDurationMs = 15UL * 60UL * 1000UL;
constexpr unsigned long interMessageGapMs = 5;
constexpr unsigned long receptionTimeoutMs = 250;

static uint32_t messageChecksum(const char *data, size_t length){
    uint32_t checksum = 0xFFFFFFFF;

    for (size_t index = 0; index < length; index++){
        checksum ^= static_cast<uint8_t>(data[index]);

        for (uint8_t bit = 0; bit < 8; bit++){
            checksum = (checksum >> 1) ^
                ((checksum & 1U) ? 0xEDB88320 : 0);
        }
    }

    return ~checksum;
}


#ifdef SENDER
static const char *words[] = {
    "Lumentum", "Hanish", "Taif", "Emille", "Kelvin", "Ella", "Drew", "Amirreza", "Saskia", "Chelsea", "Safin",
};

static bool testRunning = false;
static bool waitingForGap = false;
static unsigned long testStarted = 0;
static unsigned long transmissionFinished = 0;
static uint32_t sentMessages = 0;


static void processSender(void){
    while (Serial.available()){
        char command = Serial.read();

        if ((command == 's' || command == 'S') && !testRunning &&
            !protocolIsTransmitting()){
            randomSeed(esp_random());
            sentMessages = 0;
            testStarted = millis();
            testRunning = true;
            waitingForGap = false;
            Serial.println("TEST_START,16kbps,900seconds");
        }
    }

    if (!testRunning){
        return;
    }

    if (protocolIsTransmitting()){
        waitingForGap = true;
        return;
    }

    unsigned long currentTime = millis();

    if (waitingForGap){
        transmissionFinished = currentTime;
        waitingForGap = false;
    }

    if (currentTime - testStarted >= testDurationMs){
        testRunning = false;
        Serial.print("TEST_END,sent=");
        Serial.println(sentMessages);
        return;
    }

    if (sentMessages > 0 && currentTime - transmissionFinished < interMessageGapMs){
        return;
    }

    const char *word = words[random(sizeof(words) / sizeof(words[0]))];
    char message[48];

    int prefixLength = snprintf(message, sizeof(message), "%06lu,%s",
                                (unsigned long)(sentMessages + 1), word);

    uint32_t checksum = messageChecksum(message, prefixLength);

    int length = snprintf(message + prefixLength,
                          sizeof(message) - prefixLength,
                          ",%08lX", (unsigned long)checksum);

    if (length <= 0 ||
        (size_t)length >= sizeof(message) - prefixLength){
        return;
    }

    if (protocolTransmit(reinterpret_cast<const uint8_t *>(message),
                         prefixLength + length)){
        sentMessages++;
        waitingForGap = true;
    }
}
#else
static unsigned long receptionStarted = 0;
static bool receiving = false;
static uint32_t receivedMessages = 0;
static uint32_t loggingDrops = 0;


// Nonblocking serial output keeps USB logging from delaying reception.
static void writeLogRow(const char *row){
    size_t length = strlen(row);

    if ((size_t)Serial.availableForWrite() >= length){
        Serial.write(row, length);
    }
    else{
        loggingDrops++;
    }
}


static void processReceiver(void){
    uint8_t progress = protocolGetReceiverProgress();

    if (progress == 0){
        receiving = false;
    }

    if (progress > 0 && !receiving){
        receiving = true;
        receptionStarted = millis();
    }

    if (protocolIsReady()){
        char message[48];
        uint16_t length = protocolGetMessageLength();
        int16_t correctedSymbols = protocolGetCorrectedSymbolCount();

        // Copy before requesting the asynchronous receiver reset.
        if (length >= sizeof(message)){
            protocolResetReceiver();
            receiving = false;
            return;
        }

        memcpy(message, protocolGetMessageData(), length);
        message[length] = 0;
        protocolResetReceiver();
        receiving = false;

        char *separator = strrchr(message, ',');
        bool checksumValid = false;

        if (separator != nullptr && strlen(separator + 1) == 8){
            char *checksumEnd = nullptr;
            uint32_t expectedChecksum =
                strtoul(separator + 1, &checksumEnd, 16);

            checksumValid = *checksumEnd == 0 &&
                messageChecksum(message, separator - message) == expectedChecksum;
        }

        receivedMessages++;
        char row[128];

        snprintf(row, sizeof(row), "DATA,%lu,%s,%d,%s,%lu,%lu\n",
                 millis(), message, correctedSymbols,
                 checksumValid ? "OK" : "BAD",
                 (unsigned long)receivedMessages,
                 (unsigned long)loggingDrops);

        writeLogRow(row);
    }
    else if (receiving &&
             millis() - receptionStarted >= receptionTimeoutMs){
        protocolResetReceiver();
        receiving = false;
        writeLogRow("TIMEOUT\n");
    }
}
#endif


void setup(){
    protocolInit();
    hardwareInit();

#ifdef SENDER
    Serial.println("Ready: enter s to start a 15-minute test.");
#else
    Serial.println("Ready: receiver USB logging at 115200 baud.");
#endif
}


void loop(){
    protocolProcess();

#ifdef SENDER
    processSender();
#else
    processReceiver();
#endif
}

