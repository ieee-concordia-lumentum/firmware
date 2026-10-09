#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "hardware.h"
#include "protocol.h"

// ============================================================
// USB SERIAL SETTINGS
// ============================================================

static constexpr uint32_t USB_SERIAL_BAUD = 115200;

// The internal CRC trailer is:
// ',' + 8 hexadecimal characters = 9 bytes.
static constexpr uint16_t CRC_TRAILER_BYTES = 9U;

// Make this long enough for the largest message your protocol
// supports at its current transmission speed.
static constexpr unsigned long receptionTimeoutMs = 5000UL;

// ============================================================
// CRC-32
// ============================================================

static uint32_t messageChecksum(const uint8_t *data, size_t length)
{
    uint32_t checksum = 0xFFFFFFFF;

    for (size_t index = 0; index < length; index++)
    {
        checksum ^= data[index];

        for (uint8_t bit = 0; bit < 8; bit++)
        {
            checksum = (checksum >> 1) ^
                       ((checksum & 1U) ? 0xEDB88320U : 0U);
        }
    }

    return ~checksum;
}

#ifdef SENDER

// ============================================================
// DESKTOP -> TRANSMITTER ESP32
//
// USB serial command:
//     SEND <number-of-payload-bytes>\n
//     <exactly that many raw payload bytes>
//
// The payload is UTF-8. It may contain newlines or commas.
//
// The ESP32 appends the CRC trailer before calling
// protocolTransmit().
// ============================================================

static constexpr uint16_t MAX_DESKTOP_PAYLOAD_BYTES =
    (PROTOCOL_MAX_MESSAGE_BYTES > CRC_TRAILER_BYTES)
        ? (PROTOCOL_MAX_MESSAGE_BYTES - CRC_TRAILER_BYTES)
        : 0U;

static char senderCommandLine[32];
static size_t senderCommandLineLength = 0;
static bool senderDiscardCommandLine = false;

static bool senderReadingPayload = false;
static bool senderPayloadTooLong = false;

static uint32_t senderExpectedPayloadLength = 0;
static uint32_t senderPayloadBytesReceived = 0;

static uint8_t senderPayload[PROTOCOL_MAX_MESSAGE_BYTES];

// Transmission status
static bool transmissionPending = false;
static uint32_t nextTransmissionId = 0;
static uint32_t activeTransmissionId = 0;

static void senderPrintReady(void)
{
    Serial.printf(
        "READY,SENDER,%u\n",
        static_cast<unsigned>(MAX_DESKTOP_PAYLOAD_BYTES));
}

static void senderPrintError(const char *message)
{
    Serial.print("ERR,");
    Serial.println(message);
}

// Called after the entire SEND payload has arrived over USB.
static void finishDesktopPayload(void)
{
    const uint32_t payloadLength = senderExpectedPayloadLength;

    if (senderPayloadTooLong ||
        payloadLength > MAX_DESKTOP_PAYLOAD_BYTES)
    {

        Serial.printf(
            "ERR,TOO_LONG,%u\n",
            static_cast<unsigned>(MAX_DESKTOP_PAYLOAD_BYTES));

        return;
    }

    if (transmissionPending || protocolIsTransmitting())
    {
        senderPrintError("BUSY");
        return;
    }

    // The text plus its CRC must fit the protocol's message buffer.
    if (payloadLength + CRC_TRAILER_BYTES >
        PROTOCOL_MAX_MESSAGE_BYTES)
    {

        senderPrintError("TOO_LONG");
        return;
    }

    uint8_t wireMessage[PROTOCOL_MAX_MESSAGE_BYTES];

    // Preserve the exact original UTF-8 bytes.
    memcpy(wireMessage, senderPayload, payloadLength);

    // Calculate CRC over the original text only.
    const uint32_t checksum =
        messageChecksum(senderPayload, payloadLength);

    // Append the same trailer format used by your original test.
    char checksumText[9];

    snprintf(
        checksumText,
        sizeof(checksumText),
        "%08lX",
        static_cast<unsigned long>(checksum));

    wireMessage[payloadLength] = ',';

    memcpy(
        wireMessage + payloadLength + 1,
        checksumText,
        8);

    const uint16_t wireLength = static_cast<uint16_t>(
        payloadLength + CRC_TRAILER_BYTES);

    // protocolTransmit() copies the data into its own TX buffers.
    if (!protocolTransmit(wireMessage, wireLength))
    {
        senderPrintError("TRANSMIT_REJECTED");
        return;
    }

    activeTransmissionId = ++nextTransmissionId;

    if (activeTransmissionId == 0)
    {
        activeTransmissionId = ++nextTransmissionId;
    }

    transmissionPending = true;

    Serial.printf(
        "TX_ACCEPTED,%lu,%lu\n",
        static_cast<unsigned long>(activeTransmissionId),
        static_cast<unsigned long>(payloadLength));
}

