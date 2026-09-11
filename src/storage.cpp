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
    constexpr const char *KEY_DEV_MODE = "dev_mode";
    constexpr const char *KEY_SCHED_EN = "sched_en";
    constexpr const char *KEY_SCHED_ON = "sched_on";
    constexpr const char *KEY_SCHED_OFF = "sched_off";
    constexpr const char *KEY_RESUME_EN = "res_en";
    constexpr const char *KEY_RESUME_DEL = "res_del";
    constexpr const char *KEY_LAST_STATE = "last_state";
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

    bool isDevelopmentDevice()
    {
        return prefs.getBool(KEY_DEV_MODE, false);
    }

    void setDevelopmentDevice(bool enabled)
    {
        prefs.putBool(KEY_DEV_MODE, enabled);

        Logger::info(TAG, String("Development device mode ") + (enabled ? "ENABLED" : "DISABLED"));
    }

    bool scheduleEnabled()
    {
        return prefs.getBool(KEY_SCHED_EN, false);
    }

    String scheduleOnTime()
    {
        return prefs.getString(KEY_SCHED_ON, "");
    }

    String scheduleOffTime()
    {
        return prefs.getString(KEY_SCHED_OFF, "");
    }

    void setScheduleConfig(bool enabled, const String &onTime, const String &offTime)
    {
        prefs.putBool(KEY_SCHED_EN, enabled);
        prefs.putString(KEY_SCHED_ON, onTime);
        prefs.putString(KEY_SCHED_OFF, offTime);

        Logger::info(TAG, "Schedule saved - enabled: " + String(enabled ? "true" : "false") +
                           ", on: " + onTime + ", off: " + offTime);
    }

    bool autoResumeEnabled()
    {
        return prefs.getBool(KEY_RESUME_EN, false);
    }

    uint8_t autoResumeDelayMinutes()
    {
        return (uint8_t)prefs.getUChar(KEY_RESUME_DEL, 5);
    }

    void setAutoResumeConfig(bool enabled, uint8_t delayMinutes)
    {
        prefs.putBool(KEY_RESUME_EN, enabled);
        prefs.putUChar(KEY_RESUME_DEL, delayMinutes);

        Logger::info(TAG, "Auto-resume saved - enabled: " + String(enabled ? "true" : "false") +
                           ", delay: " + String(delayMinutes) + "m");
    }

    String lastMotorState()
    {
        return prefs.getString(KEY_LAST_STATE, "");
    }

    void setLastMotorState(const String &state)
    {
        prefs.putString(KEY_LAST_STATE, state);
    }

    void factoryReset()
    {
        prefs.remove(KEY_WIFI_SSID);
        prefs.remove(KEY_WIFI_PASS);
        prefs.remove(KEY_DEV_NAME);
        prefs.remove(KEY_OWNER);
        prefs.remove(KEY_WA_PHONE);
        prefs.remove(KEY_DEV_MODE);
        prefs.remove(KEY_SCHED_EN);
        prefs.remove(KEY_SCHED_ON);
        prefs.remove(KEY_SCHED_OFF);
        prefs.remove(KEY_RESUME_EN);
        prefs.remove(KEY_RESUME_DEL);
        prefs.remove(KEY_LAST_STATE);

        Logger::warn(TAG, "Factory reset - all stored config cleared");
    }
}

