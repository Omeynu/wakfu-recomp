# Analyse du XEX — Islands of Wakfu (XBLA 584109AA, TU #2 appliquée)

Produit par `scripts/analyse_xex.py` sur le XEX patché (`default.xex` + `default.xexp` de la TU #2,
appliqué par `XexPatcher` de XenonRecomp via `tools-src/xexdump`). Le XEX de base et le XEX patché
ont des sections de tailles différentes (`.text` 0x5258FC → 0x52754C) : le patch change bien le code.

## Synthèse

- XEX **non chiffré, non compressé** (rien à faire côté AES/LZX).
- XDK **2.0.20353** (≈ novembre 2010, chemin `xdk-main-nov10`), compilateur C1/C2 16.0.11886.
- Moteur maison, espace de noms C++ **`MikIso`** (145 classes RTTI) ; scripts **Lua 5.1.4**
  (602 `.lua` en clair dans `Media/Scripts`) ; **zlib**.
- Rendu : D3D9 Xbox 360 + D3DX9 + XGraphics, shaders précompilés `.xpu`/`.xvu` dans `Media/Shaders`.
- Audio : **XACT3** (banques `.xwb`/`.xsb`/`.xgs`) au-dessus de XAudio2, X3DAudio, XMA probable dans les wavebanks.
- Interface : **XUI** (XUIRUN/XUIRNDR/XUIVIDE/XUIHTM), paquet `Media/XUI/IslandsOfWakfu.xzp` (259 Mio).
- Vidéo : **WMV** (`Media/Videos`, décodeur XMV/WMV du XDK, section `.XBMOVIE`).
- Réseau : XONLINE + XNET liés ; URLs `/xmatch/*.ashx` (matchmaking HTTP, service sans doute disparu).
- Pas de Havok, Bink, Scaleform ni middleware physique détecté dans les chaînes.
- Imports : 148 xboxkrnl.exe + 43 xam.xex = 179 fonctions (thunks) + 12 variables ; tous nommés par les tables Xenia.

## En-tête XEX2

- Drapeaux module : `0x1` ; données PE à `0x2000` ; infos de sécurité à `0x90`
- Taille de l'image : `0x6c0000` (6912 Kio) ; adresse de chargement : `0x82000000`

### En-têtes optionnels (15)

| Clé | Nom | Valeur / décalage |
|---|---|---|
| `0x0002ff` | RESOURCE_INFO | `0xc34` |
| `0x0003ff` | FILE_FORMAT_INFO | `0xc48` |
| `0x010100` | ENTRY_POINT | `0x821a2bb0` |
| `0x010201` | IMAGE_BASE_ADDRESS | `0x82000000` |
| `0x0103ff` | IMPORT_LIBRARIES | `0x19c4` |
| `0x018002` | CHECKSUM_TIMESTAMP | `0xc70` |
| `0x0183ff` | ORIGINAL_PE_NAME | `0xc78` |
| `0x0200ff` | STATIC_LIBRARIES | `0xc88` |
| `0x020104` | TLS_INFO | `0xdec` |
| `0x020200` | DEFAULT_STACK_SIZE | `0x40000` |
| `0x030000` | SYSTEM_FLAGS | `0x404` |
| `0x040006` | EXECUTION_INFO | `0xdfc` |
| `0x040310` | GAME_RATINGS | `0xe14` |
| `0x040404` | LAN_KEY | `0xe54` |
| `0x0405ff` | XBOX360_LOGO | `0xe64` |

- Chiffrement : **aucun** ; compression : **aucune**
- Point d'entrée : `0x821a2bb0`
- Adresse de base : `0x82000000`
- Pile par défaut : `0x40000`
- Nom PE d'origine : `default.exe`
- Title ID : `584109AA` ; version `0x201` (base `0x201`) ; disque 0/0

### Sections

