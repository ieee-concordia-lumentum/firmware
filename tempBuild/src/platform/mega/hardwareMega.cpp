#include <Arduino.h>
#include "hardware.h"
#include "protocol.h"


constexpr uint8_t transmissionPin = 22;
constexpr uint8_t ledPin = LED_BUILTIN;

constexpr uint16_t bitsPerSecond = 1000;


extern "C" {

    void hardwareInit(void){
        Serial.begin(115200);

        pinMode(transmissionPin, OUTPUT);
        pinMode(ledPin, OUTPUT);

        // Idle state
        digitalWrite(transmissionPin, LOW);
        digitalWrite(ledPin, LOW);

        cli();

        TCCR1A = 0;
        TCCR1B = 0;
        TCNT1 = 0;

        /*
         * 16 MHz clock
         * Prescaler = 1
         *
         * 16,000,000 / 9600 = approximately 1666.67 timer counts
         */
        OCR1A = 15999;

        // CTC mode
        TCCR1B |= (1 << WGM12);

        // Prescaler = 1
        TCCR1B |= (1 << CS10);

        // Enable Timer1 compare match interrupt
        TIMSK1 |= (1 << OCIE1A);

        sei();
    }


#ifdef SENDER
    void transmitBit(GpioState bit){
        digitalWrite(transmissionPin, static_cast<uint8_t>(bit));
        digitalWrite(ledPin, static_cast<uint8_t>(bit));
    }
#endif
}


ISR(TIMER1_COMPA_vect){
    protocolTransmitTick();
}
