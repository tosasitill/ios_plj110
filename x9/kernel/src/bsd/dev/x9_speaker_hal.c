/* Resource definitions are separate from PID1 code so -c can compile the
 * actual installer against this declaration without a mock/empty manifest.
 * A full kernel link REQUIRES the current builder's complete signed bundle. */
#include "x9_speaker_hal.h"
#include "../../../generated/x9_speaker_hal.inc"
