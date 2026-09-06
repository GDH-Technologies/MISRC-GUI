/*
 * MISRC GUI - Settings Persistence
 * 16/02/25 - Remediate Win settings not saving - %appdata%
 * Handles the settings file's location and I/O, plus the folder/file pickers.
 * What the file contains is the descriptor table in gui_settings_table.c.
 */

#include "gui_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#else
#include <direct.h>
#define COBJMACROS
#define INITGUID
#define Rectangle Win32_Rectangle
#define CloseWindow Win32_CloseWindow
#define ShowCursor Win32_ShowCursor
#include <shlobj.h>
#include <shobjidl.h>
#undef ShowCursor
#undef CloseWindow
#undef Rectangle
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

bool gui_settings_path_is_dir(const char *path) {
    struct stat st;
    if (!path || !path[0]) return false;
    if (stat(path, &st) != 0) return false;
#if defined(_WIN32) || defined(_WIN64)
    return (st.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode);
#endif
}

static bool gui_settings_make_dir_if_needed(const char *path) {
    if (!path || !path[0]) return false;
    if (gui_settings_path_is_dir(path)) return true;
#if defined(_WIN32) || defined(_WIN64)
    if (_mkdir(path) == 0) return true;
#else
    if (mkdir(path, 0700) == 0) return true;
#endif
    if (errno == EEXIST) return gui_settings_path_is_dir(path);
    return false;
}

