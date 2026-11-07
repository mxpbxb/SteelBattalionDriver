/*
SteelBattalionFeeder: drives the Steel Battalion controller without a custom
kernel driver, so Windows test signing is not needed.

  controller --(WinUSB, in-box Microsoft driver)--> this app --> vJoy or a
  virtual Xbox 360 controller (ViGEmBus) --> games and emulators

Usage:
  SteelBattalionFeeder.exe                  Run in the tray
  SteelBattalionFeeder.exe --verbose        Run and log every input change
  SteelBattalionFeeder.exe --vjoy or --xbox  Start in that mode, or switch the running app to it
  SteelBattalionFeeder.exe --setup [--quiet] Configure vJoy and bind WinUSB (elevates)

Settings: %LOCALAPPDATA%\SteelBattalionFeeder\settings.ini
Log:      %LOCALAPPDATA%\SteelBattalionFeeder\feeder.log
*/

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <winusb.h>
#include <share.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <ViGEm/Client.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

#define APP_NAME     L"SteelBattalionFeeder"
#define WINDOW_CLASS L"SteelBattalionFeederWindow"
#define RUN_KEY      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define HARDWARE_ID  L"USB\\VID_0A7B&PID_D000"
#define PATH_MATCH   L"vid_0a7b&pid_d000"

#define VJOY_ID      1
#define VJOY_BUTTONS 62
#define VJOY_AXIS_MAX 0x7FFF

#define LED_DATA_SIZE        34
#define LED_MAX              0x0F
#define LED_FIRST_BUTTON     4
#define LED_LAST_BUTTON      33
#define LED_GEAR_REVERSE     35
#define DIAL_LIT_UNLESS_HELD 13   /* Dial position reported as button 60 */
#define DIAL_RUMBLE_FLASH    14   /* Button 61: lit while held, flash on rumble */
#define DIAL_ALL_OFF         15   /* Button 62: every light off */
#define FLASH_HALF_PERIOD_MS 60

#define REPORT_MIN_SIZE      26

/* Interface GUID written by the in-box winusb.inf */
static const GUID GUID_WINUSB_INBOX =
    { 0xF72FE0D4, 0xCBCB, 0x407D, { 0x88, 0x14, 0x9E, 0xD6, 0x73, 0xD0, 0xDD, 0x6B } };
/* Exposed for every USB device, used if another tool bound WinUSB with its own GUID */
static const GUID GUID_USB_DEVICE =
    { 0xA5DCBF10, 0x6530, 0x11D2, { 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED } };

/* Controller report, same layout the kernel driver read */
#include <pshpack1.h>
typedef struct {
    BYTE   pad0, pad1;
    BYTE   buttons[5];
    BYTE   pad2;
    USHORT aimX, aimY;
    SHORT  rotation, sightX, sightY;
    USHORT clutch, brake, throttle;
    BYTE   tuner;
    CHAR   gear;
} SBC_REPORT;
#include <poppack.h>

/* From vJoy SDK public.h, JOYSTICK_POSITION_V2 */
typedef struct {
    BYTE bDevice;
    LONG wThrottle, wRudder, wAileron;
    LONG wAxisX, wAxisY, wAxisZ, wAxisXRot, wAxisYRot, wAxisZRot;
    LONG wSlider, wDial, wWheel;
    LONG wAxisVX, wAxisVY, wAxisVZ, wAxisVBRX, wAxisVBRY, wAxisVBRZ;
    LONG lButtons;
    DWORD bHats, bHatsEx1, bHatsEx2, bHatsEx3;
    LONG lButtonsEx1, lButtonsEx2, lButtonsEx3;
} VJOY_POSITION;

typedef enum { VJD_OWN, VJD_FREE, VJD_BUSY, VJD_MISS, VJD_UNKN } VJD_STATUS;

static BOOL (__cdecl *pvJoyEnabled)(void);
static int  (__cdecl *pGetVJDStatus)(UINT);
static BOOL (__cdecl *pAcquireVJD)(UINT);
static VOID (__cdecl *pRelinquishVJD)(UINT);
static BOOL (__cdecl *pUpdateVJD)(UINT, PVOID);
static int  (__cdecl *pGetVJDButtonNumber)(UINT);

static volatile LONG g_quit;
static HANDLE g_quitEvent;
static HWND g_window;
static BOOL g_verbose;
static FILE *g_log;
static CRITICAL_SECTION g_statusLock;
static wchar_t g_status[128] = L"Starting";

#define WM_TRAY       (WM_APP + 1)
#define WM_STATUS     (WM_APP + 2)
#define MENU_AUTOSTART 101
#define MENU_EXIT      102
#define MENU_VJOY      103
#define MENU_XBOX      104
#define MENU_SETTINGS  105
#define MENU_RELOAD    106
#define MENU_CENTER    107

/* ---- Logging ---- */

static void LogOpen(const wchar_t *mode)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 64) return;
    wcscat_s(path, MAX_PATH, L"\\" APP_NAME);
    CreateDirectoryW(path, NULL);
    wcscat_s(path, MAX_PATH, L"\\feeder.log");
    g_log = _wfsopen(path, mode, _SH_DENYWR);
}

static void Log(const char *fmt, ...)
{
    SYSTEMTIME t;
    va_list args;
    if (!g_log) return;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

static void SetStatus(const wchar_t *text)
{
    BOOL changed;
    EnterCriticalSection(&g_statusLock);
    changed = wcscmp(g_status, text) != 0;
    if (changed) wcsncpy_s(g_status, 128, text, _TRUNCATE);
    LeaveCriticalSection(&g_statusLock);
    if (changed)
    {
        Log("status: %ls", text);
        if (g_window) PostMessageW(g_window, WM_STATUS, 0, 0);
    }
}

/* ---- vJoy ---- */

static BOOL VjoyFolder(wchar_t *out, DWORD cch)
{
    DWORD n = GetEnvironmentVariableW(L"ProgramFiles", out, cch);
    if (n == 0 || n >= cch - 16) return FALSE;
    wcscat_s(out, cch, L"\\vJoy\\x64\\");
    return TRUE;
}

static BOOL VjoyLoad(void)
{
    wchar_t path[MAX_PATH];
    HMODULE dll;
    if (pUpdateVJD) return TRUE;
    if (!VjoyFolder(path, MAX_PATH)) return FALSE;
    wcscat_s(path, MAX_PATH, L"vJoyInterface.dll");
    dll = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) { Log("vJoyInterface.dll load failed: %lu", GetLastError()); return FALSE; }
    *(FARPROC *)&pvJoyEnabled = GetProcAddress(dll, "vJoyEnabled");
    *(FARPROC *)&pGetVJDStatus = GetProcAddress(dll, "GetVJDStatus");
    *(FARPROC *)&pAcquireVJD = GetProcAddress(dll, "AcquireVJD");
    *(FARPROC *)&pRelinquishVJD = GetProcAddress(dll, "RelinquishVJD");
    *(FARPROC *)&pGetVJDButtonNumber = GetProcAddress(dll, "GetVJDButtonNumber");
    *(FARPROC *)&pUpdateVJD = GetProcAddress(dll, "UpdateVJD");
    if (!pvJoyEnabled || !pGetVJDStatus || !pAcquireVJD || !pRelinquishVJD || !pGetVJDButtonNumber || !pUpdateVJD)
    {
        pUpdateVJD = NULL;
        Log("vJoyInterface.dll is missing expected functions");
        return FALSE;
    }
    return TRUE;
}