| Nom | Adresse | Taille | Type |
|---|---|---|---|
| `.rdata` | `0x82000400` | `0x000DEC4C` | data |
| `.pdata` | `0x820DF200` | `0x00015FB0` | data |
| `.text` | `0x82100000` | `0x0052754C` | code |
| `.data` | `0x82630000` | `0x0005BB1C` | data |
| `.XBMOVIE` | `0x8268BC00` | `0x0000000C` | data |
| `.idata` | `0x82690000` | `0x00000374` | data |
| `.XBLD` | `0x826A0000` | `0x00000160` | data |
| `.reloc` | `0x826A0200` | `0x00033CA4` | data |

## Bibliothèques XDK liées statiquement

| Bibliothèque | Version |
|---|---|
| XAPILIB | 2.0.20353.0 |
| D3D9 | 2.0.20353.1 |
| D3DX9 | 2.0.20353.0 |
| XGRAPHC | 2.0.20353.0 |
| XBOXKRNL | 2.0.20353.0 |
| XAUDIO2 | 2.0.20353.0 |
| XAPOBA | 2.0.20353.0 |
| XACT3 | 2.0.20353.0 |
| X3DAUD | 2.0.20353.0 |
| LIBCPMT | 2.0.11886.0 |
| XMCORE | 2.0.20353.0 |
| XUIRUN | 2.0.20353.0 |
| XUIRNDR | 2.0.20353.0 |
| XUIVIDE | 2.0.20353.0 |
| XONLINE | 2.0.20353.0 |
| XNET | 2.0.20353.0 |
| XUIHTM | 2.0.20353.0 |
| XMP | 2.0.20353.0 |
| XAUD | 2.0.20353.0 |
| LINK | 10.0.11886.0 |
| C2 | 16.0.11886.0 |
| C1 | 16.0.11886.0 |

## Imports

- **xam.xex** v2.0.20353.0 (min 2.0.9199.0) : 43 imports
- **xboxkrnl.exe** v2.0.20353.0 (min 2.0.9199.0) : 148 imports

