// main.cpp is the high level, using hardware.h and protocol.h to use board specific hardware and protocol implementations

#include <Arduino.h>
#include "hardware.h"
#include "protocol.h"

void setup() {
  hardwareInit();
  protocolInit();
}

void loop() {
  Serial.print("Checking if can transmit bit...\n");

  if (canTransmitBit()) {
    Serial.println("Transmitting bit\n");
  }
}
