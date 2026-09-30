#include <Arduino.h>
#include "hardware.h"
#include "protocol.h"


constexpr uint8_t statusLedPin = 2;


void setup(){
    protocolInit();
    hardwareInit();

#ifdef SENDER
    Serial.println("ESP32 sender ready");

#elif defined(RECEIVER)
    pinMode(statusLedPin, OUTPUT);
    digitalWrite(statusLedPin, LOW);

    Serial.println("ESP32 receiver waiting...");
#endif
}


void loop(){
    protocolProcess();

#ifdef SENDER
    static unsigned long previousTransmission = 0;

    if ((millis() - previousTransmission >= 1000) &&
        !protocolIsTransmitting()){
        previousTransmission = millis();
        protocolTransmit("Hello, World!");
        Serial.println("Sending RS-protected header: A0 00 D5 + 4 ECC bytes");
    }


#elif defined(RECEIVER)
    static uint8_t previousProgress = 255;
    static bool headerReported = false;

    uint8_t progress = protocolGetReceiverProgress();

    if (progress != previousProgress){
        Serial.print("Receiver progress: ");
        Serial.println(progress);
        previousProgress = progress;
    }

    if (protocolIsReady() && !headerReported){
        digitalWrite(statusLedPin, HIGH);
        Serial.println("HEADER SUCCESS");
        Serial.print("Message length: ");
        Serial.print(protocolGetMessageLength());
        Serial.println(" bytes");
        Serial.print("ECC corrected symbols: ");
        Serial.println(protocolGetCorrectedSymbolCount());
        headerReported = true;
    }
#endif
}
