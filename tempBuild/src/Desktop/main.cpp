#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

#include "protocol.h"

static constexpr uint16_t CRC_TRAILER_BYTES = 9U;
static constexpr uint16_t MAX_DESKTOP_PAYLOAD_BYTES =
    PROTOCOL_MAX_MESSAGE_BYTES - CRC_TRAILER_BYTES;

// Build from the project directory:
// g++ -std=c++11 -DDESKTOP -Iinclude src/Desktop/main.cpp src/protocol.c -o .pio/desktop.exe
// Start receiving first: .pio/desktop.exe toReceive COM4
// Then send .txt/.png/.jpg/.jpeg files:  .pio/desktop.exe toTransmit COM3

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

static constexpr uint16_t FILE_HEADER_BYTES = 13U;
static constexpr uint16_t FILE_DATA_BYTES = MAX_DESKTOP_PAYLOAD_BYTES - FILE_HEADER_BYTES;

static void storeFileNumber(uint8_t *data, uint32_t value)
{
    for (unsigned index = 0; index < 4; index++)
    {
        data[index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

static uint32_t readFileNumber(const uint8_t *data)
{
    uint32_t value = 0;
    for (unsigned index = 0; index < 4; index++)
    {
        value |= static_cast<uint32_t>(data[index]) << (index * 8);
    }
    return value;
}

static void updateFileChecksum(uint32_t *checksum, const uint8_t *data, size_t length)
{
    for (size_t index = 0; index < length; index++)
    {
        *checksum ^= data[index];
        for (unsigned bit = 0; bit < 8; bit++)
        {
            *checksum = (*checksum >> 1) ^ ((*checksum & 1U) ? 0xEDB88320U : 0U);
        }
    }
}

static bool supportedFileExtension(const char *name)
{
    const char *extension = strrchr(name, '.');
    return extension != NULL &&
           (lstrcmpiA(extension, ".txt") == 0 ||
            lstrcmpiA(extension, ".png") == 0 ||
            lstrcmpiA(extension, ".jpg") == 0 ||
            lstrcmpiA(extension, ".jpeg") == 0);
}
static bool validFileName(const char *name)
{
    const size_t length = strlen(name);
    if (length < 5 || length > FILE_DATA_BYTES ||
        !supportedFileExtension(name) || name[0] == '.')
    {
        return false;
    }
    for (size_t index = 0; index < length; index++)
    {
        if (static_cast<unsigned char>(name[index]) < 32 ||
            strchr("\\/:*?\"<>|", name[index]) != NULL)
        {
            return false;
        }
    }
    // Windows device names remain reserved even with a file extension.
    char base[FILE_DATA_BYTES + 1U];
    strcpy(base, name);
    *strchr(base, '.') = '\0';
    if (lstrcmpiA(base, "CON") == 0 || lstrcmpiA(base, "PRN") == 0 ||
        lstrcmpiA(base, "AUX") == 0 || lstrcmpiA(base, "NUL") == 0 ||
        (strlen(base) == 4 && base[3] >= '1' && base[3] <= '9' &&
         (lstrcmpiA(std::string(base, 3).c_str(), "COM") == 0 ||
          lstrcmpiA(std::string(base, 3).c_str(), "LPT") == 0)))
    {
        return false;
    }
    return true;
}

static void makeFileHeader(uint8_t *message, uint8_t type, uint32_t id, uint32_t offset)
{
    memcpy(message, "FT01", 4);
    message[4] = type;
    storeFileNumber(message + 5, id);
    storeFileNumber(message + 9, offset);
}

static bool transmitFile(HANDLE port, const char *name, uint32_t id)
{
    if (!validFileName(name))
    {
        fprintf(stderr, "Unsupported filename: %s\n", name);
        return false;
    }
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "toTransmit\\%s", name);
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER size;
    if (file == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "Cannot open %s.\n", path);
        return false;
    }
    if (!GetFileSizeEx(file, &size) || size.QuadPart > UINT32_MAX)
    {
        CloseHandle(file);
        fprintf(stderr, "Cannot read file size, or file exceeds 4 GB.\n");
        return false;
    }
    const uint32_t length = static_cast<uint32_t>(size.QuadPart);
    uint8_t message[MAX_DESKTOP_PAYLOAD_BYTES];
    makeFileHeader(message, 'B', id, length);
    memcpy(message + FILE_HEADER_BYTES, name, strlen(name));
    printf("Sending %s (%lu bytes).\n", name, static_cast<unsigned long>(length));
    bool success = toTransmit(port, message, FILE_HEADER_BYTES + strlen(name));
    uint32_t offset = 0;
    uint32_t checksum = 0xFFFFFFFFU;
    while (success && offset < length)
    {
        const DWORD remaining = length - offset;
        const DWORD requested = remaining < FILE_DATA_BYTES ? remaining : FILE_DATA_BYTES;
        DWORD received = 0;
        if (!ReadFile(file, message + FILE_HEADER_BYTES, requested, &received, NULL) ||
            received != requested)
        {
            fprintf(stderr, "Cannot read %s.\n", path);
            success = false;
            break;
        }
        makeFileHeader(message, 'D', id, offset);
        updateFileChecksum(&checksum, message + FILE_HEADER_BYTES, received);
        success = toTransmit(port, message, FILE_HEADER_BYTES + received);
        offset += received;
    }
    CloseHandle(file);
    if (success)
    {
        makeFileHeader(message, 'E', id, length);
        storeFileNumber(message + FILE_HEADER_BYTES, ~checksum);
        success = toTransmit(port, message, FILE_HEADER_BYTES + 4U);
    }
    return success;
}

static bool transmitFiles(HANDLE port)
{
    WIN32_FIND_DATAA entry;
    HANDLE search = FindFirstFileA("toTransmit\\*", &entry);
    if (search == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "Cannot scan toTransmit.\n");
        return false;
    }
    std::vector<std::string> names;
    do
    {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            supportedFileExtension(entry.cFileName))
        {
            names.push_back(entry.cFileName);
        }
    } while (FindNextFileA(search, &entry));
    const DWORD searchError = GetLastError();
    FindClose(search);
    if (searchError != ERROR_NO_MORE_FILES)
    {
        fprintf(stderr, "Cannot finish scanning toTransmit.\n");
        return false;
    }
    std::sort(names.begin(), names.end());
    for (size_t index = 0; index < names.size(); index++)
    {
        if (!transmitFile(port, names[index].c_str(), static_cast<uint32_t>(index + 1)))
        {
            return false;
        }
    }
    printf("Finished sending %u files. Check the receiver for delivery.\n",
           static_cast<unsigned>(names.size()));
    return true;
}

