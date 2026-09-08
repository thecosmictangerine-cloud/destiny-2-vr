// Loader probe DLL. Its DllMain writes one marker file beside the game so a run can tell a DLL
// that was mapped but never entered from one whose entry ran. Built twice: with no CRT at all
// (SVR_PROBE_NOCRT) and with the static CRT, to bisect what the game refuses.
#include <Windows.h>

#ifdef SVR_PROBE_NOCRT
#define SVR_PROBE_MARKER L"SVR_Probe_NoCrt.txt"
#else
#define SVR_PROBE_MARKER L"SVR_Probe_Crt.txt"
#endif

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        const HANDLE file = CreateFileW(
            SVR_PROBE_MARKER, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            const char text[] = "entered\n";
            DWORD written = 0;
            WriteFile(file, text, sizeof text - 1, &written, nullptr);
            CloseHandle(file);
        }
    }
    return TRUE;
}
