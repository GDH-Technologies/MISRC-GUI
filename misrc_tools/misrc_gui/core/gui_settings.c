/*
 * MISRC GUI - Settings Persistence
 * 16/02/25 - Remediate Win settings not saving - %appdata%
 * Handles the settings file's location and I/O, plus the folder/file pickers.
 * What the file contains is the descriptor table in gui_settings_table.c.
 */

#include "gui_settings.h"
#include "../../common/file_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
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
#include <knownfolders.h>
#include <shobjidl.h>
#undef ShowCursor
#undef CloseWindow
#undef Rectangle
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif
// CPU core detection for the FLAC thread default. get_num_cores() is
// defined in the CLI lib (misrc_capture_cli.a, compiled from
// misrc_capture.c which includes numcores.h) and linked into the GUI
// binary, so declare it extern here instead of including the header
// (which would cause a multiple-definition link error).
// Stock threads = min(8, available cores) so an 8+-core system gets 8,
// a smaller system gets its core count, and a detection failure falls
// back to 0 (auto).
#if !defined(__ANDROID__)
extern uint32_t get_num_cores(void);
#endif

// Reject oversized paths instead of silently opening a different filename.
static bool gui_settings_copy(char *dst, size_t cap, const char *src) {
    size_t len = strlen(src);
    if (len >= cap) return false;
    memcpy(dst, src, len + 1);
    return true;
}

static bool gui_settings_format(char *dst, size_t cap, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(dst, cap, fmt, args);
    va_end(args);
    if (len < 0 || (size_t)len >= cap) {
        dst[0] = '\0';
        return false;
    }
    return true;
}

static bool gui_settings_separator(char c) {
#if defined(_WIN32) || defined(_WIN64)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

bool gui_settings_path_is_dir(const char *path) {
    bool is_dir = false;
    return path && path[0] && file_path_info_utf8(path, NULL, &is_dir) && is_dir;
}

static bool gui_settings_make_dir_if_needed(const char *path) {
    if (!path || !path[0]) return false;
    if (gui_settings_path_is_dir(path)) return true;
    if (file_mkdir_utf8(path, 0700) == 0) return true;
    return errno == EEXIST && gui_settings_path_is_dir(path);
}

static bool gui_settings_ensure_parent_dirs(const char *file_path) {
    char path_copy[FILE_PATH_MAX];
    if (!file_path || !file_path[0] ||
        !gui_settings_copy(path_copy, sizeof(path_copy), file_path)) return false;
    size_t start = 1;
#if defined(_WIN32) || defined(_WIN64)
    // A UNC server/share root already exists; do not try mkdir on its server.
    if (gui_settings_separator(path_copy[0]) && gui_settings_separator(path_copy[1])) {
        start = 2;
        while (path_copy[start] && !gui_settings_separator(path_copy[start])) start++;
        if (path_copy[start]) start++;
        while (path_copy[start] && !gui_settings_separator(path_copy[start])) start++;
        if (path_copy[start]) start++;
    }
#endif
    for (size_t i = start; path_copy[i]; i++) {
        char c = path_copy[i];
        if (!gui_settings_separator(c)) continue;
#if defined(_WIN32) || defined(_WIN64)
        if (i == 2 && path_copy[1] == ':') continue;
#endif
        path_copy[i] = '\0';
        bool ok = gui_settings_make_dir_if_needed(path_copy);
        path_copy[i] = c;
        if (!ok) return false;
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

static gui_settings_persist_filter_fn s_persist_filter = NULL;

void gui_settings_set_persist_filter(gui_settings_persist_filter_fn fn) {
    s_persist_filter = fn;
}

// Settings file location
static const char* get_settings_file_path(void) {
    static char settings_path[FILE_PATH_MAX];
    static bool initialized = false;
    if (s_override_settings_path) return s_override_settings_path;
    if (initialized) return settings_path;
    char *base = NULL;
#if defined(__ANDROID__)
    extern const char *android_get_storage_path(void);
    const char *ext = android_get_storage_path();
    gui_settings_format(settings_path, sizeof(settings_path), "%s/misrc_gui_settings.json",
                        ext && ext[0] ? ext : "/sdcard/Android/data/dev.misrc.gui/files");
#elif defined(_WIN32) || defined(_WIN64)
    base = file_getenv_utf8("APPDATA");
    if (!base || !base[0]) { free(base); base = file_getenv_utf8("LOCALAPPDATA"); }
    if (base && base[0]) {
        gui_settings_format(settings_path, sizeof(settings_path), "%s/MISRC/misrc_gui_settings.json", base);
    } else {
        free(base);
        base = file_getenv_utf8("USERPROFILE");
        if (base && base[0])
            gui_settings_format(settings_path, sizeof(settings_path), "%s/AppData/Roaming/MISRC/misrc_gui_settings.json", base);
        else
            strcpy(settings_path, "./misrc_gui_settings.json");
    }
#else
    const char *suffix = "/.config/misrc_gui_settings.json";
#ifdef __APPLE__
    suffix = "/Library/Preferences/com.misrc.gui.json";
#else
    base = file_getenv_utf8("XDG_CONFIG_HOME");
    if (base && base[0]) suffix = "/misrc_gui_settings.json";
#endif
    if (!base || !base[0]) { free(base); base = file_getenv_utf8("HOME"); }
    if (base && base[0])
        gui_settings_format(settings_path, sizeof(settings_path), "%s%s", base, suffix);
    else
        strcpy(settings_path, "./misrc_gui_settings.json");
#endif
    free(base);
    initialized = true;
    return settings_path;
}

#if defined(_WIN32) || defined(_WIN64)
static bool gui_settings_from_wide(char *dst, size_t cap, const wchar_t *src) {
    int need = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1, NULL, 0, NULL, NULL);
    return need > 0 && (size_t)need <= cap &&
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1, dst, (int)cap, NULL, NULL) == need;
}
#elif !defined(__ANDROID__) && !defined(__APPLE__)
// user-dirs.dirs contains shell-style quoted paths, but no shell is executed.
static void gui_settings_xdg_desktop(char *dst, size_t cap, const char *home) {
    char config[FILE_PATH_MAX];
    char *xdg = file_getenv_utf8("XDG_CONFIG_HOME");
    bool ok = xdg && xdg[0]
        ? gui_settings_format(config, sizeof(config), "%s/user-dirs.dirs", xdg)
        : gui_settings_format(config, sizeof(config), "%s/.config/user-dirs.dirs", home);
    free(xdg);
    FILE *f = ok ? file_open_read_utf8(config) : NULL;
    if (!f) return;
    char line[FILE_PATH_MAX * 2];
    while (fgets(line, sizeof(line), f)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        const char key[] = "XDG_DESKTOP_DIR=\"";
        if (strncmp(p, key, sizeof(key) - 1)) continue;
        p += sizeof(key) - 1;
        char path[FILE_PATH_MAX];
        size_t len = 0;
        if (!strncmp(p, "$HOME", 5) && (p[5] == '/' || p[5] == '"')) {
            len = strlen(home);
            if (len >= sizeof(path)) break;
            memcpy(path, home, len);
            p += 5;
        }
        while (*p && *p != '"' && len + 1 < sizeof(path)) {
            if (*p == '\\' && (p[1] == '\\' || p[1] == '"' || p[1] == '$' || p[1] == '`')) p++;
            path[len++] = *p++;
        }
        path[len] = '\0';
        if (*p == '"' && gui_settings_path_is_dir(path)) gui_settings_copy(dst, cap, path);
        break;
    }
    fclose(f);
}
#endif

const char* gui_settings_get_desktop_path(void) {
    static char desktop_path[FILE_PATH_MAX];
    static bool initialized = false;
    if (initialized) return desktop_path;
#if defined(__ANDROID__)
    extern const char *android_get_storage_path(void);
    const char *ext = android_get_storage_path();
    gui_settings_format(desktop_path, sizeof(desktop_path), "%s",
                        ext && ext[0] ? ext : "/sdcard/Android/data/dev.misrc.gui/files");
#elif defined(_WIN32) || defined(_WIN64)
    PWSTR path = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Desktop, 0, NULL, &path))) {
        gui_settings_from_wide(desktop_path, sizeof(desktop_path), path);
        CoTaskMemFree(path);
    }
    if (!gui_settings_path_is_dir(desktop_path)) {
        char *home = file_getenv_utf8("USERPROFILE");
        gui_settings_copy(desktop_path, sizeof(desktop_path), home ? home : ".");
        free(home);
    }
