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

    float irms = 0.0f;
    bool readingReady = false;
    bool running = false;

    // Based on the Mottramlabs single-channel adaptation:
    // 400 samples at 1 ms gives about 20 cycles at 50 Hz.
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
    static constexpr float RUN_THRESHOLD_A = 2.0f;
    static constexpr float STOP_THRESHOLD_A = 0.05f;
};

extern CurrentSensor currentSensor;
