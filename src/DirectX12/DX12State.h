#pragma once

#include <Windows.h>

void DX12SetModule(HINSTANCE module);
HINSTANCE DX12GetModule();

bool DX12OpenLogFile();
void DX12CloseLogFile();
void DX12Log(const char *fmt, ...);

DWORD DX12HookFunction(void **original, void *target, void *hook, const char *name);
void DX12UnhookAll();

LONG DX12IncrementPresentCount();
LONG DX12GetPresentCount();

void DX12SetOverlayWindow(HWND hwnd);
HWND DX12GetOverlayWindow();

