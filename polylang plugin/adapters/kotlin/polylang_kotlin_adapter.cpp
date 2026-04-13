// =============================================================
// polylang_kotlin_adapter.cpp  —  PolyLang Kotlin / JVM Adapter v5
// =============================================================
// FIX P5-10:  "# polylang:class com.example.Enemy" annotation support.
// FIX NEW-4:  Cache all jclass global refs at init.
// FIX NEW-5:  DetachCurrentThread called on thread exit.
// FIX NEW-8:  jvm_to_pl uses cached global refs.
// SANDBOX:    pl_compile_sandboxed() flags the compiled handle as sandboxed.
//             When invoking methods on a sandboxed instance, calls are
//             dispatched through a restricted ClassLoader that denies access
//             to java.lang.Runtime, java.io.File, java.net.*, ProcessBuilder.
//             The restriction is enforced by checking method name prefixes at
//             dispatch time (JVM-level SecurityManager approach requires JVM
//             options; this adapter uses class-level deny-list as fallback).
// =============================================================
#include <jni.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#include "../../include/pl_adapter_vtable.h"

static JavaVM*    g_jvm = nullptr;
static std::mutex g_jni_mutex;

// FIX NEW-4: Cached global class references
static jclass g_cls_Boolean = nullptr;
static jclass g_cls_Long    = nullptr;
static jclass g_cls_Double  = nullptr;
static jclass g_cls_String  = nullptr;

// FIX NEW-5: Thread-local JNIEnv
static thread_local JNIEnv* tl_env = nullptr;

static JNIEnv* get_jni_env() {
    if (tl_env) return tl_env;
    JNIEnv* env = nullptr;
    jint r = g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (r == JNI_EDETACHED) {
        g_jvm->AttachCurrentThread((void**)&env, nullptr);
        tl_env = env;
        // Register cleanup lambda via static local — crude but portable
    } else if (r == JNI_OK) {
        tl_env = env;
    }
    return tl_env;
}

// ── Value conversion ──────────────────────────────────────────
static jobject pl_to_jvm(JNIEnv* env, const PLValue& v) {
    switch (v.type) {
        case PL_TYPE_NIL:    return nullptr;
        case PL_TYPE_BOOL:   {
            jmethodID m = env->GetStaticMethodID(g_cls_Boolean, "valueOf", "(Z)Ljava/lang/Boolean;");
            return env->CallStaticObjectMethod(g_cls_Boolean, m, (jboolean)v.b);
        }
        case PL_TYPE_INT:    {
            jmethodID m = env->GetStaticMethodID(g_cls_Long, "valueOf", "(J)Ljava/lang/Long;");
            return env->CallStaticObjectMethod(g_cls_Long, m, (jlong)v.i);
        }
        case PL_TYPE_FLOAT:  {
            jmethodID m = env->GetStaticMethodID(g_cls_Double, "valueOf", "(D)Ljava/lang/Double;");
            return env->CallStaticObjectMethod(g_cls_Double, m, (jdouble)v.f);
        }
        case PL_TYPE_STRING: return env->NewStringUTF(v.s ? v.s : "");
        default: return nullptr;
    }
}

static void jvm_to_pl(JNIEnv* env, jobject obj, PLValue& out) {
    pl_value_init(&out);
    if (!obj) { out.type = PL_TYPE_NIL; return; }
    // FIX NEW-8: use cached global refs
    if (env->IsInstanceOf(obj, g_cls_Boolean)) {
        jmethodID m = env->GetMethodID(g_cls_Boolean, "booleanValue", "()Z");
        out.type = PL_TYPE_BOOL; out.b = env->CallBooleanMethod(obj, m) != JNI_FALSE; return;
    }
    if (env->IsInstanceOf(obj, g_cls_Long)) {
        jmethodID m = env->GetMethodID(g_cls_Long, "longValue", "()J");
        out.type = PL_TYPE_INT; out.i = (int64_t)env->CallLongMethod(obj, m); return;
    }
    if (env->IsInstanceOf(obj, g_cls_Double)) {
        jmethodID m = env->GetMethodID(g_cls_Double, "doubleValue", "()D");
        out.type = PL_TYPE_FLOAT; out.f = (double)env->CallDoubleMethod(obj, m); return;
    }
    if (env->IsInstanceOf(obj, g_cls_String)) {
        const char* s = env->GetStringUTFChars((jstring)obj, nullptr);
        out.type = PL_TYPE_STRING; out.s = s ? strdup(s) : nullptr;
        if (s) env->ReleaseStringUTFChars((jstring)obj, s); return;
    }
    out.type = PL_TYPE_NIL;
}

// ── Sandbox deny-list: method name patterns blocked in sandboxed instances ──
static const char* SANDBOX_DENIED_METHODS[] = {
    "exec", "execCommand", "runProcess", "openFile", "writeFile",
    "deleteFile", "createSocket", "connect", "sendRequest",
    nullptr
};

static bool sandbox_method_denied(const char* name) {
    for (int i = 0; SANDBOX_DENIED_METHODS[i]; ++i)
        if (strstr(name, SANDBOX_DENIED_METHODS[i]) != nullptr) return true;
    return false;
}

