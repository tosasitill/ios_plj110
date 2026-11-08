// SPDX-License-Identifier: MIT
// x9kbdhook=1: keyboard "123" diagnostic, included by X9GeneralMetalFactory.mm.
//
// 102.txt: the More ("123") key never highlights and never switches the
// keyplane in any language, while letters, delete, emoji and globe work.
// 20H392 UIKitCore (dsc disassembly): -[UIKeyboardLayoutStar
// downActionFlagsForKey:] gives the More key (interactionType 11) flags 0x24
// unless [key visible] is NO or [self stateForKey:key] == 1, and
// -completeSendStringActionForTouchDownWithKey:withActions:executionContext:
// switches only when bit 5 is set and [_extendedTouchInfoMap count] == 1.
// This bundle is loaded by every Metal client (Spotlight at 59.6 s in
// 102.txt), so a constructor here can log, per keyboard touch-down, which key
// the hit test chose, the More key's own visible/state/frame, the action flags
// and the touch-info count. Read-only: every hook calls the original first and
// returns its result unchanged. Off unless the boot-arg token is present.
#include <objc/message.h>
#include <stdarg.h>
#include <CoreGraphics/CoreGraphics.h>

static int x9kbd_lines;
static IMP x9kbd_orig_flags, x9kbd_orig_down, x9kbd_orig_send;

static void x9kbd_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void x9kbd_log(const char *fmt, ...) {
 if (x9kbd_lines >= 400) return;
 x9kbd_lines++;
 char line[512];
 va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
 int fd = open("/dev/console", O_WRONLY | O_NOCTTY);
 if (fd >= 0) { dprintf(fd, "X9 KBDHOOK %s[%d]: %s\n", getprogname(), getpid(), line); close(fd); }
}

static id x9kbd_ivar(id obj, const char *name) {
 Ivar iv = class_getInstanceVariable(object_getClass(obj), name);
 return iv ? object_getIvar(obj, iv) : nil;
}

static const char *x9kbd_str(id s) {
 return [s isKindOfClass:[NSString class]] ? [(NSString *)s UTF8String] : "nil";
}

// One key: name, interactionType, displayType, visible, layout state, frame.
static void x9kbd_key(char *out, size_t n, id layout, id key) {
 if (!key) { snprintf(out, n, "nil"); return; }
 SEL sName = sel_registerName("name"), sIT = sel_registerName("interactionType"),
     sDT = sel_registerName("displayType"), sVis = sel_registerName("visible"),
     sFrame = sel_registerName("frame"), sState = sel_registerName("stateForKey:");
 id name = [key respondsToSelector:sName] ? ((id (*)(id, SEL))objc_msgSend)(key, sName) : nil;
 int it = [key respondsToSelector:sIT] ? ((int (*)(id, SEL))objc_msgSend)(key, sIT) : -1;
 int dt = [key respondsToSelector:sDT] ? ((int (*)(id, SEL))objc_msgSend)(key, sDT) : -1;
 int vis = [key respondsToSelector:sVis] ? ((BOOL (*)(id, SEL))objc_msgSend)(key, sVis) : -1;
 int st = [layout respondsToSelector:sState] ? ((int (*)(id, SEL, id))objc_msgSend)(layout, sState, key) : -1;
 CGRect f = [key respondsToSelector:sFrame] ? ((CGRect (*)(id, SEL))objc_msgSend)(key, sFrame) : CGRectMake(0, 0, 0, 0);
 snprintf(out, n, "'%s' it=%d dt=%d visible=%d state=%d frame=%.1f,%.1f %.1fx%.1f",
     x9kbd_str(name), it, dt, vis, st, f.origin.x, f.origin.y, f.size.width, f.size.height);
}

static unsigned long long x9kbd_flags(id self, SEL _cmd, id key) {
 unsigned long long r = ((unsigned long long (*)(id, SEL, id))x9kbd_orig_flags)(self, _cmd, key);
 char k[256]; x9kbd_key(k, sizeof(k), self, key);
 x9kbd_log("downActionFlags=0x%llx key %s", r, k);
 return r;
}

