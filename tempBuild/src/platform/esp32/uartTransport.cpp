// Use ESP32 UART1 to send and receive frames over the one-way wire. Buffered
// output lets the sender load the next USB frame while UART1 sends the current one.

#include <Arduino.h>
#include <driver/uart.h>
#include "hardware.h"
#include "protocol.h"
#include "uart_link.h"

// Use UART1 for the wire between boards. Serial is the separate USB connection.
constexpr int8_t transmissionPin = 18;
constexpr int8_t receivePin = 19;

static HardwareSerial linkSerial(1);
#ifdef SENDER
static uint8_t outgoing[UART_LINK_MAX_RECORD];
static uint32_t sequence = 0;
#else
static UartLinkReceiver incoming;
#endif

extern "C" {
// Set the UART buffers before starting the link.
void hardwareInit(void){
    const size_t capacity = 4U * UART_LINK_MAX_RECORD;
    linkSerial.setRxBufferSize(capacity);
#ifdef SENDER
    linkSerial.setTxBufferSize(2U * UART_LINK_MAX_RECORD);
#endif
    linkSerial.begin(UART_LINK_BAUD, SERIAL_8N1, receivePin, transmissionPin);
}

void protocolInit(void){
#ifdef SENDER
    sequence = 0;
#else
    uartLinkInit(&incoming);
#endif
}
// Queue one frame on UART1 so the main loop can read the next USB frame.
bool protocolTransmit(const uint8_t *data, uint16_t length){
#ifdef SENDER
    if (!data || length != PROTOCOL_FRAME_BYTES){
        return false;
    }
    const size_t count = uartLinkEncode(data, ++sequence, outgoing, sizeof(outgoing));
    return count != 0 && linkSerial.write(outgoing, count) == count;
#else
    (void)data;
    (void)length;
    return false;
#endif
}
// Stop reading once a complete frame is ready for the USB queue.
void protocolProcess(void){
#ifndef SENDER
    while (!incoming.ready && linkSerial.available() > 0) {
        const int byte = linkSerial.read();
        if (byte < 0){
            break;
        }
        uartLinkFeed(&incoming, static_cast<uint8_t>(byte));
    }
#endif
}

bool protocolIsReady(void){
#ifdef SENDER
    return false;
#else
    return incoming.ready;
#endif
}
// Check whether the UART finished sending without blocking the main loop.
bool protocolIsTransmitting(void){
#ifdef SENDER
    return uart_wait_tx_done(UART_NUM_1, 0) != ESP_OK;
#else
    return false;
#endif
}

const uint8_t *protocolGetMessageData(void){
#ifdef SENDER
    return nullptr;
#else
    return uartLinkFrame(&incoming);
#endif
}

uint16_t protocolGetMessageLength(void){
    return protocolIsReady() ? PROTOCOL_FRAME_BYTES : 0;
}

int16_t protocolGetCorrectedSymbolCount(void){
    return 0;
}

enum ReceiveProgress protocolGetReceiverProgress(void){
#ifdef SENDER
    return PROG_WAITING;
#else
    if (incoming.ready){
        return PROG_READY;
    }
    return incoming.used ? PROG_DATA : PROG_WAITING;
#endif
}
// Release a forwarded frame or abandon a record that timed out.
void protocolResetReceiver(void){
#ifndef SENDER
    if (incoming.ready){
        uartLinkRelease(&incoming);
    }
    else{
        uartLinkAbort(&incoming);
    }
#endif
}

const UartLinkStats *protocolGetUartStats(void){
#ifdef SENDER
    return nullptr;
#else
    return &incoming.stats;
#endif
}
// The timer build uses these entry points. UART1 handles the bits in this build.
void protocolTransmitTick(void) {}
void protocolReceiveTick(bool sample){
    (void)sample;
}
#ifdef SENDER
void transmitBit(GpioState bit){
    (void)bit;
}
#endif
}
