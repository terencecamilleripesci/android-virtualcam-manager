/*
 * Camera1 surface replacement — original VCAM algorithm, no LSPosed.
 *
 * Hook jniRegisterNativeMethods / ART RegisterNatives and swap:
 *   setPreviewTexture / setPreviewDisplay / startPreview / stopPreview
 */
#include "jni_camera_hooks.h"
#include "status_io.h"

#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <mutex>
#include <string>

#define LOG_TAG "VirtualCamJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace vcam {
namespace {

JavaVM *g_vm = nullptr;
std::mutex g_mu;
std::string g_video;
jobject g_saved_st = nullptr;
jobject g_saved_surface = nullptr;
jobject g_dummy_st = nullptr;
jobject g_player = nullptr;
bool g_hooks_on = false;

using sh_hook_fn = void *(*)(const char *, const char *, void *, void **);
using sh_init_fn = int (*)(int, bool);
using jni_reg_fn = int (*)(JNIEnv *, const char *, const JNINativeMethod *, int);
using art_reg_fn = jint (*)(JNIEnv *, jclass, const JNINativeMethod *, jint);
using set_tex_fn = void (*)(JNIEnv *, jobject, jobject);
using set_disp_fn = void (*)(JNIEnv *, jobject, jobject);
using start_fn = void (*)(JNIEnv *, jobject);
using stop_fn = void (*)(JNIEnv *, jobject);

jni_reg_fn orig_jni_reg = nullptr;
art_reg_fn orig_art_reg = nullptr;
set_tex_fn orig_set_tex = nullptr;
set_disp_fn orig_set_disp = nullptr;
start_fn orig_start = nullptr;
stop_fn orig_stop = nullptr;

void clear_ex(JNIEnv *env) {
    if (env->ExceptionCheck()) env->ExceptionClear();
}

JNIEnv *env_now() {
    if (!g_vm) return nullptr;
    JNIEnv *env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    if (g_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) return env;
    return nullptr;
}

const char *video_path() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_video.empty() && vcam_io::file_ok(g_video.c_str())) return g_video.c_str();
    if (vcam_io::file_ok("/storage/emulated/0/DCIM/Camera1/virtual.mp4"))
        return "/storage/emulated/0/DCIM/Camera1/virtual.mp4";
    if (vcam_io::file_ok("/data/adb/virtualcam/virtual.mp4"))
        return "/data/adb/virtualcam/virtual.mp4";
    return nullptr;
}

void drop_global(JNIEnv *env, jobject *ref) {
    if (ref && *ref) { env->DeleteGlobalRef(*ref); *ref = nullptr; }
}

jobject new_dummy_st(JNIEnv *env) {
    jclass cls = env->FindClass("android/graphics/SurfaceTexture");
    if (!cls) { clear_ex(env); return nullptr; }
    jmethodID ctorBool = env->GetMethodID(cls, "<init>", "(Z)V");
    clear_ex(env);
    jobject obj = nullptr;
    if (ctorBool) { obj = env->NewObject(cls, ctorBool, JNI_FALSE); clear_ex(env); }
    if (!obj) {
        jmethodID ctorInt = env->GetMethodID(cls, "<init>", "(I)V");
        clear_ex(env);
        if (ctorInt) { obj = env->NewObject(cls, ctorInt, 10); clear_ex(env); }
    }
    env->DeleteLocalRef(cls);
    return obj;
}

jobject surface_from_st(JNIEnv *env, jobject st) {
    if (!st) return nullptr;
    jclass cls = env->FindClass("android/view/Surface");
    if (!cls) { clear_ex(env); return nullptr; }
    jmethodID ctor = env->GetMethodID(cls, "<init>", "(Landroid/graphics/SurfaceTexture;)V");
    jobject surf = ctor ? env->NewObject(cls, ctor, st) : nullptr;
    clear_ex(env);
    env->DeleteLocalRef(cls);
    return surf;
}

void stop_player_locked(JNIEnv *env) {
    if (!g_player) return;
    jclass cls = env->GetObjectClass(g_player);
    if (cls) {
        jmethodID stop = env->GetMethodID(cls, "stop", "()V");
        if (stop) env->CallVoidMethod(g_player, stop);
        clear_ex(env);
        jmethodID rel = env->GetMethodID(cls, "release", "()V");
        if (rel) env->CallVoidMethod(g_player, rel);
        clear_ex(env);
        env->DeleteLocalRef(cls);
    }
    drop_global(env, &g_player);
}