static BOOL VjoyAcquire(void)
{
    int status;
    if (!VjoyLoad()) { SetStatus(L"vJoy is not installed"); return FALSE; }
    if (!pvJoyEnabled()) { SetStatus(L"vJoy is not enabled"); return FALSE; }
    status = pGetVJDStatus(VJOY_ID);
    if (status == VJD_OWN) return TRUE;
    if (status != VJD_FREE) { SetStatus(L"vJoy device 1 is unavailable"); return FALSE; }
    if (pGetVJDButtonNumber(VJOY_ID) < VJOY_BUTTONS) { SetStatus(L"vJoy needs setup: run with --setup"); return FALSE; }
    if (!pAcquireVJD(VJOY_ID)) { SetStatus(L"Could not take vJoy device 1"); return FALSE; }
    Log("vJoy device %d acquired", VJOY_ID);
    return TRUE;
}

/* ---- Settings ---- */

typedef enum { MODE_VJOY, MODE_XBOX } OUTPUT_MODE;
static volatile LONG g_mode = MODE_VJOY;

enum { AX_AIMX, AX_AIMY, AX_ROTATION, AX_SIGHTX, AX_SIGHTY, AX_CLUTCH, AX_BRAKE, AX_THROTTLE, AX_COUNT };
static const wchar_t *const AXIS_NAMES[AX_COUNT] =
    { L"aimX", L"aimY", L"rotation", L"sightX", L"sightY", L"clutch", L"brake", L"throttle" };

typedef struct { LONG min, center, max, dead; } AXIS_CAL;
/* In 0..32767 units, seeded from a real controller. Min and max widen as the
   controller reports further. The aim stick does not return to one exact spot,
   so it needs a wide dead zone. */
static AXIS_CAL g_cal[AX_COUNT] = {
    { 8480, 16900, 22368, 3000 }, { 736, 15500, 23648, 3000 }, { 1000, 16384, 32000, 1000 },
    { 2848, 16400, 31200, 1200 }, { 6048, 15400, 29152, 1200 },
    { 0, 0, 32767, 500 }, { 0, 0, 32767, 500 }, { 0, 0, 32767, 500 } };

typedef struct { const wchar_t *key; USHORT flag; unsigned long long buttons; } XBOX_BUTTON;
static XBOX_BUTTON g_xboxButtons[] = {
    { L"A", XUSB_GAMEPAD_A }, { L"B", XUSB_GAMEPAD_B }, { L"X", XUSB_GAMEPAD_X }, { L"Y", XUSB_GAMEPAD_Y },
    { L"LB", XUSB_GAMEPAD_LEFT_SHOULDER }, { L"RB", XUSB_GAMEPAD_RIGHT_SHOULDER },
    { L"Back", XUSB_GAMEPAD_BACK }, { L"Start", XUSB_GAMEPAD_START }, { L"Guide", XUSB_GAMEPAD_GUIDE },
    { L"LS", XUSB_GAMEPAD_LEFT_THUMB }, { L"RS", XUSB_GAMEPAD_RIGHT_THUMB },
    { L"Up", XUSB_GAMEPAD_DPAD_UP }, { L"Down", XUSB_GAMEPAD_DPAD_DOWN },
    { L"Left", XUSB_GAMEPAD_DPAD_LEFT }, { L"Right", XUSB_GAMEPAD_DPAD_RIGHT } };
#define XBOX_BUTTON_COUNT (sizeof(g_xboxButtons) / sizeof(g_xboxButtons[0]))

typedef struct { const wchar_t *key; int axis; BOOL invert; unsigned long long buttons; } XBOX_ANALOG;
enum { XA_LT, XA_RT, XA_LX, XA_LY, XA_RX, XA_RY, XA_COUNT };
static XBOX_ANALOG g_xboxAnalog[XA_COUNT] = {
    { L"LT", -1 }, { L"RT", -1 }, { L"LeftX", -1 }, { L"LeftY", -1 }, { L"RightX", -1 }, { L"RightY", -1 } };

static CRITICAL_SECTION g_mapLock;
static LONG g_lastAxis[AX_COUNT];
static BOOL g_lastAxisValid;

static const char DEFAULT_SETTINGS[] =
    "; Steel Battalion Feeder settings. After editing, right-click the tray icon and\n"
    "; choose Reload settings.\n"
    ";\n"
    "; Xbox buttons take one or more controller button numbers, such as A=2 or A=2,23.\n"
    "; Leave a value empty to leave it unassigned.\n"
    ";\n"
    ";  1 Main Weapon     2 Fire            3 Lock-On         4 Eject\n"
    ";  5 Cockpit Hatch   6 Ignition        7 Start           8 Open/Close\n"
    ";  9 Map Zoom       10 Mode Select    11 Sub Monitor    12 Main Zoom In\n"
    "; 13 Main Zoom Out  14 FSS            15 Manipulator    16 Line Color\n"
    "; 17 Washing        18 Extinguisher   19 Chaff          20 Tank Detach\n"
    "; 21 Override       22 Night Scope    23 F1             24 F2\n"
    "; 25 F3             26 Weapon Main    27 Weapon Sub     28 Magazine\n"
    "; 29-33 Comm 1-5    34 Sight Change   35 Filter         36 Oxygen\n"
    "; 37 Fuel Flow      38 Buffer         39 VT Location\n"
    "; 40-46 Gear R, N, 1-5                47-62 Dial 0-15\n"
    ";\n"
    "; Sticks and triggers take an axis: aimX aimY rotation sightX sightY clutch brake\n"
    "; throttle. Put a minus in front to reverse a stick. Triggers can also take\n"
    "; button numbers.\n"
    "\n"
    "[Output]\n"
    "; vjoy or xbox. The tray menu changes this too.\n"
    "Mode=vjoy\n"
    "\n"
    "[Xbox]\n"
    "A=2\nB=3\nX=1\nY=34\nLB=26\nRB=27\nBack=6\nStart=7\nGuide=\nLS=4\nRS=5\n"
    "Up=29\nDown=30\nLeft=31\nRight=32\n"
    "LT=brake\nRT=throttle\nLeftX=aimX\nLeftY=-aimY\nRightX=sightX\nRightY=-sightY\n";

