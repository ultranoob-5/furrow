#include <Arduino.h>

#include "motor.h"
#include "button.h"
#include "current_sensor.h"
#include "logger.h"

Motor motor;

namespace
{
    constexpr const char *TAG = "Motor";
}

void Motor::begin()
{
    buttonInit();
    currentSensor.begin();

    Logger::info(TAG, "Ready - state will follow current feedback");
}

void Motor::start()
{
    // Do not set state here. The current sensor must confirm that the
    // motor actually started. This also means a failed starter does not
    // appear as RUNNING on the dashboard.
    if (state == MotorState::RUNNING)
        return;

    pressStartButton();

    Logger::info(TAG, "Start commanded - waiting for current feedback");
}

void Motor::stop()
{
    // Send STOP even if the last measured state is uncertain; the physical
    // starter remains the authority for stopping the motor.
    if (state == MotorState::OFF)
        return;

    pressStopButton();

    Logger::info(TAG, "Stop commanded - waiting for current feedback");
}

void Motor::update()
{
    buttonUpdate();
    currentSensor.update();

    state = currentSensor.isRunning() ? MotorState::RUNNING : MotorState::OFF;
}

bool Motor::isRunning()
{
    return state == MotorState::RUNNING;
}

float Motor::currentAmps()
{
    return currentSensor.currentAmps();
}
