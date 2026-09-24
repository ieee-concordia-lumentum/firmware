#include <Arduino.h>
#include "hardware.h"

// have board specific pin layout and initialization here
#define trasmissionPin 19

// if you see a red squigly line, ignore it, its a error in IDE but the code will compile and run properly.
hw_timer_t *timer1 = NULL;          
volatile bool timerFlag = false; // Flag to indicate when the timer interrupt has occurred

void IRAM_ATTR onTimer() {
    timerFlag = true; // Set the flag to indicate that the timer interrupt has occurred
} // Forward declaration of the interrupt service routine

extern "C" {
    void hardwareInit(void) {
        Serial.begin(9600); // Initialize serial communication for debugging

        // --------------------------------- PIN INITIALIZATION --------------------------------- //
        pinMode(trasmissionPin, OUTPUT); // Set the transmission pin as an output

        // --------------------------------- TIMER INITIALIZATION --------------------------------- //
        timer1 = timerBegin(1000000);
        timerAttachInterrupt(timer1, &onTimer);
        timerAlarm(timer1, 1000000, true, 0);  // 1 bit per second, lower it for faster transmission
    }

    bool canTransmitBit(void) {
        if (timerFlag){
            timerFlag = false;
            return true;
        }
        return false;
    }

}