static BOOL SettingsPath(wchar_t *path)
{
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 64) return FALSE;
    wcscat_s(path, MAX_PATH, L"\\" APP_NAME);
    CreateDirectoryW(path, NULL);
    wcscat_s(path, MAX_PATH, L"\\settings.ini");
    return TRUE;
}

static unsigned long long ParseButtons(const wchar_t *text)
{
    unsigned long long bits = 0;
    const wchar_t *p = text;
    while (*p)
    {
        wchar_t *end;
        const long n = wcstol(p, &end, 10);
        if (end == p) break;
        if (n >= 1 && n <= VJOY_BUTTONS) bits |= 1ULL << (n - 1);
        p = end;
        while (*p == L',' || *p == L' ') ++p;
    }
    return bits;
}

static void ParseAnalog(const wchar_t *text, XBOX_ANALOG *out)
{
    const wchar_t *name = text;
    int i;
    out->axis = -1;
    out->invert = FALSE;
    out->buttons = 0;
    while (*name == L' ') ++name;
    if (*name == L'-') { out->invert = TRUE; ++name; }
    for (i = 0; i < AX_COUNT; ++i)
    {
        if (_wcsicmp(name, AXIS_NAMES[i]) == 0) { out->axis = i; return; }
    }
    out->buttons = ParseButtons(text);
}

static void LoadSettings(void)
{
    wchar_t path[MAX_PATH], value[128];
    size_t i;
    if (!SettingsPath(path)) return;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
    {
        FILE *file = NULL;
        _wfopen_s(&file, path, L"w");
        if (file) { fputs(DEFAULT_SETTINGS, file); fclose(file); }
    }
    EnterCriticalSection(&g_mapLock);
    for (i = 0; i < XBOX_BUTTON_COUNT; ++i)
    {
        GetPrivateProfileStringW(L"Xbox", g_xboxButtons[i].key, L"", value, 128, path);
        g_xboxButtons[i].buttons = ParseButtons(value);
    }
    for (i = 0; i < XA_COUNT; ++i)
    {
        GetPrivateProfileStringW(L"Xbox", g_xboxAnalog[i].key, L"", value, 128, path);
        ParseAnalog(value, &g_xboxAnalog[i]);
    }
    for (i = 0; i < AX_COUNT; ++i)
    {
        AXIS_CAL c;
        GetPrivateProfileStringW(L"Calibration", AXIS_NAMES[i], L"", value, 128, path);
        if (swscanf_s(value, L"%ld,%ld,%ld,%ld", &c.min, &c.center, &c.max, &c.dead) == 4 && c.min <= c.center && c.center <= c.max)
            g_cal[i] = c;
    }
    LeaveCriticalSection(&g_mapLock);
    GetPrivateProfileStringW(L"Output", L"Mode", L"vjoy", value, 128, path);
    InterlockedExchange(&g_mode, _wcsicmp(value, L"xbox") == 0 ? MODE_XBOX : MODE_VJOY);
}

static void SaveCalibration(void)
{
    wchar_t path[MAX_PATH], value[128];
    int i;
    if (!SettingsPath(path)) return;
    EnterCriticalSection(&g_mapLock);
    for (i = 0; i < AX_COUNT; ++i)
    {
        swprintf_s(value, 128, L"%ld,%ld,%ld,%ld", g_cal[i].min, g_cal[i].center, g_cal[i].max, g_cal[i].dead);
        WritePrivateProfileStringW(L"Calibration", AXIS_NAMES[i], value, path);
    }
    LeaveCriticalSection(&g_mapLock);
}

static void SaveMode(LONG mode)
{
    wchar_t path[MAX_PATH];
    if (SettingsPath(path)) WritePrivateProfileStringW(L"Output", L"Mode", mode == MODE_XBOX ? L"xbox" : L"vjoy", path);
}

/* Uses the sticks' current resting spot as center, fixing drift */
static void RecenterSticks(void)
{
    int i;
    EnterCriticalSection(&g_mapLock);
    if (g_lastAxisValid)
    {
        for (i = AX_AIMX; i <= AX_SIGHTY; ++i) g_cal[i].center = g_lastAxis[i];
    }
    LeaveCriticalSection(&g_mapLock);
    SaveCalibration();
    Log("sticks recentered");
}

/* ---- Mapping ---- */

static LONG AxisU(USHORT raw) { return (LONG)(raw >> 1); }
static LONG AxisS(SHORT raw)  { return (LONG)(((int)raw + 32768) >> 1); }

/* Buttons 1..39 are the raw bits, 40..46 gear R, N, 1..5, 47..62 dial 0..15 */
static unsigned long long ReportButtons(const SBC_REPORT *r)
{
    unsigned long long bits = 0;
    int i;
    const int gear = r->gear;
    for (i = 0; i < 5; ++i) bits |= (unsigned long long)r->buttons[i] << (8 * i);
    bits &= (1ULL << 39) - 1;
    if (gear >= -2 && gear <= 5 && gear != 0)
        bits |= 1ULL << (39 + (gear < 0 ? gear + 2 : gear + 1));
    bits |= 1ULL << (39 + 7 + (r->tuner & 0x0F));
    return bits;
}

static void BuildPosition(const SBC_REPORT *r, VJOY_POSITION *p)
{
    const unsigned long long bits = ReportButtons(r);
    ZeroMemory(p, sizeof(*p));
    p->bDevice = VJOY_ID;
    p->wAxisX = AxisU(r->aimX);
    p->wAxisY = AxisU(r->aimY);
    p->wAxisZ = AxisS(r->rotation);
    p->wAxisXRot = AxisS(r->sightX);
    p->wAxisYRot = AxisS(r->sightY);
    /* JoyToKey axis 7 (first slider) was the clutch under the kernel driver */
    p->wAxisZRot = AxisU(r->brake);
    p->wSlider = AxisU(r->clutch);
    p->wDial = AxisU(r->throttle);
    p->lButtons = (LONG)(bits & 0xFFFFFFFFULL);
    p->lButtonsEx1 = (LONG)(bits >> 32);
    p->bHats = p->bHatsEx1 = p->bHatsEx2 = p->bHatsEx3 = 0xFFFFFFFF;
}

