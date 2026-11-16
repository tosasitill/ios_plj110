/* 20H392 XPC bootstrap adapter, METHOD: UniXNU/QcaBluetooth BlueTool.c.
 * Original 1001d5804 constructs kMsgId/kMsgArgs; 10027227c..290 sends
 * RunBuiltinScript with script, and 100272574..630 reads result/firmware/
 * manufacturer. Only the kernel's complete Android session can succeed.
 * No EPERM success fallback, invented firmware/manufacturer or sleep ACK. */
#include "X9Bootstrap16.h"
#include <dispatch/dispatch.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "X9XPC16.h"

static void handle(xpc_connection_t peer, xpc_object_t message)
{
    if (xpc_get_type(message) != XPC_TYPE_DICTIONARY) return;
    xpc_object_t reply = xpc_dictionary_create_reply(message);
    if (!reply) return;
    const char *id = xpc_dictionary_get_string(message, "kMsgId");
    const char *operation = "unsupported";
    int error = ENOTSUP;
    if (id && !strcmp(id, "RunBuiltinScript")) {
        xpc_object_t args = xpc_dictionary_get_value(message, "kMsgArgs");
        const char *script = args && xpc_get_type(args) == XPC_TYPE_DICTIONARY ?
            xpc_dictionary_get_string(args, "script") : NULL;
        if (!script) error = EINVAL;
        else if (!strcmp(script, "boot") || !strcmp(script, "init")) {
            operation = script;
            error = x9_bootstrap16_resume();
        } else if (!strcmp(script, "deepsleep")) {
            operation = script;
            error = x9_bootstrap16_sleep();
        }
        /* Only the three observed native script names are supported. The
         * kernel executes and verifies ownership/clock changes; a busy
         * transport, firmware refusal or permission error is not success. */
    }
    /* Native result is a script code, not BSD errno (10/13 have special
     * meaning). Preserve exact failure separately; never collapse it to0. */
    xpc_dictionary_set_int64(reply, "result", error ? 1 : 0);
    xpc_dictionary_set_int64(reply, "x9_errno", error);
    if (id && !strcmp(id, "RunCommand"))
        xpc_dictionary_set_string(reply, "output", "Unsupported controller command");
    /* Firmware/manufacturer fields are deliberately absent: they are not
     * supplied by this board ABI. Native HCI Read Local Version owns those. */
    x9_bt_note("X9 BlueTool: request=%s script=%s errno=%d\n",
            id && (!strcmp(id, "RunBuiltinScript") || !strcmp(id, "RunCommand")) ? id : "unsupported",
            operation, error);
    xpc_connection_send_message(peer, reply);
    xpc_release(reply);
}

int main(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "-R") || geteuid() != 501) return 2;
    dispatch_queue_t queue = dispatch_queue_create("org.x9.bluetooth.bootstrap", DISPATCH_QUEUE_SERIAL);
    if (!queue) return 1;
    xpc_connection_t listener = xpc_connection_create_mach_service("com.apple.BlueTool", queue,
                                               XPC_CONNECTION_MACH_SERVICE_LISTENER);
    if (!listener) return 1;
    xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
        if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) return;
        xpc_connection_t connection = (xpc_connection_t)peer;
        xpc_connection_set_target_queue(connection, queue);
        xpc_connection_set_event_handler(connection, ^(xpc_object_t event) { handle(connection, event); });
        xpc_connection_resume(connection);
    });
    xpc_connection_resume(listener);
    dispatch_main();
}