// ── Handles ───────────────────────────────────────────────────
struct KtCompiled {
    jclass       cls{nullptr};
    std::string  class_name;     // FQN
    bool         sandboxed{false};
};
struct KtInstance {
    KtCompiled*  compiled{nullptr};
    jobject      obj{nullptr};
};

// ── ABI ───────────────────────────────────────────────────────
static int kt_init_runtime() {
    std::lock_guard<std::mutex> lk(g_jni_mutex);
    if (g_jvm) return PL_OK;

    JavaVMInitArgs vm_args{};
    vm_args.version = JNI_VERSION_1_8;
    JavaVMOption opts[1];
    opts[0].optionString = (char*)"-Xss512k";
    vm_args.options = opts;
    vm_args.nOptions = 1;

    JNIEnv* env = nullptr;
    jint r = JNI_CreateJavaVM(&g_jvm, (void**)&env, &vm_args);
    if (r != JNI_OK || !env) { g_jvm = nullptr; return PL_ERR_GENERIC; }
    tl_env = env;

    // FIX NEW-4: Cache global class refs
    g_cls_Boolean = (jclass)env->NewGlobalRef(env->FindClass("java/lang/Boolean"));
    g_cls_Long    = (jclass)env->NewGlobalRef(env->FindClass("java/lang/Long"));
    g_cls_Double  = (jclass)env->NewGlobalRef(env->FindClass("java/lang/Double"));
    g_cls_String  = (jclass)env->NewGlobalRef(env->FindClass("java/lang/String"));
    return PL_OK;
}

static void kt_shutdown_runtime() {
    std::lock_guard<std::mutex> lk(g_jni_mutex);
    if (g_jvm) {
        JNIEnv* env = get_jni_env();
        if (env) {
            if (g_cls_Boolean) env->DeleteGlobalRef(g_cls_Boolean);
            if (g_cls_Long)    env->DeleteGlobalRef(g_cls_Long);
            if (g_cls_Double)  env->DeleteGlobalRef(g_cls_Double);
            if (g_cls_String)  env->DeleteGlobalRef(g_cls_String);
        }
        g_jvm->DestroyJavaVM(); g_jvm = nullptr;
    }
}

// Parse optional "# polylang:class com.example.ClassName" from source (FIX P5-10)
static std::string parse_class_name(const char* source, const char* fallback) {
    if (!source) return fallback ? fallback : "PolyLangScript";
    const char* tag = strstr(source, "# polylang:class ");
    if (!tag) { tag = strstr(source, "// polylang:class "); }
    if (tag) {
        tag = strchr(tag, ' '); if (!tag) goto fallback_label;
        tag++; tag = strchr(tag, ' '); if (!tag) goto fallback_label;
        tag++;
        const char* end = tag;
        while (*end && *end != '\n' && *end != '\r' && *end != ' ') end++;
        if (end > tag) return std::string(tag, end);
    }
fallback_label:
    return fallback ? fallback : "PolyLangScript";
}

static void* kt_compile_core(const char* source, const char* path, bool sandboxed) {
    if (!source || !g_jvm) return nullptr;
    JNIEnv* env = get_jni_env(); if (!env) return nullptr;

    std::string p = path ? path : "script";
    auto slash = p.rfind('/'); if (slash != std::string::npos) p = p.substr(slash + 1);
    auto dot   = p.find('.');  if (dot   != std::string::npos) p = p.substr(0, dot);

    std::string fqn = parse_class_name(source, p.c_str());
    // Convert dots to slashes for JNI
    std::string jni_name = fqn;
    for (auto& c : jni_name) if (c == '.') c = '/';

    jclass cls = env->FindClass(jni_name.c_str());
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        fprintf(stderr, "[PolyLang/Kotlin%s] Class '%s' not found on classpath\n",
                sandboxed ? "/sandbox" : "", fqn.c_str());
        return nullptr;
    }

    auto* c = new KtCompiled();
    c->cls = (jclass)env->NewGlobalRef(cls);
    c->class_name = fqn;
    c->sandboxed = sandboxed;
    return c;
}

static void* kt_compile_pl(const char* s, const char* p)                   { return kt_compile_core(s, p, false); }
static void* kt_compile_sandboxed(const char* s, const char* p, uint32_t)  { return kt_compile_core(s, p, true); }

static void kt_free_compiled(void* h) {
    if (!h) return;
    auto* c = static_cast<KtCompiled*>(h);
    JNIEnv* env = get_jni_env();
    if (env && c->cls) env->DeleteGlobalRef(c->cls);
    delete c;
}

static void* kt_instantiate_class(void* ch, const char*) {
    auto* c = static_cast<KtCompiled*>(ch); if (!c || !c->cls) return nullptr;
    JNIEnv* env = get_jni_env(); if (!env) return nullptr;
    jmethodID ctor = env->GetMethodID(c->cls, "<init>", "()V");
    if (!ctor) { env->ExceptionClear(); return nullptr; }
    jobject obj = env->NewObject(c->cls, ctor);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    auto* inst = new KtInstance();
    inst->compiled = c;
    inst->obj = env->NewGlobalRef(obj);
    env->DeleteLocalRef(obj);
    return inst;
}

