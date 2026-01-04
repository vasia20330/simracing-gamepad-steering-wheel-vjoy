#include <windows.h>
#include <Xinput.h>
#include <iostream>
#include <cmath>
#include <thread>
#include <fstream>

#pragma comment(lib, "Xinput9_1_0.lib")

#define VJOY_ID 1

// ================== INI helpers ==================
float iniFloat(const char* section, const char* key, float def)
{
    char buf[64];
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), ".\\config.ini");
    return (buf[0] == 0) ? def : (float)atof(buf);
}

int iniInt(const char* section, const char* key, int def)
{
    return GetPrivateProfileIntA(section, key, def, ".\\config.ini");
}

// ================== Utils ==================
float clamp(float v, float a, float b) { return (v < a) ? a : (v > b ? b : v); }
float apply_gamma(float v, float gamma)
{
    float s = (v >= 0.f) ? 1.f : -1.f;
    return s * powf(fabs(v), gamma);
}
LONG normAxis(float v)
{
    v = clamp(v, -1.f, 1.f);
    return LONG((v + 1.f) * 16383.5f);
}

// ================== vJoy typedefs ==================
typedef BOOL(__cdecl* f_vJoyEnabled)();
typedef BOOL(__cdecl* f_AcquireVJD)(UINT);
typedef BOOL(__cdecl* f_RelinquishVJD)(UINT);
typedef BOOL(__cdecl* f_SetAxis)(LONG, UINT, UINT);
typedef BOOL(__cdecl* f_SetBtn)(BOOL, UINT, UCHAR);

// HID usages
#define HID_X   0x30
#define HID_Y   0x31
#define HID_Z   0x32

// ================== Default Config ==================
void createDefaultConfig()
{
    std::ofstream cfg(".\\config.ini");
    if (!cfg.is_open())
    {
        std::cout << "Failed to create config.ini\n";
        return;
    }

    cfg << "[STEERING]\n";
    cfg << "deadzone=0.06\n";
    cfg << "gamma=1.3\n";
    cfg << "alpha=0.04\n";
    cfg << "center_spring=0.12\n";
    cfg << "max_angle=540\n\n";

    cfg << "[PEDALS]\n";
    cfg << "alpha=0.3\n\n";

    cfg << "[GENERAL]\n";
    cfg << "update_ms=5\n";

    cfg.close();
    std::cout << "A new config.ini with default settings has been created.\n";
}

bool fileExists(const char* filename)
{
    std::ifstream f(filename);
    return f.good();
}

// ================== MAIN ==================
int main()
{
    if (!fileExists(".\\config.ini"))
        createDefaultConfig();

    // ---------- Load config ----------
    float deadzone = iniFloat("STEERING", "deadzone", 0.06f);
    float gamma = iniFloat("STEERING", "gamma", 1.3f);
    float alphaWheelBase = iniFloat("STEERING", "alpha", 0.04f);
    float centerSpring = iniFloat("STEERING", "center_spring", 0.12f);
    int maxAngleDeg = iniInt("STEERING", "max_angle", 540);

    float alphaPedal = iniFloat("PEDALS", "alpha", 0.3f);
    int sleepMs = iniInt("GENERAL", "update_ms", 5);

    float angleLimit = clamp(maxAngleDeg / 900.f, 0.1f, 1.f);

    // ---------- Load vJoy ----------
    HMODULE vjoyDll = LoadLibraryA("vJoyInterface.dll");
    if (!vjoyDll) { std::cout << "vJoyInterface.dll not found\n"; return 1; }

    auto vJoyEnabled = (f_vJoyEnabled)GetProcAddress(vjoyDll, "vJoyEnabled");
    auto AcquireVJD = (f_AcquireVJD)GetProcAddress(vjoyDll, "AcquireVJD");
    auto RelinquishVJD = (f_RelinquishVJD)GetProcAddress(vjoyDll, "RelinquishVJD");
    auto SetAxis = (f_SetAxis)GetProcAddress(vjoyDll, "SetAxis");
    auto SetBtn = (f_SetBtn)GetProcAddress(vjoyDll, "SetBtn");

    if (!vJoyEnabled() || !AcquireVJD(VJOY_ID))
    {
        std::cout << "vJoy not ready\n";
        return 1;
    }

    // ---------- State ----------
    float steer = 0.f;
    float gas = -1.f;
    float brake = -1.f;

    SetAxis(normAxis(0.f), VJOY_ID, HID_X);
    SetAxis(normAxis(-1.f), VJOY_ID, HID_Y);
    SetAxis(normAxis(-1.f), VJOY_ID, HID_Z);
    Sleep(50);

    std::cout << "vJoy feeder running...\n";

    // ---------- Main loop ----------
    while (true)
    {
        XINPUT_STATE s{};
        XInputGetState(0, &s);

        // ----- STEERING -----
        float raw = s.Gamepad.sThumbLX / 32768.f;

        if (fabs(raw) < deadzone)
            raw = 0.f;
        else
            raw = (fabs(raw) - deadzone) / (1.f - deadzone) * (raw > 0.f ? 1.f : -1.f);

        bool xPressed = (s.Gamepad.wButtons & XINPUT_GAMEPAD_X) != 0;
        bool bPressed = (s.Gamepad.wButtons & XINPUT_GAMEPAD_B) != 0;

        float alphaWheelCurrent = alphaWheelBase;
        if (xPressed) alphaWheelCurrent *= 1.5f;
        if (bPressed) alphaWheelCurrent *= 0.5f;

        if (raw == 0.f)
        {
            float k = clamp(fabs(steer) * 2.5f, 0.15f, 1.0f);
            steer += (0.f - steer) * centerSpring * k;
        }
        else
        {
            float target = apply_gamma(raw, gamma) * angleLimit;
            steer += (target - steer) * alphaWheelCurrent;
        }

        steer = clamp(steer, -angleLimit, angleLimit);
        SetAxis(normAxis(steer), VJOY_ID, HID_X);

        // ----- GAS -----
        float rawGas = s.Gamepad.bRightTrigger / 255.f;
        gas += ((rawGas * 2.f - 1.f) - gas) * alphaPedal;
        gas = clamp(gas, -1.f, 1.f);
        SetAxis(normAxis(gas), VJOY_ID, HID_Y);

        // ----- BRAKE -----
        float rawBrake = s.Gamepad.bLeftTrigger / 255.f;
        brake += ((rawBrake * 2.f - 1.f) - brake) * alphaPedal;
        brake = clamp(brake, -1.f, 1.f);
        SetAxis(normAxis(brake), VJOY_ID, HID_Z);

        // ----- BUTTONS -----
        WORD buttons = s.Gamepad.wButtons;
        for (int i = 0; i < 16; i++)
            SetBtn((buttons & (1 << i)) != 0, VJOY_ID, i + 1);

        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }

    RelinquishVJD(VJOY_ID);
    FreeLibrary(vjoyDll);
    return 0;
}
