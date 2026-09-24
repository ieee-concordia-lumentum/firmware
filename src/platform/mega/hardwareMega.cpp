#include <Arduino.h>
#include "hardware.h"

// have board specific pin layout and initialization here
#define trasmissionPin 22

volatile bool timerFlag = false; // Flag to indicate when the timer interrupt has occurred

extern "C" {
    void hardwareInit(void) {
        Serial.begin(9600); // Initialize serial communication for debugging

        // --------------------------------- PIN INITIALIZATION --------------------------------- //
        pinMode(trasmissionPin, OUTPUT); // Set the transmission pin as an output

        // --------------------------------- TIMER INITIALIZATION --------------------------------- //
        cli();  // Clear global interrupts to ensure a clean setup

        // Clear default values for Timer1 registers
        TCCR1A = 0;
        TCCR1B = 0;
        TCNT1 = 0;  // Clear the timer counter

        OCR1A = 7812; // TCNT1 counts upto this value and then triggers the interrupt, 15624 triggers the interrupt every 1 second.
        TCCR1B |= (1 << WGM12); // Set to CTC mode (Clear Timer on Compare Match), this will reset the TCNT1 to 0 after it reaches the value in OCR1A

        TCCR1B |= (1 << CS12) | (1 << CS10);    // Set prescaler to 1024, so the timer increments every 1024 clock cycles, it does 15624 increments before triggering the interrupt, which is 1 second at 16MHz clock speed

        TIMSK1 |= (1 << OCIE1A);    // Enable Timer1 compare interrupt

        sei();  // Enable global interrupts
    }

    bool canTransmitBit(void){
        if (timerFlag){
            timerFlag = false;
            return true;
        }
        return false; // Return the value of the timerFlag to indicate if the timer interrupt has occurred
    }
}

ISR(TIMER1_COMPA_vect) {
    timerFlag = true; // Set the flag to indicate that the timer interrupt has occurred
}