| Appels `bl` | Module | Ordinal | Nom | Type |
|---:|---|---:|---|---|
| 271 | xboxkrnl.exe | 304 (`0x130`) | `RtlLeaveCriticalSection` | Function |
| 213 | xboxkrnl.exe | 293 (`0x125`) | `RtlEnterCriticalSection` | Function |
| 51 | xboxkrnl.exe | 102 (`0x66`) | `KeGetCurrentProcessType` | Function |
| 38 | xboxkrnl.exe | 302 (`0x12e`) | `RtlInitializeCriticalSection` | Function |
| 15 | xboxkrnl.exe | 341 (`0x155`) | `KeTlsSetValue` | Function |
| 13 | xboxkrnl.exe | 340 (`0x154`) | `KeTlsGetValue` | Function |
| 12 | xboxkrnl.exe | 16 (`0x10`) | `ExGetXConfigSetting` | Function |
| 11 | xboxkrnl.exe | 190 (`0xbe`) | `MmGetPhysicalAddress` | Function |
| 11 | xboxkrnl.exe | 204 (`0xcc`) | `NtAllocateVirtualMemory` | Function |
| 9 | xam.xex | 503 (`0x1f7`) | `XMsgStartIORequest` | Function |
| 9 | xboxkrnl.exe | 21 (`0x15`) | `ExRegisterTitleTerminateNotification` | Function |
| 9 | xboxkrnl.exe | 157 (`0x9d`) | `KeSetEvent` | Function |
| 9 | xboxkrnl.exe | 300 (`0x12c`) | `RtlInitAnsiString` | Function |
| 8 | xboxkrnl.exe | 261 (`0x105`) | `ObDereferenceObject` | Function |
| 8 | xboxkrnl.exe | 318 (`0x13e`) | `swprintf` | Function |
| 7 | xboxkrnl.exe | 82 (`0x52`) | `KeBugCheck` | Function |
| 7 | xboxkrnl.exe | 283 (`0x11b`) | `RtlCompareMemoryUlong` | Function |
| 7 | xboxkrnl.exe | 315 (`0x13b`) | `sprintf` | Function |
| 6 | xboxkrnl.exe | 220 (`0xdc`) | `NtFreeVirtualMemory` | Function |
| 5 | xam.xex | 791 (`0x317`) | `XamSessionRefObjByHandle` | Function |
| 5 | xboxkrnl.exe | 3 (`0x3`) | `DbgPrint` | Function |
| 5 | xboxkrnl.exe | 180 (`0xb4`) | `KfReleaseSpinLock` | Function |
| 5 | xboxkrnl.exe | 317 (`0x13d`) | `_snwprintf` | Function |
| 5 | xboxkrnl.exe | 319 (`0x13f`) | `RtlTimeFieldsToTime` | Function |
| 4 | xboxkrnl.exe | 83 (`0x53`) | `KeBugCheckEx` | Function |
| 4 | xboxkrnl.exe | 177 (`0xb1`) | `KfAcquireSpinLock` | Function |
| 4 | xboxkrnl.exe | 186 (`0xba`) | `MmAllocatePhysicalMemoryEx` | Function |
| 4 | xboxkrnl.exe | 189 (`0xbd`) | `MmFreePhysicalMemory` | Function |
| 4 | xboxkrnl.exe | 272 (`0x110`) | `ObReferenceObjectByHandle` | Function |
| 4 | xboxkrnl.exe | 309 (`0x135`) | `RtlNtStatusToDosError` | Function |
| 4 | xboxkrnl.exe | 320 (`0x140`) | `RtlTimeToTimeFields` | Function |
| 3 | xam.xex | 500 (`0x1f4`) | `XMsgInProcessCall` | Function |
| 3 | xam.xex | 642 (`0x282`) | `XamGetSystemVersion` | Function |
| 3 | xboxkrnl.exe | 95 (`0x5f`) | `KeEnterCriticalRegion` | Function |
| 3 | xboxkrnl.exe | 125 (`0x7d`) | `KeLeaveCriticalRegion` | Function |
| 3 | xboxkrnl.exe | 131 (`0x83`) | `KeQueryPerformanceFrequency` | Function |
| 3 | xboxkrnl.exe | 176 (`0xb0`) | `KeWaitForSingleObject` | Function |
| 3 | xboxkrnl.exe | 207 (`0xcf`) | `NtClose` | Function |
| 3 | xboxkrnl.exe | 247 (`0xf7`) | `NtSetInformationFile` | Function |
| 3 | xboxkrnl.exe | 253 (`0xfd`) | `NtWaitForSingleObjectEx` | Function |
| 3 | xboxkrnl.exe | 310 (`0x136`) | `RtlRaiseException` | Function |
| 3 | xboxkrnl.exe | 321 (`0x141`) | `RtlTryEnterCriticalSection` | Function |
| 3 | xboxkrnl.exe | 327 (`0x147`) | `RtlUnwind` | Function |
| 3 | xboxkrnl.exe | 338 (`0x152`) | `KeTlsAlloc` | Function |
| 3 | xboxkrnl.exe | 339 (`0x153`) | `KeTlsFree` | Function |
| 3 | xboxkrnl.exe | 442 (`0x1ba`) | `VdGetCurrentDisplayInformation` | Function |
| 3 | xboxkrnl.exe | 458 (`0x1ca`) | `VdQueryVideoMode` | Function |
| 3 | xboxkrnl.exe | 473 (`0x1d9`) | `VdSetSystemCommandBufferGpuIdentifierAddress` | Function |
| 2 | xam.xex | 425 (`0x1a9`) | `XamLoaderTerminateTitle` | Function |
| 2 | xam.xex | 492 (`0x1ec`) | `XamFree` | Function |
| 2 | xam.xex | 522 (`0x20a`) | `XamUserGetXUID` | Function |
| 2 | xam.xex | 651 (`0x28b`) | `XNotifyGetNext` | Function |
| 2 | xam.xex | 759 (`0x2f7`) | `XamUserCreateStatsEnumerator` | Function |
| 2 | xam.xex | 972 (`0x3cc`) | `XGetGameRegion` | Function |
| 2 | xboxkrnl.exe | 132 (`0x84`) | `KeQuerySystemTime` | Function |
| 2 | xboxkrnl.exe | 153 (`0x99`) | `KeSetBasePriorityThread` | Function |
| 2 | xboxkrnl.exe | 154 (`0x9a`) | `KeSetCurrentProcessType` | Function |
| 2 | xboxkrnl.exe | 175 (`0xaf`) | `KeWaitForMultipleObjects` | Function |
| 2 | xboxkrnl.exe | 209 (`0xd1`) | `NtCreateEvent` | Function |
| 2 | xboxkrnl.exe | 223 (`0xdf`) | `NtOpenFile` | Function |
| 2 | xboxkrnl.exe | 238 (`0xee`) | `NtQueryVirtualMemory` | Function |
| 2 | xboxkrnl.exe | 255 (`0xff`) | `NtWriteFile` | Function |
| 2 | xboxkrnl.exe | 281 (`0x119`) | `RtlCaptureContext` | Function |
| 2 | xboxkrnl.exe | 333 (`0x14d`) | `_vsnprintf` | Function |
| 2 | xboxkrnl.exe | 469 (`0x1d5`) | `VdSetGraphicsInterruptCallback` | Function |
| 2 | xboxkrnl.exe | 476 (`0x1dc`) | `VdShutdownEngines` | Function |
| 2 | xboxkrnl.exe | 617 (`0x269`) | `VdRetrainEDRAM` | Function |
| 1 | xam.xex | 1 (`0x1`) | `NetDll_WSAStartup` | Function |
| 1 | xam.xex | 400 (`0x190`) | `XamInputGetCapabilities` | Function |
| 1 | xam.xex | 402 (`0x192`) | `XamInputSetState` | Function |
| 1 | xam.xex | 408 (`0x198`) | `XamInputGetKeystrokeEx` | Function |
| 1 | xam.xex | 490 (`0x1ea`) | `XamAlloc` | Function |
| 1 | xam.xex | 504 (`0x1f8`) | `XMsgCancelIORequest` | Function |
| 1 | xam.xex | 508 (`0x1fc`) | `XMsgStartIORequestEx` | Function |
| 1 | xam.xex | 537 (`0x219`) | `XamUserReadProfileSettings` | Function |
| 1 | xam.xex | 538 (`0x21a`) | `XamUserWriteProfileSettings` | Function |
| 1 | xam.xex | 551 (`0x227`) | `XamUserGetSigninInfo` | Function |
| 1 | xam.xex | 601 (`0x259`) | `XamContentCreateEx` | Function |
| 1 | xam.xex | 640 (`0x280`) | `XamGetExecutionId` | Function |
| 1 | xam.xex | 652 (`0x28c`) | `XNotifyPositionUI` | Function |
| 1 | xam.xex | 732 (`0x2dc`) | `XamShowMessageBoxUIEx` | Function |
| 1 | xam.xex | 790 (`0x316`) | `XamSessionCreateHandle` | Function |
| 1 | xam.xex | 971 (`0x3cb`) | `XGetAVPack` | Function |
| 1 | xam.xex | 973 (`0x3cd`) | `XGetLanguage` | Function |
| 1 | xam.xex | 977 (`0x3d1`) | `XGetVideoMode` | Function |
| 1 | xboxkrnl.exe | 13 (`0xd`) | `ExCreateThread` | Function |
| 1 | xboxkrnl.exe | 25 (`0x19`) | `ExTerminateThread` | Function |
| 1 | xboxkrnl.exe | 33 (`0x21`) | `FscSetCacheElementCount` | Function |
| 1 | xboxkrnl.exe | 40 (`0x28`) | `HalReturnToFirmware` | Function |
| 1 | xboxkrnl.exe | 77 (`0x4d`) | `KeAcquireSpinLockAtRaisedIrql` | Function |
| 1 | xboxkrnl.exe | 90 (`0x5a`) | `KeDelayExecutionThread` | Function |
| 1 | xboxkrnl.exe | 93 (`0x5d`) | `KeEnableFpuExceptions` | Function |
| 1 | xboxkrnl.exe | 107 (`0x6b`) | `KeLockL2` | Function |
| 1 | xboxkrnl.exe | 108 (`0x6c`) | `KeUnlockL2` | Function |
| 1 | xboxkrnl.exe | 111 (`0x6f`) | `KeInitializeDpc` | Function |
| 1 | xboxkrnl.exe | 123 (`0x7b`) | `KeInsertQueueDpc` | Function |
| 1 | xboxkrnl.exe | 129 (`0x81`) | `KeQueryBasePriorityThread` | Function |
| 1 | xboxkrnl.exe | 137 (`0x89`) | `KeReleaseSpinLockFromRaisedIrql` | Function |
| 1 | xboxkrnl.exe | 143 (`0x8f`) | `KeResetEvent` | Function |
| 1 | xboxkrnl.exe | 151 (`0x97`) | `KeSetAffinityThread` | Function |
| 1 | xboxkrnl.exe | 194 (`0xc2`) | `MmMapIoSpace` | Function |
| 1 | xboxkrnl.exe | 198 (`0xc6`) | `MmQueryStatistics` | Function |
| 1 | xboxkrnl.exe | 206 (`0xce`) | `NtClearEvent` | Function |
| 1 | xboxkrnl.exe | 210 (`0xd2`) | `NtCreateFile` | Function |
| 1 | xboxkrnl.exe | 213 (`0xd5`) | `NtCreateSemaphore` | Function |
| 1 | xboxkrnl.exe | 219 (`0xdb`) | `NtFlushBuffersFile` | Function |
| 1 | xboxkrnl.exe | 231 (`0xe7`) | `NtQueryFullAttributesFile` | Function |
| 1 | xboxkrnl.exe | 232 (`0xe8`) | `NtQueryInformationFile` | Function |
| 1 | xboxkrnl.exe | 239 (`0xef`) | `NtQueryVolumeInformationFile` | Function |
| 1 | xboxkrnl.exe | 243 (`0xf3`) | `NtReleaseSemaphore` | Function |
| 1 | xboxkrnl.exe | 245 (`0xf5`) | `NtResumeThread` | Function |
| 1 | xboxkrnl.exe | 246 (`0xf6`) | `NtSetEvent` | Function |
| 1 | xboxkrnl.exe | 252 (`0xfc`) | `NtSuspendThread` | Function |
| 1 | xboxkrnl.exe | 254 (`0xfe`) | `NtWaitForMultipleObjectsEx` | Function |
| 1 | xboxkrnl.exe | 259 (`0x103`) | `ObCreateSymbolicLink` | Function |
| 1 | xboxkrnl.exe | 260 (`0x104`) | `ObDeleteSymbolicLink` | Function |
| 1 | xboxkrnl.exe | 294 (`0x126`) | `RtlFillMemoryUlong` | Function |
| 1 | xboxkrnl.exe | 295 (`0x127`) | `RtlFreeAnsiString` | Function |
| 1 | xboxkrnl.exe | 299 (`0x12b`) | `RtlImageXexHeaderField` | Function |
| 1 | xboxkrnl.exe | 301 (`0x12d`) | `RtlInitUnicodeString` | Function |
| 1 | xboxkrnl.exe | 303 (`0x12f`) | `RtlInitializeCriticalSectionAndSpinCount` | Function |
| 1 | xboxkrnl.exe | 307 (`0x133`) | `RtlMultiByteToUnicodeN` | Function |
| 1 | xboxkrnl.exe | 322 (`0x142`) | `RtlUnicodeStringToAnsiString` | Function |
| 1 | xboxkrnl.exe | 323 (`0x143`) | `RtlUnicodeToMultiByteN` | Function |
| 1 | xboxkrnl.exe | 335 (`0x14f`) | `_vscwprintf` | Function |
| 1 | xboxkrnl.exe | 337 (`0x151`) | `vswprintf` | Function |
| 1 | xboxkrnl.exe | 358 (`0x166`) | `XeCryptBnQwBeSigVerify` | Function |
| 1 | xboxkrnl.exe | 398 (`0x18e`) | `XeCryptRotSumSha` | Function |
| 1 | xboxkrnl.exe | 402 (`0x192`) | `XeCryptSha` | Function |
| 1 | xboxkrnl.exe | 404 (`0x194`) | `XexCheckExecutablePrivilege` | Function |
| 1 | xboxkrnl.exe | 405 (`0x195`) | `XexGetModuleHandle` | Function |
| 1 | xboxkrnl.exe | 406 (`0x196`) | `XexGetModuleSection` | Function |
| 1 | xboxkrnl.exe | 407 (`0x197`) | `XexGetProcedureAddress` | Function |
| 1 | xboxkrnl.exe | 433 (`0x1b1`) | `VdCallGraphicsNotificationRoutines` | Function |
| 1 | xboxkrnl.exe | 436 (`0x1b4`) | `VdEnableDisableClockGating` | Function |
| 1 | xboxkrnl.exe | 438 (`0x1b6`) | `VdEnableRingBufferRPtrWriteBack` | Function |
| 1 | xboxkrnl.exe | 441 (`0x1b9`) | `VdGetCurrentDisplayGamma` | Function |
| 1 | xboxkrnl.exe | 445 (`0x1bd`) | `VdGetSystemCommandBuffer` | Function |
| 1 | xboxkrnl.exe | 450 (`0x1c2`) | `VdInitializeEngines` | Function |
| 1 | xboxkrnl.exe | 451 (`0x1c3`) | `VdInitializeRingBuffer` | Function |
| 1 | xboxkrnl.exe | 453 (`0x1c5`) | `VdInitializeScalerCommandBuffer` | Function |
| 1 | xboxkrnl.exe | 454 (`0x1c6`) | `VdIsHSIOTrainingSucceeded` | Function |
| 1 | xboxkrnl.exe | 455 (`0x1c7`) | `VdPersistDisplay` | Function |
| 1 | xboxkrnl.exe | 457 (`0x1c9`) | `VdQueryVideoFlags` | Function |
| 1 | xboxkrnl.exe | 467 (`0x1d3`) | `VdSetDisplayMode` | Function |
| 1 | xboxkrnl.exe | 479 (`0x1df`) | `KiApcNormalRoutineNop` | Function |
| 1 | xboxkrnl.exe | 499 (`0x1f3`) | `XAudioRegisterRenderDriverClient` | Function |
| 1 | xboxkrnl.exe | 500 (`0x1f4`) | `XAudioUnregisterRenderDriverClient` | Function |
| 1 | xboxkrnl.exe | 501 (`0x1f5`) | `XAudioSubmitRenderDriverFrame` | Function |
| 1 | xboxkrnl.exe | 504 (`0x1f8`) | `XAudioGetVoiceCategoryVolume` | Function |
| 1 | xboxkrnl.exe | 511 (`0x1ff`) | `XAudioGetSpeakerConfig` | Function |
| 1 | xboxkrnl.exe | 548 (`0x224`) | `XMACreateContext` | Function |
| 1 | xboxkrnl.exe | 550 (`0x226`) | `XMAReleaseContext` | Function |
| 1 | xboxkrnl.exe | 580 (`0x244`) | `XeKeysGetKey` | Function |
| 1 | xboxkrnl.exe | 603 (`0x25b`) | `VdSwap` | Function |
| 1 | xboxkrnl.exe | 618 (`0x26a`) | `VdRetrainEDRAMWorker` | Function |
| 0 | xam.xex | 401 (`0x191`) | `XamInputGetState` | Function |
| 0 | xam.xex | 420 (`0x1a4`) | `XamLoaderLaunchTitle` | Function |
| 0 | xam.xex | 528 (`0x210`) | `XamUserGetSigninState` | Function |
| 0 | xam.xex | 592 (`0x250`) | `XamEnumerate` | Function |
| 0 | xam.xex | 602 (`0x25a`) | `XamContentClose` | Function |
| 0 | xam.xex | 606 (`0x25e`) | `XamContentGetDeviceData` | Function |
| 0 | xam.xex | 613 (`0x265`) | `XamContentGetDeviceState` | Function |
| 0 | xam.xex | 614 (`0x266`) | `XamContentGetLicenseMask` | Function |
| 0 | xam.xex | 650 (`0x28a`) | `XamNotifyCreateListener` | Function |
| 0 | xam.xex | 700 (`0x2bc`) | `XamShowSigninUI` | Function |
| 0 | xam.xex | 709 (`0x2c5`) | `XamShowAchievementsUI` | Function |
| 0 | xam.xex | 711 (`0x2c7`) | `XamShowMarketplaceUI` | Function |
| 0 | xam.xex | 715 (`0x2cb`) | `XamShowDeviceSelectorUI` | Function |
| 0 | xam.xex | 725 (`0x2d5`) | `XamShowGamerCardUIForXUID` | Function |
| 0 | xam.xex | 752 (`0x2f0`) | `XamWriteGamerTile` | Function |
| 0 | xboxkrnl.exe | 1 (`0x1`) | `DbgBreakPoint` | Function |
| 0 | xboxkrnl.exe | 27 (`0x1b`) | `ExThreadObjectType` | Variable |
| 0 | xboxkrnl.exe | 89 (`0x59`) | `KeDebugMonitorData` | Variable |
| 0 | xboxkrnl.exe | 173 (`0xad`) | `KeTimeStampBundle` | Variable |
| 0 | xboxkrnl.exe | 196 (`0xc4`) | `MmQueryAddressProtect` | Function |
| 0 | xboxkrnl.exe | 197 (`0xc5`) | `MmQueryAllocationSize` | Function |
| 0 | xboxkrnl.exe | 218 (`0xda`) | `NtDuplicateObject` | Function |
| 0 | xboxkrnl.exe | 228 (`0xe4`) | `NtQueryDirectoryFile` | Function |
| 0 | xboxkrnl.exe | 240 (`0xf0`) | `NtReadFile` | Function |
| 0 | xboxkrnl.exe | 241 (`0xf1`) | `NtReadFileScatter` | Function |
| 0 | xboxkrnl.exe | 342 (`0x156`) | `XboxHardwareInfo` | Variable |
| 0 | xboxkrnl.exe | 344 (`0x158`) | `XboxKrnlVersion` | Variable |
| 0 | xboxkrnl.exe | 403 (`0x193`) | `XexExecutableModuleHandle` | Variable |
| 0 | xboxkrnl.exe | 421 (`0x1a5`) | `__C_specific_handler` | Function |
| 0 | xboxkrnl.exe | 430 (`0x1ae`) | `ExLoadedCommandLine` | Variable |
| 0 | xboxkrnl.exe | 446 (`0x1be`) | `VdGlobalDevice` | Variable |
| 0 | xboxkrnl.exe | 447 (`0x1bf`) | `VdGlobalXamDevice` | Variable |
| 0 | xboxkrnl.exe | 448 (`0x1c0`) | `VdGpuClockInMHz` | Variable |
| 0 | xboxkrnl.exe | 449 (`0x1c1`) | `VdHSIOCalibrationLock` | Variable |
| 0 | xboxkrnl.exe | 614 (`0x266`) | `KeCertMonitorData` | Variable |

