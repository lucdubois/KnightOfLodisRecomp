// kol_setup_mac.mm — the setup screen's file picker on a Mac: the system's
// own Open panel (asked 2026-11-18, in place of the screen's file browser,
// which stays for Linux, where no system dialog can be counted on).

#import <AppKit/AppKit.h>

#include <string>

namespace kol {

bool setup_mac_open_panel(const char* message, std::string* path) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        panel.message = [NSString stringWithUTF8String:message];
        panel.prompt = @"Select";
        if ([panel runModal] != NSModalResponseOK || panel.URL == nil) return false;
        const char* p = panel.URL.path.UTF8String;
        if (!p) return false;
        *path = p;
        return true;
    }
}

// The main screen's notch: the height (points) of the strip above the safe
// area, 0 on a screen without one. A full-screen window is placed below it,
// so a notched MacBook's 1512x982 plays as 1512x945, 16:10
// (kol_desktop: Options > Display > Ratio's choice for the screen).
int mac_main_screen_top_inset() {
    @autoreleasepool {
        NSScreen* screen = [[NSScreen screens] firstObject];
        if (!screen) return 0;
        if (@available(macOS 12.0, *)) return static_cast<int>(screen.safeAreaInsets.top + 0.5);
        return 0;
    }
}

}  // namespace kol
