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

}  // namespace kol
