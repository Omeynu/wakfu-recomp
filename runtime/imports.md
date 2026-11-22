# État des imports du runtime

Généré par `runtime/tools/imports_state.py`. « Appelé » : vu lors de la dernière exécution de référence.

147 implémentés sur 179.

| Import | État | Appelé |
|---|---|---|
| `DbgBreakPoint` | stub |  |
| `DbgPrint` | implémenté |  |
| `ExCreateThread` | implémenté | oui |
| `ExGetXConfigSetting` | implémenté | oui |
| `ExRegisterTitleTerminateNotification` | implémenté | oui |
| `ExTerminateThread` | implémenté | oui |
| `FscSetCacheElementCount` | implémenté | oui |
| `HalReturnToFirmware` | stub |  |
| `KeAcquireSpinLockAtRaisedIrql` | implémenté | oui |
| `KeBugCheck` | implémenté |  |
| `KeBugCheckEx` | implémenté |  |
| `KeDelayExecutionThread` | implémenté | oui |
| `KeEnableFpuExceptions` | stub |  |
| `KeEnterCriticalRegion` | implémenté | oui |
| `KeGetCurrentProcessType` | implémenté | oui |
| `KeInitializeDpc` | implémenté | oui |
| `KeInsertQueueDpc` | implémenté |  |
| `KeLeaveCriticalRegion` | implémenté | oui |
| `KeLockL2` | stub |  |
| `KeQueryBasePriorityThread` | implémenté | oui |
| `KeQueryPerformanceFrequency` | implémenté | oui |
| `KeQuerySystemTime` | implémenté | oui |
| `KeReleaseSpinLockFromRaisedIrql` | implémenté | oui |
| `KeResetEvent` | implémenté | oui |
| `KeSetAffinityThread` | implémenté | oui |
| `KeSetBasePriorityThread` | implémenté | oui |
| `KeSetCurrentProcessType` | implémenté |  |
| `KeSetEvent` | implémenté | oui |
| `KeTlsAlloc` | implémenté | oui |
| `KeTlsFree` | implémenté | oui |
| `KeTlsGetValue` | implémenté | oui |
| `KeTlsSetValue` | implémenté | oui |
| `KeUnlockL2` | stub |  |
| `KeWaitForMultipleObjects` | implémenté | oui |
| `KeWaitForSingleObject` | implémenté | oui |
| `KfAcquireSpinLock` | implémenté | oui |
| `KfReleaseSpinLock` | implémenté | oui |
| `KiApcNormalRoutineNop` | implémenté | oui |
| `MmAllocatePhysicalMemoryEx` | implémenté | oui |
| `MmFreePhysicalMemory` | implémenté | oui |
| `MmGetPhysicalAddress` | implémenté | oui |
| `MmMapIoSpace` | implémenté | vue 0xA0000000 de l'adresse physique (contexte XMA) |
| `MmQueryAddressProtect` | implémenté | oui |
| `MmQueryAllocationSize` | stub |  |
| `MmQueryStatistics` | implémenté | oui |
| `NetDll_WSAStartup` | implémenté | oui |
| `NtAllocateVirtualMemory` | implémenté | oui |
| `NtClearEvent` | implémenté | oui |
| `NtClose` | implémenté | oui |
| `NtCreateEvent` | implémenté | oui |
| `NtCreateFile` | implémenté | oui |
| `NtCreateSemaphore` | implémenté |  |
| `NtDuplicateObject` | implémenté |  |
| `NtFlushBuffersFile` | implémenté |  |
| `NtFreeVirtualMemory` | implémenté | oui |
| `NtOpenFile` | implémenté |  |
| `NtQueryDirectoryFile` | implémenté |  |
| `NtQueryFullAttributesFile` | implémenté | oui |
| `NtQueryInformationFile` | implémenté | oui |
| `NtQueryVirtualMemory` | stub |  |
| `NtQueryVolumeInformationFile` | implémenté |  |
| `NtReadFile` | implémenté | oui |
| `NtReadFileScatter` | stub |  |
| `NtReleaseSemaphore` | implémenté |  |
| `NtResumeThread` | implémenté | oui |
| `NtSetEvent` | implémenté | oui |
| `NtSetInformationFile` | implémenté | oui |
| `NtSuspendThread` | implémenté | oui |
| `NtWaitForMultipleObjectsEx` | implémenté | oui |
| `NtWaitForSingleObjectEx` | implémenté | oui |
| `NtWriteFile` | implémenté |  |
| `ObCreateSymbolicLink` | implémenté |  |
| `ObDeleteSymbolicLink` | implémenté |  |
| `ObDereferenceObject` | implémenté | oui |
| `ObReferenceObjectByHandle` | implémenté | oui |
| `RtlCaptureContext` | stub |  |
| `RtlCompareMemoryUlong` | implémenté |  |
| `RtlEnterCriticalSection` | implémenté | oui |
| `RtlFillMemoryUlong` | implémenté | oui |
| `RtlFreeAnsiString` | stub |  |
| `RtlImageXexHeaderField` | implémenté | oui |
| `RtlInitAnsiString` | implémenté | oui |
| `RtlInitUnicodeString` | implémenté |  |
| `RtlInitializeCriticalSection` | implémenté | oui |
| `RtlInitializeCriticalSectionAndSpinCount` | implémenté | oui |
| `RtlLeaveCriticalSection` | implémenté | oui |
| `RtlMultiByteToUnicodeN` | stub |  |
| `RtlNtStatusToDosError` | implémenté | oui |
| `RtlRaiseException` | stub |  |
| `RtlTimeFieldsToTime` | implémenté |  |
| `RtlTimeToTimeFields` | implémenté | oui |
| `RtlTryEnterCriticalSection` | implémenté | oui |
| `RtlUnicodeStringToAnsiString` | stub |  |
| `RtlUnicodeToMultiByteN` | stub |  |
| `RtlUnwind` | stub |  |
| `VdCallGraphicsNotificationRoutines` | implémenté | oui |
| `VdEnableDisableClockGating` | implémenté |  |
| `VdEnableRingBufferRPtrWriteBack` | implémenté | oui |
| `VdGetCurrentDisplayGamma` | implémenté | oui |
| `VdGetCurrentDisplayInformation` | implémenté | oui |
| `VdGetSystemCommandBuffer` | implémenté | oui |
| `VdInitializeEngines` | implémenté | oui |
| `VdInitializeRingBuffer` | implémenté | oui |
| `VdInitializeScalerCommandBuffer` | implémenté | oui |
| `VdIsHSIOTrainingSucceeded` | implémenté | oui |
| `VdPersistDisplay` | implémenté | oui |
| `VdQueryVideoFlags` | implémenté | oui |
| `VdQueryVideoMode` | implémenté | oui |
| `VdRetrainEDRAM` | implémenté | oui |
| `VdRetrainEDRAMWorker` | implémenté | oui |
| `VdSetDisplayMode` | implémenté | oui |
| `VdSetGraphicsInterruptCallback` | implémenté | oui |
| `VdSetSystemCommandBufferGpuIdentifierAddress` | implémenté | oui |
| `VdShutdownEngines` | implémenté |  |
| `VdSwap` | implémenté | oui |
| `XAudioGetSpeakerConfig` | implémenté | oui |
| `XAudioGetVoiceCategoryVolume` | implémenté | oui |
| `XAudioRegisterRenderDriverClient` | implémenté | oui |
| `XAudioSubmitRenderDriverFrame` | implémenté | oui |
| `XAudioUnregisterRenderDriverClient` | implémenté |  |
| `XGetAVPack` | implémenté | oui |
| `XGetGameRegion` | implémenté | oui |
| `XGetLanguage` | implémenté |  |
| `XGetVideoMode` | implémenté | oui |
| `XMACreateContext` | implémenté |  |
| `XMAReleaseContext` | implémenté |  |
| `XMsgCancelIORequest` | implémenté |  |
| `XMsgInProcessCall` | implémenté | oui |
| `XMsgStartIORequest` | implémenté | oui |
| `XMsgStartIORequestEx` | implémenté | oui |
| `XNotifyGetNext` | implémenté | oui |
| `XNotifyPositionUI` | implémenté | oui |
| `XamAlloc` | implémenté |  |
| `XamContentClose` | implémenté | oui |
| `XamContentCreateEx` | implémenté | oui |
| `XamContentGetDeviceData` | implémenté | oui |
| `XamContentGetDeviceState` | implémenté | oui |
| `XamContentGetLicenseMask` | implémenté | oui |
| `XamEnumerate` | implémenté |  |
| `XamFree` | implémenté |  |
| `XamGetExecutionId` | implémenté |  |
| `XamGetSystemVersion` | implémenté | oui |
| `XamInputGetCapabilities` | implémenté |  |
| `XamInputGetKeystrokeEx` | implémenté | oui |
| `XamInputGetState` | implémenté |  |
| `XamInputSetState` | implémenté |  |
| `XamLoaderLaunchTitle` | stub |  |
| `XamLoaderTerminateTitle` | implémenté |  |
| `XamNotifyCreateListener` | implémenté | oui |
| `XamSessionCreateHandle` | stub |  |
| `XamSessionRefObjByHandle` | stub |  |
| `XamShowAchievementsUI` | stub |  |
| `XamShowDeviceSelectorUI` | implémenté | oui |
| `XamShowGamerCardUIForXUID` | stub |  |
| `XamShowMarketplaceUI` | stub |  |
| `XamShowMessageBoxUIEx` | implémenté |  |
| `XamShowSigninUI` | implémenté |  |
| `XamUserCreateStatsEnumerator` | stub |  |
| `XamUserGetSigninInfo` | implémenté | oui |
| `XamUserGetSigninState` | implémenté | oui |
| `XamUserGetXUID` | implémenté |  |
| `XamUserReadProfileSettings` | implémenté | oui |
| `XamUserWriteProfileSettings` | implémenté | oui |
| `XamWriteGamerTile` | stub |  |
| `XeCryptBnQwBeSigVerify` | stub |  |
| `XeCryptRotSumSha` | stub |  |
| `XeCryptSha` | stub |  |
| `XeKeysGetKey` | stub |  |
| `XexCheckExecutablePrivilege` | implémenté | oui |
| `XexGetModuleHandle` | implémenté |  |
| `XexGetModuleSection` | implémenté |  |
| `XexGetProcedureAddress` | stub |  |
| `__C_specific_handler` | stub |  |
| `_snwprintf` | implémenté | oui |
| `_vscwprintf` | implémenté | oui |
| `_vsnprintf` | implémenté |  |
| `sprintf` | implémenté |  |
| `swprintf` | implémenté |  |
| `vswprintf` | implémenté | oui |