static SHORT ScaleStick(LONG v, const AXIS_CAL *c, BOOL invert)
{
    const LONG off = v - c->center;
    const LONG size = off < 0 ? -off : off;
    const LONG span = (off < 0 ? c->center - c->min : c->max - c->center) - c->dead;
    LONG out;
    if (size <= c->dead || span <= 0) return 0;
    out = (LONG)(((long long)(size - c->dead) * 32767) / span);
    if (out > 32767) out = 32767;
    if (off < 0) out = -out;
    return (SHORT)(invert ? -out : out);
}

static BYTE ScaleTrigger(LONG v, const AXIS_CAL *c)
{
    const LONG span = c->max - c->min - c->dead;
    const LONG size = v - c->min - c->dead;
    LONG out;
    if (size <= 0 || span <= 0) return 0;
    out = (LONG)(((long long)size * 255) / span);
    return (BYTE)(out > 255 ? 255 : out);
}

static SHORT AnalogStick(const XBOX_ANALOG *a, const LONG *axes, unsigned long long bits)
{
    if (a->axis >= 0) return ScaleStick(axes[a->axis], &g_cal[a->axis], a->invert);
    return (bits & a->buttons) ? (SHORT)(a->invert ? -32767 : 32767) : 0;
}

static BYTE AnalogTrigger(const XBOX_ANALOG *a, const LONG *axes, unsigned long long bits)
{
    if (a->axis >= 0) return ScaleTrigger(axes[a->axis], &g_cal[a->axis]);
    return (bits & a->buttons) ? 255 : 0;
}

static void BuildXbox(const SBC_REPORT *r, XUSB_REPORT *x)
{
    const unsigned long long bits = ReportButtons(r);
    LONG axes[AX_COUNT];
    size_t i;
    axes[AX_AIMX] = AxisU(r->aimX);
    axes[AX_AIMY] = AxisU(r->aimY);
    axes[AX_ROTATION] = AxisS(r->rotation);
    axes[AX_SIGHTX] = AxisS(r->sightX);
    axes[AX_SIGHTY] = AxisS(r->sightY);
    axes[AX_CLUTCH] = AxisU(r->clutch);
    axes[AX_BRAKE] = AxisU(r->brake);
    axes[AX_THROTTLE] = AxisU(r->throttle);

    ZeroMemory(x, sizeof(*x));
    EnterCriticalSection(&g_mapLock);
    for (i = 0; i < AX_COUNT; ++i)
    {
        if (axes[i] < g_cal[i].min) g_cal[i].min = axes[i];
        if (axes[i] > g_cal[i].max) g_cal[i].max = axes[i];
        g_lastAxis[i] = axes[i];
    }
    g_lastAxisValid = TRUE;
    for (i = 0; i < XBOX_BUTTON_COUNT; ++i)
    {
        if (bits & g_xboxButtons[i].buttons) x->wButtons |= g_xboxButtons[i].flag;
    }
    x->bLeftTrigger = AnalogTrigger(&g_xboxAnalog[XA_LT], axes, bits);
    x->bRightTrigger = AnalogTrigger(&g_xboxAnalog[XA_RT], axes, bits);
    x->sThumbLX = AnalogStick(&g_xboxAnalog[XA_LX], axes, bits);
    x->sThumbLY = AnalogStick(&g_xboxAnalog[XA_LY], axes, bits);
    x->sThumbRX = AnalogStick(&g_xboxAnalog[XA_RX], axes, bits);
    x->sThumbRY = AnalogStick(&g_xboxAnalog[XA_RY], axes, bits);
    LeaveCriticalSection(&g_mapLock);
}

static void NeutralPosition(VJOY_POSITION *p)
{
    ZeroMemory(p, sizeof(*p));
    p->bDevice = VJOY_ID;
    p->wAxisX = p->wAxisY = p->wAxisZ = p->wAxisXRot = p->wAxisYRot = (VJOY_AXIS_MAX + 1) / 2;
    p->bHats = p->bHatsEx1 = p->bHatsEx2 = p->bHatsEx3 = 0xFFFFFFFF;
}

/* Strongest rumble motor the game asked for, 0..255 (Xbox mode only) */
static volatile LONG g_rumble;

static void SetLedLevel(BYTE *leds, int led, BYTE level)
{
    leds[led / 2] &= (BYTE)((led & 1) ? 0x0F : 0xF0);
    leds[led / 2] |= (BYTE)((led & 1) ? (level << 4) : level);
}

static void SetLed(BYTE *leds, int led) { SetLedLevel(leds, led, LED_MAX); }

/* Dial on 13 (button 60): lit unless held. Dial on 14 (button 61): lit while
   held, and the others flash while the game rumbles, brighter for stronger
   rumble. Dial on 15 (button 62): everything off. Any other position: lit
   only while held. One gear light follows the lever. */
static void BuildLeds(const SBC_REPORT *r, BYTE *leds)
{
    const int dial = r->tuner & 0x0F;
    const BOOL litUnlessHeld = dial == DIAL_LIT_UNLESS_HELD;
    const LONG rumble = g_rumble;
    const BOOL flashOn = dial == DIAL_RUMBLE_FLASH && rumble > 0 && (GetTickCount() / FLASH_HALF_PERIOD_MS) % 2 == 0;
    const BYTE flashLevel = (BYTE)(1 + rumble * (LED_MAX - 1) / 255);
    const int gear = r->gear;
    int led;
    ZeroMemory(leds, LED_DATA_SIZE);
    if (dial == DIAL_ALL_OFF) return;
    for (led = LED_FIRST_BUTTON; led <= LED_LAST_BUTTON; ++led)
    {
        const int bit = led - 1;
        const BOOL held = (r->buttons[bit / 8] & (1 << (bit % 8))) != 0;
        if (held != litUnlessHeld) SetLed(leds, led);
        else if (flashOn) SetLedLevel(leds, led, flashLevel);
    }
    if (gear >= -2 && gear <= 5 && gear != 0)
        SetLed(leds, LED_GEAR_REVERSE + (gear < 0 ? gear + 2 : gear + 1));
}

/* ---- Controller ---- */

typedef struct {
    HANDLE file;
    WINUSB_INTERFACE_HANDLE usb;
    UCHAR inPipe, outPipe;
} CONTROLLER;

