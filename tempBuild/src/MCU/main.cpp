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

// Physical frames contain bytes already encoded by the desktop.
static constexpr unsigned long receptionTimeoutMs = 1000UL;

#ifdef SENDER

// ============================================================
// DESKTOP -> TRANSMITTER ESP32
//
// USB serial command:
//     SEND <number-of-payload-bytes>\n
//     <exactly that many raw payload bytes>
//
// The payload is an encoded frame, not application text.
// ============================================================

static constexpr uint16_t MAX_DESKTOP_PAYLOAD_BYTES = PROTOCOL_FRAME_BYTES;

static char senderCommandLine[32];
static size_t senderCommandLineLength = 0;
static bool senderDiscardCommandLine = false;

static bool senderReadingPayload = false;
static bool senderPayloadTooLong = false;

static uint32_t senderExpectedPayloadLength = 0;
static uint32_t senderPayloadBytesReceived = 0;

static uint8_t senderPayload[PROTOCOL_FRAME_BYTES];
static unsigned long senderPayloadStarted = 0;

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

    // Only the desktop constructs packets and performs CRC/ECC.
    if (!protocolTransmit(senderPayload, static_cast<uint16_t>(payloadLength)))
    {
        senderPrintError("INVALID_FRAME");
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
    senderPayloadStarted = millis();
}

// Read incoming USB data without blocking the protocol timer.
static void processSender(void)
{
    if (senderReadingPayload && millis() - senderPayloadStarted >= 5000UL)
    {
        senderReadingPayload = false;
        senderPrintError("SERIAL_TIMEOUT");
    }

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
//   RX,<encoded frame length>,<count>\n
//   <exactly that many encoded bytes>\n
//
// Packet reconstruction, ECC, and CRC validation run on the desktop.
// ============================================================

static bool receiving = false;
static bool receiverForwarding = false;
static unsigned long receptionStarted = 0;

static uint32_t receivedMessages = 0;
static uint32_t loggingDrops = 0;

// The queue keeps USB logging from blocking the bit-reception timer.
static constexpr size_t SERIAL_TX_QUEUE_CAPACITY =
    2U * (static_cast<size_t>(PROTOCOL_FRAME_BYTES) + 96U);

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

// Put one complete message record into the nonblocking serial queue.
static void queueReceivedMessage(
    const uint8_t *message,
    uint16_t payloadLength,
    uint32_t messageNumber)
{
    char header[72];

    const int headerLength = snprintf(
        header,
        sizeof(header),
        "RX,%u,%lu\n",
        static_cast<unsigned>(payloadLength),
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

// INFO replies use the same queue so they cannot interrupt a binary record.
static void processReceiverCommands(void)
{
    static char commandLine[16];
    static size_t commandLength = 0;
    static bool discardCommand = false;

    while (Serial.available() > 0)
    {
        const int value = Serial.read();

        if (value < 0)
        {
            break;
        }
        if (value == '\r')
        {
            continue;
        }
        if (value == '\n')
        {
            commandLine[commandLength] = '\0';

            if (!discardCommand && strcmp(commandLine, "INFO") == 0)
            {
                char reply[40];
                int length = snprintf(reply, sizeof(reply), "READY,RECEIVER,%u\n",
                                      static_cast<unsigned>(PROTOCOL_FRAME_BYTES));

                if (length > 0 && static_cast<size_t>(length) < sizeof(reply) &&
                    static_cast<size_t>(length) <= SERIAL_TX_QUEUE_CAPACITY - serialTxCount)
                {
                    queueSerialBytes(reinterpret_cast<const uint8_t *>(reply), length);
                }
            }

            commandLength = 0;
            discardCommand = false;
        }
        else if (commandLength < sizeof(commandLine) - 1U)
        {
            commandLine[commandLength++] = static_cast<char>(value);
        }
        else
        {
            discardCommand = true;
        }
    }
}

static void processReceiver(void)
{
    processReceiverCommands();

    // Keep the captured frame immutable until all queued USB bytes are sent.
    if (receiverForwarding)
    {
        if (serialTxCount == 0)
        {
            protocolResetReceiver();
            receiverForwarding = false;
            receiving = false;
        }
        return;
    }

    const enum ReceiveProgress progress = protocolGetReceiverProgress();

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
        if (SERIAL_TX_QUEUE_CAPACITY - serialTxCount < PROTOCOL_FRAME_BYTES + 72U)
        {
            return;
        }

        const uint16_t wireLength = protocolGetMessageLength();

        if (wireLength != PROTOCOL_FRAME_BYTES)
        {
            protocolResetReceiver();
            receiving = false;
            return;
        }

        queueReceivedMessage(protocolGetMessageData(), wireLength, ++receivedMessages);
        receiverForwarding = true;
        return;
    }

    if (receiving && millis() - receptionStarted >= receptionTimeoutMs)
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
    Serial.printf("READY,RECEIVER,%u\n",
                  static_cast<unsigned>(PROTOCOL_FRAME_BYTES));
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