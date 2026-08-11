#include "device.h"
#include "network.h"
#include "config.h"
#include "logger.h"
#include "storage.h"

Device device;

namespace
{
    constexpr const char *TAG = "Device";

    // Computed once and cached - the MAC never changes at runtime, and
    // this avoids rebuilding the string on every call to Device::id().
    String cachedId = "";

    String computeId()
    {
        String mac = Network::macAddress(); // e.g. "AA:BB:CC:DD:EE:FF"
        mac.replace(":", "");
        mac.toLowerCase();

        return "esp32-" + mac;
    }
}

void Device::begin()
{
    cachedId = computeId();

    Logger::info(TAG, "Ready - id: " + cachedId);
}

void Device::loop()
{

}

String Device::id()
{
    return cachedId;
}

String Device::name()
{
    return AppStorage::deviceName();
}

String Device::firmware()
{
    return Config::FIRMWARE_VERSION;
}

String Device::ip()
{
    return Network::ipAddress();
}

int Device::rssi()
{
    return Network::signalStrength();
}

bool Device::online()
{
    return Network::isConnected();
}

unsigned long Device::uptime()
{
    return millis();
}