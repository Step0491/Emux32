# Emux x32 Game Translator (ARM64 Frontend + Dynamic Binary Translator)

Frontend Android nativo (ARM64-v8a) e Dynamic Binary Translation (DBT) layer per l'esecuzione diretta di librerie e giochi a 32-bit (ARMv7 / `armeabi-v7a`) su moderni SoC privi di supporto hardware AArch32.

---

## Architettura del Sistema

```
                  ┌──────────────────────────────────────────────┐
                  │       FRONTEND (Kotlin + Compose)            │
                  │  - XMB orthogonal scroll (PSP Style)         │
                  │  - NetherSX2 Grid View (DVD Aspect Ratio)    │
                  │  - SAF Game & Cover Parser                   │
                  └──────────────────────┬───────────────────────┘
                                         │ JNI Bridge
                  ┌──────────────────────▼───────────────────────┐
                  │          CUSTOM ELF32 LOADER                 │
                  │  - ELF Header validation (EM_ARM, 32-bit)    │
                  │  - PT_LOAD virtual memory mapping (mmap)     │
                  │  - W^X enforcement (mprotect RW -> RX)       │
                  └──────────────────────┬───────────────────────┘
                                         │
                  ┌──────────────────────▼───────────────────────┐
                  │   LINKER HOOK & TRANSLATION LAYER (DBT)      │
                  │  - JNI Proxy Thunking (JNIEnv32 <-> JNIEnv64)│
                  │  - Syscall Trapping (ARM32 SVC #0 -> Host)   │
                  │  - OpenGL ES Pass-Through (Host libGLESv2)   │
                  └──────────────────────────────────────────────┘
```

## Struttura del Progetto

- `app/src/main/cpp/`:
  - `elf_loader.h` / `elf_loader.cpp`: Custom ELF 32-bit Loader con gestione segmenti PT_LOAD e W^X.
  - `linker_hook.h` / `linker_hook.cpp`: JNI Shim a 32-bit, dispatcher Syscall ARM32 EABI e resolver GLES host.
  - `dbt_engine.h` / `dbt_engine.cpp`: Gestore CPU virtuale ARM32, stack guest e execution loop.
  - `jni_bridge.cpp`: Interfaccia JNI tra Kotlin e l'engine C++.
- `app/src/main/java/com/emux/translator/`:
  - `ui/xmb/XmbScreen.kt`: Interfaccia XMB stile PlayStation/PSP con snapping e focus dinamico.
  - `ui/grid/EmuxGridScreen.kt`: Interfaccia a griglia stile NetherSX2 con dark theme e copertine 3:4.
  - `model/EmuxRom.kt`: Modello dati e parser SAF per cartelle e APK.
  - `MainActivity.kt`: Controller principale e coordinatore ciclo di vita.
