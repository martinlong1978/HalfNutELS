"""
PlatformIO extra_script for the `native` (host unit-test) env.

The real Spindle implementation, ESPSpindle.cpp, depends on ESP32 hardware
(ESP32Encoder, gpio_pullup_en, ...) and defines the Spindle:: methods. On host
we instead compile the test double in TestSpindle.cpp (guarded by
PIO_UNIT_TESTING), which provides the ONLY Spindle:: definition for native.

To avoid pulling ESPSpindle.cpp into the native build (which would fail to
compile on host and would clash with the test double), we register a build
middleware that drops that source node. This keeps ESPSpindle.cpp byte-for-byte
untouched while excluding it from this env only.
"""

Import("env")


def _skip_espspindle(node):
    # Returning None removes the file from the build.
    return None


# Matches the library source regardless of path separator / prefix.
env.AddBuildMiddleware(_skip_espspindle, "*ESPSpindle.cpp")
