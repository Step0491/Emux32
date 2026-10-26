#pragma once

#include <cstdint>
#include <unordered_map>
#include <string>
#include <vector>
#include <memory>
#include <jni.h>
#include "unicorn/unicorn.h"

namespace emux {

using jint32_t = int32_t;
using jlong32_t = int64_t;
using jptr32_t = uint32_t;

// Tabella di mapping per Handle 32-bit <-> Puntatori Host 64-bit
class JniHandleTable {
public:
    static JniHandleTable& getInstance();

    jptr32_t registerObject(jobject obj);
    jobject getObject(jptr32_t handle);
    void unregisterObject(jptr32_t handle);

    jptr32_t registerMethod(jmethodID method);
    jmethodID getMethod(jptr32_t handle);

private:
    JniHandleTable();
    jptr32_t nextHandle_{0x1000};
    std::unordered_map<jptr32_t, jobject> objectMap_;
    std::unordered_map<jptr32_t, jmethodID> methodMap_;
};

// Simulazione dell'interfaccia JavaVM a 32-bit
struct JNIInvokeInterface32 {
    jptr32_t reserved0;
    jptr32_t reserved1;
    jptr32_t reserved2;
    jptr32_t DestroyJavaVM;
    jptr32_t AttachCurrentThread;
    jptr32_t DetachCurrentThread;
    jptr32_t GetEnv;
    jptr32_t AttachCurrentThreadAsDaemon;
};

struct JavaVM32 {
    jptr32_t functions;
};

// Simulazione della vtable JNINativeInterface conforme all'ABI a 32-bit
struct JNINativeInterface32 {
    jptr32_t reserved0;
    jptr32_t reserved1;
    jptr32_t reserved2;
    jptr32_t reserved3;

    jptr32_t GetVersion;
    jptr32_t DefineClass;
    jptr32_t FindClass;
    jptr32_t FromReflectedMethod;
    jptr32_t FromReflectedField;
    jptr32_t ToReflectedMethod;
    jptr32_t GetSuperclass;
    jptr32_t IsAssignableFrom;
    jptr32_t ToReflectedField;

    jptr32_t Throw;
    jptr32_t ThrowNew;
    jptr32_t ExceptionOccurred;
    jptr32_t ExceptionDescribe;
    jptr32_t ExceptionClear;
    jptr32_t FatalError;

    jptr32_t PushLocalFrame;
    jptr32_t PopLocalFrame;

    jptr32_t NewGlobalRef;
    jptr32_t DeleteGlobalRef;
    jptr32_t DeleteLocalRef;
    jptr32_t IsSameObject;
    jptr32_t NewLocalRef;
    jptr32_t EnsureLocalCapacity;

    jptr32_t AllocObject;
    jptr32_t NewObject;
    jptr32_t NewObjectA;
    jptr32_t NewObjectV;

    jptr32_t GetObjectClass;
    jptr32_t IsInstanceOf;
    jptr32_t GetMethodID;

    jptr32_t CallObjectMethod;
    jptr32_t CallObjectMethodA;
    jptr32_t CallObjectMethodV;
    jptr32_t CallBooleanMethod;
    jptr32_t CallByteMethod;
    jptr32_t CallCharMethod;
    jptr32_t CallShortMethod;
    jptr32_t CallIntMethod;
    jptr32_t CallLongMethod;
    jptr32_t CallFloatMethod;
    jptr32_t CallDoubleMethod;
    jptr32_t CallVoidMethod;

    jptr32_t GetFieldID;
    jptr32_t GetObjectField;
    jptr32_t SetObjectField;

    jptr32_t GetStaticMethodID;
    jptr32_t CallStaticVoidMethod;

    jptr32_t NewStringUTF;
    jptr32_t GetStringUTFLength;
    jptr32_t GetStringUTFChars;
    jptr32_t ReleaseStringUTFChars;

    jptr32_t RegisterNatives;
    jptr32_t UnregisterNatives;
};

struct JNIEnv32 {
    jptr32_t functions;
};

// Dispatcher Syscall Linux ARM32 -> Host Android 64-bit
class SyscallDispatcher {
public:
    static int32_t handleSyscall(
        uint32_t syscallNum,
        uint32_t r0,
        uint32_t r1,
        uint32_t r2,
        uint32_t r3,
        uint32_t r4,
        uint32_t r5,
        uc_engine* uc
    );
};

// Intercettore simboli e Forwarder verso librerie native host (OpenGL ES pass-through)
class SymbolResolver {
public:
    static void* resolveHostSymbol(const char* symbolName);
};

// Gestore Runtime JNI e Trampoline Bridge per Unicorn Engine
class GuestRuntimeEnvironment {
public:
    static GuestRuntimeEnvironment& getInstance();

    bool initialize(uc_engine* uc);
    uint32_t getJavaVMPointer() const { return guestJavaVMAddr_; }
    uint32_t getJNIEnvPointer() const { return guestJNIEnvAddr_; }

    uint32_t registerSymbolTrampoline(const std::string& symbolName, uc_engine* uc);
    void handleTrampolineCall(uint32_t trampolineId, uc_engine* uc);

private:
    GuestRuntimeEnvironment();

    uint32_t guestJniBase_{0x7D000000};
    uint32_t guestJavaVMAddr_{0x7D000000};
    uint32_t guestJNIEnvAddr_{0x7D000200};

    uint32_t guestTrampolineBase_{0x70000000};
    uint32_t nextTrampolineId_{1};

    std::unordered_map<std::string, uint32_t> symbolToTrampolineMap_;
    std::unordered_map<uint32_t, std::string> trampolineToSymbolMap_;
};

} // namespace emux