// Best-effort recursive mkdir for a file path's parent directories.
static bool gui_settings_ensure_parent_dirs(const char *file_path) {
    if (!file_path || !file_path[0]) return false;

    char path_copy[512];
    strncpy(path_copy, file_path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    size_t len = strlen(path_copy);
    if (len == 0) return false;

    for (size_t i = 0; i < len; i++) {
        char c = path_copy[i];
        if (c != '/' && c != '\\') continue;
        if (i == 0) continue;
#if defined(_WIN32) || defined(_WIN64)
        // Skip "C:\" root separator.
        if (i == 2 && path_copy[1] == ':') continue;
#endif
        path_copy[i] = '\0';
        if (!gui_settings_make_dir_if_needed(path_copy)) return false;
        path_copy[i] = c;
    }

    return true;
}

// Optional override path for the settings file, set via --config <path> on
// the GUI command line. When non-NULL, gui_settings_load/save use this path
// instead of the platform-default location. This makes automated GUI testing
// easy: launch the GUI with a pre-written config (e.g. Server mode + port)
// without touching the user's real settings.
static const char *s_override_settings_path = NULL;

void gui_settings_set_override_path(const char *path) {
    s_override_settings_path = (path && path[0]) ? path : NULL;
}

bool gui_settings_override_active(void) {
    return s_override_settings_path != NULL;
}

// Settings file location
static const char* get_settings_file_path(void) {
    static char settings_path[512];
    static bool initialized = false;

    if (s_override_settings_path && s_override_settings_path[0]) {
        // --config <path> override: use it directly (no platform logic).
        snprintf(settings_path, sizeof(settings_path), "%s", s_override_settings_path);
        return settings_path;
    }
    if (!initialized) {
#if defined(__ANDROID__)
        // Android 11+ scoped storage blocks native fopen() on /sdcard/... .
        // MainActivity.onCreate calls nativeSetStoragePath(getExternalFilesDir)
        // before main() runs, so android_get_storage_path() returns the real,
        // scoped-storage-exempt, writable app-external files dir. Fall back to
        // the static path only if the JNI handoff hasn't happened yet.
        extern const char *android_get_storage_path(void);
        const char *ext = android_get_storage_path();
        if (ext && ext[0]) {
            snprintf(settings_path, sizeof(settings_path),
                    "%s/misrc_gui_settings.json", ext);
        } else {
            strcpy(settings_path, "/sdcard/Android/data/dev.misrc.gui/files/misrc_gui_settings.json");
        }
#elif defined(__APPLE__)
        // Use ~/Library/Preferences on macOS
        const char* home = getenv("HOME");
        if (home) {
            snprintf(settings_path, sizeof(settings_path),
                    "%s/Library/Preferences/com.misrc.gui.json", home);
        } else {
            strcpy(settings_path, "./misrc_gui_settings.json");
        }
#elif defined(_WIN32) || defined(_WIN64)
        // Use %APPDATA% on Windows, then LOCALAPPDATA/USERPROFILE fallbacks.
        const char* appdata = getenv("APPDATA");
        if (!appdata || !appdata[0]) {
            appdata = getenv("LOCALAPPDATA");
        }
        if (appdata && appdata[0]) {
            snprintf(settings_path, sizeof(settings_path),
                    "%s\\MISRC\\misrc_gui_settings.json", appdata);
        } else {
            const char* userprofile = getenv("USERPROFILE");
            if (userprofile && userprofile[0]) {
                snprintf(settings_path, sizeof(settings_path),
                        "%s\\AppData\\Roaming\\MISRC\\misrc_gui_settings.json", userprofile);
            } else {
                strcpy(settings_path, "./misrc_gui_settings.json");
            }
        }
#else
        // Use XDG config directory on Linux/BSD.
        const char* xdg_config_home = getenv("XDG_CONFIG_HOME");
        if (xdg_config_home && xdg_config_home[0]) {
            snprintf(settings_path, sizeof(settings_path),
                    "%s/misrc_gui_settings.json", xdg_config_home);
        } else {
            const char* home = getenv("HOME");
            if (home && home[0]) {
                snprintf(settings_path, sizeof(settings_path),
                        "%s/.config/misrc_gui_settings.json", home);
            } else {
                strcpy(settings_path, "./misrc_gui_settings.json");
            }
        }
#endif
        initialized = true;
    }

    return settings_path;
}

const char* gui_settings_get_desktop_path(void) {
    static char desktop_path[512];
    static bool initialized = false;

    if (!initialized) {
#if defined(__ANDROID__)
        // Android 11+: use the scoped-storage-exempt external files dir handed
        // in from Java (getExternalFilesDir), so native fopen() can write
        // capture logs/FLAC here and the user can adb pull them. Fall back to
        // the static path only if the JNI handoff hasn't happened yet.
        extern const char *android_get_storage_path(void);
        const char *ext = android_get_storage_path();
        if (ext && ext[0]) {
            snprintf(desktop_path, sizeof(desktop_path), "%s", ext);
        } else {
            strcpy(desktop_path, "/sdcard/Android/data/dev.misrc.gui/files");
        }
#elif defined(__APPLE__)
        const char* home = getenv("HOME");
        if (home) {
            snprintf(desktop_path, sizeof(desktop_path), "%s/Desktop", home);
        } else {
            strcpy(desktop_path, ".");
        }
#elif defined(_WIN32) || defined(_WIN64)
        const char* userprofile = getenv("USERPROFILE");
        if (userprofile) {
            snprintf(desktop_path, sizeof(desktop_path), "%s\\Desktop", userprofile);
        } else {
            strcpy(desktop_path, ".");
        }
#else
        const char* home = getenv("HOME");
        if (home) {
            snprintf(desktop_path, sizeof(desktop_path), "%s/Desktop", home);
        } else {
            strcpy(desktop_path, ".");
        }
#endif
        initialized = true;
    }

    return desktop_path;
}

// Simple JSON-like format for settings
// 16.02.25 - Remediate Win save
void gui_settings_save(const gui_settings_t *settings) {
    if (!settings) return;
    if (gui_settings_save_suspended()) {
        /* A caller is editing a scratch copy; it saves once when it is done. */
        gui_settings_note_save_requested();
        return;
    }

    const char* path = get_settings_file_path();
    if (!gui_settings_ensure_parent_dirs(path)) {
        return;
    }
    char *text = malloc(GUI_SETTINGS_MAX_FILE_BYTES);
    if (!text) {
        return;
    }
    size_t needed = gui_settings_format_file(settings, text, GUI_SETTINGS_MAX_FILE_BYTES);
    if (needed >= GUI_SETTINGS_MAX_FILE_BYTES) {
        /* Never write a file the loader would refuse. */
        free(text);
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        free(text);
        return;
    }
    fwrite(text, 1, needed, f);
    fclose(f);
    free(text);
    gui_settings_bump_generation();
}

// Helpers
static void trim_newlines(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

// Output-folder picker. Returns true if output_path changed.
// NOTE: On Android this is no longer called — the settings click handler in
// gui_ui.c launches the async SAF picker (android_pick_output_folder_async)
// and applies the result via the per-frame poll, so the render thread never
// blocks across the picker Activity transition. This sync path remains for
// desktop platforms.
bool gui_settings_choose_output_folder(gui_settings_t *settings) {
    if (!settings) return false;
    char picked[512] = {0};
#if defined(__ANDROID__)
    /* Not reached on Android (async path in gui_ui.c handles it). */
    (void)picked;
    return false;
#elif defined(__APPLE__)
    // Use AppleScript choose folder dialog and return POSIX path.
    const char *cmd = "osascript -e 'POSIX path of (choose folder with prompt \"Select output folder for MISRC captures\")'";
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
    if (!fgets(picked, sizeof(picked), fp)) {
        pclose(fp);
        return false;
    }
    (void)pclose(fp);
#elif defined(_WIN32) || defined(_WIN64)
    // Native Win32 folder picker.
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    BROWSEINFOA bi = {0};
    bi.lpszTitle = "Select output folder for MISRC captures";
    bi.ulFlags = BIF_RETURNONLYFSDIRS;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl) {
        SHGetPathFromIDListA(pidl, picked);
        CoTaskMemFree(pidl);
    }
    if (hr == S_OK) CoUninitialize();
#else
    // Linux/BSD: try zenity first, then kdialog.
    const char *cmd =
        "sh -c '"
        "if command -v zenity >/dev/null 2>&1; then "
        "zenity --file-selection --directory --title=\"Select output folder for MISRC captures\"; "
        "elif command -v kdialog >/dev/null 2>&1; then "
        "kdialog --getexistingdirectory \"$HOME\" \"Select output folder for MISRC captures\"; "
        "fi'";
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
    if (!fgets(picked, sizeof(picked), fp)) {
        pclose(fp);
        return false;
    }
    (void)pclose(fp);
#endif
    trim_newlines(picked);
    if (picked[0] == '\0') return false;

    size_t len = strlen(picked);
    while (len > 1 && (picked[len - 1] == '/' || picked[len - 1] == '\\')) {
        picked[--len] = '\0';
    }

    if (strncmp(settings->output_path, picked, MAX_FILENAME_LEN) == 0) {
        return false;
    }
    strncpy(settings->output_path, picked, MAX_FILENAME_LEN - 1);
    settings->output_path[MAX_FILENAME_LEN - 1] = '\0';
    return true;
}

// Cross-platform (best-effort) playback file picker.
bool gui_settings_choose_playback_file(gui_settings_t *settings, int channel) {
    if (!settings) return false;
    if (channel != 0 && channel != 1) return false;

    char picked[512] = {0};
#if defined(__ANDROID__)
    /* Not reached on Android (async path in gui_ui.c handles it). */
    (void)picked;
    return false;
#elif defined(__APPLE__)
    // choose file, return POSIX path
    const char *cmd = "osascript -e 'POSIX path of (choose file with prompt \"Select FLAC playback file\")'";
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
    if (!fgets(picked, sizeof(picked), fp)) {
        pclose(fp);
        return false;
    }
    (void)pclose(fp);
    trim_newlines(picked);
#elif defined(_WIN32) || defined(_WIN64)
    // Native Win32 file picker using IFileOpenDialog
    HRESULT hr2 = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog *pfd = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                    &IID_IFileOpenDialog, (void **)&pfd))) {
        COMDLG_FILTERSPEC filter = { L"FLAC files", L"*.flac" };
        IFileOpenDialog_SetFileTypes(pfd, 1, &filter);
        IFileOpenDialog_SetTitle(pfd, L"Select FLAC playback file");
        if (SUCCEEDED(IFileOpenDialog_Show(pfd, NULL))) {
            IShellItem *psi = NULL;
            if (SUCCEEDED(IFileOpenDialog_GetResult(pfd, &psi))) {
                PWSTR wpath = NULL;
                if (SUCCEEDED(IShellItem_GetDisplayName(psi, SIGDN_FILESYSPATH, &wpath))) {
                    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, picked, sizeof(picked), NULL, NULL);
                    CoTaskMemFree(wpath);
                }
                IShellItem_Release(psi);
            }
        }
        IFileOpenDialog_Release(pfd);
    }
    if (hr2 == S_OK) CoUninitialize();
    trim_newlines(picked);
