/* launchd Program override for com.apple.bluetoothd; stock MachServices stay
 * intact. With a ready native board session, exec stock bluetoothd with the
 * H4/vendor bridge. Otherwise exec stock bluetoothd unmodified, exactly what
 * the stock job does: its Mach services must always be served (log 322: a
 * failing launcher left all 13 unserved and respawned every 10 s). */
#include "X9Bootstrap16.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int stock(const char *why, int error)
{
    char *const args[] = {"/usr/sbin/bluetoothd", NULL};
    fprintf(stderr, "X9 Bluetooth launch: %s errno=%d; executing stock bluetoothd without bridge\n", why, error);
    unsetenv("DYLD_INSERT_LIBRARIES");
    unsetenv("BT_HCI_TRANSPORT");
    unsetenv("X9_BT_VENDOR");
    execv(args[0], args);
    fprintf(stderr, "X9 Bluetooth launch: stock exec errno=%d\n", errno);
    return 1;
}

int main(void)
{
    if (geteuid() != 501) return stock("not the stock mobile job identity", EPERM);
    int error = x9_bootstrap16_wait(60);
    if (!error || error == EHOSTDOWN) error = x9_bootstrap16_resume();
    if (error) return stock("board session unavailable", error);
    if (access("/usr/lib/X9H4Transport.dylib", R_OK) || access("/usr/sbin/bluetoothd", X_OK))
        return stock("bridge runtime missing", errno);
    /* Constructor must run AFTER board readiness. Injecting into a daemon
     * already running during fwdl would leave its vendor hooks uninstalled. */
    if (setenv("BT_HCI_TRANSPORT", "H4:POSIX", 1) ||
        setenv("X9_BT_VENDOR", "MT6653", 1) ||
        setenv("DYLD_INSERT_LIBRARIES", "/usr/lib/X9H4Transport.dylib", 1))
        return stock("environment setup failed", errno);
    char *const args[] = {"/usr/sbin/bluetoothd", NULL};
    fprintf(stderr, "X9 Bluetooth launch: board awake; executing stock bluetoothd with H4/vendor bridge\n");
    execv(args[0], args);
    return stock("bridge exec failed", errno);
}
