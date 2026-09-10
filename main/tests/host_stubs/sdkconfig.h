// Host stand-in for the sdkconfig.h that the IDF build generates from Kconfig.
//
// Only the options that host-testable sources actually branch on belong here,
// and each one must match the value in the repository's checked-in sdkconfig,
// because a source compiled with a different configuration than the firmware
// is not the source under test. When a value here and the value in sdkconfig
// disagree, the tests are the ones that are wrong.
#pragma once

// sdkconfig:1319 CONFIG_DEMO_MODE=y (Kconfig.projbuild default y). Demo mode
// is the simulated heat pump used for kiosk and UI work; app_preferences
// compiles the whole toggle out when this is off, so building the tests with
// it off would silently reduce three scenarios to assertions about #else.
#define CONFIG_DEMO_MODE 1