static BOOL OpenByInterface(const GUID *guid, CONTROLLER *c)
{
    HDEVINFO set = SetupDiGetClassDevsW(guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA ifd;
    DWORD index;
    BOOL opened = FALSE;
    if (set == INVALID_HANDLE_VALUE) return FALSE;
    ifd.cbSize = sizeof(ifd);
    for (index = 0; !opened && SetupDiEnumDeviceInterfaces(set, NULL, guid, index, &ifd); ++index)
    {
        DWORD need = 0;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, NULL, 0, &need, NULL);
        if (need == 0) continue;
        detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)malloc(need);
        if (!detail) continue;
        detail->cbSize = sizeof(*detail);
        if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, NULL, NULL))
        {
            wchar_t lower[512];
            wcsncpy_s(lower, 512, detail->DevicePath, _TRUNCATE);
            _wcslwr_s(lower, 512);
            if (wcsstr(lower, PATH_MATCH))
            {
                c->file = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
                if (c->file == INVALID_HANDLE_VALUE)
                {
                    Log("open failed (%lu): %ls", GetLastError(), detail->DevicePath);
                }
                else if (!WinUsb_Initialize(c->file, &c->usb))
                {
                    Log("WinUsb_Initialize failed (%lu): %ls", GetLastError(), detail->DevicePath);
                    CloseHandle(c->file);
                    c->file = INVALID_HANDLE_VALUE;
                }
                else
                {
                    Log("opened %ls", detail->DevicePath);
                    opened = TRUE;
                }
            }
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(set);
    return opened;
}

static void CloseController(CONTROLLER *c)
{
    if (c->usb) { WinUsb_Free(c->usb); c->usb = NULL; }
    if (c->file != INVALID_HANDLE_VALUE) { CloseHandle(c->file); c->file = INVALID_HANDLE_VALUE; }
}