#else
    // Linux/BSD: try zenity first, then kdialog.
    const char *cmd =
        "sh -c '"
        "if command -v zenity >/dev/null 2>&1; then "
        "zenity --file-selection --title=\"Select FLAC playback file\" --file-filter=\"*.flac\"; "
        "elif command -v kdialog >/dev/null 2>&1; then "
        "kdialog --getopenfilename \"$HOME\" \"*.flac|FLAC files (*.flac)\"; "
        "fi'";
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
    if (!fgets(picked, sizeof(picked), fp)) {
        pclose(fp);
        return false;
    }
    (void)pclose(fp);
    trim_newlines(picked);
#endif

    if (picked[0] == '\0') return false;
    char *dst = (channel == 0) ? settings->playback_file_a : settings->playback_file_b;
    if (strncmp(dst, picked, MAX_FILENAME_LEN) == 0) {
        return false;
    }
    strncpy(dst, picked, MAX_FILENAME_LEN - 1);
    dst[MAX_FILENAME_LEN - 1] = '\0';
    return true;
}

void gui_settings_load(gui_settings_t *settings) {
    if (!settings) return;

    // Start with defaults
    gui_settings_init_defaults(settings);

    const char* path = get_settings_file_path();
    FILE* f = fopen(path, "r");
    if (!f) return; // No settings file, use defaults

    // Read entire file
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > GUI_SETTINGS_MAX_FILE_BYTES) { // Sanity check
        fclose(f);
        return;
    }

    char* content = malloc(size + 1);
    if (!content) {
        fclose(f);
        return;
    }

    size_t read_size = fread(content, 1, size, f);
    content[read_size] = '\0';
    fclose(f);

    // Parse values (every key the table knows), then the post-load migrations.
    gui_settings_parse_text(settings, content);

    free(content);
}
