// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <iostream>

// Project Headers
#include "isocmd/ui/syncOutput.h"

SyncOutput::SyncOutput()  { std::cout << kSyncBegin; }
SyncOutput::~SyncOutput() { std::cout << kSyncEnd << std::flush; }
