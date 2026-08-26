#ifndef MOTOR_H
#define MOTOR_H

// Motor state is now based on real current feedback from the SCT013-030
// CT (see current_sensor.h for details), not on the last command sent to
// the starter. This means physical START/STOP operation is detected
// automatically as well as remote control.
enum class MotorState
{
    OFF,
    RUNNING
};

class Motor
{
public:

    void begin();

    void start();

    void stop();

    void update();

    bool isRunning();

    // True once the CT sensor has completed at least one real RMS
    // reading (see CurrentSensor::hasReading()) - false in the brief
    // window right after boot before that first reading exists. Used
    // to avoid seeding a false baseline from CurrentSensor's default
    // (not-running) state before any real measurement has happened.
    bool hasReading();

    float currentAmps();

private:

    MotorState state = MotorState::OFF;
};

extern Motor motor;

#endif