bool start_player_locked(JNIEnv *env) {
    const char *path = video_path();
    if (!path) { vcam_io::write_status("surface_no_video"); return false; }
    stop_player_locked(env);
    jobject target = nullptr;
    if (g_saved_surface) target = env->NewLocalRef(g_saved_surface);
    else if (g_saved_st) target = surface_from_st(env, g_saved_st);
    if (!target) { vcam_io::write_status("surface_no_target"); return false; }

    jclass mpCls = env->FindClass("android/media/MediaPlayer");
    if (!mpCls) { clear_ex(env); return false; }
    jmethodID ctor = env->GetMethodID(mpCls, "<init>", "()V");
    jobject mp = ctor ? env->NewObject(mpCls, ctor) : nullptr;
    if (!mp) { clear_ex(env); env->DeleteLocalRef(mpCls); return false; }

    jstring jpath = env->NewStringUTF(path);
    bool ok = true;
    jmethodID mid;
    mid = env->GetMethodID(mpCls, "setDataSource", "(Ljava/lang/String;)V");
    if (!mid) { clear_ex(env); ok = false; } else { env->CallVoidMethod(mp, mid, jpath); if (env->ExceptionCheck()) { env->ExceptionClear(); ok = false; } }
    mid = env->GetMethodID(mpCls, "setSurface", "(Landroid/view/Surface;)V");
    if (!mid) { clear_ex(env); ok = false; } else { env->CallVoidMethod(mp, mid, target); if (env->ExceptionCheck()) { env->ExceptionClear(); ok = false; } }
    mid = env->GetMethodID(mpCls, "setLooping", "(Z)V");
    if (mid) env->CallVoidMethod(mp, mid, JNI_TRUE);
    clear_ex(env);
    mid = env->GetMethodID(mpCls, "setVolume", "(FF)V");
    if (mid) env->CallVoidMethod(mp, mid, 0.f, 0.f);
    clear_ex(env);
    mid = env->GetMethodID(mpCls, "prepare", "()V");
    if (mid) {
        env->CallVoidMethod(mp, mid);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            jmethodID pa = env->GetMethodID(mpCls, "prepareAsync", "()V");
            if (pa) env->CallVoidMethod(mp, pa);
            clear_ex(env);
        }
    }
    mid = env->GetMethodID(mpCls, "start", "()V");
    if (mid) {
        env->CallVoidMethod(mp, mid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); ok = false; }
    }
    env->DeleteLocalRef(jpath);
    env->DeleteLocalRef(mpCls);
    env->DeleteLocalRef(target);
    if (!ok) {
        vcam_io::write_status("surface_player_fail");
        jclass cls = env->GetObjectClass(mp);
        if (cls) {
            jmethodID rel = env->GetMethodID(cls, "release", "()V");
            if (rel) env->CallVoidMethod(mp, rel);
            clear_ex(env);
            env->DeleteLocalRef(cls);
        }
        env->DeleteLocalRef(mp);
        return false;
    }
    g_player = env->NewGlobalRef(mp);
    env->DeleteLocalRef(mp);
    vcam_io::write_status("surface_playing");
    LOGI("MediaPlayer started path=%s", path);
    return true;
}

void hooked_setPreviewTexture(JNIEnv *env, jobject thiz, jobject st) {
    LOGI("setPreviewTexture st=%p", st);
    std::lock_guard<std::mutex> lock(g_mu);
    drop_global(env, &g_saved_st);
    drop_global(env, &g_saved_surface);
    drop_global(env, &g_dummy_st);
    if (st) g_saved_st = env->NewGlobalRef(st);
    jobject dummy = new_dummy_st(env);
    if (dummy) {
        g_dummy_st = env->NewGlobalRef(dummy);
        vcam_io::write_status("texture_swapped");
        if (orig_set_tex) orig_set_tex(env, thiz, dummy);
        env->DeleteLocalRef(dummy);
        return;
    }
    vcam_io::write_status("texture_dummy_fail");
    if (orig_set_tex) orig_set_tex(env, thiz, st);
}