// Parse one ASCII command line.
static void handleSenderCommandLine(void)
{
    senderCommandLine[senderCommandLineLength] = '\0';

    // The desktop requests this after opening the serial port.
    // Reply with the maximum supported desktop payload size.
    if (strcmp(senderCommandLine, "INFO") == 0)
    {
        senderPrintReady();
        return;
    }

    if (strncmp(senderCommandLine, "SEND ", 5) != 0)
    {
        senderPrintError("UNKNOWN_COMMAND");
        return;
    }

    const char *numberText = senderCommandLine + 5;
    char *endPointer = nullptr;

    const unsigned long requestedLength =
        strtoul(numberText, &endPointer, 10);

    if (numberText == endPointer ||
        *endPointer != '\0' ||
        requestedLength == 0 ||
        requestedLength > 65535UL)
    {

        senderPrintError("INVALID_LENGTH");
        return;
    }

    // Oversized but otherwise valid requests are consumed and discarded
    // so their binary payload cannot be mistaken for later commands.
    senderExpectedPayloadLength =
        static_cast<uint32_t>(requestedLength);

    senderPayloadBytesReceived = 0;

    senderPayloadTooLong =
        requestedLength > MAX_DESKTOP_PAYLOAD_BYTES;

    senderReadingPayload = true;
}

// Read incoming USB data without blocking the protocol timer.
static void processSender(void)
{
    while (Serial.available() > 0)
    {
        const int incoming = Serial.read();

        if (incoming < 0)
        {
            break;
        }

        const uint8_t value = static_cast<uint8_t>(incoming);

        if (senderReadingPayload)
        {
            if (!senderPayloadTooLong &&
                senderPayloadBytesReceived <
                    sizeof(senderPayload))
            {

                senderPayload[senderPayloadBytesReceived] = value;
            }

            senderPayloadBytesReceived++;

            if (senderPayloadBytesReceived >=
                senderExpectedPayloadLength)
            {

                senderReadingPayload = false;
                finishDesktopPayload();
            }

            continue;
        }

        // Ignore the CR in Windows-style CRLF command endings.
        if (value == '\r')
        {
            continue;
        }

        if (value == '\n')
        {
            if (senderDiscardCommandLine)
            {
                senderPrintError("COMMAND_TOO_LONG");
            }
            else
            {
                handleSenderCommandLine();
            }

            senderCommandLineLength = 0;
            senderDiscardCommandLine = false;
            continue;
        }

        if (senderDiscardCommandLine)
        {
            continue;
        }

        if (senderCommandLineLength <
            sizeof(senderCommandLine) - 1U)
        {

            senderCommandLine[senderCommandLineLength++] =
                static_cast<char>(value);
        }
        else
        {
            senderDiscardCommandLine = true;
        }
    }

    // protocolTransmitTick() runs from your hardware timer.
    // Detect when the asynchronous transmission has finished.
    if (transmissionPending && !protocolIsTransmitting())
    {
        Serial.printf(
            "TX_DONE,%lu\n",
            static_cast<unsigned long>(activeTransmissionId));

        transmissionPending = false;
    }
}

#else

// ============================================================
// RECEIVER ESP32 -> DESKTOP
//
// USB serial response format:
//
//   RX,<payload length>,<corrected symbols>,<CRC status>,<count>\n
//   <exactly payload length raw UTF-8 bytes>\n
//
// The desktop reads the length before consuming the payload.
// Therefore, newlines and commas in received text are safe.
// ============================================================

static bool receiving = false;
static unsigned long receptionStarted = 0;

static uint32_t receivedMessages = 0;
static uint32_t loggingDrops = 0;

// The queue keeps USB logging from blocking the bit-reception timer.
static constexpr size_t SERIAL_TX_QUEUE_CAPACITY =
    2U * (static_cast<size_t>(PROTOCOL_MAX_MESSAGE_BYTES) + 96U);

static uint8_t serialTxQueue[SERIAL_TX_QUEUE_CAPACITY];

static size_t serialTxHead = 0;
static size_t serialTxTail = 0;
static size_t serialTxCount = 0;

// Append bytes to the queue. Only called from the main loop.
static void queueSerialBytes(const uint8_t *data, size_t length)
{
    for (size_t index = 0; index < length; index++)
    {
        serialTxQueue[serialTxHead] = data[index];

        serialTxHead =
            (serialTxHead + 1U) % SERIAL_TX_QUEUE_CAPACITY;

        serialTxCount++;
    }
}

static void flushSerialQueue(void)
{
    int available = Serial.availableForWrite();

    while (available > 0 && serialTxCount > 0)
    {
        size_t contiguous;

        if (serialTxHead > serialTxTail)
        {
            contiguous = serialTxHead - serialTxTail;
        }
        else
        {
            contiguous = SERIAL_TX_QUEUE_CAPACITY - serialTxTail;
        }

        size_t chunkLength = contiguous;

        if (chunkLength > serialTxCount)
        {
            chunkLength = serialTxCount;
        }

        if (chunkLength > static_cast<size_t>(available))
        {
            chunkLength = static_cast<size_t>(available);
        }

        const size_t written = Serial.write(
            serialTxQueue + serialTxTail,
            chunkLength);

        if (written == 0)
        {
            break;
        }

        serialTxTail =
            (serialTxTail + written) % SERIAL_TX_QUEUE_CAPACITY;

        serialTxCount -= written;
        available -= static_cast<int>(written);
    }
}

