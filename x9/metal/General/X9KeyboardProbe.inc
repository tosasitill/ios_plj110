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
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <CoreGraphics/CoreGraphics.h>

static int x9kbd_lines;
static IMP x9kbd_orig_flags, x9kbd_orig_down, x9kbd_orig_send, x9kbd_orig_setplane, x9kbd_orig_trackpad;

static void x9kbd_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void x9kbd_log(const char *fmt, ...) {
 if (x9kbd_lines >= 800) return;
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

// BOOL ivars (object_getIvar only handles object ivars).
static int x9kbd_bool(id obj, const char *name) {
 Ivar iv = class_getInstanceVariable(object_getClass(obj), name);
 return iv ? (int)*((const unsigned char *)(__bridge const void *)obj + ivar_getOffset(iv)) : -1;
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
 if (!(r & 0x20)) return r; // 103.txt: letters repeat this 4x per touch; keep the More key only
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
 if (actions & 0x20) {
  // 103.txt: More hit, flags 0x24, infos=1, yet the plane stays. Show what
  // the More branch hands to setKeyplaneName: and the gates in front of it.
  SEL sAlt = sel_registerName("alternateKeyplaneName"), sSub = sel_registerName("subtreeWithName:"),
      sChooser = sel_registerName("isShiftKeyPlaneChooser");
  id plane = x9kbd_ivar(self, "_keyplane"), board = x9kbd_ivar(self, "_keyboard");
  id alt = (plane && [plane respondsToSelector:sAlt]) ? ((id (*)(id, SEL))objc_msgSend)(plane, sAlt) : nil;
  id sub = (board && alt && [board respondsToSelector:sSub]) ? ((id (*)(id, SEL, id))objc_msgSend)(board, sSub, alt) : nil;
  int chooser = (plane && [plane respondsToSelector:sChooser]) ? ((BOOL (*)(id, SEL))objc_msgSend)(plane, sChooser) : -1;
  id bname = (board && [board respondsToSelector:sel_registerName("name")]) ?
      ((id (*)(id, SEL))objc_msgSend)(board, sel_registerName("name")) : nil;
  x9kbd_log("  more-branch trackpad=%d rebuilding=%d chooser=%d keyboard='%s' kbname='%s' alt='%s' altTree=%s",
      x9kbd_bool(self, "_isTrackpadMode"), x9kbd_bool(self, "_isRebuilding"), chooser,
      x9kbd_str(bname), x9kbd_str(x9kbd_ivar(self, "_keyboardName")), x9kbd_str(alt), sub ? "present" : "nil");
  // -[UIKBTree alternateKeyplaneName] (20H392): more-alternate-small-display when
  // +[UIKeyboardLayout _showSmallDisplayKeyplane] (= _AXSLowercaseKeyboardDisplayEnabled),
  // then more-alternate, else the plane's OWN name -> setKeyplaneName: is a no-op.
  static int done;
  if (done++ < 3) {
   SEL sStr = sel_registerName("stringForProperty:"), sDyn = sel_registerName("dynamicLayout"),
       sSubs = sel_registerName("subtrees"), sName = sel_registerName("name");
   id mas = [plane respondsToSelector:sStr] ? ((id (*)(id, SEL, id))objc_msgSend)(plane, sStr, @"more-alternate-small-display") : nil;
   id ma = [plane respondsToSelector:sStr] ? ((id (*)(id, SEL, id))objc_msgSend)(plane, sStr, @"more-alternate") : nil;
   id sas = [plane respondsToSelector:sStr] ? ((id (*)(id, SEL, id))objc_msgSend)(plane, sStr, @"shift-alternate-small-display") : nil;
   id sa = [plane respondsToSelector:sStr] ? ((id (*)(id, SEL, id))objc_msgSend)(plane, sStr, @"shift-alternate") : nil;
   int dyn = [board respondsToSelector:sDyn] ? ((BOOL (*)(id, SEL))objc_msgSend)(board, sDyn) : -1;
   int (*lower)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "_AXSLowercaseKeyboardDisplayEnabled");
   x9kbd_log("  plane props more-alt-sd='%s' more-alt='%s' shift-alt-sd='%s' shift-alt='%s' dynamicLayout=%d axLowercase=%d",
       x9kbd_str(mas), x9kbd_str(ma), x9kbd_str(sas), x9kbd_str(sa), dyn, lower ? lower() : -1);
   NSArray *subs = [board respondsToSelector:sSubs] ? ((NSArray *(*)(id, SEL))objc_msgSend)(board, sSubs) : nil;
   char line[400] = {0}; size_t o = 0;
   for (id t in subs) {
    id n = [t respondsToSelector:sName] ? ((id (*)(id, SEL))objc_msgSend)(t, sName) : nil;
    int w = snprintf(line + o, sizeof(line) - o, " '%s'", x9kbd_str(n));
    if (w < 0 || o + (size_t)w >= sizeof(line) - 1) { x9kbd_log("  keyboard subtrees:%s", line); o = 0; line[0] = 0;
     w = snprintf(line, sizeof(line), " '%s'", x9kbd_str(n)); }
    o += (size_t)(w > 0 ? w : 0);
   }
   x9kbd_log("  keyboard subtrees(%lu):%s", (unsigned long)subs.count, line);
  }
 }
 ((void (*)(id, SEL, id, unsigned long long, id))x9kbd_orig_send)(self, _cmd, key, actions, ctx);
 x9kbd_log("  after sendDown plane='%s'", x9kbd_str(x9kbd_ivar(self, "_keyplaneName")));
}