void hooked_setPreviewDisplay(JNIEnv *env, jobject thiz, jobject surface) {
    LOGI("setPreviewDisplay surface=%p", surface);
    std::lock_guard<std::mutex> lock(g_mu);
    drop_global(env, &g_saved_st);
    drop_global(env, &g_saved_surface);
    drop_global(env, &g_dummy_st);
    if (surface) g_saved_surface = env->NewGlobalRef(surface);
    jobject dummySt = new_dummy_st(env);
    jobject dummySurf = dummySt ? surface_from_st(env, dummySt) : nullptr;
    if (dummySt) g_dummy_st = env->NewGlobalRef(dummySt);
    if (dummySurf) {
        vcam_io::write_status("display_swapped");
        if (orig_set_disp) orig_set_disp(env, thiz, dummySurf);
        env->DeleteLocalRef(dummySurf);
        if (dummySt) env->DeleteLocalRef(dummySt);
        return;
    }
    if (dummySt) env->DeleteLocalRef(dummySt);
    vcam_io::write_status("display_dummy_fail");
    if (orig_set_disp) orig_set_disp(env, thiz, surface);
}

void hooked_startPreview(JNIEnv *env, jobject thiz) {
    LOGI("startPreview");
    if (orig_start) orig_start(env, thiz);
    std::lock_guard<std::mutex> lock(g_mu);
    start_player_locked(env);
}

void hooked_stopPreview(JNIEnv *env, jobject thiz) {
    LOGI("stopPreview");
    {
        std::lock_guard<std::mutex> lock(g_mu);
        stop_player_locked(env);
        vcam_io::write_status("surface_stopped");
    }
    if (orig_stop) orig_stop(env, thiz);
}

bool patch_camera_methods(JNINativeMethod *methods, int n) {
    bool any = false;
    for (int i = 0; i < n; i++) {
        const char *name = methods[i].name ? methods[i].name : "";
        const char *sig = methods[i].signature ? methods[i].signature : "";
        if (strcmp(name, "setPreviewTexture") == 0) {
            orig_set_tex = reinterpret_cast<set_tex_fn>(methods[i].fnPtr);
            methods[i].fnPtr = reinterpret_cast<void *>(hooked_setPreviewTexture);
            any = true;
        } else if (strcmp(name, "setPreviewDisplay") == 0 && strstr(sig, "Landroid/view/Surface;")) {
            orig_set_disp = reinterpret_cast<set_disp_fn>(methods[i].fnPtr);
            methods[i].fnPtr = reinterpret_cast<void *>(hooked_setPreviewDisplay);
            any = true;
        } else if (strcmp(name, "startPreview") == 0) {
            orig_start = reinterpret_cast<start_fn>(methods[i].fnPtr);
            methods[i].fnPtr = reinterpret_cast<void *>(hooked_startPreview);
            any = true;
        } else if (strcmp(name, "stopPreview") == 0 || strcmp(name, "_stopPreview") == 0) {
            orig_stop = reinterpret_cast<stop_fn>(methods[i].fnPtr);
            methods[i].fnPtr = reinterpret_cast<void *>(hooked_stopPreview);
            any = true;
        }
    }
    return any;
}

bool is_camera1_class(const char *name) {
    if (!name) return false;
    return strcmp(name, "android/hardware/Camera") == 0 || strcmp(name, "android.hardware.Camera") == 0;
}

char *class_name(JNIEnv *env, jclass clazz) {
    jclass clsCls = env->FindClass("java/lang/Class");
    if (!clsCls) { clear_ex(env); return nullptr; }
    jmethodID getName = env->GetMethodID(clsCls, "getName", "()Ljava/lang/String;");
    env->DeleteLocalRef(clsCls);
    if (!getName) { clear_ex(env); return nullptr; }
    auto jname = reinterpret_cast<jstring>(env->CallObjectMethod(clazz, getName));
    if (!jname) { clear_ex(env); return nullptr; }
    const char *utf = env->GetStringUTFChars(jname, nullptr);
    char *out = utf ? strdup(utf) : nullptr;
    if (utf) env->ReleaseStringUTFChars(jname, utf);
    env->DeleteLocalRef(jname);
    if (out) { for (char *p = out; *p; ++p) if (*p == '.') *p = '/'; }
    return out;
}