#else
    char *home = file_getenv_utf8("HOME");
    if (home && home[0]) {
        gui_settings_format(desktop_path, sizeof(desktop_path), "%s/Desktop", home);
        if (!gui_settings_path_is_dir(desktop_path)) gui_settings_copy(desktop_path, sizeof(desktop_path), home);
#ifndef __APPLE__
        gui_settings_xdg_desktop(desktop_path, sizeof(desktop_path), home);
#endif
    }
    free(home);
#endif
    if (!desktop_path[0]) strcpy(desktop_path, ".");
    initialized = true;
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
    /* A persist filter edits a copy: what is in memory stays as it is. */
    gui_settings_t *filtered = NULL;
    if (s_persist_filter) {
        filtered = malloc(sizeof(*filtered));
        if (!filtered) {
            /* Never write unfiltered values the filter exists to keep out. */
            free(text);
            return;
        }
        *filtered = *settings;
        s_persist_filter(filtered);
        settings = filtered;
    }
    size_t needed = gui_settings_format_file(settings, text, GUI_SETTINGS_MAX_FILE_BYTES);
    free(filtered);
    settings = NULL;
    if (needed >= GUI_SETTINGS_MAX_FILE_BYTES) {
        /* Never write a file the loader would refuse. */
        free(text);
        return;
    }
    FILE *f = file_open_utf8(path, "w");
    if (!f) {
        free(text);
        return;
    }
    fwrite(text, 1, needed, f);
    fclose(f);
    free(text);
    gui_settings_bump_generation();
}

