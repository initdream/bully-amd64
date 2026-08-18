#ifndef JNI_SHIM_H
#define JNI_SHIM_H
void jni_load(void);
void jni_init_input(void);
void *NVThreadGetCurrentJNIEnv(void);

void jni_gamepad_connect(int which);
void jni_gamepad_disconnect(int instance);
void jni_pump_gamepad(void);
void jni_mark_can_render(void);
void jni_update_rockstar(void);
int jni_draw_frame(void *env, float dt);
#endif
