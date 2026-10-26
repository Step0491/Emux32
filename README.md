# Emux32 - Android x32 Game Runner (ARM64-v8a)

[![Download Latest APK](https://img.shields.io/badge/Download%20APK-Latest%20(ARM64)-brightgreen?style=for-the-badge&logo=android)](https://github.com/Step0491/Emux32/releases/download/latest/Emux32-app-debug.apk)
[![Build Status](https://img.shields.io/badge/CI%2FCD-Auto--Release%20Passing-success?style=for-the-badge&logo=githubactions)](https://github.com/Step0491/Emux32/actions)
[![Architecture](https://img.shields.io/badge/Host-ARM64--v8a-blue?style=for-the-badge)](https://github.com/Step0491/Emux32)
[![Target ABI](https://img.shields.io/badge/Guest-ARMv7%20(32--bit)-orange?style=for-the-badge)](https://github.com/Step0491/Emux32)

Applicazione Android nativa a 64-bit (ARM64-v8a) che funge da **Frontend + Dynamic Binary Translator (DBT)** per eseguire APK e librerie native a 32-bit (`armeabi-v7a`) su moderni SoC Android privi di supporto hardware AArch32 in EL0 (es. Snapdragon 8 Gen 2/3, Google Tensor G3/G4, Dimensity 9200/9300), **senza istanziare una macchina virtuale Android completa**.

> ⚡ **Continuous Delivery**: L'APK collegato al badge di download viene **ricompilato e aggiornato automaticamente ad ogni singolo push** sul repository tramite GitHub Actions.

---

## 📲 Struttura Cartelle e Utilizzo

All'avvio e installazione, Emux32 crea e monitora automaticamente la seguente cartella nella memoria principale del dispositivo:

```text
/sdcard/Emux32/
   ├── apk/   <-- Inserisci qui i file .apk a 32 bit (ed eventuali cover .jpg/.png con lo stesso nome)
   └── obb/   <-- Inserisci qui i file di espansione .obb (o cartelle package con .obb)
```

- **Lista immediata**: L'app si apre direttamente mostrando la griglia di giochi (2 per riga in verticale).
- **Icone quadrate con Upscale 4x**: Copertina quadrata (1:1) ad alta definizione elaborata da algoritmo adattivo offline.
- **Lancio diretto al tocco**: Nessun selettore a scorrimento, nessun menu superiore, basta toccare l'icona del gioco per avviarlo.
- **Orientamento dinamico adattivo**: Riconosce automaticamente se il titolo è nativo Portrait o Landscape.

---

## 🏛️ Architettura del Sistema

```text
                  ┌──────────────────────────────────────────────┐
                  │       FRONTEND (Kotlin + Compose)            │
                  │  - Griglia touch 2 colonne con icone quadrate │
                  │  - Cartelle automatiche Emux32/apk e obb     │
                  │  - Continuous Auto-Release Pipeline (CI/CD)  │
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
