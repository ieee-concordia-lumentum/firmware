#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "protocol.h"

static constexpr uint16_t CRC_TRAILER_BYTES = 9U;
static constexpr uint16_t MAX_DESKTOP_PAYLOAD_BYTES =
    PROTOCOL_MAX_MESSAGE_BYTES - CRC_TRAILER_BYTES;

// Build from the project directory:
// g++ -std=c++11 -DDESKTOP -Iinclude src/Desktop/main.cpp src/protocol.c -o .pio/desktop.exe
// Start receiving first: .pio/desktop.exe toReceive COM4
// Then send:            .pio/desktop.exe toTransmit COM3 "Hello"

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

static HANDLE openSerialPort(const char *portName)
{
    char deviceName[80];

    if (strlen(portName) > sizeof(deviceName) - 5U)
    {
        return INVALID_HANDLE_VALUE;
    }

    snprintf(deviceName, sizeof(deviceName), "\\\\.\\%s", portName);

    HANDLE port = CreateFileA(deviceName, GENERIC_READ | GENERIC_WRITE,
                              0, NULL, OPEN_EXISTING, 0, NULL);

    if (port == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "Cannot open %s. Close other serial monitors. Windows error %lu.\n",
                portName, static_cast<unsigned long>(GetLastError()));
        return port;
    }

    DCB settings = {};
    settings.DCBlength = sizeof(settings);

    if (!GetCommState(port, &settings))
    {
        CloseHandle(port);
        return INVALID_HANDLE_VALUE;
    }

    settings.BaudRate = 115200;
    settings.ByteSize = 8;
    settings.Parity = NOPARITY;
    settings.StopBits = ONESTOPBIT;
    settings.fBinary = TRUE;
    settings.fParity = FALSE;
    settings.fOutxCtsFlow = FALSE;
    settings.fOutxDsrFlow = FALSE;
    settings.fDtrControl = DTR_CONTROL_DISABLE;
    settings.fRtsControl = RTS_CONTROL_DISABLE;
    settings.fDsrSensitivity = FALSE;
    settings.fOutX = FALSE;
    settings.fInX = FALSE;
    settings.fErrorChar = FALSE;
    settings.fNull = FALSE;
    settings.fAbortOnError = FALSE;

    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutConstant = 2000;

    if (!SetCommState(port, &settings) || !SetCommTimeouts(port, &timeouts) ||
        !PurgeComm(port, PURGE_RXCLEAR | PURGE_TXCLEAR))
    {
        fprintf(stderr, "Cannot configure %s.\n", portName);
        CloseHandle(port);
        return INVALID_HANDLE_VALUE;
    }

    return port;
}

static bool writeSerialBytes(HANDLE port, const uint8_t *data, size_t length)
{
    size_t offset = 0;

    while (offset < length)
    {
        DWORD written = 0;

        if (!WriteFile(port, data + offset, static_cast<DWORD>(length - offset),
                       &written, NULL) || written == 0)
        {
            fprintf(stderr, "Serial write failed or timed out.\n");
            return false;
        }

        offset += written;
    }

    return true;
}

static bool readSerialBytes(HANDLE port, uint8_t *data, size_t length, DWORD timeoutMs)
{
    const DWORD started = GetTickCount();
    size_t offset = 0;

    while (offset < length)
    {
        DWORD received = 0;

        if (!ReadFile(port, data + offset, static_cast<DWORD>(length - offset),
                      &received, NULL))
        {
            return false;
        }

        offset += received;

        if (timeoutMs != 0 && GetTickCount() - started >= timeoutMs && offset < length)
        {
            return false;
        }
    }

    return true;
}

static bool readSerialLine(HANDLE port, char *line, size_t capacity, DWORD timeoutMs)
{
    const DWORD started = GetTickCount();
    size_t length = 0;

    while (timeoutMs == 0 || GetTickCount() - started < timeoutMs)
    {
        uint8_t value;
        DWORD remaining = timeoutMs == 0 ? 0 : timeoutMs - (GetTickCount() - started);

        if (timeoutMs != 0 && (remaining == 0 || remaining > timeoutMs))
        {
            return false;
        }
        if (!readSerialBytes(port, &value, 1, remaining))
        {
            return false;
        }
        if (value == '\r')
        {
            continue;
        }
        if (value == '\n')
        {
            line[length] = '\0';
            return true;
        }
        if (length == capacity - 1U)
        {
            return false;
        }

        line[length++] = static_cast<char>(value);
    }

    return false;
}

