#include <windows.h>
#include <cstdio>
int main(){
    std::printf("RetroSound compatibility probe is running.\n");
    std::fflush(stdout);
    int r=MessageBoxW(nullptr,
        L"If you can read this, Win32 x86 programs compiled by RetroSound run correctly on this Windows installation.",
        L"RetroSound Compatibility Probe",
        MB_OK|MB_ICONINFORMATION|MB_SYSTEMMODAL);
    std::printf("MessageBox returned %d.\n",r);
    return 0;
}
