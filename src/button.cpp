#include <Arduino.h>

#include "button.h"
#include "config.h"
#include "relay.h"

namespace
{
    enum class ButtonState
    {
        IDLE,
        START,
        STOP
    };

    ButtonState state = ButtonState::IDLE;

    unsigned long timer = 0;
}

void buttonInit()
{
    relayInit();
}

void pressStartButton()
{
    if (state != ButtonState::IDLE)
        return;

    relayStartOn();

    timer = millis();

    state = ButtonState::START;
}

void pressStopButton()
{
    // Always preempts, unlike pressStartButton() - if a START pulse is
    // still physically in progress (within BUTTON_PRESS_TIME) when a
    // STOP is requested, immediately cancel it and switch to STOP
    // instead of silently dropping the request the way the old "if
    // (state != IDLE) return;" guard used to for every case. This is
    // what actually completes the "STOP must never be silently
    // swallowed" fix in motor.cpp - that fix alone only guaranteed
    // Motor::stop() always calls this function, not that this
    // function actually acts on it. A STOP arriving mid-START-pulse
    // (entirely plausible - hit Start, immediately correct with Stop)
    // used to fall through this same guard and do nothing at all.
    // Mirrors how a real panel's STOP button (normally-closed, wired
    // to always be able to interrupt) takes priority over START -
    // these two relay channels don't have that interlock in hardware
    // (see relay.cpp - two independent GPIO pins, no cross-wiring),
    // so it has to be enforced here instead.
    if (state == ButtonState::START)
        relayStartOff();

    relayStopOn();

    timer = millis();

    state = ButtonState::STOP;
}

void buttonUpdate()
{
    switch (state)
    {
        case ButtonState::IDLE:
            break;

        case ButtonState::START:

            if (millis() - timer >= Config::BUTTON_PRESS_TIME)
            {
                relayStartOff();
                state = ButtonState::IDLE;
            }

            break;

        case ButtonState::STOP:

            if (millis() - timer >= Config::BUTTON_PRESS_TIME)
            {
                relayStopOff();
                state = ButtonState::IDLE;
            }

            break;
    }
}