static BOOL OpenController(CONTROLLER *c)
{
    USB_INTERFACE_DESCRIPTOR desc;
    UCHAR i;
    ULONG timeout;
    c->file = INVALID_HANDLE_VALUE;
    c->usb = NULL;
    c->inPipe = c->outPipe = 0;
    if (!OpenByInterface(&GUID_WINUSB_INBOX, c) && !OpenByInterface(&GUID_USB_DEVICE, c)) return FALSE;
    if (!WinUsb_QueryInterfaceSettings(c->usb, 0, &desc))
    {
        Log("WinUsb_QueryInterfaceSettings failed: %lu", GetLastError());
        CloseController(c);
        return FALSE;
    }
    for (i = 0; i < desc.bNumEndpoints; ++i)
    {
        WINUSB_PIPE_INFORMATION pipe;
        if (!WinUsb_QueryPipe(c->usb, 0, i, &pipe)) continue;
        Log("pipe 0x%02X type %d max %u", pipe.PipeId, (int)pipe.PipeType, pipe.MaximumPacketSize);
        if (USB_ENDPOINT_DIRECTION_IN(pipe.PipeId)) { if (!c->inPipe) c->inPipe = pipe.PipeId; }
        else if (!c->outPipe) c->outPipe = pipe.PipeId;
    }
    if (!c->inPipe)
    {
        Log("no input pipe");
        CloseController(c);
        return FALSE;
    }
    timeout = 50; /* short, so lights keep flashing if reports pause */
    WinUsb_SetPipePolicy(c->usb, c->inPipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
    if (c->outPipe)
    {
        timeout = 100;
        WinUsb_SetPipePolicy(c->usb, c->outPipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
    }
    return TRUE;
}

static void WriteLeds(CONTROLLER *c, const BYTE *leds)
{
    ULONG written = 0;
    if (c->outPipe && !WinUsb_WritePipe(c->usb, c->outPipe, (PUCHAR)leds, LED_DATA_SIZE, &written, NULL))
        Log("light write failed: %lu", GetLastError());
}

static void LogChanges(VJOY_POSITION *logged, const VJOY_POSITION *b)
{
    unsigned long long x = ((unsigned long long)(unsigned long)logged->lButtonsEx1 << 32) | (unsigned long)logged->lButtons;
    unsigned long long y = ((unsigned long long)(unsigned long)b->lButtonsEx1 << 32) | (unsigned long)b->lButtons;
    int i;
    for (i = 0; i < VJOY_BUTTONS; ++i)
    {
        if (((x ^ y) >> i) & 1) Log("button %d %s", i + 1, ((y >> i) & 1) ? "down" : "up");
    }
    logged->lButtons = b->lButtons;
    logged->lButtonsEx1 = b->lButtonsEx1;
#define AXIS(name, field) if (labs(logged->field - b->field) > 400) { Log("%s %ld", name, b->field); logged->field = b->field; }
    AXIS("aimX", wAxisX); AXIS("aimY", wAxisY); AXIS("rotation", wAxisZ);
    AXIS("sightX", wAxisXRot); AXIS("sightY", wAxisYRot);
    AXIS("brake", wAxisZRot); AXIS("clutch", wSlider); AXIS("throttle", wDial);
#undef AXIS
}

/* ---- Output ---- */

static PVIGEM_CLIENT g_vigem;
static PVIGEM_TARGET g_xboxPad;

static void XboxClose(void)
{
    if (g_xboxPad)
    {
        vigem_target_x360_unregister_notification(g_xboxPad);
        InterlockedExchange(&g_rumble, 0);
        vigem_target_remove(g_vigem, g_xboxPad);
        vigem_target_free(g_xboxPad);
        g_xboxPad = NULL;
    }
    if (g_vigem)
    {
        vigem_disconnect(g_vigem);
        vigem_free(g_vigem);
        g_vigem = NULL;
    }
}

static VOID CALLBACK OnRumble(PVIGEM_CLIENT client, PVIGEM_TARGET target, UCHAR large, UCHAR small, UCHAR ledNumber, LPVOID user)
{
    UNREFERENCED_PARAMETER(client); UNREFERENCED_PARAMETER(target);
    UNREFERENCED_PARAMETER(ledNumber); UNREFERENCED_PARAMETER(user);
    InterlockedExchange(&g_rumble, large > small ? large : small);
    if (g_verbose) Log("rumble large=%u small=%u", large, small);
}

static BOOL XboxOpen(void)
{
    VIGEM_ERROR error;
    g_vigem = vigem_alloc();
    if (!g_vigem) return FALSE;
    error = vigem_connect(g_vigem);
    if (!VIGEM_SUCCESS(error))
    {
        Log("ViGEmBus connect failed: 0x%08X", (unsigned)error);
        SetStatus(L"ViGEmBus is not installed");
        XboxClose();
        return FALSE;
    }
    g_xboxPad = vigem_target_x360_alloc();
    error = g_xboxPad ? vigem_target_add(g_vigem, g_xboxPad) : VIGEM_ERROR_NO_FREE_SLOT;
    if (!VIGEM_SUCCESS(error))
    {
        Log("virtual Xbox pad failed: 0x%08X", (unsigned)error);
        SetStatus(L"Could not create the Xbox controller");
        if (g_xboxPad) { vigem_target_free(g_xboxPad); g_xboxPad = NULL; }
        XboxClose();
        return FALSE;
    }
    error = vigem_target_x360_register_notification(g_vigem, g_xboxPad, OnRumble, NULL);
    if (!VIGEM_SUCCESS(error)) Log("rumble notifications unavailable: 0x%08X", (unsigned)error);
    Log("virtual Xbox pad created");
    return TRUE;
}

static BOOL OutputOpen(LONG mode) { return mode == MODE_XBOX ? XboxOpen() : VjoyAcquire(); }

static void OutputNeutral(LONG mode)
{
    if (mode == MODE_XBOX)
    {
        XUSB_REPORT x;
        ZeroMemory(&x, sizeof(x));
        if (g_xboxPad) vigem_target_x360_update(g_vigem, g_xboxPad, x);
    }
    else
    {
        VJOY_POSITION p;
        NeutralPosition(&p);
        if (pUpdateVJD) pUpdateVJD(VJOY_ID, &p);
    }
}

static void OutputClose(LONG mode)
{
    OutputNeutral(mode);
    if (mode == MODE_XBOX) XboxClose();
    else if (pRelinquishVJD) pRelinquishVJD(VJOY_ID);
}

static DWORD WINAPI Worker(LPVOID unused)
{
    LONG active = -1;
    CONTROLLER c = { 0 };
    BOOL controllerOpen = FALSE;
    UNREFERENCED_PARAMETER(unused);
    while (!g_quit)
    {
        BYTE lastLeds[LED_DATA_SIZE], leds[LED_DATA_SIZE];
        BYTE lastReport[64];
        BOOL ledsValid = FALSE, announced = FALSE, haveReport = FALSE;
        VJOY_POSITION last, now, logged;
        XUSB_REPORT lastXbox, nowXbox;
        BOOL xboxValid = FALSE;
        DWORD lastXboxSend = 0;
        int nudgesLeft = 3;
        const LONG mode = g_mode;

        if (active != mode)
        {
            if (active != -1) OutputClose(active);
            active = -1;
            if (!OutputOpen(mode)) { WaitForSingleObject(g_quitEvent, 2000); continue; }
            active = mode;
        }
        if (!controllerOpen)
        {
            if (!OpenController(&c))
            {
                SetStatus(L"Controller not found");
                WaitForSingleObject(g_quitEvent, 1000);
                continue;
            }
            controllerOpen = TRUE;
        }
        SetStatus(mode == MODE_XBOX ? L"Running as Xbox controller" : L"Running as vJoy");
        NeutralPosition(&last);
        logged = last;

        while (!g_quit && g_mode == active)
        {
            BYTE buffer[64];
            ULONG got = 0;
            const SBC_REPORT *report = (const SBC_REPORT *)buffer;
            if (!WinUsb_ReadPipe(c.usb, c.inPipe, buffer, sizeof(buffer), &got, NULL))
            {
                const DWORD error = GetLastError();
                if (error == ERROR_SEM_TIMEOUT)
                {
                    /* No new report: still redraw the lights from the last one */
                    if (haveReport)
                    {
                        BuildLeds((const SBC_REPORT *)lastReport, leds);
                        if (!ledsValid || memcmp(leds, lastLeds, LED_DATA_SIZE) != 0)
                        {
                            WriteLeds(&c, leds);
                            memcpy(lastLeds, leds, LED_DATA_SIZE);
                            ledsValid = TRUE;
                        }
                    }
                    continue;
                }
                Log("read failed: %lu", error);
                CloseController(&c);
                controllerOpen = FALSE;
                OutputNeutral(active);
                SetStatus(L"Controller disconnected");
                break;
            }
            if (got < REPORT_MIN_SIZE)
            {
                if (!announced) { Log("short report: %lu bytes", got); announced = TRUE; }
                continue;
            }
            memcpy(lastReport, buffer, sizeof(lastReport));
            haveReport = TRUE;
            BuildPosition(report, &now);
            if (g_verbose) LogChanges(&logged, &now);
            if (active == MODE_XBOX)
            {
                BuildXbox(report, &nowXbox);
                const BOOL changed = !xboxValid || memcmp(&nowXbox, &lastXbox, sizeof(nowXbox)) != 0;
                /* A new pad shows random stick values until it gets a report that
                   isn't all zeros, which a controller at rest would never send.
                   So nudge it a few times just as it starts up. */
                if (changed || GetTickCount() - lastXboxSend > 500)
                {
                    if (nudgesLeft > 0)
                    {
                        XUSB_REPORT nudge = nowXbox;
                        if (nudge.sThumbLX == 0) nudge.sThumbLX = 1;
                        vigem_target_x360_update(g_vigem, g_xboxPad, nudge);
                        Sleep(5);
                        --nudgesLeft;
                    }
                    const VIGEM_ERROR error = vigem_target_x360_update(g_vigem, g_xboxPad, nowXbox);
                    if (g_verbose && changed) Log("xbox btn=%04X lt=%u rt=%u L=%d,%d R=%d,%d", nowXbox.wButtons, nowXbox.bLeftTrigger, nowXbox.bRightTrigger, nowXbox.sThumbLX, nowXbox.sThumbLY, nowXbox.sThumbRX, nowXbox.sThumbRY);
                    if (!VIGEM_SUCCESS(error)) Log("Xbox update failed: 0x%08X", (unsigned)error);
                    lastXbox = nowXbox;
                    lastXboxSend = GetTickCount();
                    xboxValid = TRUE;
                }
            }
            else if (memcmp(&now, &last, sizeof(now)) != 0)
            {
                if (!pUpdateVJD(VJOY_ID, &now)) Log("vJoy update failed");
                last = now;
            }
            BuildLeds(report, leds);
            if (!ledsValid || memcmp(leds, lastLeds, LED_DATA_SIZE) != 0)
            {
                WriteLeds(&c, leds);
                memcpy(lastLeds, leds, LED_DATA_SIZE);
                ledsValid = TRUE;
            }
        }
    }
    /* Nothing stays held and no lights stay on once we stop */
    if (active != -1) OutputClose(active);
    if (controllerOpen)
    {
        BYTE off[LED_DATA_SIZE];
        ZeroMemory(off, LED_DATA_SIZE);
        WriteLeds(&c, off);
        CloseController(&c);
    }
    return 0;
}

/* ---- Setup ---- */

static BOOL IsAdmin(void)
{
    BOOL admin = FALSE;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID group;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &group))
    {
        CheckTokenMembership(NULL, group, &admin);
        FreeSid(group);
    }
    return admin;
}

