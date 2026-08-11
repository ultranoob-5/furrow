#include <Arduino.h>

#include "config.h"
#include "relay.h"

void relayInit()
{
    pinMode(Config::RELAY_START, OUTPUT);
    pinMode(Config::RELAY_STOP, OUTPUT);

    relayAllOff();
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