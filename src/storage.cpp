#include <Preferences.h>

#include "storage.h"
#include "logger.h"

namespace
{
    Preferences prefs;

    // "smc" (Smart Motor Controller, this project's name before the
    // Furrow rename) is left as-is deliberately, not a missed cleanup
    // spot: it's the actual NVS namespace key every already-flashed
    // device's saved WiFi/owner/WhatsApp config lives under. Changing
    // it would make prefs.begin() open an empty namespace on next
    // boot for any existing device - a silent factory reset requiring
    // reprovisioning, not a cosmetic rename.
    constexpr const char *NAMESPACE = "smc";
    constexpr const char *KEY_WIFI_SSID = "wifi_ssid";
    constexpr const char *KEY_WIFI_PASS = "wifi_pass";
    constexpr const char *KEY_DEV_NAME = "dev_name";
    constexpr const char *KEY_OWNER = "owner";
    constexpr const char *KEY_WA_PHONE = "wa_phone";
    constexpr const char *TAG = "Storage";
}

namespace AppStorage
{
    void begin()
    {
        prefs.begin(NAMESPACE, false);

        Logger::info(TAG, "Ready");
    }

    bool hasWifiCredentials()
    {
        return prefs.isKey(KEY_WIFI_SSID) && prefs.getString(KEY_WIFI_SSID).length() > 0;
    }

    String wifiSSID()
    {
        return prefs.getString(KEY_WIFI_SSID, "");
    }

    String wifiPassword()
    {
        return prefs.getString(KEY_WIFI_PASS, "");
    }

    void setWifiCredentials(const String &ssid, const String &password)
    {
        prefs.putString(KEY_WIFI_SSID, ssid);
        prefs.putString(KEY_WIFI_PASS, password);

        Logger::info(TAG, "WiFi credentials saved for SSID: " + ssid);
    }

    void clearWifiCredentials()
    {
        prefs.remove(KEY_WIFI_SSID);
        prefs.remove(KEY_WIFI_PASS);

        Logger::info(TAG, "WiFi credentials cleared");
    }

    bool hasDeviceConfig()
    {
        return prefs.isKey(KEY_OWNER) && prefs.getString(KEY_OWNER).length() > 0;
    }

    String deviceName()
    {
        return prefs.getString(KEY_DEV_NAME, "");
    }

    String ownerEmail()
    {
        return prefs.getString(KEY_OWNER, "");
    }

    void setDeviceConfig(const String &name, const String &owner)
    {
        prefs.putString(KEY_DEV_NAME, name);
        prefs.putString(KEY_OWNER, owner);

        Logger::info(TAG, "Device config saved - name: " + name + ", owner: " + owner);
    }

    bool hasWhatsAppConfig()
    {
        return prefs.isKey(KEY_WA_PHONE) && prefs.getString(KEY_WA_PHONE).length() > 0;
    }

    String whatsAppPhone()
    {
        return prefs.getString(KEY_WA_PHONE, "");
    }

    void setWhatsAppConfig(const String &phone)
    {
        prefs.putString(KEY_WA_PHONE, phone);

        Logger::info(TAG, "WhatsApp notification recipient saved");
    }

    void factoryReset()
    {
        prefs.remove(KEY_WIFI_SSID);
        prefs.remove(KEY_WIFI_PASS);
        prefs.remove(KEY_DEV_NAME);
        prefs.remove(KEY_OWNER);
        prefs.remove(KEY_WA_PHONE);

        Logger::warn(TAG, "Factory reset - all stored config cleared");
    }
}
