#include <Arduino.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "hardware.h"
#include "protocol.h"


// Allow the largest protocol frame plus two byte intervals of margin.
constexpr unsigned long maxFrameBytes =
    10UL + ((PROTOCOL_MAX_MESSAGE_BYTES + 5UL) / 6UL) * PROTOCOL_PACKET_BYTES;
static_assert((maxFrameBytes + 2ULL) * PROTOCOL_BYTE_PERIOD_MS <= UINT32_MAX,
              "Byte period is too large for the receiver timeout.");
constexpr unsigned long receptionTimeoutMs =
    (maxFrameBytes + 2UL) * PROTOCOL_BYTE_PERIOD_MS;

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
constexpr size_t maxWordLength = 32;
static char inputWord[maxWordLength + 1];
static size_t inputLength = 0;
static bool invalidWord = false;

// A line is one request. Reject the whole line if it contains whitespace
// or exceeds the buffer, rather than transmitting a truncated word.
static void processSender(void){
    while (Serial.available()){
        char character = Serial.read();

        if (character == '\r' || character == '\n'){
            if (invalidWord){
                Serial.println("ERROR: enter one word, maximum 32 printable ASCII characters.");
            }
            else if (inputLength > 0){
                if (protocolIsTransmitting()){
                    Serial.println("BUSY: word not sent; try again after SENT.");
                }
                else{
                    char message[maxWordLength + 10];
                    inputWord[inputLength] = 0;
                    uint32_t checksum = messageChecksum(inputWord, inputLength);
                    int length = snprintf(message, sizeof(message), "%s,%08lX",
                                          inputWord, (unsigned long)checksum);

                    if (length > 0 && (size_t)length < sizeof(message) &&
                        protocolTransmit(reinterpret_cast<const uint8_t *>(message),
                                         (uint16_t)length)){
                        Serial.print("SENDING: ");
                        Serial.println(inputWord);
                    }
                    else{
                        Serial.println("ERROR: word not sent.");
                    }
                }
            }
            inputLength = 0;
            invalidWord = false;
        }
        else if (character == '\b' || character == 127){
            if (inputLength > 0 && !invalidWord){
                inputLength--;
            }
        }
        else if (character < '!' || character > '~' ||
                 inputLength >= maxWordLength){
            invalidWord = true;
        }
        else if (!invalidWord){
            inputWord[inputLength++] = character;
        }
    }

    static bool wasTransmitting = false;
    bool transmitting = protocolIsTransmitting();
    if (wasTransmitting && !transmitting){
        Serial.println("SENT: ready for the next word.");
    }
    wasTransmitting = transmitting;
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


// Debug rows are best-effort and nonblocking, like completed-message logs.
static void printReceiverDebug(void){
    static const char *stages[] = {
        "WAITING", "PREAMBLE", "SYNC", "HEADER", "DATA", "READY"
    };
    static const char *errors[] = {
        "none", "preamble mismatch", "sync mismatch", "invalid header/ECC",
        "packet marker mismatch", "uncorrectable packet ECC"
    };
    static ProtocolReceiverDebug previous = {};
    static bool first = true;
    static unsigned long lastStatus = 0;
    ProtocolReceiverDebug debug;
    protocolGetReceiverDebug(&debug);
    char row[192];

    if (first || debug.progress != previous.progress){
        snprintf(row, sizeof(row), "RX STATE: %s\n",
                 debug.progress < 6 ? stages[debug.progress] : "UNKNOWN");
        writeLogRow(row);
    }
    if (debug.errors != previous.errors){
        snprintf(row, sizeof(row), "RX ERROR: %s; total=%lu\n",
                 debug.lastError < 6 ? errors[debug.lastError] : "unknown",
                 (unsigned long)debug.errors);
        writeLogRow(row);
    }
    if (debug.headerBits != previous.headerBits && debug.headerBits > 0 &&
        debug.headerBits % 8 == 0){
        snprintf(row, sizeof(row), "RX HEADER: %u/7 bytes\n", debug.headerBits / 8);
        writeLogRow(row);
    }
    if (debug.wireBytes != previous.wireBytes && debug.wireBytes > 0){
        snprintf(row, sizeof(row), "RX DATA: packet %u, byte %u/14\n",
                 (debug.wireBytes - 1) / PROTOCOL_PACKET_BYTES + 1,
                 (debug.wireBytes - 1) % PROTOCOL_PACKET_BYTES + 1);
        writeLogRow(row);
    }
    if (debug.packets != previous.packets){
        snprintf(row, sizeof(row), "RX PACKET OK: total=%lu, frame length=%u, corrected=%d\n",
                 (unsigned long)debug.packets, debug.length, debug.corrected);
        writeLogRow(row);
    }
    if (first || millis() - lastStatus >= 1000UL){
        lastStatus = millis();
        snprintf(row, sizeof(row), "RX STATUS: GPIO18=%u, edges=%lu, state=%s, logDrops=%lu\n",
                 debug.input, (unsigned long)debug.edges,
                 debug.progress < 6 ? stages[debug.progress] : "UNKNOWN",
                 (unsigned long)loggingDrops);
        writeLogRow(row);
    }
    previous = debug;
    first = false;
}

static void processReceiver(void){
    printReceiverDebug();
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

        if (checksumValid){
            *separator = 0;
        }

        receivedMessages++;
        char row[128];

        snprintf(row, sizeof(row), "RECEIVED,%lu,%s,%d,%s,%lu,%lu\n",
                 millis(), message, correctedSymbols,
                 checksumValid ? "OK" : "BAD",
                 (unsigned long)receivedMessages,
                 (unsigned long)loggingDrops);

        if (checksumValid){
            writeLogRow(row);
        }
        else{
            writeLogRow("ERROR: word checksum failed.\n");
        }
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
    Serial.println("Ready: enter one word (up to 32 characters) and press Enter.");
#else
    Serial.println("Ready: receiver debug on GPIO18; Serial Monitor at 115200 baud.");
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

