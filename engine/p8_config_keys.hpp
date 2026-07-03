#pragma once

/// JSON configuration key names — single point of definition.
/// Used by engine and tests; keeps key strings in sync with doc/config_example.json.

#define P8_CFG_KEY_SINK                "sink"
#define P8_CFG_KEY_DESTINATION         "destination"
#define P8_CFG_KEY_MAX_MEMORY_SIZE     "max_memory_size"
#define P8_CFG_KEY_INITIAL_MEMORY_SIZE "initial_memory_size"

/// Recognized values for the "sink" key.
#define P8_CFG_VAL_SINK_FILE_BIN       "file.bin"
#define P8_CFG_VAL_SINK_NETWORK_TCP    "network.tcp"
#define P8_CFG_VAL_SINK_NETWORK_NULL   "null"

/// "FileBin" sink section and its keys.
#define P8_CFG_KEY_FILE_BIN            "FileBin"
#define P8_CFG_KEY_FILE_OUT_DIR        "OutDir"