(Comptage : appels directs `bl` vers le thunk dans le code. Les variables et appels indirects ne sont pas comptés.)

## Indices dans les chaînes de l'image

- **Lua** (4) : `$Lua: Lua 5.1.4 Copyright (C) 1994-2008 Lua.org, PUC-Rio $`, `Lua 5.1`, `Lua function expected`, `PANIC: unprotected error in call to Lua API (%s)`
- **hk** (1) : ` "$&),/258;>ADGJMPSVY\_behknqtwz}`
- **XACT** (5) : `Could not create a XACT Engine`, `Failed to create event object for XACTNOTIFICATIONTYPE_MARKER notifica`, `m_pXACTEngine->CreateSoundBank failed`, `m_pXACTEngine->Initialize failed`, `xUnable to shutdown XACT engine`
- **xma** (4) : `/xmatch/xmatchclient2.ashx`, `/xmatch/xmatchhost2.ashx`, `/xmatch/xmatchhostdelete2.ashx`, `/xmatch/xmatchsplit.ashx`
- **Direct3D** (1) : `The IDirect3DAsyncCommandBufferCall9 involved (0x%p) hasn't been signa`
- **D3D** (29) : `          versions of D3D (D3D9D.LIB or D3D9I.LIB) to capture a`, `    D3DCREATE_CLIPPER_MODE_1 to implement a safe workaround which`, `    It was a growable command buffer with its D3DALLOCATECALLBACK rout`, `    build using instrumented or debug versions of the D3D library.`, `    by calling DrawVertices using D3DPT_TRILIST with a vertex`, `    command buffer call (D3DAsyncCommandBufferCall) at address 0x%08p`
- **XUI** (1) : `XUI Unable to load target image %S`
- **Xui** (16) : `.?AVAppXuiModule@MikIso@@`, `.?AVAppXuiScene@MikIso@@`, `.?AVCXuiBase@@`, `.?AVCXuiCachingModule@@`, `.?AVCXuiControl@@`, `.?AVCXuiElement@@`
- **WMV** (1) : `E:\xenon\xdk-main-nov10\core\private\del\codecs\wmv\codecdsp-cayenne\v`
- **zlib** (3) : `Unknown zlib error`, `zlib memory error`, `zlib version error`
- **Wakfu** (24) : ` Islands of Wakfu `, ` du gel de Wakfu pour faire pousser 10 calices de Manolias.`, ` il simbolo del continuo fluire del Wakfu!`, `-Use Wakfu gel to create 10 Manolias chalices.`, `.Wakfu gel used to create 10 Manolias chalices.`, `4Usa Gel de Wakfu para crear 10 c`

