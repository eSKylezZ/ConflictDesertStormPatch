#pragma once
#include <windows.h>

// Export of the real system dinput8.dll (this DLL replaces it in the game folder).
FARPROC RealDInput8(const char* name);