#if !defined(_WIN32) && !defined(_WIN64) && !defined(__ANDROID__)
// Pickers append one newline. Read their complete output so embedded/trailing
// newlines in a POSIX filename survive; reject overlong output, never a prefix.
static bool gui_settings_read_picker(FILE *fp, char *dst, size_t cap) {
    size_t len = 0;
    bool overflow = false;
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (!c) overflow = true;
        if (len < cap) dst[len++] = (char)c;
        else overflow = true;
    }
    if (ferror(fp) || overflow) return false;
    if (len && dst[len - 1] == '\n') len--;
    if (!len || len >= cap) return false;
    dst[len] = '\0';
    return true;
}
#endif

// Output-folder and playback-file picker. On Android this is no longer
// called -- the settings click handler in gui_ui.c launches the async SAF
// picker (android_pick_output_folder_async) and applies the result via the
// per-frame poll, so the render thread never blocks across the picker
// Activity transition. This sync path remains for desktop platforms.
static bool gui_settings_pick_path(char *picked, size_t cap, bool folder) {
#if defined(__ANDROID__)
    (void)picked; (void)cap; (void)folder;
    return false; // Android uses the async SAF picker.
#elif defined(_WIN32) || defined(_WIN64)
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog *pfd = NULL;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IFileOpenDialog, (void **)&pfd))) {
        if (folder) {
            DWORD options = 0;
            IFileOpenDialog_GetOptions(pfd, &options);
            IFileOpenDialog_SetOptions(pfd, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        } else {
            COMDLG_FILTERSPEC filter = { L"FLAC files", L"*.flac" };
            IFileOpenDialog_SetFileTypes(pfd, 1, &filter);
        }
        IFileOpenDialog_SetTitle(pfd, folder ? L"Select output folder for MISRC captures" : L"Select FLAC playback file");
        if (SUCCEEDED(IFileOpenDialog_Show(pfd, NULL))) {
            IShellItem *psi = NULL;
            if (SUCCEEDED(IFileOpenDialog_GetResult(pfd, &psi))) {
                PWSTR wpath = NULL;
                if (SUCCEEDED(IShellItem_GetDisplayName(psi, SIGDN_FILESYSPATH, &wpath))) {
                    ok = gui_settings_from_wide(picked, cap, wpath);
                    CoTaskMemFree(wpath);
                }
                IShellItem_Release(psi);
            }
        }
        IFileOpenDialog_Release(pfd);
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    return ok;
#else
#ifdef __APPLE__
    const char *cmd = folder
        ? "osascript -e 'POSIX path of (choose folder with prompt \"Select output folder for MISRC captures\")'"
        : "osascript -e 'POSIX path of (choose file with prompt \"Select FLAC playback file\")'";
#else
    const char *cmd = folder
        ? "sh -c 'if command -v zenity >/dev/null 2>&1; then "
          "zenity --file-selection --directory --title=\"Select output folder for MISRC captures\"; "
          "elif command -v kdialog >/dev/null 2>&1; then "
          "kdialog --getexistingdirectory \"$HOME\" \"Select output folder for MISRC captures\"; fi'"
        : "sh -c 'if command -v zenity >/dev/null 2>&1; then "
          "zenity --file-selection --title=\"Select FLAC playback file\" --file-filter=\"*.flac\"; "
          "elif command -v kdialog >/dev/null 2>&1; then "
          "kdialog --getopenfilename \"$HOME\" \"*.flac|FLAC files (*.flac)\"; fi'";
#endif
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
    bool ok = gui_settings_read_picker(fp, picked, cap);
    return pclose(fp) == 0 && ok;
#endif
}

// Returns true if output_path changed.
bool gui_settings_choose_output_folder(gui_settings_t *settings) {
    if (!settings) return false;
    char picked[FILE_PATH_MAX];
    if (!gui_settings_pick_path(picked, sizeof(picked), true)) return false;
    // Keep native root and literal POSIX backslash characters unchanged.
    if (!strcmp(settings->output_path, picked)) return false;
    return gui_settings_copy(settings->output_path, sizeof(settings->output_path), picked);
}

// Cross-platform (best-effort) playback file picker.
bool gui_settings_choose_playback_file(gui_settings_t *settings, int channel) {
    if (!settings || (channel != 0 && channel != 1)) return false;
    char picked[FILE_PATH_MAX];
    if (!gui_settings_pick_path(picked, sizeof(picked), false)) return false;
    char *dst = channel == 0 ? settings->playback_file_a : settings->playback_file_b;
    if (!strcmp(dst, picked)) return false;
    return gui_settings_copy(dst, sizeof(settings->playback_file_a), picked);
}

void gui_settings_load(gui_settings_t *settings) {
    if (!settings) return;

    // Start with defaults
    gui_settings_init_defaults(settings);

    const char* path = get_settings_file_path();
    FILE* f = file_open_utf8(path, "rb");
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

    // Editors may prefix either legacy or current settings with a UTF-8 BOM.
    if (read_size >= 3 && !memcmp(content, "\xef\xbb\xbf", 3)) {
        read_size -= 3;
        memmove(content, content + 3, read_size + 1);
    }

    // Parse values (every key the table knows), then the post-load migrations.
    gui_settings_parse_text(settings, content);

    free(content);
}
