#include <Arduino.h>

#include "config.h"
#include "relay.h"

void relayInit()
{
    // Latch HIGH (relay off - the module is active-LOW) before switching
    // the pins to output. pinMode() first would drive them LOW for an
    // instant on every boot, a brief press of Start/Stop.
    relayAllOff();

    pinMode(Config::RELAY_START, OUTPUT);
    pinMode(Config::RELAY_STOP, OUTPUT);
}

void relayStartOn()
{
    digitalWrite(Config::RELAY_START, LOW);
}

void relayStartOff()
{
    digitalWrite(Config::RELAY_START, HIGH);
}

void relayStopOn()
{
    digitalWrite(Config::RELAY_STOP, LOW);
}

void relayStopOff()
{
    digitalWrite(Config::RELAY_STOP, HIGH);
}

void relayAllOff()
{
    relayStartOff();
    relayStopOff();
}