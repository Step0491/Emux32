# Emux32 - Android x32 Game Runner (ARM64-v8a)

[![Download APK](https://img.shields.io/badge/Download%20APK-v1.0.0%20(ARM64)-brightgreen?style=for-the-badge&logo=android)](https://github.com/Step0491/Emux32/releases/latest/download/Emux32-app-debug.apk)
[![Build Status](https://img.shields.io/badge/CI%2FCD-Build%20Passing-success?style=for-the-badge&logo=githubactions)](https://github.com/Step0491/Emux32/actions)
[![Architecture](https://img.shields.io/badge/Host-ARM64--v8a-blue?style=for-the-badge)](https://github.com/Step0491/Emux32)
[![Target ABI](https://img.shields.io/badge/Guest-ARMv7%20(32--bit)-orange?style=for-the-badge)](https://github.com/Step0491/Emux32)

Applicazione Android nativa a 64-bit (ARM64-v8a) che funge da **Frontend + Dynamic Binary Translator (DBT)** per eseguire APK e librerie native a 32-bit (`armeabi-v7a`) su moderni SoC Android privi di supporto hardware AArch32 in EL0 (es. Snapdragon 8 Gen 2/3, Google Tensor G3/G4, Dimensity 9200/9300), **senza istanziare una macchina virtuale Android completa**.

---

## 📲 Come Installare (Per gli Utenti)

Per chi vuole semplicemente usare l'applicazione sul proprio dispositivo Android moderno:

1. **Scarica l'APK**: Clicca sul pulsante verde [Download APK v1.0.0](https://github.com/Step0491/Emux32/releases/latest/download/Emux32-app-debug.apk) oppure apri la sezione **Releases** nella colonna a destra della pagina GitHub.
2. **Installa sul telefono**: Apri il file scaricato `Emux32-app-debug.apk` e conferma l'installazione (consenti l'installazione da origini sconosciute se richiesto dal browser).
3. **Prepara i giochi**: Crea una cartella nella memoria del telefono (es. `/sdcard/Giochi32/`) e metti le cartelle dei tuoi vecchi giochi:
   ```text
   /sdcard/Giochi32/
      ├── Gioco1/
      │     ├── game.apk    <-- L'APK a 32 bit originale
      │     └── cover.jpg   <-- Locandina verticale (opzionale)
      └── Gioco2/
            ├── game.apk
            └── cover.png
   ```
4. **Avvia Emux32**: Apri l'app, tocca l'icona della cartella in alto a destra, seleziona `/sdcard/Giochi32/` e tocca il gioco per avviarlo con l'interfaccia in stile PSP (XMB) o a Griglia.

---

## 🏛️ Architettura del Sistema

```text
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

---

## 🛠️ Tecnologie Utilizzate

- **Frontend**: Kotlin, Jetpack Compose Material 3, Coil Image Loader, Storage Access Framework (SAF).
- **Core Engine**: C++20, Android NDK (r26), Clang.
- **Memory Management**: Custom virtual memory layout con rispetto rigoroso di W^X (Write XOR Execute) compatibile con kernel Android moderni e page-size 4KB/16KB.
- **Graphics Pipeline**: OpenGL ES Pass-through diretto alla `libGLESv2.so` a 64-bit dell'host per rendering hardware a 60 FPS.
