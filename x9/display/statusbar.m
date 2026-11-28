/* 20H392 UIKitCore, own-code method replacement; no text patch/W+X/SpringBoard
 * aperture hook. Native SystemAperture is selected separately by MG artwork.
 */
#import <UIKit/UIKit.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <os/log.h>
#include <string.h>
#include "statusbar_geometry.h"

static void (*original_layout)(id, SEL);
static SEL statusbar_selector;
static os_log_t ui_log;
static unsigned observations;

static BOOL compatible_uikit(void)
{
    static const unsigned char expected[16] = {
        0x12,0x42,0x97,0x8a,0x2c,0x2c,0x37,0x81,
        0x8d,0x6c,0x97,0x77,0xed,0xce,0x28,0x04
    };
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
        const char *name = _dyld_get_image_name(i);
        if (!name || !strstr(name, "/UIKitCore.framework/UIKitCore")) continue;
        const struct mach_header_64 *h = (const void *)_dyld_get_image_header(i);
        if (!h || h->magic != MH_MAGIC_64) return NO;
        const unsigned char *p = (const void *)(h + 1);
        const unsigned char *end = p + h->sizeofcmds;
        for (uint32_t j = 0; j < h->ncmds; ++j) {
            if ((size_t)(end - p) < sizeof(struct load_command)) return NO;
            const struct load_command *c = (const void *)p;
            if (c->cmdsize < sizeof(*c) || c->cmdsize > (size_t)(end - p)) return NO;
            if (c->cmd == LC_UUID && c->cmdsize == sizeof(struct uuid_command))
                return memcmp(((const struct uuid_command *)c)->uuid, expected, 16) == 0;
            p += c->cmdsize;
        }
        return NO;
    }
    return NO;
}

static void shifted_layout(UIView *wrapper, SEL sel)
{
    /* The original 0x189244f3c resets inner _UIStatusBar.frame to
     * wrapper.bounds on EVERY call. Hence this cannot accumulate +20 px.
     * Moving the inner root preserves all its item sizes/fonts/constraints.
     */
    original_layout(wrapper, sel);
    if (![NSThread isMainThread]) return;
    UIView *bar = ((id (*)(id, SEL))objc_msgSend)(wrapper, statusbar_selector);
    UIScreen *screen = wrapper.window.screen;
    if (!bar || bar.superview != wrapper || screen != UIScreen.mainScreen) return;
    CGRect bounds = screen.bounds;
    double dy = x9_statusbar_offset(screen.nativeScale, bounds.size.height > bounds.size.width);
    if (dy == 0.0) return;
    CGRect frame = bar.frame;
    frame.origin.y += dy;
    bar.frame = frame;
    if (observations++ < 4)
        os_log_with_type(ui_log, OS_LOG_TYPE_DEFAULT,
            "X9 UI: entire inner status bar shifted 50 native px (%{public}.6f pt), y=%{public}.6f; wrapper clips=%d",
            dy, (double)frame.origin.y, wrapper.clipsToBounds);
}

__attribute__((constructor)) static void install_statusbar(void)
{
    ui_log = os_log_create("com.x9.display", "statusbar");
    if (!compatible_uikit()) {
        os_log_error(ui_log, "X9 UI: UIKitCore UUID mismatch; no hook");
        return;
    }
    Class cls = objc_getClass("UIStatusBar_Modern");
    SEL layout = sel_registerName("layoutSubviews");
    statusbar_selector = sel_registerName("statusBar");
    Method method = class_getInstanceMethod(cls, layout);
    Method getter = class_getInstanceMethod(cls, statusbar_selector);
    if (!method || !getter || strcmp(method_getTypeEncoding(method), "v16@0:8") ||
        strcmp(method_getTypeEncoding(getter), "@16@0:8")) {
        os_log_error(ui_log, "X9 UI: original Modern layout/getter contract mismatch; no hook");
        return;
    }
    original_layout = (void (*)(id, SEL))method_getImplementation(method);
    method_setImplementation(method, (IMP)shifted_layout);
    os_log_with_type(ui_log, OS_LOG_TYPE_DEFAULT,
        "X9 UI: 20H392 Modern root layout hook installed; 50 native px, portrait main display only");
}
