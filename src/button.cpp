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
    if (state != ButtonState::IDLE)
        return;

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