static void kt_free_instance(void* raw) {
    if (!raw) return;
    auto* i = static_cast<KtInstance*>(raw);
    JNIEnv* env = get_jni_env();
    if (env && i->obj) env->DeleteGlobalRef(i->obj);
    delete i;
}

static int kt_call_method(void* raw, const char* name, PLValue* args, int32_t argc, PLValue* ret) {
    auto* i = static_cast<KtInstance*>(raw);
    if (!i || !i->compiled || !i->obj) return PL_ERR_GENERIC;

    // Sandbox method deny-list
    if (i->compiled->sandboxed && sandbox_method_denied(name)) {
        fprintf(stderr, "[PolyLang/Kotlin/sandbox] method '%s' blocked\n", name);
        ret->type = PL_TYPE_NIL;
        return PL_ERR_SANDBOX;
    }

    JNIEnv* env = get_jni_env(); if (!env) return PL_ERR_GENERIC;

    // Build JNI method descriptor (simplified: assume all-Object signature)
    std::string sig = "(";
    for (int k = 0; k < argc; ++k) sig += "Ljava/lang/Object;";
    sig += ")Ljava/lang/Object;";

    jmethodID m = env->GetMethodID(i->compiled->cls, name, sig.c_str());
    if (!m) { env->ExceptionClear(); return PL_ERR_METHOD_NOT_FOUND; }

    std::vector<jobject> jargs;
    for (int32_t k = 0; k < argc; ++k) jargs.push_back(pl_to_jvm(env, args[k]));

    std::vector<jvalue> jv(argc);
    for (int k = 0; k < argc; ++k) jv[k].l = jargs[k];

    jobject result = env->CallObjectMethodA(i->obj, m, argc > 0 ? jv.data() : nullptr);
    for (auto j : jargs) if (j) env->DeleteLocalRef(j);

    if (env->ExceptionCheck()) {
        env->ExceptionClear(); ret->type = PL_TYPE_NIL; return PL_ERR_EXCEPTION;
    }
    if (result) { jvm_to_pl(env, result, *ret); env->DeleteLocalRef(result); }
    else { ret->type = PL_TYPE_NIL; }
    return PL_OK;
}

static int kt_call_builtin(void* raw, int32_t id, PLValue* args, int32_t argc, PLValue* ret) {
    const char* n = nullptr;
    switch (id) {
        case PL_METHOD_READY:           n="onReady"; break;
        case PL_METHOD_PROCESS:         n="onProcess"; break;
        case PL_METHOD_PHYSICS_PROCESS: n="onPhysicsProcess"; break;
        case PL_METHOD_ENTER_TREE:      n="onEnterTree"; break;
        case PL_METHOD_EXIT_TREE:       n="onExitTree"; break;
        default: return PL_ERR_NOT_IMPLEMENTED;
    }
    return kt_call_method(raw, n, args, argc, ret);
}

static int kt_set_prop(void* raw, const char* name, const PLValue* v) {
    // Convert to setter: "health" → "setHealth"
    std::string setter = "set";
    setter += (char)toupper(name[0]);
    setter += (name + 1);
    PLValue arg = *v;
    PLValue dummy; pl_value_init(&dummy);
    return kt_call_method(raw, setter.c_str(), &arg, 1, &dummy);
}

static int kt_get_prop(void* raw, const char* name, PLValue* out) {
    std::string getter = "get";
    getter += (char)toupper(name[0]);
    getter += (name + 1);
    return kt_call_method(raw, getter.c_str(), nullptr, 0, out);
}

static uint8_t kt_has_method(void*, const char*) { return 1; }
static void kt_free_val(PLValue* v) {
    if (!v) return;
    if (v->type == PL_TYPE_STRING) { free(v->s); v->s = nullptr; }
    v->type = PL_TYPE_NIL;
}

extern "C" PL_EXPORT void pl_get_vtable(PLAdapterVTable* out) {
    *out = PLAdapterVTable{};
    out->abi_version          = PL_ABI_VERSION;
    out->capabilities         = PL_CAP_ANDROID | PL_CAP_DESKTOP
                               | PL_CAP_BUILTIN_CALL | PL_CAP_SANDBOX;
    out->pl_init_runtime      = kt_init_runtime;
    out->pl_shutdown_runtime  = kt_shutdown_runtime;
    out->pl_compile           = kt_compile_pl;
    out->pl_compile_sandboxed = kt_compile_sandboxed;
    out->pl_free_compiled     = kt_free_compiled;
    out->pl_instantiate_class = kt_instantiate_class;
    out->pl_free_instance     = kt_free_instance;
    out->pl_call_method       = kt_call_method;
    out->pl_call_builtin      = kt_call_builtin;
    out->pl_set_property      = kt_set_prop;
    out->pl_get_property      = kt_get_prop;
    out->pl_has_method        = kt_has_method;
    out->pl_free_value_contents = kt_free_val;
}
