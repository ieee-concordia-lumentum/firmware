#include <Arduino.h>
#include <driver/gpio.h>

#include "hardware.h"
#include "protocol.h"


// Sender output GPIO 18; receiver digital input GPIO 19.
// For a wired test, connect those pins and a common ground.
// For an optical test, GPIO 19 needs a conditioned digital receiver signal.
constexpr gpio_num_t transmissionPin = GPIO_NUM_18;
constexpr gpio_num_t receivePin = GPIO_NUM_19;
constexpr gpio_num_t ledPin = GPIO_NUM_2;

constexpr uint32_t timerFrequency = PROTOCOL_SPEED_KBPS * 1000UL * 4UL;
constexpr uint16_t timerDivider = 80000000UL / timerFrequency;
constexpr uint64_t senderAlarmTicks = 4;
constexpr uint64_t receiverAlarmTicks = 1;

hw_timer_t *transmissionTimer = nullptr;


void ARDUINO_ISR_ATTR onTimer(){
#ifdef SENDER
    protocolTransmitTick();

#elif defined(RECEIVER)
    bool sample = gpio_get_level(receivePin);
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

        gpio_set_level(transmissionPin, 0);
        gpio_set_level(ledPin, 0);

#elif defined(RECEIVER)
        pinMode(receivePin, INPUT_PULLDOWN);
        pinMode(ledPin, OUTPUT);

        gpio_set_level(ledPin, 0);
#endif

        transmissionTimer = timerBegin(0, timerDivider, true);

        if (transmissionTimer == nullptr){
            Serial.println("ERROR: hardware timer creation failed");
            return;
        }

        timerAttachInterrupt(transmissionTimer, &onTimer, true);

#ifdef SENDER
        timerAlarmWrite(transmissionTimer, senderAlarmTicks, true);
        Serial.print("Sender timer base frequency: ");

#elif defined(RECEIVER)
        timerAlarmWrite(transmissionTimer, receiverAlarmTicks, true);
        Serial.print("Receiver timer base frequency: ");
#endif

        Serial.println(80000000UL / timerDivider);
        timerAlarmEnable(transmissionTimer);
    }


#ifdef SENDER
    void transmitBit(GpioState bit){
        gpio_set_level(transmissionPin, static_cast<uint32_t>(bit));
        gpio_set_level(ledPin, static_cast<uint32_t>(bit));
    }
#endif
}