static void x9kbd_down(id self, SEL _cmd, id touch, id ctx) {
 SEL sLoc = sel_registerName("locationInView:"), sHit = sel_registerName("keyHitTestWithoutCharging:"),
     sFirst = sel_registerName("firstCachedKeyWithName:");
 CGPoint p = CGPointMake(-1, -1);
 if ([touch respondsToSelector:sLoc]) p = ((CGPoint (*)(id, SEL, id))objc_msgSend)(touch, sLoc, self);
 id hit = [self respondsToSelector:sHit] ? ((id (*)(id, SEL, CGPoint))objc_msgSend)(self, sHit, p) : nil;
 id plane = x9kbd_ivar(self, "_keyplane");
 id more = (plane && [plane respondsToSelector:sFirst]) ?
     ((id (*)(id, SEL, id))objc_msgSend)(plane, sFirst, @"More-Key") : nil;
 id map = x9kbd_ivar(self, "_extendedTouchInfoMap");
 char h[256], m[256];
 x9kbd_key(h, sizeof(h), self, hit);
 x9kbd_key(m, sizeof(m), self, more);
 CGRect b = [self isKindOfClass:NSClassFromString(@"UIView")] ?
     ((CGRect (*)(id, SEL))objc_msgSend)(self, sel_registerName("bounds")) : CGRectMake(0, 0, 0, 0);
 x9kbd_log("touchDown at %.1f,%.1f (layout %.1fx%.1f) plane='%s' infos=%lu hit %s",
     p.x, p.y, b.size.width, b.size.height, x9kbd_str(x9kbd_ivar(self, "_keyplaneName")),
     (unsigned long)[(NSDictionary *)map count], h);
 x9kbd_log("  More-Key %s", m);
 ((void (*)(id, SEL, id, id))x9kbd_orig_down)(self, _cmd, touch, ctx);
}

static void x9kbd_send(id self, SEL _cmd, id key, unsigned long long actions, id ctx) {
 id map = x9kbd_ivar(self, "_extendedTouchInfoMap");
 char k[256]; x9kbd_key(k, sizeof(k), self, key);
 x9kbd_log("sendDown actions=0x%llx infos=%lu key %s", actions, (unsigned long)[(NSDictionary *)map count], k);
 ((void (*)(id, SEL, id, unsigned long long, id))x9kbd_orig_send)(self, _cmd, key, actions, ctx);
 x9kbd_log("  after sendDown plane='%s'", x9kbd_str(x9kbd_ivar(self, "_keyplaneName")));
}

static IMP x9kbd_swap(Class c, const char *sel, IMP imp) {
 Method m = class_getInstanceMethod(c, sel_registerName(sel));
 if (!m) { x9kbd_log("hook missing %s", sel); return NULL; }
 return method_setImplementation(m, imp);
}

__attribute__((constructor)) static void x9kbd_install(void) {
 char args[2048] = {0}; size_t bytes = sizeof(args) - 1; bool on = false;
 if (sysctlbyname("kern.bootargs", args, &bytes, NULL, 0)) return;
 char *save = NULL;
 for (char *t = strtok_r(args, " \t\r\n", &save); t; t = strtok_r(NULL, " \t\r\n", &save))
  if (!strcmp(t, "x9kbdhook=1")) on = true;
 if (!on) return;
 Class c = objc_getClass("UIKeyboardLayoutStar");
 if (!c) return; // not a UIKit process
 x9kbd_orig_flags = x9kbd_swap(c, "downActionFlagsForKey:", (IMP)x9kbd_flags);
 x9kbd_orig_down = x9kbd_swap(c, "touchDown:executionContext:", (IMP)x9kbd_down);
 x9kbd_orig_send = x9kbd_swap(c, "completeSendStringActionForTouchDownWithKey:withActions:executionContext:", (IMP)x9kbd_send);
 x9kbd_log("installed flags=%d down=%d send=%d", x9kbd_orig_flags != NULL, x9kbd_orig_down != NULL, x9kbd_orig_send != NULL);
}