## Arborescence du jeu (base : 4677 fichiers, 804 Mio)

| Dossier | Fichiers | Mio | Extensions |
|---|---:|---:|---|
| `(racine)` | 23 | 9.9 | png (20), xml (1), - (1), xex (1) |
| `Media/AP3D` | 494 | 7.9 | ap3d (494) |
| `Media/APB` | 107 | 7.6 | apb (107) |
| `Media/APM` | 143 | 117.4 | apm (143) |
| `Media/Data` | 10 | 0.3 | bin (9), inf (1) |
| `Media/Maps` | 140 | 110.7 | sub (95), bis (15), dat (15), pre (15) |
| `Media/Scripts` | 602 | 1.0 | lua (602) |
| `Media/Shaders` | 148 | 0.5 | xpu (57), xpupdb (57), xvu (17), xvupdb (17) |
| `Media/Sounds` | 19 | 72.5 | xwb (16), xsb (2), xgs (1) |
| `Media/Strings` | 42 | 1.5 | xus (42) |
| `Media/Textures` | 2942 | 98.7 | png (2793), dds (149) |
| `Media/Videos` | 6 | 116.8 | wmv (6) |
| `Media/XUI` | 1 | 259.2 | xzp (1) |

## Mise à jour TU #2 (49 fichiers)

`default.xexp` (patch delta du XEX, 358 Kio) + remplacements dans `Media/Data`, `Media/Scripts` (21 .lua), `Media/Strings` (.xus), `Media/XUI` (.xur).
