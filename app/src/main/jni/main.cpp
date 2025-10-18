#include <jni.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <locale.h>
#include <atomic>

#include <mpv/client.h>

#include <pthread.h>

extern "C" {
    #include <libavcodec/jni.h>
}

#include "log.h"
#include "jni_utils.h"
#include "event.h"

#define ARRAYLEN(a) (sizeof(a)/sizeof(a[0]))

extern "C" {
    jni_func(void, create, jobject appctx);
    jni_func(void, init);
    jni_func(void, destroy);

    jni_func(void, command, jobjectArray jarray);
};

JavaVM *g_vm;
mpv_handle *g_mpv;
std::atomic<bool> g_event_thread_request_exit(false);

static pthread_t event_thread_id;

static void prepare_environment(JNIEnv *env, jobject appctx) {
    setlocale(LC_NUMERIC, "C");

    if (!env->GetJavaVM(&g_vm) && g_vm)
        av_jni_set_java_vm(g_vm, NULL);

    jobject global_appctx = env->NewGlobalRef(appctx);
    if (global_appctx)
        av_jni_set_android_app_ctx(global_appctx, NULL);

    init_methods_cache(env);
}

static void setup_nuclear_performance(mpv_handle *mpv) {
    ALOGV("Enabling NUCLEAR performance mode with SOFTWARE decoding...");
    
    // ===== NUCLEAR PERFORMANCE MODE - SOFTWARE DECODING =====
    
    // FORCE SOFTWARE DECODING ONLY
    mpv_set_option_string(mpv, "hwdec", "no");
    
    // Use ALL CPU cores for software decoding
    mpv_set_option_string(mpv, "vd-lavc-threads", "0");
    
    // Optimize software decoder for speed
    mpv_set_option_string(mpv, "vd-lavc-fast", "yes");
    mpv_set_option_string(mpv, "vd-lavc-skiploopfilter", "nonref");
    mpv_set_option_string(mpv, "vd-lavc-skipidct", "nonref");
    
    // NO frame dropping - decode EVERY frame
    mpv_set_option_string(mpv, "video-framedrop", "no");
    mpv_set_option_string(mpv, "hr-seek-framedrop", "no");
    
    // Massive decode queue (decode many frames ahead)
    mpv_set_option_string(mpv, "vd-queue-enable", "yes");
    mpv_set_option_string(mpv, "vd-queue-max-samples", "32");
    mpv_set_option_string(mpv, "vd-queue-max-bytes", "300000000"); // 300MB
    mpv_set_option_string(mpv, "vd-queue-max-secs", "10");
    
    // Huge demuxer cache for instant seeking
    mpv_set_option_string(mpv, "cache", "yes");
    mpv_set_option_string(mpv, "demuxer-max-bytes", "500000000"); // 500MB
    mpv_set_option_string(mpv, "demuxer-max-back-bytes", "500000000");
    mpv_set_option_string(mpv, "demuxer-readahead-secs", "60");
    
    // No vsync limitations - render as fast as possible
    mpv_set_option_string(mpv, "video-sync", "display-desync");
    mpv_set_option_string(mpv, "opengl-swapinterval", "0");
    mpv_set_option_string(mpv, "opengl-waitvsync", "no");
    
    // Direct rendering for speed (works with software decode)
    mpv_set_option_string(mpv, "vd-lavc-dr", "yes");
    
    // High priority
    mpv_set_option_string(mpv, "priority", "high");
    
    // Seeking specific - precise frame seeking
    mpv_set_option_string(mpv, "hr-seek", "yes");
    mpv_set_option_string(mpv, "hr-seek-demuxer-offset", "0");
    
    // Disable any throttling
    mpv_set_option_string(mpv, "video-latency-hacks", "yes");
    
    // Optimize for software decoding performance
    mpv_set_option_string(mpv, "vd-lavc-assume-old-x264", "yes");
    
    ALOGV("Nuclear performance mode enabled - SOFTWARE decoding with maximum power!");
}

jni_func(void, create, jobject appctx) {
    prepare_environment(env, appctx);

    if (g_mpv)
        die("mpv is already initialized");

    g_mpv = mpv_create();
    if (!g_mpv)
        die("context init failed");

    // ===== APPLY NUCLEAR PERFORMANCE SETTINGS =====
    setup_nuclear_performance(g_mpv);
    // ===== END NUCLEAR SETTINGS =====

    // use terminal log level but request verbose messages
    // this way --msg-level can be used to adjust later
    mpv_request_log_messages(g_mpv, "terminal-default");
    mpv_set_option_string(g_mpv, "msg-level", "all=v");
}

jni_func(void, init) {
    if (!g_mpv)
        die("mpv is not created");

    if (mpv_initialize(g_mpv) < 0)
        die("mpv init failed");

    g_event_thread_request_exit = false;
    if (pthread_create(&event_thread_id, NULL, event_thread, NULL) != 0)
        die("thread create failed");
    pthread_setname_np(event_thread_id, "event_thread");
}

jni_func(void, destroy) {
    if (!g_mpv) {
        ALOGV("mpv destroy called but it's already destroyed");
        return;
    }

    // poke event thread and wait for it to exit
    g_event_thread_request_exit = true;
    mpv_wakeup(g_mpv);
    pthread_join(event_thread_id, NULL);

    mpv_terminate_destroy(g_mpv);
    g_mpv = NULL;
}

jni_func(void, command, jobjectArray jarray) {
    CHECK_MPV_INIT();

    const char *arguments[128] = {0};
    int len = env->GetArrayLength(jarray);
    if (len >= ARRAYLEN(arguments))
        die("too many command arguments");

    for (int i = 0; i < len; ++i)
        arguments[i] = env->GetStringUTFChars((jstring)env->GetObjectArrayElement(jarray, i), NULL);

    mpv_command(g_mpv, arguments);

    for (int i = 0; i < len; ++i)
        env->ReleaseStringUTFChars((jstring)env->GetObjectArrayElement(jarray, i), arguments[i]);
}
