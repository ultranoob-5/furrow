#ifndef MOTOR_H
#define MOTOR_H

// NOTE: This is a temporary, command-based state model. There is no real
// feedback source right now (the GPIO25 aux-contact wiring was only ever
// a manual test jumper, not real hardware). This will be replaced once
// current sensing (3x CT clamps on the outgoing/motor-side phases) is
// wired and calibrated - at that point, isRunning() will reflect real
// current draw instead of "what we last commanded."
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

private:

    MotorState state = MotorState::OFF;
};

extern Motor motor;

#endif