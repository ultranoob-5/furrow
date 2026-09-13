#include "current_sensor.h"

#include "config.h"
#include "logger.h"

CurrentSensor currentSensor;

namespace
{
    constexpr const char *TAG = "Current";
}

void CurrentSensor::begin()
{
    pinMode(Config::CURRENT_ADC_PIN, INPUT);

    // ESP32 ADC1 pin; keep current sensing on ADC1 so Wi-Fi does not
    // conflict with ADC2.
    analogSetPinAttenuation(Config::CURRENT_ADC_PIN, ADC_11db);

    // The schematic biases the AC waveform around 1.65 V, so starting
    // close to the corresponding ADC midpoint avoids a long warm-up.
    offsetI = ADC_COUNTS / 2.0f;
    samplesTaken = 0;
    sumI = 0.0f;
    irms = 0.0f;
    readingReady = false;
    running = false;
    consecutiveAboveRunThreshold = 0;
    consecutiveBelowStopThreshold = 0;
    lastSampleUs = micros();

    Logger::info(TAG, "Ready - GPIO" + String(Config::CURRENT_ADC_PIN));
}

void CurrentSensor::update()
{
    const uint32_t now = micros();

    // Unsigned subtraction is rollover-safe across the full 2^32 us
    // (~71 min) micros() cycle - no cast needed, this is always correct.
    if (now - lastSampleUs < SAMPLE_INTERVAL_US)
        return;

    // If loop() was delayed significantly longer than one interval (e.g.
    // a long cloud.loop() SSL transaction or WiFi event), don't try to
    // catch up - just anchor to now and resume at the normal rate. This
    // prevents any lag from ever accumulating across loop iterations.
    // With the old nextSampleUs approach, jitter made nextSampleUs fall
    // behind 'now' by a tiny amount every loop tick (loop is ~1.5-2ms,
    // SAMPLE_INTERVAL_US is 1ms), so the deficit grew without bound.
    // After ~35 minutes of accumulated lag the signed cast in
    // (int32_t)(now - nextSampleUs) wrapped negative, making the guard
    // fire on every tick and permanently halting all sampling - confirmed
    // to happen in practice after a few hours of runtime. This approach
    // cannot accumulate any lag at all.
    if (now - lastSampleUs >= 2 * SAMPLE_INTERVAL_US)
        lastSampleUs = now;
    else
        lastSampleUs += SAMPLE_INTERVAL_US;

    if (samplesTaken == 0)
        windowStartUs = now; // first sample of a new window - mark its real start time

    const int sampleI = analogRead(Config::CURRENT_ADC_PIN);

    // Same adaptive DC-offset filter used by the Mottramlabs firmware.
    // It tracks the 1.65 V bias and leaves only the AC component.
    offsetI += (sampleI - offsetI) / 1024.0f;

    const float filteredI = (float)sampleI - offsetI;
    sumI += filteredI * filteredI;
    samplesTaken++;

    if (samplesTaken >= SAMPLES)
        finishSample();
}

void CurrentSensor::finishSample()
{
    // How long this window actually took in real wall-clock time -
    // logged unconditionally (not just when it's surprising), so real
    // hardware behavior is directly observable rather than assumed.
    // Intended is 400ms (SAMPLES * SAMPLE_INTERVAL_US); if this is
    // printing something meaningfully larger, main.cpp's loop() isn't
    // cycling fast enough to keep up with the 1ms sampling target -
    // see SAMPLES's comment in current_sensor.h.
    uint32_t windowDurationUs = micros() - windowStartUs;
    Logger::info(TAG, "Window: " + String(windowDurationUs / 1000) + "ms (intended 400ms)");

    // Mottramlabs-style RMS conversion:
    //   ADC RMS counts -> volts -> amps using the CT calibration.
    const float adcRms = sqrtf(sumI / SAMPLES);
    const float voltsRms = adcRms * (SUPPLY_VOLTAGE / ADC_COUNTS);

    irms = voltsRms * ICAL;

    // Clamp tiny ADC noise to zero. This is not the running threshold;
    // it only keeps the reported value from bouncing around at idle.
    if (irms < 0.05f)
        irms = 0.0f;

    const bool oldRunning = running;

    if (running)
    {
        // Motor is considered OFF once measured current stays below STOP_THRESHOLD_A
        // for STOP_CONFIRM_WINDOWS consecutive windows. Requiring 2 consecutive
        // windows prevents a single transient noise dip or ADC glitch from falsely
        // declaring the motor OFF while it is genuinely operating.
        if (irms <= STOP_THRESHOLD_A)
        {
            consecutiveBelowStopThreshold++;

            if (consecutiveBelowStopThreshold >= STOP_CONFIRM_WINDOWS)
            {
                running = false;
                consecutiveBelowStopThreshold = 0;
                consecutiveAboveRunThreshold = 0;
            }
        }
        else
        {
            consecutiveBelowStopThreshold = 0;
        }
    }
    else
    {
        consecutiveBelowStopThreshold = 0;

        // Do not declare RUNNING for small/noise currents, and do not
        // declare it from a single noisy window either - require
        // RUN_CONFIRM_WINDOWS consecutive windows above threshold first.
        if (irms > RUN_THRESHOLD_A)
        {
            consecutiveAboveRunThreshold++;

            if (consecutiveAboveRunThreshold >= RUN_CONFIRM_WINDOWS)
                running = true;
        }
        else
        {
            consecutiveAboveRunThreshold = 0;
        }
    }

    if (running != oldRunning)
    {
        Logger::info(TAG, String("Motor feedback: ") +
                              (running ? "RUNNING" : "OFF") +
                              " (" + String(irms, 2) + " A RMS)");
    }

    readingReady = true;
    samplesTaken = 0;
    sumI = 0.0;
}

bool CurrentSensor::hasReading() const
{
    return readingReady;
}

float CurrentSensor::currentAmps() const
{
    return irms;
}

bool CurrentSensor::isRunning() const
{
    return running;
}
