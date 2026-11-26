// SPDX-License-Identifier: BSD-2-Clause
// Device-side acceptance probe for original 20H392 IOSurface.framework.
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

@protocol ProbeEvent
- (instancetype)initWithMachPort:(mach_port_t)port;
- (mach_port_t)eventPort;
- (uint64_t)globalTraceObjectID;
- (uint64_t)signaledValue;
- (void)setSignaledValue:(uint64_t)value;
- (BOOL)waitUntilSignaledValue:(uint64_t)value timeoutMS:(uint64_t)milliseconds;
- (void)notifyListener:(id)listener atValue:(uint64_t)value
    block:(void (^)(id<ProbeEvent>, uint64_t))callback;
@end

@protocol ProbeListener
- (instancetype)initWithDispatchQueue:(dispatch_queue_t)queue;
@end

static char queueKey;
static dispatch_time_t deadline(uint64_t ms)
{
	return dispatch_time(DISPATCH_TIME_NOW, ms * NSEC_PER_MSEC);
}

#define REQUIRE(cond, stage) do { if (!(cond)) { \
	fprintf(stderr, "FAIL %s\n", stage); return 1; \
} } while (0)

static int probe(void)
{
	// Load the original framework; its private classes are resolved below.
	REQUIRE(dlopen("/System/Library/Frameworks/IOSurface.framework/IOSurface", RTLD_NOW | RTLD_LOCAL), "load-iosurface");
	Class eventClass = NSClassFromString(@"IOSurfaceSharedEvent");
	Class listenerClass = NSClassFromString(@"IOSurfaceSharedEventListener");
	REQUIRE(eventClass && listenerClass, "native-classes");
	id<ProbeEvent> event = [eventClass new];
	// Check handle before dereferencing the original getter's mapped address.
	REQUIRE(event && MACH_PORT_VALID([event eventPort]), "create-event");
	id<ProbeEvent> imported = [(id<ProbeEvent>)[eventClass alloc] initWithMachPort:[event eventPort]];
	REQUIRE(imported && [event globalTraceObjectID] != 0 &&
	    [imported globalTraceObjectID] == [event globalTraceObjectID], "same-task-import");
	REQUIRE([event signaledValue] == 0 && [imported signaledValue] == 0, "initial-counter");
	REQUIRE(![imported waitUntilSignaledValue:1 timeoutMS:0], "unreached-zero-timeout");

	dispatch_queue_t callbacks = dispatch_queue_create("x9.shared-event.probe", DISPATCH_QUEUE_SERIAL);
	dispatch_queue_set_specific(callbacks, &queueKey, &queueKey, NULL);
	id listener = [(id<ProbeListener>)[listenerClass alloc] initWithDispatchQueue:callbacks];
	REQUIRE(listener, "listener-init");
	dispatch_semaphore_t notified = dispatch_semaphore_create(0);
	__block unsigned deliveries = 0;
	__block bool validPayload = false;
	const uint64_t target = UINT64_C(0x100000007);
	[imported notifyListener:listener atValue:target block:^(id<ProbeEvent> object, uint64_t value) {
		++deliveries;
		validPayload = object == imported && value == target &&
		    dispatch_get_specific(&queueKey) == &queueKey;
		dispatch_semaphore_signal(notified);
	}];
	[event setSignaledValue:target - 1];
	REQUIRE(dispatch_semaphore_wait(notified, deadline(50)) != 0, "no-early-callback");

	dispatch_semaphore_t waiting = dispatch_semaphore_create(0), completed = dispatch_semaphore_create(0);
	__block bool waitSucceeded = false;
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
		dispatch_semaphore_signal(waiting);
		waitSucceeded = [imported waitUntilSignaledValue:target timeoutMS:1000];
		dispatch_semaphore_signal(completed);
	});
	REQUIRE(dispatch_semaphore_wait(waiting, deadline(1000)) == 0, "wait-dispatch");
	[event setSignaledValue:target];
	REQUIRE(dispatch_semaphore_wait(completed, deadline(2000)) == 0 && waitSucceeded, "wait-result");
	REQUIRE(dispatch_semaphore_wait(notified, deadline(2000)) == 0, "notification-timeout");
	__block bool oneValid = false;
	dispatch_sync(callbacks, ^{ oneValid = validPayload && deliveries == 1; });
	REQUIRE(oneValid, "notification-payload");
	[event setSignaledValue:target - 2];
	REQUIRE([event signaledValue] == target && [imported signaledValue] == target, "monotonic-counter");
	REQUIRE(dispatch_semaphore_wait(notified, deadline(50)) != 0, "no-duplicate-callback");

	// Make and drop more than the X9 simultaneous live-object cap in one
	// connection. Allow Mach no-senders to run; do not conflate reuse with peak.
	for (unsigned i = 0; i < 160; ++i) {
		bool issued = false;
		for (unsigned retry = 0; retry < 100 && !issued; ++retry) {
			@autoreleasepool {
				id<ProbeEvent> transient = [eventClass new];
				issued = transient && MACH_PORT_VALID([transient eventPort]);
			}
			if (!issued) usleep(10000);
		}
		REQUIRE(issued, "one-connection-event-reclamation");
	}
	puts("PASS native SharedEvent: creation, same-task import, wait, notification, reuse");
	return 0;
}

int main(void)
{
	@autoreleasepool {
		@try { return probe(); }
		@catch (NSException *exception) {
			fprintf(stderr, "FAIL native exception %s\n", exception.name.UTF8String);
			return 1;
		}
	}
}
