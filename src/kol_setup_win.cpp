// The setup screen's file picker on Windows: the system's Open dialog
// (kol_setup.cpp start_pick; the Mac's is kol_setup_mac.mm). Its own file so
// <windows.h> stays out of kol_setup.cpp.

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>

#include <SDL.h>
#include <SDL_syswm.h>

#include <string>

namespace kol {

namespace {
std::wstring wide(const char* s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<std::size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}
}  // namespace

// `what`: 0 the ROM, 1 the BIOS, 2 a mod. The chosen file's path in UTF-8
// (the program runs with UTF-8 as its code page: src/kol_windows.manifest);
// false when cancelled.
bool setup_win_open_dialog(SDL_Window* window, int what, const char* title, std::string* path) {
    static const wchar_t* const kFilters[3] = {
        L"Game Boy Advance ROM (*.gba)\0*.gba\0All files\0*.*\0",
        L"GBA BIOS (*.bin)\0*.bin\0All files\0*.*\0",
        L"Mods (*.ips, *.ups, *.bps)\0*.ips;*.ups;*.bps\0All files\0*.*\0",
    };
    HWND owner = nullptr;
    if (window) {
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (SDL_GetWindowWMInfo(window, &info) && info.subsystem == SDL_SYSWM_WINDOWS) owner = info.info.win.window;
    }
    wchar_t file[4096] = L"";
    const std::wstring wtitle = wide(title);
    OPENFILENAMEW o{};
    o.lStructSize = sizeof o;
    o.hwndOwner = owner;
    o.lpstrFilter = kFilters[what < 0 || what > 2 ? 0 : what];
    o.lpstrFile = file;
    o.nMaxFile = static_cast<DWORD>(sizeof file / sizeof file[0]);
    o.lpstrTitle = wtitle.c_str();
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&o)) return false;
    const int n = WideCharToMultiByte(CP_UTF8, 0, file, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return false;
    path->assign(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, file, -1, path->data(), n, nullptr, nullptr);
    return true;
}

}  // namespace kol

#endif
