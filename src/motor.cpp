#include <Arduino.h>

#include "motor.h"
#include "button.h"
#include "current_sensor.h"
#include "storage.h"
#include "logger.h"

Motor motor;

namespace
{
    constexpr const char *TAG = "Motor";
}

void Motor::begin()
{
    devMode = AppStorage::isDevelopmentDevice();
    buttonInit();

    if (devMode)
    {
        Logger::warn(TAG, "Ready [DEV MODE] - current feedback disabled, state is command-driven");
    }
    else
    {
        currentSensor.begin();
        Logger::info(TAG, "Ready - state will follow current feedback");
    }
}

void Motor::start()
{
    // In production, do not set state here. The current sensor must confirm
    // that the motor actually started. In devMode, CT feedback is bypassed
    // so state transitions directly to RUNNING.
    if (state == MotorState::RUNNING)
        return;

    pressStartButton();

    if (devMode)
    {
        state = MotorState::RUNNING;
        AppStorage::setLastMotorState("RUNNING");
        Logger::info(TAG, "Start commanded [DEV MODE] - state set to RUNNING");
    }
    else
    {
        Logger::info(TAG, "Start commanded - waiting for current feedback");
    }
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

    if (devMode)
    {
        state = MotorState::OFF;
        AppStorage::setLastMotorState("OFF");
        Logger::info(TAG, "Stop commanded [DEV MODE] - state set to OFF");
    }
    else
    {
        Logger::info(TAG, "Stop commanded - waiting for current feedback");
    }
}

void Motor::update()
{
    buttonUpdate();

    if (!devMode)
    {
        currentSensor.update();
        state = currentSensor.isRunning() ? MotorState::RUNNING : MotorState::OFF;
    }
}

bool Motor::isRunning()
{
    return state == MotorState::RUNNING;
}

bool Motor::hasReading()
{
    if (devMode)
        return true;
    return currentSensor.hasReading();
}

float Motor::currentAmps()
{
    if (devMode)
        return state == MotorState::RUNNING ? 5.5f : 0.0f;
    return currentSensor.currentAmps();
}

bool Motor::isDevelopment() const
{
    return devMode;
}