// Parse exactly eight hexadecimal characters without relying on
// a null terminator in the incoming message buffer.
static bool parseHex8(const uint8_t *text, uint32_t *result)
{
    uint32_t value = 0;

    for (uint8_t index = 0; index < 8; index++)
    {
        const uint8_t character = text[index];
        uint8_t digit;

        if (character >= '0' && character <= '9')
        {
            digit = character - '0';
        }
        else if (character >= 'A' && character <= 'F')
        {
            digit = character - 'A' + 10;
        }
        else if (character >= 'a' && character <= 'f')
        {
            digit = character - 'a' + 10;
        }
        else
        {
            return false;
        }

        value = (value << 4) | digit;
    }

    *result = value;
    return true;
}

// Put one complete message record into the nonblocking serial queue.
static void queueReceivedMessage(
    const uint8_t *message,
    uint16_t payloadLength,
    int16_t correctedSymbols,
    bool checksumValid,
    uint32_t messageNumber)
{
    char header[72];

    const int headerLength = snprintf(
        header,
        sizeof(header),
        "RX,%u,%d,%s,%lu\n",
        static_cast<unsigned>(payloadLength),
        static_cast<int>(correctedSymbols),
        checksumValid ? "OK" : "BAD",
        static_cast<unsigned long>(messageNumber));

    if (headerLength <= 0 ||
        static_cast<size_t>(headerLength) >= sizeof(header))
    {

        loggingDrops++;
        return;
    }

    // One extra newline separates the payload from the next record.
    const size_t requiredBytes =
        static_cast<size_t>(headerLength) +
        payloadLength + 1U;

    if (requiredBytes >
        SERIAL_TX_QUEUE_CAPACITY - serialTxCount)
    {

        loggingDrops++;
        return;
    }

    queueSerialBytes(
        reinterpret_cast<const uint8_t *>(header),
        static_cast<size_t>(headerLength));

    if (payloadLength > 0)
    {
        queueSerialBytes(message, payloadLength);
    }

    const uint8_t delimiter = '\n';
    queueSerialBytes(&delimiter, 1);
}

static void processReceiver(void)
{
    const enum ReceiveProgress progress =
        protocolGetReceiverProgress();

    if (progress == PROG_WAITING)
    {
        receiving = false;
    }
    else if (!receiving)
    {
        receiving = true;
        receptionStarted = millis();
    }

    if (protocolIsReady())
    {
        const uint16_t wireLength = protocolGetMessageLength();

        if (wireLength > PROTOCOL_MAX_MESSAGE_BYTES)
        {
            protocolResetReceiver();
            receiving = false;
            return;
        }

        uint8_t message[PROTOCOL_MAX_MESSAGE_BYTES];

        // Copy before requesting the asynchronous protocol reset.
        memcpy(
            message,
            protocolGetMessageData(),
            wireLength);

        const int16_t correctedSymbols =
            protocolGetCorrectedSymbolCount();

        uint16_t payloadLength = wireLength;
        bool checksumValid = false;

        // Our transmitter appends:
        //     ',' + eight hexadecimal CRC-32 digits.
        //
        // Its location is based on the message length, not the last comma
        // in the text, so commas and newlines in the original text work.
        if (wireLength >= CRC_TRAILER_BYTES &&
            message[wireLength - CRC_TRAILER_BYTES] == ',')
        {

            uint32_t expectedChecksum;

            if (parseHex8(
                    message + wireLength - 8U,
                    &expectedChecksum))
            {

                payloadLength = wireLength - CRC_TRAILER_BYTES;

                checksumValid =
                    messageChecksum(message, payloadLength) ==
                    expectedChecksum;
            }
        }

        receivedMessages++;

        queueReceivedMessage(
            message,
            payloadLength,
            correctedSymbols,
            checksumValid,
            receivedMessages);

        protocolResetReceiver();
        receiving = false;
        return;
    }

    // This is a maximum whole-frame timeout, not a per-bit timeout.
    // It is intentionally longer than the old 250 ms limit.
    if (receiving &&
        millis() - receptionStarted >= receptionTimeoutMs)
    {

        protocolResetReceiver();
        receiving = false;
    }
}

#endif

// ============================================================
// ARDUINO ENTRY POINTS
// ============================================================

void setup()
{
    Serial.begin(USB_SERIAL_BAUD);

    // Preserve the initialization order required by your hardware timer.
    protocolInit();
    hardwareInit();

#ifdef SENDER
    senderPrintReady();
#else
    Serial.println("READY,RECEIVER");
#endif
}

void loop()
{
    protocolProcess();

#ifdef SENDER
    processSender();
#else
    processReceiver();
    flushSerialQueue();
#endif
}