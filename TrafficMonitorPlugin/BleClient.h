#pragma once

#include <string>
#include <windows.h>

/* 1 connected, 0 radio not found, -1 found but pairing was not finished.
 * abort_fn stops the attempt so a plugged-in USB cable can take over. */
int BleOpen(DWORD scan_ms, DWORD auth_ms,
            void (*status)(const wchar_t* text, void* ctx), void* ctx,
            bool (*abort_fn)(void* ctx), void* abort_ctx);
void BleClose();
bool BleIsOpen();
bool BleLinkUp();
bool BleWrite(const char* data, DWORD n);
void BleTake(std::string& out);
