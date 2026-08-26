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
    // Always send STOP, even if the last CT-sensed state already reads
    // OFF - that reading can be wrong (a transient noise dip in a
    // single RMS window is enough, since the OFF-detection path is
    // deliberately immediate/undebounced - see current_sensor.h's
    // comment on why RUNNING requires 3 consecutive windows but OFF
    // doesn't), and a stale or simply incorrect OFF reading must never
    // be able to silently swallow an explicit STOP request. The
    // physical starter remains the authority for whether the motor
    // actually stops; this only ever pulses the same button a person
    // would press by hand, and doing that on an already-stopped
    // machine is a complete, harmless no-op - exactly like a human
    // pressing Stop twice.
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

bool Motor::hasReading()
{
    return currentSensor.hasReading();
}

float Motor::currentAmps()
{
    return currentSensor.currentAmps();
}