static int x9kbd_depth;
static void x9kbd_bt(char *bt, size_t n) {
 NSArray<NSNumber *> *rets = [NSThread callStackReturnAddresses];
 intptr_t slide = 0;
 for (uint32_t i = 0; i < _dyld_image_count(); i++) {
  const char *nm = _dyld_get_image_name(i);
  if (nm && strstr(nm, "/UIKitCore")) { slide = _dyld_get_image_vmaddr_slide(i); break; }
 }
 size_t o = 0; bt[0] = 0;
 for (NSUInteger i = 2; i < rets.count && i < 16 && o + 20 < n; i++)
  o += (size_t)snprintf(bt + o, n - o, " %llx", (unsigned long long)(rets[i].unsignedLongLongValue - (unsigned long long)slide));
}
static void x9kbd_setplane(id self, SEL _cmd, id name) {
 const char *before = x9kbd_str(x9kbd_ivar(self, "_keyplaneName"));
 char b[128]; snprintf(b, sizeof(b), "%s", before);
 int tp = x9kbd_bool(self, "_isTrackpadMode");
 // 104.txt: More -> setKeyplaneName('numbers-and-punctuation') sets the plane, then a nested
 // setKeyplaneName('small-letters-small-display') inside the same call reverts it. Log who.
 if (x9kbd_depth > 0) {
  char bt[400]; x9kbd_bt(bt, sizeof(bt));
  x9kbd_log("nested setKeyplaneName '%s' depth=%d current='%s' unslid-bt%s", x9kbd_str(name), x9kbd_depth, b, bt);
 }
 /* 105/106.txt: the revert is -[UIKeyboard _didChangeKeyplaneWithContext:] ->
  * [context sizeDidChange] -> setFrame: -> ... -> showKeyboardWithInputMode:reload:.
  * Log both planes' sizes (sizeForKeyplane:) and the layout bounds. */
 if (x9kbd_depth == 0) {
  SEL sSize = sel_registerName("sizeForKeyplane:"), sSub = sel_registerName("subtreeWithName:");
  id board = x9kbd_ivar(self, "_keyboard"), cur = x9kbd_ivar(self, "_keyplane");
  id to = (board && [board respondsToSelector:sSub] && name) ? ((id (*)(id, SEL, id))objc_msgSend)(board, sSub, name) : nil;
  if ([self respondsToSelector:sSize]) {
   CGSize a = cur ? ((CGSize (*)(id, SEL, id))objc_msgSend)(self, sSize, cur) : CGSizeMake(-1, -1);
   CGSize z = to ? ((CGSize (*)(id, SEL, id))objc_msgSend)(self, sSize, to) : CGSizeMake(-1, -1);
   CGRect bd = ((CGRect (*)(id, SEL))objc_msgSend)(self, sel_registerName("bounds"));
   x9kbd_log("plane sizes from '%s' %.2fx%.2f to '%s' %.2fx%.2f bounds %.2fx%.2f", b, a.width, a.height,
       x9kbd_str(name), z.width, z.height, bd.size.width, bd.size.height);
  }
 }
 x9kbd_depth++;
 ((void (*)(id, SEL, id))x9kbd_orig_setplane)(self, _cmd, name);
 x9kbd_depth--;
 x9kbd_log("setKeyplaneName '%s' trackpad=%d before='%s' after='%s'", x9kbd_str(name), tp, b,
     x9kbd_str(x9kbd_ivar(self, "_keyplaneName")));
}

// Return addresses with the UIKitCore slide, for host symbolication (dsc-20H392).
static void x9kbd_trackpad(id self, SEL _cmd, BOOL on, BOOL animated) {
 NSArray<NSNumber *> *rets = [NSThread callStackReturnAddresses];
 intptr_t slide = 0;
 for (uint32_t i = 0; i < _dyld_image_count(); i++) {
  const char *n = _dyld_get_image_name(i);
  if (n && strstr(n, "/UIKitCore")) { slide = _dyld_get_image_vmaddr_slide(i); break; }
 }
 char bt[256] = {0}; size_t o = 0;
 for (NSUInteger i = 1; i < rets.count && i < 9 && o + 20 < sizeof(bt); i++)
  o += (size_t)snprintf(bt + o, sizeof(bt) - o, " %llx", (unsigned long long)(rets[i].unsignedLongLongValue - (unsigned long long)slide));
 x9kbd_log("setTrackpadMode:%d animated:%d was=%d slide=0x%lx unslid-bt%s", on, animated,
     x9kbd_bool(self, "_isTrackpadMode"), (long)slide, bt);
 ((void (*)(id, SEL, BOOL, BOOL))x9kbd_orig_trackpad)(self, _cmd, on, animated);
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
 x9kbd_orig_setplane = x9kbd_swap(c, "setKeyplaneName:", (IMP)x9kbd_setplane);
 x9kbd_orig_trackpad = x9kbd_swap(c, "setTrackpadMode:animated:", (IMP)x9kbd_trackpad);
 x9kbd_log("installed flags=%d down=%d send=%d setplane=%d trackpad=%d", x9kbd_orig_flags != NULL,
     x9kbd_orig_down != NULL, x9kbd_orig_send != NULL, x9kbd_orig_setplane != NULL, x9kbd_orig_trackpad != NULL);
}
