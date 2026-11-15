/* Future launchd Program override; stock bluetoothd/MachServices stay intact.
 * Build only: no launch job is installed until SSPM and runtime are proven. */
#include "X9Bootstrap16.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    if (geteuid() != 501) {
        fprintf(stderr, "X9 Bluetooth launch: requires stock mobile job identity\n");
        return 1;
    }
    int error = x9_bootstrap16_wait(60);
    if (error) {
        fprintf(stderr, "X9 Bluetooth launch: board session unavailable errno=%d; daemon not executed\n", error);
        return 1;
    }
    if (access("/usr/lib/X9H4Transport.dylib", R_OK) || access("/usr/sbin/bluetoothd", X_OK)) {
        fprintf(stderr, "X9 Bluetooth launch: required runtime missing errno=%d\n", errno);
        return 1;
    }
    /* Constructor must run AFTER board readiness. Injecting into a daemon
     * already running during fwdl would leave its vendor hooks uninstalled. */
    if (setenv("BT_HCI_TRANSPORT", "H4:POSIX", 1) ||
        setenv("X9_BT_VENDOR", "MT6653", 1) ||
        setenv("DYLD_INSERT_LIBRARIES", "/usr/lib/X9H4Transport.dylib", 1)) {
        fprintf(stderr, "X9 Bluetooth launch: environment setup errno=%d\n", errno);
        return 1;
    }
    char *const args[] = {"/usr/sbin/bluetoothd", NULL};
    execv(args[0], args);
    fprintf(stderr, "X9 Bluetooth launch: exec errno=%d\n", errno);
    return 1;
}
