// conflict.h — Store.dat conflict resolution dialog
#pragma once

#include <windows.h>
#include <string>

enum class ConflictChoice { UseLeft, UseRight, UseMerged, Cancel };

// Show a dialog comparing two store.dat files side by side.
// leftDir/rightDir are the directories containing store.dat.
// Returns the user's choice.
ConflictChoice ShowConflictDialog(HWND parent, const std::wstring& leftDir, const std::wstring& rightDir);
