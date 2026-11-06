// Read-only consistency check of a card profile (card_profile_t), run once at boot by card_runtime_init() and callable
// from the host tests.  It never changes the profile and never stops the boot: every finding is one console line
//
//   profile: <error|warning|note>: <subject>: <what is wrong> (<the rule it breaks>)
//
// and the number of findings is returned.  The few defects that would make the framework read or write outside the
// profile's objects are also refused where the objects are used: fs.c skips a file for which profile_file_defect()
// returns a reason, dispatch.c an applet for which profile_applet_defect() does, sec_init() uses no PINs when `pins`
// is NULL.  docs/card-profile.md (section on profile validation) lists the checks.
#pragma once

#include "iso7816_pico/card.h"

#include <stddef.h>

// At most this many findings are printed (the boot log ring must keep room for the rest of the boot messages); the
// rest are counted and summarised.
#define PROFILE_CHECK_MAX_LINES 24

// Check `p` and log the findings.  Returns their number (0 for a clean profile); when there are any, a summary line
// "profile: N finding(s), E error(s)" follows them (not counted).
unsigned profile_check(const card_profile_t *p);

// Why file `f` (an element of files[0..n-1]) cannot be used without reading or writing outside the profile's objects,
// or NULL when it can: an EF with a size but no data, a record EF without room for the record header, a linear
// variable EF whose slot size overflows, a parent pointer outside files[] (also for any ancestor) or an unusable
// ancestor.  The checks are O(depth), cheap enough for every lookup.
const char *profile_file_defect(const fs_file_t *files, size_t n, const fs_file_t *f);

// Same for an applet: NULL, an AID length without an AID, a command count without a command table.
const char *profile_applet_defect(const applet_t *a);
