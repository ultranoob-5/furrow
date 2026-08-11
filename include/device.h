#pragma once

#include <Arduino.h>
#include "config.h"

class Device
{
public:

    void begin();

    void loop();

    String id();

    String name();

    String firmware();

    String ip();

    int rssi();

    bool online();

    unsigned long uptime();

};

extern Device device;