#pragma once

//
// This is the definitive master header to resolve all Windows and Raylib conflicts.
// test

// Instruct the Windows header to exclude GDI function definitions like Rectangle,
// which is the source of the main conflict.
#define NOGDI

// Define PLATFORM_DESKTOP to solve remaining minor conflicts like CloseWindow.
#define PLATFORM_DESKTOP

// Standard macros to reduce Windows header bloat.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

// Include headers in the correct, controlled order.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include "raylib.h"