static bool connectDesktop(HANDLE port, const char *role)
{
    const uint8_t command[] = "INFO\n";
    char expected[48];
    snprintf(expected, sizeof(expected), "READY,%s,%u", role,
             static_cast<unsigned>(PROTOCOL_FRAME_BYTES));

    for (uint8_t attempt = 0; attempt < 8; attempt++)
    {
        if (!writeSerialBytes(port, command, sizeof(command) - 1U))
        {
            return false;
        }

        const DWORD started = GetTickCount();
        char reply[128];

        while (GetTickCount() - started < 700UL)
        {
            if (!readSerialLine(port, reply, sizeof(reply), 700UL))
            {
                break;
            }
            if (strcmp(reply, expected) == 0)
            {
                return true;
            }
        }
    }

    fprintf(stderr, "No matching %s firmware response. Check the COM port and flash both boards.\n", role);
    return false;
}

static bool toTransmit(HANDLE port, const uint8_t *payload, uint16_t payloadLength)
{
    if (payloadLength > MAX_DESKTOP_PAYLOAD_BYTES)
    {
        fprintf(stderr, "Maximum message length is %u bytes.\n",
                static_cast<unsigned>(MAX_DESKTOP_PAYLOAD_BYTES));
        return false;
    }

    uint8_t message[PROTOCOL_MAX_MESSAGE_BYTES];
    memcpy(message, payload, payloadLength);
    message[payloadLength] = ',';

    char checksumText[9];
    snprintf(checksumText, sizeof(checksumText), "%08lX",
             static_cast<unsigned long>(messageChecksum(payload, payloadLength)));
    memcpy(message + payloadLength + 1U, checksumText, 8);

    uint8_t frame[PROTOCOL_FRAME_BYTES];

    if (!protocolEncode(message, payloadLength + CRC_TRAILER_BYTES, frame))
    {
        return false;
    }

    char command[32];
    int commandLength = snprintf(command, sizeof(command), "SEND %u\n",
                                 static_cast<unsigned>(sizeof(frame)));

    if (!writeSerialBytes(port, reinterpret_cast<const uint8_t *>(command), commandLength) ||
        !writeSerialBytes(port, frame, sizeof(frame)))
    {
        return false;
    }

    const DWORD started = GetTickCount();
    unsigned long transmissionId = 0;
    bool accepted = false;
    char reply[128];

    while (GetTickCount() - started < 5000UL)
    {
        if (!readSerialLine(port, reply, sizeof(reply), 1000UL))
        {
            break;
        }
        if (strncmp(reply, "ERR,", 4) == 0)
        {
            fprintf(stderr, "%s\n", reply);
            return false;
        }
        if (sscanf(reply, "TX_ACCEPTED,%lu", &transmissionId) == 1)
        {
            accepted = true;
        }

        unsigned long completedId = 0;

        if (accepted && sscanf(reply, "TX_DONE,%lu", &completedId) == 1 &&
            completedId == transmissionId)
        {
            printf("Transmitted %u bytes.\n", static_cast<unsigned>(payloadLength));
            // Allow the receiver to forward its captured frame before the next send.
            // This is local pacing, not a remote delivery acknowledgment.
            Sleep(200);
            return true;
        }
    }

    fprintf(stderr, "Transmission timed out.\n");
    return false;
}

