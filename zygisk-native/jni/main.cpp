/*
 * VirtualCam Manager — Zygisk native module v2.1.0
 * Pure Magisk (NO LSPosed).
 *
 * Paths:
 *   1) Camera1 JNI surface swap + MediaPlayer (original VCAM algorithm)
 *   2) GL bind/draw redirect + MediaCodec (fallback for GLES previews)
 */
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <android/log.h>
#include <cstdio>

#include "zygisk.hpp"
#include "gl_hooks.h"
#include "jni_camera_hooks.h"
#include "status_io.h"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

#define LOG_TAG "VirtualCamZygisk"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

static constexpr const char *kEnabledPath   = "/data/adb/virtualcam/enabled";
static constexpr const char *kCamera1Video  = "/storage/emulated/0/DCIM/Camera1/virtual.mp4";
static constexpr const char *kDisableFlag   = "/storage/emulated/0/DCIM/Camera1/disable.jpg";
static constexpr const char *kLastPkg       = "/data/adb/virtualcam/last_hook_pkg";

static bool file_exists(const char *path) {
    struct stat st{};
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool read_enabled() {
    int fd = open(kEnabledPath, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[8]{};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n > 0 && buf[0] == '1';
}

static void write_text(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    if (text) write(fd, text, strlen(text));
    close(fd);
}

static void report_status(const char *pkg, const char *status) {
    vcam_io::write_status(status);
    write_text(kLastPkg, pkg ? pkg : "");
    write_text("/storage/emulated/0/DCIM/Camera1/.vcam_pkg", pkg ? pkg : "");
}

static void write_module_version() {
    write_text("/data/adb/virtualcam/module_version", "2.1.0");
    write_text("/data/adb/virtualcam/version", "2.1.0");
}

static bool skip_package(const char *pkg) {
    if (!pkg || !pkg[0]) return true;
    static const char *kSkip[] = {
        "com.virtualcam.manager",
        "com.topjohnwu.magisk",
        "io.github.vvb2060.magisk",
        "com.android.systemui",
        "com.android.phone",
        "com.android.settings",
        "com.google.android.gms",
        "com.google.android.gsf",
        "webview",
        nullptr
    };
    for (int i = 0; kSkip[i]; i++) {
        if (strstr(pkg, kSkip[i])) return true;
    }
    if (strncmp(pkg, "com.android.", 12) == 0) return true;
    return false;
}

static bool copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) return false;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    if (out < 0) {
        close(in);
        return false;
    }
    char buf[8192];
    ssize_t n;
    bool ok = true;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)n) != n) { ok = false; break; }
    }
    close(in);
    close(out);
    return ok;
}

static void stage_shadowhook(const char *pkg) {
    static const char *kSrc[] = {
        "/data/adb/modules/virtualcam_manager/libshadowhook.so",
        "/data/adb/modules/virtualcam_manager/libs/arm64-v8a/libshadowhook.so",
        "/data/adb/modules/virtualcam_manager/libs/armeabi-v7a/libshadowhook.so",
        "/data/adb/modules/virtualcam_manager/libs/x86_64/libshadowhook.so",
        nullptr
    };
    const char *src = nullptr;
    for (int i = 0; kSrc[i]; i++) {
        if (file_exists(kSrc[i])) { src = kSrc[i]; break; }
    }
    if (!src) return;

    mkdir("/data/local/tmp/virtualcam", 0755);
    copy_file(src, "/data/local/tmp/virtualcam/libshadowhook.so");
    chmod("/data/local/tmp/virtualcam/libshadowhook.so", 0755);

    if (pkg && pkg[0]) {
        char dir[320], dst[360];
        snprintf(dir, sizeof(dir), "/data/data/%s/code_cache", pkg);
        mkdir(dir, 0755);
        snprintf(dst, sizeof(dst), "%s/libshadowhook.so", dir);
        copy_file(src, dst);
        chmod(dst, 0755);
        snprintf(dir, sizeof(dir), "/data/user/0/%s/code_cache", pkg);
        mkdir(dir, 0755);
        snprintf(dst, sizeof(dst), "%s/libshadowhook.so", dir);
        copy_file(src, dst);
        chmod(dst, 0755);
    }
}

class VirtualCamModule : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
        if (env) {
            JavaVM *vm = nullptr;
            env->GetJavaVM(&vm);
            vcam::set_java_vm(vm);
        }
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        write_module_version();

        const char *pkg = nullptr;
        if (args && args->nice_name) {
            pkg = env->GetStringUTFChars(args->nice_name, nullptr);
        }

        bool enabled = read_enabled();
        bool disabled = file_exists(kDisableFlag);
        bool has_video = file_exists(kCamera1Video) ||
                         file_exists("/data/adb/virtualcam/virtual.mp4");
        bool skipped = skip_package(pkg);
        bool should = enabled && !disabled && has_video && !skipped;

        LOGI("preAppSpecialize pkg=%s enabled=%d disable=%d video=%d skip=%d hook=%d",
             pkg ? pkg : "?", enabled ? 1 : 0, disabled ? 1 : 0,
             has_video ? 1 : 0, skipped ? 1 : 0, should ? 1 : 0);

        if (pkg) {
            strncpy(process, pkg, sizeof(process) - 1);
            env->ReleaseStringUTFChars(args->nice_name, pkg);
        }

        if (!should) {
            if (skipped) { /* stay quiet */ }
            else if (!enabled || disabled) report_status(process, "gate_off");
            else report_status(process, "no_video");
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        stage_shadowhook(process);
        should_hook = true;
        vcam::set_enabled(true);
        vcam::set_video_path(kCamera1Video);
        vcam::jni_set_video_path(kCamera1Video);
    }

    void postAppSpecialize(const AppSpecializeArgs *args) override {
        (void)args;
        if (!should_hook) return;

        report_status(process, "hooks_installing");
        bool jni_ok = vcam::install_jni_camera_hooks(env);
        bool gl_ok = vcam::install_gl_hooks();
        vcam::start_decoder_if_needed();

        if (jni_ok && gl_ok) report_status(process, "hooks_ready");
        else if (jni_ok) report_status(process, "jni_ready_gl_partial");
        else if (gl_ok) report_status(process, "gl_ready");
        else report_status(process, "hooks_partial");
    }

    void preServerSpecialize(ServerSpecializeArgs *args) override {
        (void)args;
        api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }

private:
    Api *api = nullptr;
    JNIEnv *env = nullptr;
    bool should_hook = false;
    char process[256]{};
};

REGISTER_ZYGISK_MODULE(VirtualCamModule)
