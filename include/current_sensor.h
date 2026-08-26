#pragma once

#include <Arduino.h>

class CurrentSensor
{
public:
    void begin();
    void update();

    bool hasReading() const;
    float currentAmps() const;
    bool isRunning() const;

private:
    void finishSample();

    uint32_t nextSampleUs = 0;
    uint16_t samplesTaken = 0;
    double sumI = 0.0;
    double offsetI = 2048.0;

    // Tracks how long a 400-sample window actually takes in real
    // wall-clock time, logged once per window in finishSample() - not
    // used for any threshold/state logic, purely observability. The
    // scheduling here only takes one sample per update() call
    // regardless of elapsed time, so how close this gets to the
    // intended 400ms depends entirely on how often update() itself
    // gets called (main.cpp's loop() cadence) - this is how to check
    // what's actually being achieved on real hardware, rather than
    // assuming the intended rate is what's really happening.
    uint32_t windowStartUs = 0;

    float irms = 0.0f;
    bool readingReady = false;
    bool running = false;
    uint8_t consecutiveAboveRunThreshold = 0;

    // Based on the Mottramlabs single-channel adaptation:
    // 400 samples at 1 ms gives about 20 cycles at 50 Hz - that's the
    // *intended* rate, achieved only if update() is actually called at
    // least that often. It's called once per main.cpp loop() iteration,
    // which takes at most one sample regardless of how much real time
    // has passed since the last one - so how close a window actually
    // gets to 400ms depends entirely on loop()'s own cadence, not
    // anything in this file. Confirmed this was a real problem, not
    // theoretical: at loop()'s previous delay(10)-dominated cadence, a
    // window was taking closer to 4-5 real seconds, not 400ms - see
    // main.cpp's delay(1) comment for the fix and windowStartUs's log
    // for how to check what's actually being achieved on real hardware.
    static constexpr uint16_t SAMPLES = 400;
    static constexpr uint32_t SAMPLE_INTERVAL_US = 1000;

    // This CT is the YHDC SCT013-030: per its datasheet, it has the
    // sampling (burden) resistor built in and is a genuine voltage-output
    // type - rated 30 A RMS primary -> 1 V RMS output, max input 60 A.
    // The datasheet doesn't publish the internal burden resistance or
    // turns ratio (not needed - you use the rated V/A directly), so the
    // calibration is just that rated ratio:
    //   ICAL = CT_RATED_PRIMARY_A / CT_RATED_OUTPUT_V
    //
    // IMPORTANT hardware note: the CT_1 schematic also has an external
    // R6 (100 ohm) wired across the CT's own output leads. Since this CT
    // already has its burden resistor built in, R6 is an *extra* load in
    // parallel with that internal (undisclosed) burden - it will pull the
    // output down from the datasheet's 1 V/30A figure by an amount this
    // firmware has no way to calculate, because YHDC doesn't publish the
    // internal burden value. Recommend removing/bypassing R6 so the CT's
    // own rated output reaches C3 unloaded; R8/R9 (bias) and C3 (AC
    // coupling) are unaffected either way and don't need to change.
    // Whether or not R6 stays, calibrate ICAL against a clamp meter on a
    // known motor current before trusting the Amps value for anything
    // beyond the ON/OFF threshold below.
    static constexpr float CT_RATED_PRIMARY_A = 30.0f;
    static constexpr float CT_RATED_OUTPUT_V = 1.0f;
    static constexpr float ICAL = CT_RATED_PRIMARY_A / CT_RATED_OUTPUT_V;

    static constexpr float SUPPLY_VOLTAGE = 3.3f;
    static constexpr float ADC_COUNTS = 4095.0f;

    // Motor feedback only: current above 2 A confirms the motor is running.
    // Current below the noise floor is treated as OFF. The calibrated RMS
    // value is retained for Serial/debugging, but is not published to Firebase.
    //
    // STOP_THRESHOLD_A was originally 0.05 A - too tight for this CT in
    // practice. Confirmed on real hardware: after a genuine STOP (current
    // actually dropping, not a stuck reading), it settles around 0.37-0.39A,
    // not near-zero, and stayed there indefinitely rather than reporting
    // OFF. This isn't a wiring fault - it's ESP32 ADC noise amplified by
    // this CT's very sensitive calibration (ICAL = 30 A per volt, see
    // the ICAL comment above): each single ADC count already corresponds
    // to about 0.024 A at this gain (3.3V / 4095 counts * 30 A/V), so a
    // completely ordinary ADC noise floor of a dozen-ish counts shows up
    // as several tenths of an amp. 0.8 A keeps a solid margin above that
    // observed noise ceiling while staying well clear of RUN_THRESHOLD_A,
    // so the hysteresis gap between "definitely off" and "definitely
    // running" stays wide either way.
    static constexpr float RUN_THRESHOLD_A = 2.0f;
    static constexpr float STOP_THRESHOLD_A = 0.8f;

    // A single noisy RMS window can spike over RUN_THRESHOLD_A with
    // nothing actually running - confirmed on real hardware: a false
    // RUNNING (3.85A) fired from one window, then decayed back down
    // over the next several (1.78, 0.84, 0.62 A) with the motor never
    // having been on the whole time. Requiring the current to stay
    // above threshold for several consecutive windows filters that
    // out, at some added delay before a genuine start is confirmed -
    // 3 windows, whatever one window's real duration actually is (see
    // SAMPLES's comment above - not a fixed ~1.2s guarantee, since
    // that depends on main.cpp's loop() cadence; check
    // windowStartUs's log for the real number on real hardware). A
    // small price either way for a farm pump that was never going to
    // be checked on a sub-second timescale anyway.
    // OFF deliberately stays immediate (no equivalent counter) - there's
    // no reason to delay reporting a real stop, and a single low-current
    // window is already good evidence the motor isn't drawing power.
    static constexpr uint8_t RUN_CONFIRM_WINDOWS = 3;
};

extern CurrentSensor currentSensor;
