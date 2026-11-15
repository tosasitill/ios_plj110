/* Minimal C libxpc ABI declarations. Xcode's iPhoneOS app SDK omits xpc.h.
 * Types/signatures come from the Xcode MacOSX14 SDK's xpc/{xpc,connection}.h;
 * every used function/type symbol is imported by the original 20H392
 * /usr/sbin/BlueTool. This header does not define any libxpc structure layout. */
#ifndef X9_XPC16_H
#define X9_XPC16_H
#include <dispatch/dispatch.h>
#include <stdint.h>
typedef void *xpc_object_t;
typedef struct _xpc_connection_s *xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
typedef void (^xpc_handler_t)(xpc_object_t);
extern const struct _xpc_type_s _xpc_type_dictionary, _xpc_type_connection;
#define XPC_TYPE_DICTIONARY (&_xpc_type_dictionary)
#define XPC_TYPE_CONNECTION (&_xpc_type_connection)
#define XPC_CONNECTION_MACH_SERVICE_LISTENER (1u << 0)
extern xpc_type_t xpc_get_type(xpc_object_t);
extern void xpc_release(xpc_object_t);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t);
extern xpc_object_t xpc_dictionary_get_value(xpc_object_t, const char *);
extern const char *xpc_dictionary_get_string(xpc_object_t, const char *);
extern void xpc_dictionary_set_int64(xpc_object_t, const char *, int64_t);
extern void xpc_dictionary_set_string(xpc_object_t, const char *, const char *);
extern xpc_connection_t xpc_connection_create_mach_service(const char *, dispatch_queue_t, uint64_t);
extern void xpc_connection_set_target_queue(xpc_connection_t, dispatch_queue_t);
extern void xpc_connection_set_event_handler(xpc_connection_t, xpc_handler_t);
extern void xpc_connection_resume(xpc_connection_t);
extern void xpc_connection_send_message(xpc_connection_t, xpc_object_t);
#endif
