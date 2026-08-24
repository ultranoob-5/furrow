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

    float currentAmps();

private:

    MotorState state = MotorState::OFF;
};

extern Motor motor;

#endif
