#include <Arduino.h>
#include "hardware.h"
#include "protocol.h"

constexpr uint8_t transmissionPin = 22;
constexpr uint8_t ledPin = LED_BUILTIN;

constexpr uint16_t bitsPerSecond = 1;

volatile bool timerFlag = false;

extern "C"{
    void hardwareInit(void) {
        Serial.begin(9600);

        // ---------------- PIN INITIALIZATION ----------------

        pinMode(transmissionPin, OUTPUT);
        pinMode(ledPin, OUTPUT);

        // ---------------- TIMER INITIALIZATION ----------------

        cli();

        // Timer0 is used by Arduino for millis(), micros(), delay(), etc.
        // Timer1 is used for custom transmission timing.
        TCCR1A = 0;
        TCCR1B = 0;
        TCNT1 = 0;

        OCR1A = (F_CPU / (1024UL * bitsPerSecond)) - 1;

        // CTC mode
        TCCR1B |= (1 << WGM12);

        // Prescaler = 1024
        TCCR1B |= (1 << CS12) | (1 << CS10);

        // Enable Timer1 compare match interrupt
        TIMSK1 |= (1 << OCIE1A);

        sei();
    }

    bool canTransmitBit(void){
        if (timerFlag){
            timerFlag = false;
            return true;
        }

        return false;
    }

    bool transmitBit(char bitValue){
        Serial.print("Transmitting bit: ");
        Serial.println(bitValue);
        if (bitValue == '1'){
            digitalWrite(transmissionPin, HIGH);
            digitalWrite(ledPin, HIGH);
        }
        else if (bitValue == '0'){
            digitalWrite(transmissionPin, LOW);
            digitalWrite(ledPin, LOW);
        }
        else{
            return false;
        }

        return true;
    }
}

ISR(TIMER1_COMPA_vect){
    timerFlag = true;
}