static bool toReceive(HANDLE port)
{
    char header[128];
    printf("Listening. Start the sender now. Ctrl+C stops receiving.\n");

    while (readSerialLine(port, header, sizeof(header), 0))
    {
        if (strncmp(header, "RX,", 3) != 0)
        {
            continue;
        }

        unsigned frameLength;
        unsigned long messageNumber;
        int consumed = 0;

        if (sscanf(header, "RX,%u,%lu%n", &frameLength, &messageNumber, &consumed) != 2 ||
            header[consumed] != '\0' || frameLength != PROTOCOL_FRAME_BYTES)
        {
            fprintf(stderr, "Invalid receiver record. Reconnect and retry.\n");
            return false;
        }

        uint8_t frame[PROTOCOL_FRAME_BYTES];
        uint8_t delimiter;

        if (!readSerialBytes(port, frame, sizeof(frame), 3000UL) ||
            !readSerialBytes(port, &delimiter, 1, 1000UL) || delimiter != '\n')
        {
            fprintf(stderr, "Incomplete receiver record. Reconnect and retry.\n");
            return false;
        }

        uint8_t message[PROTOCOL_MAX_MESSAGE_BYTES];
        uint16_t messageLength;
        int16_t correctedSymbols;

        if (!protocolDecode(frame, sizeof(frame), message, &messageLength, &correctedSymbols))
        {
            fprintf(stderr, "Message %lu rejected: invalid packet or uncorrectable ECC.\n", messageNumber);
            continue;
        }

        uint32_t expectedChecksum;

        if (messageLength < CRC_TRAILER_BYTES ||
            message[messageLength - CRC_TRAILER_BYTES] != ',' ||
            !parseHex8(message + messageLength - 8U, &expectedChecksum) ||
            messageChecksum(message, messageLength - CRC_TRAILER_BYTES) != expectedChecksum)
        {
            fprintf(stderr, "Message %lu rejected: CRC mismatch.\n", messageNumber);
            continue;
        }

        printf("Received %lu, corrected symbols %d, CRC OK: ",
               messageNumber, static_cast<int>(correctedSymbols));
        fwrite(message, 1, messageLength - CRC_TRAILER_BYTES, stdout);
        putchar('\n');
        fflush(stdout);
    }

    fprintf(stderr, "Serial connection failed.\n");
    return false;
}

int main(int argc, char *argv[])
{
    if (argc < 3 || (strcmp(argv[1], "toTransmit") != 0 && strcmp(argv[1], "toReceive") != 0) ||
        argc > 4 || (strcmp(argv[1], "toReceive") == 0 && argc != 3))
    {
        printf("Usage:\n  desktop.exe toTransmit COM3 [\"message\"]\n  desktop.exe toReceive COM4\n");
        return 1;
    }

    HANDLE port = openSerialPort(argv[2]);

    if (port == INVALID_HANDLE_VALUE)
    {
        return 1;
    }

    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    protocolInit();

    const bool sender = strcmp(argv[1], "toTransmit") == 0;
    bool success = connectDesktop(port, sender ? "SENDER" : "RECEIVER");

    if (success && !sender)
    {
        success = toReceive(port);
    }
    else if (success && argc == 4)
    {
        const size_t length = strlen(argv[3]);

        if (length > MAX_DESKTOP_PAYLOAD_BYTES)
        {
            fprintf(stderr, "Maximum message length is %u bytes.\n",
                    static_cast<unsigned>(MAX_DESKTOP_PAYLOAD_BYTES));
            success = false;
        }
        else
        {
            success = toTransmit(port, reinterpret_cast<const uint8_t *>(argv[3]), length);
        }
    }
    else if (success)
    {
        char message[PROTOCOL_MAX_MESSAGE_BYTES + 2U];
        printf("Enter messages, up to %u bytes each. Ctrl+Z then Enter exits.\n",
               static_cast<unsigned>(MAX_DESKTOP_PAYLOAD_BYTES));

        while (fgets(message, sizeof(message), stdin) != NULL)
        {
            size_t length = strlen(message);

            if (length != 0 && message[length - 1U] == '\n')
            {
                message[--length] = '\0';
            }
            else if (!feof(stdin))
            {
                int value;
                while ((value = getchar()) != '\n' && value != EOF) {}
            }
            if (length != 0 && message[length - 1U] == '\r')
            {
                message[--length] = '\0';
            }
            if (length > MAX_DESKTOP_PAYLOAD_BYTES)
            {
                fprintf(stderr, "Message too long.\n");
                continue;
            }
            if (!toTransmit(port, reinterpret_cast<const uint8_t *>(message), length))
            {
                success = false;
                break;
            }
        }
    }

    CloseHandle(port);
    return success ? 0 : 1;
}
