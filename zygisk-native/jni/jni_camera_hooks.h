#pragma once
#include <jni.h>

namespace vcam {

void set_java_vm(JavaVM *vm);
bool install_jni_camera_hooks(JNIEnv *env);
void jni_set_video_path(const char *path);
void jni_stop_player(JNIEnv *env);

}  // namespace vcam
