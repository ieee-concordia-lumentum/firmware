#include <Arduino.h>
#include "hardware.h"
#include "protocol.h"

// ---------------- BOARD CONFIGURATION ----------------
constexpr uint8_t transmissionPin = 19;
constexpr uint8_t ledPin = 2;

constexpr uint32_t timerFrequency = 1000000;
constexpr uint32_t bitsPerSecond = 1;

hw_timer_t *transmissionTimer = nullptr;
volatile bool timerFlag = false;

// ---------------- TIMER ISR ----------------
void IRAM_ATTR onTimer(){
    timerFlag = true;
}

extern "C"{
    void hardwareInit(void){
        Serial.begin(9600);

        // ---------------- PIN INITIALIZATION ----------------
        pinMode(transmissionPin, OUTPUT);
        pinMode(ledPin, OUTPUT);

        // ---------------- TIMER INITIALIZATION ----------------
        transmissionTimer = timerBegin(timerFrequency);

        timerAttachInterrupt(transmissionTimer, &onTimer);

        timerAlarm(transmissionTimer, timerFrequency / bitsPerSecond, true, 0);
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