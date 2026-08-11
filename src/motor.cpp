#include <Arduino.h>

#include "motor.h"
#include "button.h"
#include "logger.h"

Motor motor;

namespace
{
    constexpr const char *TAG = "Motor";
}

void Motor::begin()
{
    buttonInit();

    Logger::info(TAG, "Ready");
}

void Motor::start()
{
    if (state == MotorState::RUNNING)
        return;

    pressStartButton();

    state = MotorState::RUNNING;

    Logger::info(TAG, "Start commanded");
}

void Motor::stop()
{
    if (state == MotorState::OFF)
        return;

    pressStopButton();

    state = MotorState::OFF;

    Logger::info(TAG, "Stop commanded");
}

void Motor::update()
{
    buttonUpdate();
}

bool Motor::isRunning()
{
    return state == MotorState::RUNNING;
}