static BOOL ConfigureVjoy(void)
{
    wchar_t folder[MAX_PATH], exe[MAX_PATH], cmd[512];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    if (!VjoyFolder(folder, MAX_PATH)) return FALSE;
    swprintf_s(exe, MAX_PATH, L"%svJoyConfig.exe", folder);
    swprintf_s(cmd, 512, L"\"%s\" %d -f -a x y z rx ry rz sl0 sl1 -b %d", exe, VJOY_ID, VJOY_BUTTONS);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, folder, &si, &pi))
    {
        Log("vJoyConfig could not start: %lu", GetLastError());
        return FALSE;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    Log("vJoyConfig exit code %lu", code);
    return code == 0;
}

static BOOL FindControllerInstance(wchar_t *instanceId, DWORD cch, wchar_t *classGuid, DWORD classCch)
{
    HDEVINFO set = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    SP_DEVINFO_DATA dev;
    DWORD index;
    BOOL found = FALSE;
    if (set == INVALID_HANDLE_VALUE) return FALSE;
    dev.cbSize = sizeof(dev);
    for (index = 0; !found && SetupDiEnumDeviceInfo(set, index, &dev); ++index)
    {
        wchar_t ids[1024] = L"";
        wchar_t *id;
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, NULL, (PBYTE)ids, sizeof(ids) - sizeof(wchar_t), NULL)) continue;
        for (id = ids; *id; id += wcslen(id) + 1)
        {
            if (_wcsicmp(id, HARDWARE_ID) == 0)
            {
                classGuid[0] = 0;
                SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_CLASSGUID, NULL, (PBYTE)classGuid, (classCch - 1) * sizeof(wchar_t), NULL);
                found = SetupDiGetDeviceInstanceIdW(set, &dev, instanceId, cch, NULL);
                break;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

static BOOL SetDeviceClass(const wchar_t *instanceId, const wchar_t *classGuid)
{
    DEVINST inst;
    CONFIGRET cr = CM_Locate_DevNodeW(&inst, (DEVINSTID_W)instanceId, CM_LOCATE_DEVNODE_NORMAL);
    if (cr == CR_SUCCESS)
        cr = CM_Set_DevNode_Registry_PropertyW(inst, CM_DRP_CLASSGUID, (PVOID)classGuid, (ULONG)((wcslen(classGuid) + 1) * sizeof(wchar_t)), 0);
    if (cr != CR_SUCCESS) Log("set device class failed: CONFIGRET %lu", cr);
    return cr == CR_SUCCESS;
}

static BOOL BindWinUsb(void)
{
    wchar_t instanceId[256], oldClass[64] = L"";
    HDEVINFO set;
    SP_DEVINFO_DATA dev;
    SP_DEVINSTALL_PARAMS_W params;
    SP_DRVINFO_DATA_W drv;
    DWORD i;
    BOOL picked = FALSE, reboot = FALSE, ok = FALSE;
    UINT n;

    if (!FindControllerInstance(instanceId, 256, oldClass, 64))
    {
        Log("controller is not plugged in");
        return FALSE;
    }
    Log("controller %ls, class %ls", instanceId, oldClass);

    /* Windows only lists drivers from the device's own setup class, and
       winusb.inf belongs to the USB devices class. Device Manager's manual
       driver picker makes the same switch. */
    if (!SetDeviceClass(instanceId, L"{88BAE032-5A81-49F0-BC3D-A4FF138216D6}")) return FALSE;

    set = SetupDiCreateDeviceInfoList(NULL, NULL);
    dev.cbSize = sizeof(dev);
    if (set == INVALID_HANDLE_VALUE || !SetupDiOpenDeviceInfoW(set, instanceId, NULL, 0, &dev))
    {
        Log("open device failed: %lu", GetLastError());
        goto done;
    }

    params.cbSize = sizeof(params);
    SetupDiGetDeviceInstallParamsW(set, &dev, &params);
    n = GetWindowsDirectoryW(params.DriverPath, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 24) goto done;
    wcscat_s(params.DriverPath, MAX_PATH, L"\\INF\\winusb.inf");
    params.Flags |= DI_ENUMSINGLEINF;
    params.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    if (!SetupDiSetDeviceInstallParamsW(set, &dev, &params)) Log("set params failed: %lu", GetLastError());

    if (!SetupDiBuildDriverInfoList(set, &dev, SPDIT_CLASSDRIVER))
        Log("build driver list failed: %lu", GetLastError());

    drv.cbSize = sizeof(drv);
    for (i = 0; SetupDiEnumDriverInfoW(set, &dev, SPDIT_CLASSDRIVER, i, &drv); ++i)
    {
        Log("candidate driver: %ls", drv.Description);
        if (_wcsicmp(drv.Description, L"WinUsb Device") == 0) { picked = TRUE; break; }
    }
    if (!picked)
    {
        Log("WinUsb Device was not offered: %lu", GetLastError());
    }
    else if (!SetupDiSetSelectedDriverW(set, &dev, &drv))
    {
        Log("select driver failed: %lu", GetLastError());
    }
    else if (!DiInstallDevice(NULL, set, &dev, &drv, 0, &reboot))
    {
        Log("DiInstallDevice failed: 0x%08lX", GetLastError());
    }
    else
    {
        Log("WinUSB bound, reboot needed: %d", reboot);
        ok = TRUE;
    }

done:
    if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    if (!ok && oldClass[0]) SetDeviceClass(instanceId, oldClass);
    return ok;
}

static int RunSetup(BOOL quiet)
{
    HWND running = FindWindowW(WINDOW_CLASS, NULL);
    BOOL vjoy, usb;
    wchar_t message[256];
    if (running)
    {
        PostMessageW(running, WM_CLOSE, 0, 0);
        Sleep(1500);
    }
    vjoy = ConfigureVjoy();
    usb = BindWinUsb();
    Log("setup finished: vjoy=%d winusb=%d", vjoy, usb);
    if (!quiet)
    {
        swprintf_s(message, 256, L"vJoy buttons and axes: %s\nController on WinUSB: %s\n\nDetails are in feeder.log.",
            vjoy ? L"done" : L"not set (only needed for vJoy mode)", usb ? L"done" : L"FAILED");
        MessageBoxW(NULL, message, L"Steel Battalion Feeder setup", usb ? MB_ICONINFORMATION : MB_ICONERROR);
    }
    return usb ? 0 : 1;
}

static int ElevateSetup(BOOL quiet)
{
    wchar_t self[MAX_PATH];
    SHELLEXECUTEINFOW sei;
    DWORD code = 1;
    GetModuleFileNameW(NULL, self, MAX_PATH);
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = quiet ? L"--setup --quiet" : L"--setup";
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return 1;
    WaitForSingleObject(sei.hProcess, INFINITE);
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return (int)code;
}

/* ---- Tray ---- */

static BOOL AutostartEnabled(void)
{
    HKEY key;
    BOOL on = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &key) == ERROR_SUCCESS)
    {
        on = RegQueryValueExW(key, APP_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
        RegCloseKey(key);
    }
    return on;
}

