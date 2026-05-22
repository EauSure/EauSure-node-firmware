#pragma once

// Semantic firmware version reported in MEASURE_RESP ("fw" field).
// Build flag NODE_FIRMWARE_VERSION is injected via build_opt.h (see version.txt + tools/embed_version.py).
const char* getNodeFirmwareVersion();
