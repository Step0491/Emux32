# Emux32 - Android x32 Game Runner (ARM64-v8a)

[![Download Latest APK](https://img.shields.io/badge/Download%20APK-Latest%20(ARM64)-brightgreen?style=for-the-badge&logo=android)](https://github.com/Step0491/Emux32/releases/download/latest/Emux32-app-debug.apk)
[![Build Status](https://img.shields.io/badge/CI%2FCD-Auto--Release%20Passing-success?style=for-the-badge&logo=githubactions)](https://github.com/Step0491/Emux32/actions)
[![Architecture](https://img.shields.io/badge/Host-ARM64--v8a-blue?style=for-the-badge)](https://github.com/Step0491/Emux32)
[![Target ABI](https://img.shields.io/badge/Guest-ARMv7%20(32--bit)-orange?style=for-the-badge)](https://github.com/Step0491/Emux32)

Applicazione Android nativa a 64-bit (ARM64-v8a) che funge da **Frontend + Dynamic Binary Translator (DBT)** per eseguire APK e librerie native a 32-bit (`armeabi-v7a`) su moderni SoC Android privi di supporto hardware AArch32 in EL0 (es. Snapdragon 8 Gen 2/3, Google Tensor G3/G4, Dimensity 9200/9300), **senza istanziare una macchina virtuale Android completa**.

> ⚡ **Continuous Delivery**: L'APK collegato al badge di download viene **ricompilato e aggiornato automaticamente ad ogni singolo push** sul repository tramite GitHub Actions.

---

## 📲 Come Installare (Per gli Utenti)

Per chi vuole semplicemente usare l'applicazione sul proprio dispositivo Android moderno:

1. **Scarica l'APK più recente**: Tocca il pulsante verde [Download APK Latest](https://github.com/Step0491/Emux32/releases/download/latest/Emux32-app-debug.apk) (scarica sempre la versione aggiornata all'ultimo commit).
2. **Installa sul telefono**: Apri il file scaricato `Emux32-app-debug.apk` e conferma l'installazione.
3. **Prepara i giochi**: Crea una cartella nella memoria del telefono (es. `/sdcard/Giochi32/`) con la seguente gerarchia:
   ```text
   /sdcard/Giochi32/
      ├── Gioco1/
      │     ├── game.apk    <-- L'APK a 32 bit originale
      │     └── cover.jpg   <-- Locandina verticale (opzionale)
      └── Gioco2/
            ├── game.apk
            └── cover.png
   ```
4. **Avvia Emux32**: Apri l'app in verticale, tocca l'icona della cartella in alto per selezionare `/sdcard/Giochi32/` e tocca un titolo per avviare l'emulatore.

---

## 🏛️ Architettura del Sistema

```text
                  ┌──────────────────────────────────────────────┐
                  │       FRONTEND (Kotlin + Compose)            │
                  │  - Portrait Mode locked for Game Selector    │
                  │  - Continuous Auto-Release Pipeline (CI/CD)  │
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
                  │  - Dynamic Adaptive Sensor In-Game           │
                  └──────────────────────────────────────────────┘
```

---

## 🛠️ Pipeline di Rilascio Automatico (CI/CD)

Il repository è configurato con automazione completa:
- **Build ad ogni Push**: Il runner GitHub compila NDK Clang C++20, assembla il DEX Compose e genera l'APK.
- **Auto-Release**: Ad ogni push sul branch `main`, l'action aggiorna la tag permanente `latest` e sostituisce il binario `Emux32-app-debug.apk`, garantendo che chiunque scarichi dal link ottenga sempre il codice più recente.