static void AutostartSet(BOOL on)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (on)
    {
        wchar_t self[MAX_PATH], quoted[MAX_PATH + 2];
        GetModuleFileNameW(NULL, self, MAX_PATH);
        swprintf_s(quoted, MAX_PATH + 2, L"\"%s\"", self);
        RegSetValueExW(key, APP_NAME, 0, REG_SZ, (const BYTE *)quoted, (DWORD)((wcslen(quoted) + 1) * sizeof(wchar_t)));
    }
    else
    {
        RegDeleteValueW(key, APP_NAME);
    }
    RegCloseKey(key);
}

static void TrayUpdate(HWND hwnd, DWORD action)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    EnterCriticalSection(&g_statusLock);
    swprintf_s(nid.szTip, 128, L"Steel Battalion: %s", g_status);
    LeaveCriticalSection(&g_statusLock);
    Shell_NotifyIconW(action, &nid);
}

static void TrayMenu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    POINT pt;
    wchar_t status[128];
    EnterCriticalSection(&g_statusLock);
    wcsncpy_s(status, 128, g_status, _TRUNCATE);
    LeaveCriticalSection(&g_statusLock);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (g_mode == MODE_XBOX ? MF_CHECKED : 0), MENU_XBOX, L"Xbox controller (games and emulators)");
    AppendMenuW(menu, MF_STRING | (g_mode == MODE_VJOY ? MF_CHECKED : 0), MENU_VJOY, L"vJoy (JoyToKey, all 62 buttons)");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, MENU_CENTER, L"Recenter sticks");
    AppendMenuW(menu, MF_STRING, MENU_SETTINGS, L"Edit settings");
    AppendMenuW(menu, MF_STRING, MENU_RELOAD, L"Reload settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), MENU_AUTOSTART, L"Start with Windows");
    AppendMenuW(menu, MF_STRING, MENU_EXIT, L"Exit");
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static UINT taskbarCreated;
    if (taskbarCreated == 0) taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbarCreated) { TrayUpdate(hwnd, NIM_ADD); return 0; }
    switch (msg)
    {
    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) TrayMenu(hwnd);
        return 0;
    case WM_STATUS:
        TrayUpdate(hwnd, NIM_MODIFY);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == MENU_VJOY || LOWORD(wp) == MENU_XBOX)
        {
            const LONG mode = LOWORD(wp) == MENU_XBOX ? MODE_XBOX : MODE_VJOY;
            InterlockedExchange(&g_mode, mode);
            SaveMode(mode);
        }
        else if (LOWORD(wp) == MENU_CENTER) RecenterSticks();
        else if (LOWORD(wp) == MENU_RELOAD) LoadSettings();
        else if (LOWORD(wp) == MENU_SETTINGS)
        {
            wchar_t path[MAX_PATH];
            if (SettingsPath(path)) ShellExecuteW(NULL, L"open", L"notepad.exe", path, NULL, SW_SHOWNORMAL);
        }
        else if (LOWORD(wp) == MENU_AUTOSTART) AutostartSet(!AutostartEnabled());
        else if (LOWORD(wp) == MENU_EXIT) DestroyWindow(hwnd);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        TrayUpdate(hwnd, NIM_DELETE);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev, PWSTR cmdLine, int show)
{
    WNDCLASSW wc;
    HANDLE mutex, worker;
    MSG msg;
    BOOL setup, quiet;
    LONG forcedMode = -1;

    UNREFERENCED_PARAMETER(prev);
    UNREFERENCED_PARAMETER(show);

    setup = wcsstr(cmdLine, L"--setup") != NULL;
    quiet = wcsstr(cmdLine, L"--quiet") != NULL;
    g_verbose = wcsstr(cmdLine, L"--verbose") != NULL;
    if (wcsstr(cmdLine, L"--vjoy")) forcedMode = MODE_VJOY;
    else if (wcsstr(cmdLine, L"--xbox")) forcedMode = MODE_XBOX;

    if (setup)
    {
        int result;
        if (!IsAdmin()) return ElevateSetup(quiet);
        LogOpen(L"a");
        result = RunSetup(quiet);
        if (g_log) fclose(g_log);
        return result;
    }

    mutex = CreateMutexW(NULL, FALSE, L"Local\\" APP_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND running = FindWindowW(WINDOW_CLASS, NULL);
        if (running && forcedMode >= 0) PostMessageW(running, WM_COMMAND, forcedMode == MODE_XBOX ? MENU_XBOX : MENU_VJOY, 0);
        return 0;
    }

    LogOpen(L"w");
    InitializeCriticalSection(&g_statusLock);
    InitializeCriticalSection(&g_mapLock);
    LoadSettings();
    if (forcedMode >= 0)
    {
        InterlockedExchange(&g_mode, forcedMode);
        SaveMode(forcedMode);
    }
    g_quitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = WINDOW_CLASS;
    RegisterClassW(&wc);
    g_window = CreateWindowW(WINDOW_CLASS, APP_NAME, 0, 0, 0, 0, 0, NULL, NULL, instance, NULL);
    TrayUpdate(g_window, NIM_ADD);

    worker = CreateThread(NULL, 0, Worker, NULL, 0, NULL);

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    InterlockedExchange(&g_quit, 1);
    SetEvent(g_quitEvent);
    if (worker)
    {
        WaitForSingleObject(worker, 3000);
        CloseHandle(worker);
    }
    SaveCalibration();
    if (g_log) fclose(g_log);
    if (mutex) CloseHandle(mutex);
    return 0;
}
