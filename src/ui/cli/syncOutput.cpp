// SPDX-License-Identifier: GPL-3.0-or-later

import std;

import isocmd.ui.cli.syncOutput;

SyncOutput::SyncOutput()  { std::cout << kSyncBegin; }
SyncOutput::~SyncOutput() { std::cout << kSyncEnd << std::flush; }