struct FileReception
{
    HANDLE file = INVALID_HANDLE_VALUE;
    char temporary[MAX_PATH] = {};
    char destination[MAX_PATH] = {};
    uint32_t id = 0;
    uint32_t length = 0;
    uint32_t offset = 0;
    uint32_t checksum = 0xFFFFFFFFU;

    void discard()
    {
        if (file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
        }
        if (temporary[0] != '\0')
        {
            DeleteFileA(temporary);
            temporary[0] = '\0';
        }
    }

    ~FileReception()
    {
        discard();
    }
};

static bool receiveFileMessage(FileReception *state, const uint8_t *message, uint16_t length)
{
    if (length < FILE_HEADER_BYTES || memcmp(message, "FT01", 4) != 0)
    {
        fprintf(stderr, "Ignored message without a file header.\n");
        return false;
    }
    const uint32_t id = readFileNumber(message + 5);
    const uint32_t offset = readFileNumber(message + 9);
    const uint8_t *data = message + FILE_HEADER_BYTES;
    const uint16_t dataLength = length - FILE_HEADER_BYTES;
    if (message[4] == 'B')
    {
        state->discard();
        char name[FILE_DATA_BYTES + 1U];
        if (dataLength == 0 || dataLength > FILE_DATA_BYTES || memchr(data, 0, dataLength))
        {
            return false;
        }
        memcpy(name, data, dataLength);
        name[dataLength] = '\0';
        if (!validFileName(name))
        {
            fprintf(stderr, "Rejected invalid filename.\n");
            return false;
        }
        snprintf(state->destination, sizeof(state->destination), "toReceive\\%s", name);
        if (GetFileAttributesA(state->destination) != INVALID_FILE_ATTRIBUTES)
        {
            fprintf(stderr, "Already exists: %s. Move it before retrying.\n", state->destination);
            return false;
        }
        if (!GetTempFileNameA("toReceive", "rx", 0, state->temporary))
        {
            fprintf(stderr, "Cannot create receiver temporary file.\n");
            return false;
        }
        state->file = CreateFileA(state->temporary, GENERIC_WRITE, 0, NULL,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (state->file == INVALID_HANDLE_VALUE)
        {
            state->discard();
            return false;
        }
        state->id = id;
        state->length = offset;
        state->offset = 0;
        state->checksum = 0xFFFFFFFFU;
        printf("Receiving %s (%lu bytes).\n", name, static_cast<unsigned long>(offset));
        return true;
    }
    if (state->file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    bool success = id == state->id && offset == state->offset;
    if (success && message[4] == 'D')
    {
        DWORD written = 0;
        success = dataLength > 0 && dataLength <= state->length - state->offset &&
                  WriteFile(state->file, data, dataLength, &written, NULL) && written == dataLength;
        if (success)
        {
            state->offset += dataLength;
            updateFileChecksum(&state->checksum, data, dataLength);
            return true;
        }
    }
    else if (success && message[4] == 'E')
    {
        success = dataLength == 4 && state->offset == state->length &&
                  readFileNumber(data) == ~state->checksum;
        if (success)
        {
            success = CloseHandle(state->file) != 0;
            state->file = INVALID_HANDLE_VALUE;
            if (success && MoveFileExA(state->temporary, state->destination, 0))
            {
                state->temporary[0] = '\0';
                printf("Saved %s (%lu bytes), CRC OK.\n", state->destination,
                       static_cast<unsigned long>(state->length));
                return true;
            }
        }
    }
    fprintf(stderr, "File incomplete, out of order, or could not be saved. Retry the file.\n");
    state->discard();
    return false;
}

static bool toReceive(HANDLE port)
{
    FileReception file;
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

        receiveFileMessage(&file, message, messageLength - CRC_TRAILER_BYTES);
        fflush(stdout);
    }

    fprintf(stderr, "Serial connection failed.\n");
    return false;
}

int main(int argc, char *argv[])
{
    if (argc < 3 || (strcmp(argv[1], "toTransmit") != 0 && strcmp(argv[1], "toReceive") != 0) ||
        argc != 3)
    {
        printf("Usage:\n  desktop.exe toTransmit COM3\n  desktop.exe toReceive COM4\n");
        return 1;
    }

    const char *folders[] = { "toTransmit", "toReceive" };
    for (unsigned index = 0; index < 2; index++)
    {
        if (!CreateDirectoryA(folders[index], NULL) &&
            (GetLastError() != ERROR_ALREADY_EXISTS ||
             !(GetFileAttributesA(folders[index]) & FILE_ATTRIBUTE_DIRECTORY)))
        {
            fprintf(stderr, "Cannot create %s. Run from the tempBuild folder.\n", folders[index]);
            return 1;
        }
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
    else if (success)
    {
        success = transmitFiles(port);
    }

    CloseHandle(port);
    return success ? 0 : 1;
}
