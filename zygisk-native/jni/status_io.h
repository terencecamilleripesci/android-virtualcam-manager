#pragma once
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>

// App processes cannot write /data/adb after specialize.
// Dual-write status so the Manager APK can read via root AND via Camera1.
namespace vcam_io {

constexpr const char *kAdbDir   = "/data/adb/virtualcam";
constexpr const char *kCam1Dir  = "/storage/emulated/0/DCIM/Camera1";

inline void write_to(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    if (text) write(fd, text, strlen(text));
    close(fd);
}

inline void write_status(const char *status) {
    write_to("/data/adb/virtualcam/hook_status", status);
    write_to("/storage/emulated/0/DCIM/Camera1/.vcam_status", status);
}

inline void write_named(const char *name, const char *text) {
    char a[160], b[160];
    snprintf(a, sizeof(a), "%s/%s", kAdbDir, name);
    snprintf(b, sizeof(b), "%s/.vcam_%s", kCam1Dir, name);
    write_to(a, text);
    write_to(b, text);
}

inline void write_num(const char *name, long long v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", v);
    write_named(name, buf);
}

inline bool file_ok(const char *path) {
    struct stat st{};
    return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

}  // namespace vcam_io
