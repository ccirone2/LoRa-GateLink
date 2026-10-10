// Force-included (-include) into every firmware .cpp of the libFuzzer build: all of the firmware's globals and
// statics, function-local ones included, land in two named sections, gl_bss and gl_data. The harness copies them
// after static initialisation and copies them back before every input, so each input starts from fresh firmware
// RAM (harness.cpp, Snapshot). ASan keeps instrumenting these globals: the pragma sets a section attribute, not an
// explicit section, so their redzones stay (verified in harness.cpp's self-test and by check_sections.sh).
//
// GCC has no such pragma; the g++ replay build runs each input in a fork()ed child instead.
#pragma once
#if defined(__clang__)
#pragma clang section bss = "gl_bss" data = "gl_data"
#endif