int hooked_jni_reg(JNIEnv *env, const char *className, const JNINativeMethod *gMethods, int num) {
    if (is_camera1_class(className) && gMethods && num > 0) {
        JNINativeMethod *copy = new JNINativeMethod[num];
        memcpy(copy, gMethods, sizeof(JNINativeMethod) * num);
        if (patch_camera_methods(copy, num)) {
            LOGI("patched Camera JNI natives via jniRegisterNativeMethods n=%d", num);
            vcam_io::write_status("jni_camera_patched");
        }
        int rc = orig_jni_reg ? orig_jni_reg(env, className, copy, num) : -1;
        delete[] copy;
        return rc;
    }
    return orig_jni_reg ? orig_jni_reg(env, className, gMethods, num) : -1;
}

jint hooked_art_reg(JNIEnv *env, jclass clazz, const JNINativeMethod *methods, jint n) {
    char *name = class_name(env, clazz);
    if (name && is_camera1_class(name) && methods && n > 0) {
        JNINativeMethod *copy = new JNINativeMethod[n];
        memcpy(copy, methods, sizeof(JNINativeMethod) * n);
        if (patch_camera_methods(copy, n)) {
            LOGI("patched Camera JNI natives via ART RegisterNatives n=%d", n);
            vcam_io::write_status("jni_camera_patched");
        }
        free(name);
        jint rc = orig_art_reg ? orig_art_reg(env, clazz, copy, n) : JNI_ERR;
        delete[] copy;
        return rc;
    }
    free(name);
    return orig_art_reg ? orig_art_reg(env, clazz, methods, n) : JNI_ERR;
}

void *load_shadow() {
    static const char *kPaths[] = {
        "libshadowhook.so",
        "/data/local/tmp/virtualcam/libshadowhook.so",
        "/data/adb/modules/virtualcam_manager/libshadowhook.so",
        "/data/adb/modules/virtualcam_manager/libs/arm64-v8a/libshadowhook.so",
        "/data/adb/modules/virtualcam_manager/libs/armeabi-v7a/libshadowhook.so",
        nullptr
    };
    for (int i = 0; kPaths[i]; i++) {
        void *h = dlopen(kPaths[i], RTLD_NOW);
        if (h) return h;
    }
    return nullptr;
}

bool hook_sym(void *sh, const char *lib, const char *sym, void *repl, void **orig) {
    auto hook = reinterpret_cast<sh_hook_fn>(dlsym(sh, "shadowhook_hook_sym_name"));
    if (!hook) return false;
    void *stub = hook(lib, sym, repl, orig);
    LOGI("hook %s!%s stub=%p orig=%p", lib, sym, stub, orig ? *orig : nullptr);
    return stub != nullptr;
}

}  // namespace

void set_java_vm(JavaVM *vm) { g_vm = vm; }

void jni_set_video_path(const char *path) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_video = path ? path : "";
}

void jni_stop_player(JNIEnv *env) {
    if (!env) env = env_now();
    if (!env) return;
    std::lock_guard<std::mutex> lock(g_mu);
    stop_player_locked(env);
}

bool install_jni_camera_hooks(JNIEnv *env) {
    if (g_hooks_on) return true;
    if (env && !g_vm) env->GetJavaVM(&g_vm);
    void *sh = load_shadow();
    if (!sh) {
        LOGW("ShadowHook unavailable — Camera JNI intercept not installed");
        vcam_io::write_status("jni_hook_pending_shadowhook");
        return false;
    }
    auto init = reinterpret_cast<sh_init_fn>(dlsym(sh, "shadowhook_init"));
    if (init) init(1, false);
    bool ok = false;
    ok |= hook_sym(sh, "libnativehelper.so", "jniRegisterNativeMethods",
                   (void *)hooked_jni_reg, (void **)&orig_jni_reg);
    ok |= hook_sym(sh, "libandroid_runtime.so", "jniRegisterNativeMethods",
                   (void *)hooked_jni_reg, (void **)&orig_jni_reg);
    ok |= hook_sym(sh, "libart.so",
                   "_ZN3art3JNI15RegisterNativesEP7_JNIEnvP7_jclassPK15JNINativeMethodi",
                   (void *)hooked_art_reg, (void **)&orig_art_reg);
    ok |= hook_sym(sh, "libart.so", "RegisterNatives",
                   (void *)hooked_art_reg, (void **)&orig_art_reg);
    if (ok) {
        g_hooks_on = true;
        vcam_io::write_status("jni_hooks_ready");
        LOGI("Camera JNI intercept ready");
    } else {
        vcam_io::write_status("jni_hook_fail");
        LOGW("Could not hook RegisterNatives / jniRegisterNativeMethods");
    }
    return ok;
}

}  // namespace vcam
