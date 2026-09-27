#include <Arduino.h>
#include <driver/gpio.h>

#include "hardware.h"
#include "protocol.h"


// Connect sender GPIO 26 directly to receiver GPIO 21.
constexpr uint8_t transmissionPin = 18;
constexpr uint8_t receivePin = 19;
constexpr uint8_t ledPin = 2;

constexpr uint32_t timerFrequency = 38400;
constexpr uint64_t senderAlarmTicks = 4;
constexpr uint64_t receiverAlarmTicks = 1;

hw_timer_t *transmissionTimer = nullptr;


void ARDUINO_ISR_ATTR onTimer(){
#ifdef SENDER
    protocolTransmitTick();

#elif defined(RECEIVER)
    bool sample = gpio_get_level((gpio_num_t)receivePin);
    protocolReceiveTick(sample);
#endif
}


extern "C"{
    void hardwareInit(void){
        Serial.begin(115200);
        delay(500);

#ifdef SENDER
        pinMode(transmissionPin, OUTPUT);
        pinMode(ledPin, OUTPUT);

        gpio_set_level((gpio_num_t)transmissionPin, 0);
        gpio_set_level((gpio_num_t)ledPin, 0);

#elif defined(RECEIVER)
        pinMode(receivePin, INPUT_PULLDOWN);
        pinMode(ledPin, OUTPUT);

        gpio_set_level((gpio_num_t)ledPin, 0);
#endif

        transmissionTimer = timerBegin(timerFrequency);

        if (transmissionTimer == nullptr){
            Serial.println("ERROR: hardware timer creation failed");
            return;
        }

        timerAttachInterrupt(transmissionTimer, &onTimer);

#ifdef SENDER
        timerAlarm(transmissionTimer, senderAlarmTicks, true, 0);
        Serial.print("Sender timer base frequency: ");

#elif defined(RECEIVER)
        timerAlarm(transmissionTimer, receiverAlarmTicks, true, 0);
        Serial.print("Receiver timer base frequency: ");
#endif

        Serial.println(timerGetFrequency(transmissionTimer));
    }


    bool transmitBit(char bitValue){
#ifdef SENDER
        if (bitValue == '1'){
            gpio_set_level((gpio_num_t)transmissionPin, 1);
            gpio_set_level((gpio_num_t)ledPin, 1);
        }
        else if (bitValue == '0'){
            gpio_set_level((gpio_num_t)transmissionPin, 0);
            gpio_set_level((gpio_num_t)ledPin, 0);
        }
        else{
            return false;
        }

        return true;
#else
        (void)bitValue;
        return false;
#endif
    }
}
