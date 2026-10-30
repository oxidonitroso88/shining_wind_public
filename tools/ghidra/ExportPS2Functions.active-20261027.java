// Exports PS2Recomp TOML config (+ optional CSV) from Ghidra
// @category PS2Recomp

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressRangeIterator;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolType;

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.regex.Pattern;

public class ExportPS2Functions extends GhidraScript {

    // Names and values mirror ps2recomp::MipsOpcodes/SpecialFunctions/GprRegisters.
    // would be amazing cmake create this script coping the register from the header file
    private static final int MIPS_INSTRUCTION_SIZE = 4;
    private static final int MIPS_IMMEDIATE_BITS = 16;
    private static final int MIPS_IMMEDIATE_SIGN_BIT = 0x8000;
    private static final long UINT32_MASK = 0xFFFFFFFFL;
    private static final long OPCODE_MASK = 0x3FL;
    private static final long REGISTER_MASK = 0x1FL;
    private static final long IMMEDIATE_MASK = 0xFFFFL;
    private static final int OPCODE_SHIFT = 26;
    private static final int RS_SHIFT = 21;
    private static final int RT_SHIFT = 16;
    private static final int RD_SHIFT = 11;

    private static final int GPR_ZERO = 0;
    private static final int GPR_A0 = 4;
    private static final int GPR_A3 = 7;
    private static final int GPR_SP = 29;
    private static final int GPR_RA = 31;

    private static final int OPCODE_SPECIAL = 0x00;
    private static final int OPCODE_REGIMM = 0x01;
    private static final int OPCODE_J = 0x02;
    private static final int OPCODE_JAL = 0x03;
    private static final int OPCODE_BEQ = 0x04;
    private static final int OPCODE_BNE = 0x05;
    private static final int OPCODE_BLEZ = 0x06;
    private static final int OPCODE_BGTZ = 0x07;
    private static final int OPCODE_ADDI = 0x08;
    private static final int OPCODE_ADDIU = 0x09;
    private static final int OPCODE_SLTI = 0x0A;
    private static final int OPCODE_SLTIU = 0x0B;
    private static final int OPCODE_ANDI = 0x0C;
    private static final int OPCODE_ORI = 0x0D;
    private static final int OPCODE_XORI = 0x0E;
    private static final int OPCODE_LUI = 0x0F;
    private static final int OPCODE_BEQL = 0x14;
    private static final int OPCODE_BNEL = 0x15;
    private static final int OPCODE_BLEZL = 0x16;
    private static final int OPCODE_BGTZL = 0x17;
    private static final int OPCODE_DADDI = 0x18;
    private static final int OPCODE_DADDIU = 0x19;
    private static final int OPCODE_LDL = 0x1A;
    private static final int OPCODE_LDR = 0x1B;
    private static final int OPCODE_MMI = 0x1C;
    private static final int OPCODE_LQ = 0x1E;
    private static final int OPCODE_SQ = 0x1F;
    private static final int OPCODE_LB = 0x20;
    private static final int OPCODE_LH = 0x21;
    private static final int OPCODE_LWL = 0x22;
    private static final int OPCODE_LW = 0x23;
    private static final int OPCODE_LBU = 0x24;
    private static final int OPCODE_LHU = 0x25;
    private static final int OPCODE_LWR = 0x26;
    private static final int OPCODE_LWU = 0x27;
    private static final int OPCODE_SW = 0x2B;
    private static final int OPCODE_LL = 0x30;
    private static final int OPCODE_LLD = 0x34;
    private static final int OPCODE_LD = 0x37;
    private static final int OPCODE_SD = 0x3F;

    private static final int SPECIAL_JR = 0x08;
    private static final int SPECIAL_JALR = 0x09;

    // For now I have to copy all functions from the runtime handler list
    private static final Set<String> RUNTIME_HANDLER_NAMES = new HashSet<>(Arrays.asList(
        "FlushCache", "iFlushCache", "ResetEE", "SetMemoryMode", "InitThread", "CreateThread",
        "DeleteThread", "StartThread", "ExitThread", "ExitDeleteThread", "TerminateThread", "SuspendThread",
        "ResumeThread", "GetThreadId", "ReferThreadStatus", "iReferThreadStatus", "SleepThread", "WakeupThread",
        "iWakeupThread", "CancelWakeupThread", "iCancelWakeupThread", "ChangeThreadPriority", "iChangeThreadPriority", "RotateThreadReadyQueue",
        "iRotateThreadReadyQueue", "ReleaseWaitThread", "iReleaseWaitThread", "CreateSema", "DeleteSema", "SignalSema",
        "iSignalSema", "WaitSema", "PollSema", "iPollSema", "ReferSemaStatus", "iReferSemaStatus",
        "CreateEventFlag", "DeleteEventFlag", "SetEventFlag", "iSetEventFlag", "ClearEventFlag", "iClearEventFlag",
        "WaitEventFlag", "PollEventFlag", "iPollEventFlag", "ReferEventFlagStatus", "iReferEventFlagStatus", "InitAlarm",
        "SetAlarm", "iSetAlarm", "CancelAlarm", "iCancelAlarm", "ReleaseAlarm", "iReleaseAlarm",
        "AddIntcHandler", "AddIntcHandler2", "RemoveIntcHandler", "AddDmacHandler", "AddDmacHandler2", "RemoveDmacHandler",
        "EnableIntc", "iEnableIntc", "DisableIntc", "iDisableIntc", "EnableDmac", "iEnableDmac",
        "DisableDmac", "iDisableDmac", "SifStopModule", "SifLoadModule", "SifInitRpc", "SifBindRpc",
        "SifCallRpc", "SifRegisterRpc", "SifCheckStatRpc", "SifSetRpcQueue", "SifRemoveRpcQueue", "SifRemoveRpc",
        "sceSifCallRpc", "sceSifSendCmd", "sceRpcGetPacket", "fioOpen", "fioClose", "fioRead",
        "fioWrite", "fioLseek", "fioMkdir", "fioChdir", "fioRmdir", "fioGetstat",
        "fioRemove", "SetGsCrt", "GsSetCrt", "GsGetIMR", "iGsGetIMR", "GsPutIMR",
        "iGsPutIMR", "SetVSyncFlag", "SetSyscall", "GsSetVideoMode", "GetOsdConfigParam", "SetOsdConfigParam",
        "EnableCache", "DisableCache", "SifLoadElfPart", "sceSifLoadElf", "sceSifLoadElfPart",
        "sceSifLoadModule", "sceSifLoadModuleBuffer", "SetupThread", "EndOfHeap", "GetMemorySize", "Deci2Call",
        "QueryBootMode", "GetThreadTLS", "Copy", "GetEntryAddress", "RegisterExitHandler", "ret0", "ret1", "reta0",
        "calloc_r", "free_r", "realloc_r", "memalign_r", "malloc_r", "malloc_extend_top", "malloc_trim_r", "mbtowc_r", "printf_r",
        "__malloc_lock", "__malloc_unlock",
        "abs", "__ieee754_rem_pio2f", "__kernel_cosf", "__kernel_sinf", "atan", "atan2",
        "calloc", "ceil", "close", "cos", "exit", "exp",
        "fabs", "fclose", "fflush", "floor", "fopen", "fprintf",
        "fread", "free", "fseek", "fstat", "ftell", "fwrite",
        "getpid", "log", "log10", "lseek", "malloc", "memalign", "memclr", "memchr",
        "memcmp", "memcpy", "memmove", "memset", "open", "pow",
        "printf", "puts", "rand", "read", "realloc", "sin",
        "snprintf", "sprintf", "sqrt", "srand", "stat", "strcasecmp",
        "strcat", "strchr", "strcmp", "strcpy", "strlen", "strncat",
        "strncmp", "strncpy", "strrchr", "strstr", "tan", "vfprintf",
        "vsprintf", "write", "__divdi3",
        "DmaAddr", "builtin_set_imask", "sceCdRI", "sceCdRM",
        "sceDevVif0Reset", "sceDevVu0Reset", "sceFsDbChk", "sceFsIntrSigSema", "sceFsSemExit", "sceFsSemInit",
        "sceFsSigSema", "sceIDC", "sceMpegFlush", "sceRpcFreePacket", "sceRpcGetFPacket", "sceRpcGetFPacket2",
        "sceSDC", "sceSifCmdIntrHdlr", "sceVu0ecossin", "mcCallMessageTypeSe", "mcCheckReadStartConfigFile", "mcCheckReadStartSaveFile",
        "mcCheckWriteStartConfigFile", "mcCheckWriteStartSaveFile", "mcCreateConfigInit", "mcCreateFileSelectWindow", "mcCreateIconInit", "mcCreateSaveFileInit",
        "mcDispFileName", "mcDispFileNumber", "mcDispWindowCurSol", "mcDispWindowFoundtion", "mcDisplayFileSelectWindow", "mcDisplaySelectFileInfo",
        "mcDisplaySelectFileInfoMesCount", "mcGetConfigCapacitySize", "mcGetFileSelectWindowCursol", "mcGetFreeCapacitySize", "mcGetIconCapacitySize", "mcGetIconFileCapacitySize",
        "mcGetPortSelectDirInfo", "mcGetSaveFileCapacitySize", "mcGetStringEnd", "mcMoveFileSelectWindowCursor", "mcNewCreateConfigFile", "mcNewCreateIcon",
        "mcNewCreateSaveFile", "mcReadIconData", "mcReadStartConfigFile", "mcReadStartSaveFile", "mcSelectFileInfoInit", "mcSelectSaveFileCheck",
        "mcSetFileSelectWindowCursol", "mcSetFileSelectWindowCursolInit", "mcSetStringSaveFile", "mcSetTyepWriteMode", "mcWriteIconData", "mcWriteStartConfigFile",
        "mcWriteStartSaveFile", "mceGetInfoApdx", "mceIntrReadFixAlign", "mceStorePwd", "sceCdApplyNCmd", "sceCdBreak",
        "sceCdCallback", "sceCdChangeThreadPriority", "sceCdDelayThread", "sceCdDiskReady", "sceCdGetDiskType", "sceCdGetError",
        "sceCdGetReadPos", "sceCdGetToc", "sceCdInit", "sceCdInitEeCB", "sceCdIntToPos", "sceCdMmode",
        "sceCdNcmdDiskReady", "sceCdPause", "sceCdPosToInt", "sceCdRead", "sceCdReadChain", "sceCdReadClock",
        "sceCdReadIOPm", "sceCdSearchFile", "sceCdSeek", "sceCdStInit", "sceCdStPause", "sceCdStRead",
        "sceCdStResume", "sceCdStSeek", "sceCdStSeekF", "sceCdStStart", "sceCdStStat", "sceCdStStop",
        "sceCdStandby", "sceCdStatus", "sceCdStop", "sceCdStream", "sceCdSync", "sceCdSyncS",
        "sceCdTrayReq", "sceClose", "sceDeci2Close", "sceDeci2ExLock", "sceDeci2ExRecv", "sceDeci2ExReqSend",
        "sceDeci2ExSend", "sceDeci2ExUnLock", "sceDeci2Open", "sceDeci2Poll", "sceDeci2ReqSend", "sceDmaCallback",
        "sceDmaDebug", "sceDmaGetChan", "sceDmaGetEnv", "sceDmaLastSyncTime", "sceDmaPause", "sceDmaPutEnv",
        "sceDmaPutStallAddr", "sceDmaRecv", "sceDmaRecvI", "sceDmaRecvN", "sceDmaReset", "sceDmaRestart",
        "sceDmaSend", "sceDmaSendI", "sceDmaSendM", "sceDmaSendN", "sceDmaSync", "sceDmaSyncN",
        "sceDmaWatch", "sceFsInit", "sceFsReset", "sceGifPkAddGsAD", "sceGifPkAddGsData", "sceGifPkCloseGifTag",
        "sceGifPkCnt", "sceGifPkEnd", "sceGifPkInit", "sceGifPkOpenGifTag", "sceGifPkRef", "sceGifPkRefLoadImage",
        "sceGifPkReset", "sceGifPkReserve", "sceGifPkTerminate", "sceGsExecLoadImage", "sceGsExecStoreImage", "sceGsGetGParam",
        "sceGsPutDispEnv", "sceGsPutDrawEnv", "sceGsResetGraph", "sceGsResetPath", "sceGsSetDefClear", "sceGsSetDefDBuffDc",
        "sceGsSetDefDBuff", "sceGsSetDefDispEnv", "sceGsSetDefDrawEnv", "sceGsSetDefDrawEnv2", "sceGsSetDefLoadImage", "sceGsSetDefStoreImage",
        "sceGsSwapDBuffDc", "sceGsSwapDBuff", "sceGsSyncPath", "sceGsSyncV", "sceGsSyncVCallback", "sceGszbufaddr",
        "sceVif1PkAddGsAD", "sceVif1PkAlign", "sceVif1PkCall", "sceVif1PkCloseDirectCode", "sceVif1PkCloseGifTag", "sceVif1PkCnt",
        "sceVif1PkEnd", "sceVif1PkInit", "sceVif1PkOpenDirectCode", "sceVif1PkOpenGifTag", "sceVif1PkReset", "sceVif1PkReserve",
        "sceVif1PkTerminate", "sceeFontInit", "sceeFontLoadFont", "sceeFontPrintfAt", "sceeFontPrintfAt2", "sceeFontGenerateString",
        "sceeFontClose", "sceeFontSetColour", "sceeFontSetMode", "sceeFontSetFont", "sceeFontSetScale", "sceIoctl",
        "sceIpuInit", "sceIpuRestartDMA", "sceIpuStopDMA", "sceIpuSync", "sceLseek", "sceMcChangeThreadPriority",
        "sceMcChdir", "sceMcClose", "sceMcDelete", "sceMcEnd", "sceMcFlush", "sceMcFormat",
        "sceMcGetDir", "sceMcGetEntSpace", "sceMcGetInfo", "sceMcGetSlotMax", "sceMcInit", "sceMcMkdir",
        "sceMcOpen", "sceMcRead", "sceMcRename", "sceMcSeek", "sceMcSetFileInfo", "sceMcSync",
        "sceMcUnformat", "sceMcWrite", "sceMpegAddBs", "sceMpegAddCallback", "sceMpegAddStrCallback", "sceMpegClearRefBuff",
        "sceMpegCreate", "sceMpegDelete", "sceMpegDemuxPss", "sceMpegDemuxPssRing", "sceMpegDispCenterOffX", "sceMpegDispCenterOffY",
        "sceMpegDispHeight", "sceMpegDispWidth", "sceMpegGetDecodeMode", "sceMpegGetPicture", "sceMpegGetPictureRAW8", "sceMpegGetPictureRAW8xy",
        "sceMpegInit", "sceMpegIsEnd", "sceMpegIsRefBuffEmpty", "sceMpegReset", "sceMpegResetDefaultPtsGap", "sceMpegSetDecodeMode",
        "sceMpegSetDefaultPtsGap", "sceMpegSetImageBuff", "sceOpen", "scePadEnd", "scePadEnterPressMode", "scePadExitPressMode",
        "scePadGetButtonMask", "scePadGetDmaStr", "scePadGetFrameCount", "scePadGetModVersion", "scePadGetPortMax", "scePadGetReqState",
        "scePadGetSlotMax", "scePadGetState", "scePadInfoAct", "scePadInfoComb", "scePadInfoMode", "scePadInfoPressMode",
        "scePadInit", "scePadInit2", "scePadPortClose", "scePadPortOpen", "scePadRead", "scePadReqIntToStr",
        "scePadSetActAlign", "scePadSetActDirect", "scePadSetButtonInfo", "scePadSetMainMode", "scePadSetReqState", "scePadSetVrefParam",
        "scePadSetWarningLevel", "scePadStateIntToStr", "scePrintf", "sceRead", "sceResetttyinit", "sceSSyn_BreakAtick",
        "sceSSyn_ClearBreakAtick", "sceSSyn_SendExcMsg", "sceSSyn_SendNrpnMsg", "sceSSyn_SendRpnMsg", "sceSSyn_SendShortMsg", "sceSSyn_SetChPriority",
        "sceSSyn_SetMasterVolume", "sceSSyn_SetOutPortVolume", "sceSSyn_SetOutputAssign", "sceSSyn_SetOutputMode", "sceSSyn_SetPortMaxPoly", "sceSSyn_SetPortVolume",
        "sceSSyn_SetTvaEnvMode", "sceSdCallBack", "sceSdRemote", "sceSdRemoteInit", "sceSdTransToIOP", "sceSetBrokenLink",
        "sceSetPtm", "sceSifAddCmdHandler", "sceSifAllocIopHeap", "sceSifAllocSysMemory", "sceSifBindRpc", "sceSifCheckStatRpc",
        "sceSifDmaStat", "sceSifExecRequest", "sceSifExitCmd", "sceSifExitRpc", "sceSifFreeIopHeap", "sceSifFreeSysMemory",
        "sceSifGetDataTable", "sceSifGetIopAddr", "sceSifGetNextRequest", "sceSifGetOtherData", "sceSifGetReg", "sceSifGetSreg",
        "sceSifInitCmd", "sceSifInitIopHeap", "sceSifInitRpc", "sceSifIsAliveIop", "sceSifLoadFileReset", "sceSifLoadIopHeap",
        "sceSifRebootIop", "sceSifRegisterRpc", "sceSifRemoveCmdHandler", "sceSifRemoveRpc", "sceSifRemoveRpcQueue", "sceSifResetIop",
        "sceSifRpcLoop", "sceSifSetCmdBuffer", "sceSifSetDChain", "sceSifSetDma", "isceSifSetDChain", "isceSifSetDma",
        "sceSifSetIopAddr", "sceSifSetReg", "sceSifSetRpcQueue", "sceSifSetSreg", "sceSifSetSysCmdBuffer", "sceSifStopDma",
        "sceSifSyncIop", "sceSifWriteBackDCache", "sceSynthSizerLfoTriangle", "sceSynthesizerAmpProcI", "sceSynthesizerAmpProcNI", "sceSynthesizerAssignAllNoteOff",
        "sceSynthesizerAssignAllSoundOff", "sceSynthesizerAssignHoldChange", "sceSynthesizerAssignNoteOff", "sceSynthesizerAssignNoteOn", "sceSynthesizerCalcEnv", "sceSynthesizerCalcPortamentPitch",
        "sceSynthesizerCalcTvfCoefAll", "sceSynthesizerCalcTvfCoefF0", "sceSynthesizerCent2PhaseInc", "sceSynthesizerChangeEffectSend", "sceSynthesizerChangeHsPanpot", "sceSynthesizerChangeNrpnCutOff",
        "sceSynthesizerChangeNrpnLfoDepth", "sceSynthesizerChangeNrpnLfoRate", "sceSynthesizerChangeOutAttrib", "sceSynthesizerChangeOutVol", "sceSynthesizerChangePanpot", "sceSynthesizerChangePartBendSens",
        "sceSynthesizerChangePartExpression", "sceSynthesizerChangePartHsExpression", "sceSynthesizerChangePartHsPitchBend", "sceSynthesizerChangePartModuration", "sceSynthesizerChangePartPitchBend", "sceSynthesizerChangePartVolume",
        "sceSynthesizerChangePortamento", "sceSynthesizerChangePortamentoTime", "sceSynthesizerClearKeyMap", "sceSynthesizerClearSpr", "sceSynthesizerCopyOutput", "sceSynthesizerDmaFromSPR",
        "sceSynthesizerDmaSpr", "sceSynthesizerDmaToSPR", "sceSynthesizerGetPartOutLevel", "sceSynthesizerGetPartial", "sceSynthesizerGetSampleParam", "sceSynthesizerHsMessage",
        "sceSynthesizerLfoNone", "sceSynthesizerLfoProc", "sceSynthesizerLfoSawDown", "sceSynthesizerLfoSawUp", "sceSynthesizerLfoSquare", "sceSynthesizerReadNoise",
        "sceSynthesizerReadNoiseAdd", "sceSynthesizerReadSample16", "sceSynthesizerReadSample16Add", "sceSynthesizerReadSample8", "sceSynthesizerReadSample8Add", "sceSynthesizerResetPart",
        "sceSynthesizerRestorDma", "sceSynthesizerSelectPatch", "sceSynthesizerSendShortMessage", "sceSynthesizerSetMasterVolume", "sceSynthesizerSetRVoice", "sceSynthesizerSetupDma",
        "sceSynthesizerSetupLfo", "sceSynthesizerSetupMidiModuration", "sceSynthesizerSetupMidiPanpot", "sceSynthesizerSetupNewNoise", "sceSynthesizerSetupReleaseEnv", "sceSynthesizerSetupTruncateTvaEnv",
        "sceSynthesizerSetupTruncateTvfPitchEnv", "sceSynthesizerSetuptEnv", "sceSynthesizerTonegenerator", "sceSynthesizerTransposeMatrix", "sceSynthesizerTvfProcI", "sceSynthesizerTvfProcNI",
        "sceSynthesizerWaitDmaFromSPR", "sceSynthesizerWaitDmaToSPR", "sceSynthsizerGetDrumPatch", "sceSynthsizerGetMeloPatch", "sceSynthsizerLfoNoise", "sceTtyHandler",
        "sceTtyInit", "sceTtyRead", "sceTtyWrite", "sceVpu0Reset", "sceVu0AddVector", "sceVu0ApplyMatrix",
        "sceVu0CameraMatrix", "sceVu0ClampVector", "sceVu0ClipAll", "sceVu0ClipScreen", "sceVu0ClipScreen3", "sceVu0CopyMatrix",
        "sceVu0CopyVector", "sceVu0CopyVectorXYZ", "sceVu0DivVector", "sceVu0DivVectorXYZ", "sceVu0DropShadowMatrix", "sceVu0FTOI0Vector",
        "sceVu0FTOI4Vector", "sceVu0ITOF0Vector", "sceVu0ITOF12Vector", "sceVu0ITOF4Vector", "sceVu0InnerProduct", "sceVu0InterVector",
        "sceVu0InterVectorXYZ", "sceVu0InversMatrix", "sceVu0LightColorMatrix", "sceVu0MulMatrix", "sceVu0MulVector", "sceVu0NormalLightMatrix",
        "sceVu0Normalize", "sceVu0OuterProduct", "sceVu0RotMatrix", "sceVu0RotMatrixX", "sceVu0RotMatrixY", "sceVu0RotMatrixZ",
        "sceVu0RotTransPers", "sceVu0RotTransPersN", "sceVu0ScaleVector", "sceVu0ScaleVectorXYZ", "sceVu0SubVector", "sceVu0TransMatrix",
        "sceVu0TransposeMatrix", "sceVu0UnitMatrix", "sceVu0ViewScreenMatrix", "sceWrite", "InitTLB"
    ));

    private static final Set<String> PS2_API_PREFIXES = new HashSet<>(Arrays.asList(
        "sce", "Sce", "SCE",
        "sif", "Sif", "SIF",
        "gs", "Gs", "GS",
        "dma", "Dma", "DMA",
        "iop", "Iop", "IOP",
        "vif", "Vif", "VIF",
        "spu", "Spu", "SPU",
        "mc", "Mc", "MC",
        "libc", "Libc", "LIBC"
    ));

    private static final Set<String> KNOWN_STDLIB_NAMES = new HashSet<>(Arrays.asList(
        "printf", "sprintf", "snprintf", "fprintf", "vprintf", "vfprintf", "vsprintf", "vsnprintf",
        "puts", "putchar", "getchar", "gets", "fgets",
        "__mcmp", "__sbprintf", "__sprint", "__sprint_r",
        "malloc", "free", "calloc", "realloc", "memalign", "memclr", "aligned_alloc", "posix_memalign",
        "memcpy", "memset", "memmove", "memcmp", "memchr", "bcopy", "bzero",
        "strcpy", "strncpy", "strcat", "strncat", "strcmp", "strncmp", "strlen", "strstr",
        "strchr", "strrchr", "strdup", "strtok", "strtok_r", "strerror",
        "fopen", "fclose", "fread", "fwrite", "fseek", "ftell", "rewind", "fflush",
        "fgetc", "feof", "ferror", "clearerr", "fileno", "tmpfile", "remove", "rename",
        "open", "close", "read", "write", "lseek", "stat", "fstat",
        "atoi", "atol", "atoll", "atof", "strtol", "strtoul", "strtoll", "strtoull", "strtod", "strtof",
        "rand", "srand", "random", "srandom", "drand48", "sqrt", "pow", "exp", "log", "log10",
        "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sinh", "cosh", "tanh",
        "floor", "ceil", "fabs", "fmod", "frexp", "ldexp", "modf",
        "time", "ctime", "clock", "difftime", "mktime", "localtime", "gmtime", "asctime", "strftime",
        "gettimeofday", "nanosleep", "usleep",
        "atexit", "system", "getpid", "fork", "waitpid",
        "qsort", "bsearch", "abs", "div", "labs", "ldiv", "llabs", "lldiv",
        "isalnum", "isalpha", "isdigit", "islower", "isupper", "isspace", "tolower", "toupper",
        "setjmp", "longjmp", "getenv", "setenv", "unsetenv",
        "perror", "fputc", "getc", "ungetc", "freopen", "setvbuf", "setbuf",
        "strnlen", "strspn", "strcspn", "strcasecmp", "strncasecmp"
    ));

    private static final Pattern C_LIB_PATTERN = Pattern.compile(
        "^_*(mem|str|time|f?printf|f?scanf|malloc|free|calloc|realloc|atoi|itoa|rand|srand|abort|exit|atexit|getenv|system|bsearch|qsort|abs|labs|div|ldiv|mblen|mbtowc|wctomb|mbstowcs|wcstombs).*"
    );

    private static final Pattern KERNEL_RUNTIME_NAME_PATTERN = Pattern.compile(
        "^(?:"
            + "(?:Create|Delete|Start|ExitDelete|Exit|Terminate|Suspend|Resume|Sleep|Wakeup|CancelWakeup|Change|Rotate|Release|Setup|Register|Query|Get|Set|Refer|Poll|Wait|Signal|Enable|Disable|Flush|Reset|Add|Init)"
            + "(?:Thread|Sema|EventFlag|Alarm|Intc|IntcHandler2|Dmac|DmacHandler2|OsdConfigParam|MemorySize|VSyncFlag|Heap|TLS|Status|Cache|Syscall|TLB|TLBEntry|GsCrt)"
            + "|EndOfHeap"
            + "|GsGetIMR|GsPutIMR"
            + "|Deci2Call"
            + "|Sif[A-Za-z0-9_]+"
            + "|i(?:SignalSema|PollSema|ReferSemaStatus|SetEventFlag|ClearEventFlag|PollEventFlag|ReferEventFlagStatus|WakeupThread|CancelWakeupThread|ReleaseWaitThread|SetAlarm|CancelAlarm|FlushCache|sceSifSetDma|sceSifSetDChain)"
        + ")$"
    );

    private static final class FunctionRecord {
        String name;
        long start;
        long endExclusive;
        long size;
        boolean syntheticEntry = false;
    }

    private static final class AddressTakenCandidate {
        long sourceOffset;
        long target;

        AddressTakenCandidate(long sourceOffset, long target) {
            this.sourceOffset = sourceOffset;
            this.target = target;
        }
    }

    private enum ClassificationKind {
        STUB,
        UNTRACKED_STUB,
        NONE
    }

    private static final class ClassificationResult {
        final ClassificationKind kind;
        final String name;

        ClassificationResult(ClassificationKind kind, String name) {
            this.kind = kind;
            this.name = name;
        }
    }

    private static String hex(long value) {
        return String.format("0x%08X", value & UINT32_MASK);
    }

    private static int opcode(long raw) {
        return (int) ((raw >>> OPCODE_SHIFT) & OPCODE_MASK);
    }

    private static int rs(long raw) {
        return (int) ((raw >>> RS_SHIFT) & REGISTER_MASK);
    }

    private static int rt(long raw) {
        return (int) ((raw >>> RT_SHIFT) & REGISTER_MASK);
    }

    private static int rd(long raw) {
        return (int) ((raw >>> RD_SHIFT) & REGISTER_MASK);
    }

    private static int function(long raw) {
        return (int) (raw & OPCODE_MASK);
    }

    private static int immediate(long raw) {
        return (int) (raw & IMMEDIATE_MASK);
    }

    private static String tomlString(String value) {
        if (value == null) {
            return "\"\"";
        }
        return "\"" + value.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
    }

 // Ghidra on Windows can return executable paths like "/D:/path/to/elf"
// or "/D/path/to/elf". Normalize both forms.
private static String normalizeWindowsDrivePath(String value) {
    if (value == null || value.length() < 3) {
        return value;
    }

    // "/D:/path" -> "D:/path"
    if (value.length() >= 4 &&
        value.charAt(0) == '/' &&
        Character.isLetter(value.charAt(1)) &&
        value.charAt(2) == ':' &&
        (value.charAt(3) == '/' || value.charAt(3) == '\\')) {
        return value.substring(1);
    }

    // "/D/path" -> "D:/path"
    if (value.charAt(0) == '/' &&
        Character.isLetter(value.charAt(1)) &&
        value.charAt(2) == '/') {
        return value.charAt(1) + ":" + value.substring(2);
    }

    return value;
}

    private static String normalizeOptionalLeadingUnderscore(String value) {
        if (value == null || value.isEmpty()) {
            return "";
        }
        return value.startsWith("_") && value.length() > 1 ? value.substring(1) : value;
    }

    private static String resolveRuntimeHandlerName(String name) {
        if (name == null || name.isEmpty()) {
            return "";
        }

        if (RUNTIME_HANDLER_NAMES.contains(name)) {
            return name;
        }

        String normalized = normalizeOptionalLeadingUnderscore(name);
        if (!normalized.equals(name) && RUNTIME_HANDLER_NAMES.contains(normalized)) {
            return normalized;
        }

        String underscored = "_" + name;
        if (!name.startsWith("_") && RUNTIME_HANDLER_NAMES.contains(underscored)) {
            return underscored;
        }

        return "";
    }

    private static boolean hasRuntimeHandler(String name) {
        return !resolveRuntimeHandlerName(name).isEmpty();
    }

    private static boolean hasReliableSymbolName(String name) {
        if (name == null || name.isEmpty()) {
            return false;
        }

        if (name.startsWith("sub_") || name.startsWith("FUN_") || name.startsWith("func_") ||
            name.startsWith("entry_") || name.startsWith("function_") || name.startsWith("LAB_")) {
            return false;
        }

        boolean hasAlpha = false;
        boolean allHexOrPrefix = true;
        for (int i = 0; i < name.length(); ++i) {
            char c = name.charAt(i);
            if (Character.isAlphabetic(c)) {
                hasAlpha = true;
            }
            if (!(Character.digit(c, 16) >= 0 || c == 'x' || c == 'X' || c == '_')) {
                allHexOrPrefix = false;
            }
        }

        if (!hasAlpha) {
            return false;
        }

        if ((name.startsWith("0x") || name.startsWith("0X")) && allHexOrPrefix) {
            return false;
        }

        return true;
    }

    private static boolean hasPs2ApiPrefix(String name) {
        if (name == null || name.isEmpty()) {
            return false;
        }

        String base = normalizeOptionalLeadingUnderscore(name);
        for (String prefix : PS2_API_PREFIXES) {
            if (!base.startsWith(prefix)) {
                continue;
            }

            if (base.length() == prefix.length()) {
                return true;
            }

            if (!Character.isLowerCase(base.charAt(prefix.length()))) {
                return true;
            }
        }

        return false;
    }

    private static boolean matchesWithOptionalLeadingUnderscoreAlias(String candidate, Set<String> names) {
        if (candidate == null || candidate.isEmpty() || names == null || names.isEmpty()) {
            return false;
        }

        if (names.contains(candidate)) {
            return true;
        }

        String normalized = normalizeOptionalLeadingUnderscore(candidate);
        if (!normalized.equals(candidate) && names.contains(normalized)) {
            return true;
        }

        if (!candidate.startsWith("_") && names.contains("_" + candidate)) {
            return true;
        }

        return false;
    }

    private static boolean isLibraryFunctionName(String name) {
        if (name == null || name.isEmpty() || !hasReliableSymbolName(name)) {
            return false;
        }

        if (hasRuntimeHandler(name)) {
            return true;
        }

        String normalized = normalizeOptionalLeadingUnderscore(name);
        if (hasRuntimeHandler(normalized)) {
            return true;
        }

        if (KERNEL_RUNTIME_NAME_PATTERN.matcher(normalized).matches()) {
            return true;
        }

        if (matchesWithOptionalLeadingUnderscoreAlias(normalized, KNOWN_STDLIB_NAMES)) {
            return true;
        }

        if (hasPs2ApiPrefix(name)) {
            return true;
        }

        return C_LIB_PATTERN.matcher(normalized).matches();
    }

    private static ClassificationResult classifyFunction(Function function) {
        if (function == null) {
            return new ClassificationResult(ClassificationKind.NONE, "");
        }

        String name = function.getName();
        if (name == null || name.isEmpty()) {
            return new ClassificationResult(ClassificationKind.NONE, "");
        }

        String runtimeName = resolveRuntimeHandlerName(name);
        if (!runtimeName.isEmpty()) {
            return new ClassificationResult(ClassificationKind.STUB, runtimeName);
        }

        if (function.isThunk()) {
            Function target = function.getThunkedFunction(true);
            if (target != null) {
                String targetName = target.getName();
                String targetRuntimeName = resolveRuntimeHandlerName(targetName);
                if (!targetRuntimeName.isEmpty()) {
                    return new ClassificationResult(ClassificationKind.STUB, targetRuntimeName);
                }

                if (isLibraryFunctionName(targetName)) {
                    return new ClassificationResult(ClassificationKind.UNTRACKED_STUB, targetName);
                }
            }
        }

        if (isLibraryFunctionName(name)) {
            return new ClassificationResult(ClassificationKind.UNTRACKED_STUB, name);
        }

        return new ClassificationResult(ClassificationKind.NONE, name);
    }

    private static String makeSelector(String name, long start, boolean includeAddress) {
        if (includeAddress) {
            return name + "@" + hex(start);
        }
        return name;
    }

    private boolean isExecutableAddress(Address address) {
        if (address == null) {
            return false;
        }

        MemoryBlock block = currentProgram.getMemory().getBlock(address);
        return block != null && block.isExecute();
    }

    private boolean hasCallableLabelReference(Address address) {
        if (address == null) {
            return false;
        }

        ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(address);
        while (refs.hasNext()) {
            Reference ref = refs.next();
            if (ref == null) {
                continue;
            }

            RefType type = ref.getReferenceType();
            if (type != null && type.isCall()) {
                return true;
            }

            Address from = ref.getFromAddress();
            if (from == null) {
                continue;
            }

            MemoryBlock fromBlock = currentProgram.getMemory().getBlock(from);
            if (fromBlock != null && fromBlock.isExecute()) {
                return true;
            }
        }

        return false;
    }
private boolean isCrossRangeBranchTarget(
        Function func,
        Address source,
        Address target) {

    if (func == null || source == null || target == null) {
        return false;
    }

    AddressSetView body = func.getBody();

    if (body == null ||
        !body.contains(source) ||
        !body.contains(target)) {
        return false;
    }

    AddressRange sourceRange = null;
    AddressRange targetRange = null;

    AddressRangeIterator ranges = body.getAddressRanges();

    while (ranges.hasNext()) {
        AddressRange range = ranges.next();

        if (range.contains(source)) {
            sourceRange = range;
        }

        if (range.contains(target)) {
            targetRange = range;
        }
    }

    return sourceRange != null &&
           targetRange != null &&
           !sourceRange.equals(targetRange);
}
private void collectCrossRangeBranchEntries(
        List<FunctionRecord> records,
        Set<Long> existingStarts) {

    FunctionManager fm = currentProgram.getFunctionManager();
    FunctionIterator functions = fm.getFunctions(true);

    while (functions.hasNext() && !monitor.isCancelled()) {
        Function func = functions.next();
        AddressSetView body = func.getBody();
		if (func.getEntryPoint().getOffset() == 0x00193EF0L) {
    println("DEBUG FOUND 193EF0 name=" + func.getName() +
        " ranges=" + (body != null ? body.getNumAddressRanges() : -1));
}

        if (body == null || body.getNumAddressRanges() <= 1) {
            continue;
        }

        InstructionIterator instructions =
            currentProgram.getListing().getInstructions(body, true);

        while (instructions.hasNext() && !monitor.isCancelled()) {
            Instruction instruction = instructions.next();
		if (instruction.getAddress().getOffset() == 0x00194500L) {
			println("DEBUG 194500 refs=" + instruction.getNumOperands());

			for (Reference debugRef : instruction.getReferencesFrom()) {
				println(
					"  ref target=" + debugRef.getToAddress() +
					" type=" + debugRef.getReferenceType()
				);
			}
		}
            for (Reference ref : instruction.getReferencesFrom()) {
                if (ref == null) {
                    continue;
                }

                RefType type = ref.getReferenceType();
                if (type == null || (!type.isJump() && !type.isCall())) {
                    continue;
                }

                Address target = ref.getToAddress();
				if (instruction.getAddress().getOffset() == 0x00194500L) {
    println(String.format(
        "DEBUG 194500 target=0x%08X type=%s",
        target != null ? target.getOffset() : 0L,
        ref.getReferenceType()
    ));
}

                if (!isCrossRangeBranchTarget(
        func,
        instruction.getAddress(),
        target)) {
    continue;
}

                long targetOffset = target.getOffset();

                if (!existingStarts.add(targetOffset)) {
                    continue;
                }

                FunctionRecord record = new FunctionRecord();
                record.name = makeAnonymousEntryName(targetOffset);
                record.start = targetOffset;
                record.syntheticEntry = true;
                records.add(record);

                println(String.format(
                    "CROSS RANGE %s: 0x%08X -> 0x%08X",
                    func.getName(),
                    instruction.getAddress().getOffset(),
                    targetOffset
                ));
            }
        }
    }
}
    private static String makeAnonymousEntryName(long start) {
        return String.format("entry_%08x", start & UINT32_MASK);
    }

    private Long readWord(Address address) {
        if (address == null) {
            return null;
        }

        try {
            return ((long) currentProgram.getMemory().getInt(address)) & UINT32_MASK;
        } catch (Exception ignored) {
            return null;
        }
    }

    private Address addressFromOffset(long offset) {
        try {
            return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(offset & UINT32_MASK);
        } catch (Exception ignored) {
            return null;
        }
    }

    private boolean looksLikeCallableEntry(long target, boolean allowLeafThunk) {
        if ((target % MIPS_INSTRUCTION_SIZE) != 0L) {
            return false;
        }

        Address address = addressFromOffset(target);
        if (!isExecutableAddress(address) || currentProgram.getListing().getInstructionAt(address) == null) {
            return false;
        }

        for (int index = 0; index < 16; ++index) {
            Address probe;
            try {
                probe = address.add(index * (long) MIPS_INSTRUCTION_SIZE);
            } catch (Exception ignored) {
                break;
            }

            Long rawValue = readWord(probe);
            if (rawValue == null) {
                break;
            }

            long raw = rawValue;
            int opcode = opcode(raw);
            int rs = rs(raw);
            int rt = rt(raw);
            int immediate = immediate(raw);

            if (index < 4 && (opcode == OPCODE_ADDIU || opcode == OPCODE_DADDIU) && rs == GPR_SP && rt == GPR_SP && (immediate & MIPS_IMMEDIATE_SIGN_BIT) != 0) {
                return true;
            }

            if ((opcode == OPCODE_SW || opcode == OPCODE_SD || opcode == OPCODE_SQ) &&
                rs == GPR_SP && rt == GPR_RA) {
                return true;
            }

            if (allowLeafThunk && opcode == OPCODE_SPECIAL &&
                function(raw) == SPECIAL_JR && rs == GPR_RA) {
                return true;
            }
            if (allowLeafThunk && opcode == OPCODE_J) {
                return true;
}
        }

        return false;
    }

    private static boolean writesGpr(long raw, int register) {
        if (register == GPR_ZERO) {
            return false;
        }

        int opcode = opcode(raw);
        int rt = rt(raw);
        int rd = rd(raw);

        if (opcode == OPCODE_SPECIAL || opcode == OPCODE_MMI) {
            return rd == register;
        }
        if (opcode == OPCODE_JAL) {
            return register == GPR_RA;
        }

        boolean writesRt;
        switch (opcode) {
        case OPCODE_ADDI:
        case OPCODE_ADDIU:
        case OPCODE_SLTI:
        case OPCODE_SLTIU:
        case OPCODE_ANDI:
        case OPCODE_ORI:
        case OPCODE_XORI:
        case OPCODE_LUI:
        case OPCODE_DADDI:
        case OPCODE_DADDIU:
        case OPCODE_LDL:
        case OPCODE_LDR:
        case OPCODE_LQ:
        case OPCODE_LB:
        case OPCODE_LH:
        case OPCODE_LWL:
        case OPCODE_LW:
        case OPCODE_LBU:
        case OPCODE_LHU:
        case OPCODE_LWR:
        case OPCODE_LWU:
        case OPCODE_LL:
        case OPCODE_LLD:
        case OPCODE_LD:
            writesRt = true;
            break;
        default:
            writesRt = false;
            break;
        }
        return writesRt && rt == register;
    }

    private static boolean isControlTransfer(long raw) {
        int opcode = opcode(raw);
        switch (opcode) {
        case OPCODE_REGIMM:
        case OPCODE_J:
        case OPCODE_JAL:
        case OPCODE_BEQ:
        case OPCODE_BNE:
        case OPCODE_BLEZ:
        case OPCODE_BGTZ:
        case OPCODE_BEQL:
        case OPCODE_BNEL:
        case OPCODE_BLEZL:
        case OPCODE_BGTZL:
            return true;
        default:
            break;
        }

        if (opcode != OPCODE_SPECIAL) {
            return false;
        }

        int function = function(raw);
        return function == SPECIAL_JR || function == SPECIAL_JALR;
    }

    private static boolean isCallInstruction(long raw) {
        int opcode = opcode(raw);
        return opcode == OPCODE_JAL ||
               (opcode == OPCODE_SPECIAL && function(raw) == SPECIAL_JALR);
    }
private void addSyntheticEntry(List<FunctionRecord> records, Set<Long> existingStarts, long target) {
    target &= UINT32_MASK;

    Address address = currentProgram.getAddressFactory()
    .getDefaultAddressSpace()
    .getAddress(target);
    if (address == null) {
        return;
    }

    Function containingFunction =
        currentProgram.getFunctionManager().getFunctionContaining(address);

    if (containingFunction != null &&
        containingFunction.getEntryPoint().getOffset() != target) {
        return;
    }

    if (!existingStarts.add(target)) {
        return;
    }

    FunctionRecord record = new FunctionRecord();
    record.name = makeAnonymousEntryName(target);
    record.start = target;
    record.syntheticEntry = true;
    records.add(record);
}
    private void collectMaterializedCodeEntries(List<FunctionRecord> records, Set<Long> existingStarts) {
         AddressSet executableAddresses = new AddressSet();
        for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
            if (block != null && block.isExecute()) {
                executableAddresses.addRange(block.getStart(), block.getEnd());
            }
        }

        InstructionIterator instructions = currentProgram.getListing().getInstructions(executableAddresses, true);
        while (instructions.hasNext() && !monitor.isCancelled()) {
            Instruction instruction = instructions.next();
            if (instruction == null) {
                continue;
            }

            Long upperRawValue = readWord(instruction.getAddress());
            if (upperRawValue == null) {
                continue;
            }

            long upperRaw = upperRawValue;
            if (opcode(upperRaw) != OPCODE_LUI) {
                continue;
            }

            int upperRegister = rt(upperRaw);
            if (upperRegister == GPR_ZERO) {
                continue;
            }

            long upperValue = ((long) immediate(upperRaw)) << MIPS_IMMEDIATE_BITS;
            boolean sawControlTransfer = false;
            boolean sawCallTransfer = false;

            for (int lookahead = 1; lookahead <= 12; ++lookahead) {
                Address lowAddress;
                try {
                    lowAddress = instruction.getAddress().add(lookahead * (long) MIPS_INSTRUCTION_SIZE);
                } catch (Exception ignored) {
                    break;
                }

                Long lowRawValue = readWord(lowAddress);
                if (lowRawValue == null) {
                    break;
                }

                long lowRaw = lowRawValue;
                int opcode = opcode(lowRaw);
                int rs = rs(lowRaw);
                int rt = rt(lowRaw);

                if ((opcode == OPCODE_ADDIU || opcode == OPCODE_ORI || opcode == OPCODE_DADDIU) &&
                    rs == upperRegister) {
                    int immediate = immediate(lowRaw);
                    long target;
                    if (opcode == OPCODE_ORI) {
                        target = upperValue | immediate;
                    } else {
                        target = (upperValue + (short) immediate) & UINT32_MASK;
                    }

                    Long nextRaw = readWord(addressFromOffset(
                        lowAddress.getOffset() + MIPS_INSTRUCTION_SIZE));
                    boolean followedByCall = nextRaw != null && isCallInstruction(nextRaw);
                    boolean materializedAsCallArgument =
                        rt >= GPR_A0 && rt <= GPR_A3 && (sawCallTransfer || followedByCall);

                    if (lookahead <= 4 && looksLikeCallableEntry(target, true)) {
                        addSyntheticEntry(records, existingStarts, target);
                    } else {
                        materializedRawCandidates.add(target);
                    }
                    break;
                }

                if (sawControlTransfer) {
                    break;
                }
                if (writesGpr(lowRaw, upperRegister)) {
                    break;
                }
                if (isControlTransfer(lowRaw)) {
                    sawControlTransfer = true;
                    sawCallTransfer = isCallInstruction(lowRaw);
                }
            }
        }
    }

    private static boolean isDedicatedFunctionPointerBlock(String name) {
        return ".ctors".equals(name) || ".dtors".equals(name) ||
               ".init_array".equals(name) || ".fini_array".equals(name);
    }


// BEGIN PURE POINTER POLICY
/** Pure discovery policy; embedded verbatim as a nested class in the exporter. */
static final class PointerTablePolicy {
    enum Kind { NEXT, BRANCH, JUMP, RETURN, INVALID }
    record Instruction(Kind kind, long next, long target, boolean delay) {}
    interface View {
        // null means unaligned, non-executable, undecoded, delay-slot entry,
        // unsupported instruction, or incompatible ordinary-function membership.
        Instruction at(long pc);
        boolean validDelay(long pc);
        boolean boundary(long pc);
        boolean trustedEntry(long pc);
    }
    record Result(boolean accepted, String reason, int visited) {}
    record Candidate(long source, long target, boolean shortCallable, boolean typedPointer) {}
    static boolean eligibleInstruction(long entry,long pc,boolean executable,boolean decoded,boolean delaySlot,Long containingEntry) {
        return (pc&3)==0 && executable && decoded && !delaySlot &&
            (containingEntry==null || containingEntry==entry);
    }

    static boolean hasTableEvidence(java.util.List<Candidate> candidates, int index) {
        Candidate c=candidates.get(index);
        if (!c.typedPointer) return false;
        java.util.Set<Long> anchors=new java.util.HashSet<>();
        int members=1;
        // Require a contiguous run of typed pointer slots, with two independently
        // conservative callable anchors, each within the existing 32-byte context.
        // This is structural evidence, not a claim of interprocedural call proof.
        for (int direction : new int[]{-1,1}) {
            long previous=c.source;
            for(int j=index+direction;j>=0 && j<candidates.size();j+=direction) {
                Candidate n=candidates.get(j);
                if(Math.abs(n.source-previous)!=4 || Math.abs(n.source-c.source)>32 || !n.typedPointer) break;
                members++;
                if(n.shortCallable) anchors.add(n.target);
                previous=n.source;
            }
        }
        return members>=3 && anchors.size()>=2;
    }

    static Result validate(long entry, View view) {
        java.util.ArrayDeque<Long> queue=new java.util.ArrayDeque<>();
        java.util.Set<Long> seen=new java.util.HashSet<>();
        queue.add(entry);
        boolean terminal=false;
        while(!queue.isEmpty()) {
            long pc=queue.removeFirst();
            if(!seen.add(pc)) continue;
            // Resource protection fails closed; this is not a window in which
            // finding any return certifies unrelated linear bytes as a function.
            if(seen.size()>4096) return new Result(false,"flow-resource-limit",seen.size());
            if(pc!=entry && view.boundary(pc)) return new Result(false,"other-entry-boundary",seen.size());
            Instruction i=view.at(pc);
            if(i==null || i.kind==Kind.INVALID) return new Result(false,"invalid-or-unsupported-code",seen.size());
            if(i.delay && !view.validDelay(pc+4)) return new Result(false,"invalid-delay-slot",seen.size());
            switch(i.kind) {
                case RETURN -> terminal=true;
                case NEXT -> queue.add(i.next);
                case BRANCH -> { queue.add(i.target); queue.add(i.next); }
                case JUMP -> {
                    if(i.target!=entry && view.trustedEntry(i.target)) terminal=true;
                    else queue.add(i.target);
                }
                default -> { return new Result(false,"unsupported-flow",seen.size()); }
            }
        }
        return new Result(terminal,terminal?"validated-reachable-flow":"no-proven-exit",seen.size());
    }

    // ---- Undecoded leaf routines (T-0006) -------------------------------------------------
    // Ghidra's auto-analysis can leave a small routine undisassembled when nothing calls it
    // directly. A function pointer stored in data is then the only evidence of its existence.
    // This path never trusts a lone pointer-shaped word: the target must start an undecoded
    // executable block, be a strict whitelisted leaf with a proven return, and the pointer must
    // live in data (no decoded code nearby). Decoding is local and conservative: anything
    // outside the whitelist rejects the candidate.
    interface RawView {
        // Raw big-picture word at pc, or null if unreadable.
        Long word(long pc);
        // True only for an aligned, executable address with no Ghidra instruction, function or
        // defined data covering it and not a known/trusted entry boundary.
        boolean undecoded(long pc);
        // True for the start of code that really exists (an exported/known entry or an address inside
        // a Ghidra Function): the only acceptable destination of a tail call out of undecoded bytes.
        boolean knownCode(long pc);
    }
    static final int RAW_NODE_LIMIT = 256;
    static final int RAW_PADDING_LIMIT = 64;
    static final long NOP = 0L;
    static final long JR_RA = 0x03E00008L;

    static int rawOpcode(long w) { return (int) ((w >>> 26) & 0x3F); }
    static int rawRs(long w) { return (int) ((w >>> 21) & 0x1F); }
    static int rawRt(long w) { return (int) ((w >>> 16) & 0x1F); }
    static int rawRd(long w) { return (int) ((w >>> 11) & 0x1F); }
    static int rawFunct(long w) { return (int) (w & 0x3F); }
    static boolean protectedReg(int r) { return r == 29 || r == 31; } // sp, ra

    /** Simple straight-line instruction: no control flow, no sp/ra writes, no coprocessor/MMI. */
    static boolean simpleInstruction(long w) {
        int op = rawOpcode(w);
        switch (op) {
            case 0: { // SPECIAL
                int f = rawFunct(w);
                switch (f) {
                    case 0x00: case 0x02: case 0x03: case 0x04: case 0x06: case 0x07: // shifts
                    case 0x0A: case 0x0B: // movz movn
                    case 0x10: case 0x12: // mfhi mflo
                    case 0x20: case 0x21: case 0x22: case 0x23: // add addu sub subu
                    case 0x24: case 0x25: case 0x26: case 0x27: // and or xor nor
                    case 0x2A: case 0x2B: // slt sltu
                    case 0x2C: case 0x2D: case 0x2E: case 0x2F: // dadd daddu dsub dsubu
                    case 0x38: case 0x3A: case 0x3B: // dsll dsrl dsra
                    case 0x3C: case 0x3E: case 0x3F: // dsll32 dsrl32 dsra32
                        return !protectedReg(rawRd(w));
                    default: return false;
                }
            }
            case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: // addi..xori
            case 0x18: case 0x19: // daddi daddiu
                return !protectedReg(rawRt(w));
            case 0x0F: // lui
                return !protectedReg(rawRt(w));
            case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: case 0x27: case 0x37: // lb lh lw lbu lhu lwu ld
                return !protectedReg(rawRt(w));
            case 0x28: case 0x29: case 0x2B: case 0x3F: // sb sh sw sd
                return true;
            default: return false;
        }
    }

    /** Conditional branch, including branch-likely forms (the delay slot is annulled when not taken, so both
     *  successors are pc+8 and the target; the slot itself is still validated). Returns the signed word offset or null. */
    static Long branchOffset(long w) {
        int op = rawOpcode(w);
        boolean branch = op == 0x04 || op == 0x05 || op == 0x06 || op == 0x07 ||
            op == 0x14 || op == 0x15 || op == 0x16 || op == 0x17 ||
            (op == 0x01 && (rawRt(w) >= 0x00 && rawRt(w) <= 0x03));
        if (!branch) return null;
        return (long) (short) (w & 0xFFFF);
    }

    /** Entry must start an undecoded block: only zero padding (or a decoded boundary) before it. */
    static boolean startsUndecodedBlock(long entry, RawView v) {
        Long first = v.word(entry);
        if (first == null || first == NOP || !v.undecoded(entry)) return false;
        long pc = entry - 4;
        for (int i = 0; i < RAW_PADDING_LIMIT; i++, pc -= 4) {
            if (pc < 0) return false;
            if (!v.undecoded(pc)) return true; // decoded unit (or boundary) ends right before the padding
            Long w = v.word(pc);
            if (w != null && w != NOP) {
                // A return and its delay slot end the previous (also undecoded) routine: the padding
                // that follows is a boundary exactly as if a decoded unit had ended there.
                Long before = v.word(pc - 4);
                if (before != null && before == 0x03E00008L && v.undecoded(pc - 4)) return true;
            }
            if (w == null || w != NOP) return false; // undecoded non-zero word: entry is interior
        }
        return false;
    }

    static Result validateRawLeaf(long entry, RawView v) {
        if ((entry & 3) != 0) return new Result(false, "unaligned", 0);
        if (!startsUndecodedBlock(entry, v)) return new Result(false, "not-block-start", 0);
        java.util.ArrayDeque<Long> queue = new java.util.ArrayDeque<>();
        java.util.Set<Long> seen = new java.util.HashSet<>();
        queue.add(entry);
        boolean terminal = false;
        while (!queue.isEmpty()) {
            long pc = queue.removeFirst();
            if (!seen.add(pc)) continue;
            if (seen.size() > RAW_NODE_LIMIT) return new Result(false, "raw-flow-limit", seen.size());
            Long wv = v.word(pc);
            if (wv == null || !v.undecoded(pc)) return new Result(false, "leaves-undecoded-block", seen.size());
            long w = wv;
            if (w == NOP) { queue.add(pc + 4); continue; }
            if (w == JR_RA) {
                Long d = v.word(pc + 4);
                if (d == null || !v.undecoded(pc + 4) || !(d == NOP || simpleInstruction(d)))
                    return new Result(false, "invalid-delay-slot", seen.size());
                terminal = true;
                continue;
            }
            if (rawOpcode(w) == 0x02) { // j: a jump inside the block continues there, otherwise a tail call
                Long d = v.word(pc + 4);
                if (d == null || !v.undecoded(pc + 4) || !(d == NOP || simpleInstruction(d)))
                    return new Result(false, "invalid-delay-slot", seen.size());
                long jumpTarget = (((pc + 4) & 0xF0000000L) | ((w & 0x03FFFFFFL) << 2)) & 0xFFFFFFFFL;
                if (jumpTarget >= entry && v.undecoded(jumpTarget)) queue.add(jumpTarget);
                else if (v.knownCode(jumpTarget)) terminal = true;
                else return new Result(false, "tail-call-to-unknown-code", seen.size());
                continue;
            }
            Long off = branchOffset(w);
            if (off != null) {
                Long d = v.word(pc + 4);
                if (d == null || !v.undecoded(pc + 4) || !(d == NOP || simpleInstruction(d)))
                    return new Result(false, "invalid-delay-slot", seen.size());
                long target = pc + 4 + (off << 2);
                if (target < entry) return new Result(false, "backward-exit", seen.size());
                queue.add(target);
                queue.add(pc + 8);
                continue;
            }
            if (!simpleInstruction(w)) return new Result(false, "invalid-or-unsupported-code", seen.size());
            queue.add(pc + 4);
        }
        return new Result(terminal, terminal ? "validated-raw-leaf" : "no-proven-exit", seen.size());
    }

    // ---- Undecoded fragments reached by direct control flow (T-0006, second increment) -------
    // A branch or jump in decoded code that lands on executable bytes Ghidra never disassembled
    // proves those bytes are code. They need their own entry so the dispatcher can resume there.
    // The chunk must decode as a plain R5900 integer/FPU instruction stream (known opcodes only),
    // end at a terminal control transfer (with its delay slot) or at the next decoded unit, and
    // never leave a delay slot dangling across a unit boundary.
    static final int FRAGMENT_WORD_LIMIT = 512;

    static final java.util.Set<Integer> SPECIAL_FUNCTIONS = java.util.Set.of(
        0x00, 0x02, 0x03, 0x04, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0F,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
        0x30, 0x31, 0x32, 0x33, 0x34, 0x36, 0x38, 0x3A, 0x3B, 0x3C, 0x3E, 0x3F);

    static boolean knownFragmentOpcode(long w) {
        int op = rawOpcode(w);
        switch (op) {
            case 0x00: { // SPECIAL: R5900 function codes only
                int f = rawFunct(w);
                return SPECIAL_FUNCTIONS.contains(f);
            }
            case 0x01: { // REGIMM
                int rt = rawRt(w);
                return rt <= 3 || (rt >= 0x08 && rt <= 0x0C) || rt == 0x0E || (rt >= 0x10 && rt <= 0x13) || rt == 0x18 || rt == 0x19;
            }
            case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
            case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
            case 0x10: case 0x11: case 0x12:
            case 0x14: case 0x15: case 0x16: case 0x17:
            case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1E: case 0x1F:
            case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
            case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
            case 0x31: case 0x36: case 0x37: case 0x39: case 0x3E: case 0x3F:
                return true;
            default:
                return false;
        }
    }

    /** Direct branch/jump/call target of a raw instruction, or -1 when it has none. */
    static long rawDirectTarget(long pc, long w) {
        int op = rawOpcode(w);
        boolean branch = op == 0x04 || op == 0x05 || op == 0x06 || op == 0x07 ||
            op == 0x14 || op == 0x15 || op == 0x16 || op == 0x17 || op == 0x01 && (rawRt(w) <= 3 || rawRt(w) >= 0x10 && rawRt(w) <= 0x13);
        if (branch) return (pc + 4 + ((long) (short) (w & 0xFFFF) << 2)) & 0xFFFFFFFFL;
        if (op == 0x02 || op == 0x03) return (((pc + 4) & 0xF0000000L) | ((w & 0x03FFFFFFL) << 2)) & 0xFFFFFFFFL;
        return -1;
    }

    static boolean controlTransfer(long w) {
        int op = rawOpcode(w);
        if (op == 0x00) return rawFunct(w) == 0x08 || rawFunct(w) == 0x09;
        return rawDirectTarget(0, w) != -1 || op == 0x01 && rawRt(w) <= 0x13;
    }

    /** True for transfers after which execution never falls through (j, jr, b/beq r,r). */
    static boolean terminalTransfer(long w) {
        int op = rawOpcode(w);
        if (op == 0x02) return true;
        if (op == 0x00) return rawFunct(w) == 0x08;
        if (op == 0x04) return rawRs(w) == rawRt(w);
        if (op == 0x01) return rawRs(w) == 0 && rawRt(w) == 1; // bgez zero == unconditional
        return false;
    }

    static Result validateRawFragment(long entry, RawView v) {
        if ((entry & 3) != 0) return new Result(false, "unaligned", 0);
        long pc = entry;
        int count = 0;
        while (count < FRAGMENT_WORD_LIMIT) {
            if (!v.undecoded(pc)) {
                // Reached the next decoded unit: a fall-through exit is fine unless a delay slot is pending.
                return count == 0 ? new Result(false, "empty", 0) : new Result(true, "validated-raw-fragment", count);
            }
            Long wv = v.word(pc);
            if (wv == null || !knownFragmentOpcode(wv)) return new Result(false, "invalid-or-unsupported-code", count);
            count++;
            long w = wv;
            if (controlTransfer(w)) {
                Long d = v.word(pc + 4);
                if (d == null || !v.undecoded(pc + 4) || !knownFragmentOpcode(d) || controlTransfer(d))
                    return new Result(false, "invalid-delay-slot", count);
                count++;
                if (terminalTransfer(w)) return new Result(true, "validated-raw-fragment", count);
                pc += 8;
                continue;
            }
            pc += 4;
        }
        return new Result(false, "fragment-limit", count);
    }

    static boolean hasPrologue(long entry, RawView v) {
        for (int i = 0; i < 4; i++) {
            Long w = v.word(entry + 4L * i);
            if (w == null) return false;
            int op = rawOpcode(w);
            if ((op == 0x09 || op == 0x19) && rawRs(w) == 29 && rawRt(w) == 29 && (w & 0x8000) != 0) return true;
            if ((op == 0x2B || op == 0x3F || op == 0x1F) && rawRs(w) == 29 && rawRt(w) == 31) return true;
        }
        return false;
    }

    /** Non-leaf routine in undecoded bytes: block start, standard prologue, known opcodes, proven jr ra. */
    static Result validateRawFunction(long entry, RawView v) {
        if ((entry & 3) != 0) return new Result(false, "unaligned", 0);
        if (!startsUndecodedBlock(entry, v)) return new Result(false, "not-block-start", 0);
        if (!hasPrologue(entry, v)) return new Result(false, "no-prologue", 0);
        java.util.ArrayDeque<Long> queue = new java.util.ArrayDeque<>();
        java.util.Set<Long> seen = new java.util.HashSet<>();
        queue.add(entry);
        boolean terminal = false;
        while (!queue.isEmpty()) {
            long pc = queue.removeFirst();
            if (!seen.add(pc)) continue;
            if (seen.size() > 4096) return new Result(false, "raw-flow-limit", seen.size());
            Long wv = v.word(pc);
            if (wv == null || !v.undecoded(pc)) return new Result(false, "leaves-undecoded-block", seen.size());
            long w = wv;
            if (!knownFragmentOpcode(w)) return new Result(false, "invalid-or-unsupported-code", seen.size());
            if (!controlTransfer(w)) { queue.add(pc + 4); continue; }
            Long d = v.word(pc + 4);
            if (d == null || !v.undecoded(pc + 4) || !knownFragmentOpcode(d) || controlTransfer(d))
                return new Result(false, "invalid-delay-slot", seen.size());
            int op = rawOpcode(w);
            if (op == 0x00) {
                if (rawFunct(w) == 0x08) {
                    if (rawRs(w) != 31) return new Result(false, "computed-jump", seen.size());
                    terminal = true; // jr ra
                } else queue.add(pc + 8); // jalr: call, execution resumes after the delay slot
                continue;
            }
            long target = rawDirectTarget(pc, w);
            if (op == 0x03) { queue.add(pc + 8); continue; } // jal
            if (op == 0x02) { // j: inside the block continues there, otherwise a tail call
                if (target >= entry && v.undecoded(target)) queue.add(target);
                else if (v.knownCode(target)) terminal = true;
                else return new Result(false, "tail-call-to-unknown-code", seen.size());
                continue;
            }
            // branches (including regimm / likely forms)
            if (target >= entry && v.undecoded(target)) queue.add(target);
            else if (target < entry && !v.undecoded(target)) { /* branch into decoded code: exit */ }
            else if (target < entry) return new Result(false, "backward-exit", seen.size());
            if (!terminalTransfer(w)) queue.add(pc + 8);
        }
        return new Result(terminal, terminal ? "validated-raw-function" : "no-proven-exit", seen.size());
    }
}

// END PURE POINTER POLICY
    private PointerTablePolicy.Result validateTableLeaf(long entry, Set<Long> boundaries, Set<Long> trusted) {
        return PointerTablePolicy.validate(entry, new PointerTablePolicy.View() {
            private Instruction decoded(long pc, boolean delay) {
                Address a=addressFromOffset(pc);
                Instruction i=a==null?null:currentProgram.getListing().getInstructionAt(a);
                Function f=a==null?null:currentProgram.getFunctionManager().getFunctionContaining(a);
                if(!PointerTablePolicy.eligibleInstruction(entry,pc,isExecutableAddress(a),i!=null && i.getLength()==4,
                    i!=null && i.isInDelaySlot()!=delay,f==null?null:f.getEntryPoint().getOffset())) return null;
                // This extension validates leaf routines. A call/RA/SP mutation
                // requires a separate ABI-aware non-leaf analysis, not a guessed return.
                for(Object result:i.getResultObjects()) {
                    if(result instanceof ghidra.program.model.lang.Register r &&
                       (r.getName().equalsIgnoreCase("ra") || r.getName().equalsIgnoreCase("sp"))) return null;
                }
                return i;
            }
            public PointerTablePolicy.Instruction at(long pc) {
                Instruction i=decoded(pc,false);
                if(i==null) return null;
                Long rawValue=readWord(i.getAddress());
                if(rawValue==null) return null;
                long raw=rawValue;
                var flow=i.getFlowType();
                boolean delay=i.getDelaySlotDepth()==1;
                if(opcode(raw)==OPCODE_SPECIAL && function(raw)==SPECIAL_JR && rs(raw)==GPR_RA && delay)
                    return new PointerTablePolicy.Instruction(PointerTablePolicy.Kind.RETURN,0,0,true);
                if(flow.isCall() || flow.isComputed() || flow.isTerminal() || i.getDelaySlotDepth()>1) return null;
                Address[] targets=i.getFlows();
                if(opcode(raw)==OPCODE_J && delay && targets.length==1)
                    return new PointerTablePolicy.Instruction(PointerTablePolicy.Kind.JUMP,0,targets[0].getOffset(),true);
                if(flow.isConditional() && flow.isJump() && delay && targets.length==1)
                    return new PointerTablePolicy.Instruction(PointerTablePolicy.Kind.BRANCH,pc+8,targets[0].getOffset(),true);
                if(flow.isFallthrough() && !delay && targets.length==0 && i.getFallThrough()!=null && i.getFallThrough().getOffset()==pc+4)
                    return new PointerTablePolicy.Instruction(PointerTablePolicy.Kind.NEXT,pc+4,0,false);
                return null;
            }
            public boolean validDelay(long pc) {
                Instruction i=decoded(pc,true);
                return i!=null && !boundary(pc) && i.getDelaySlotDepth()==0 && i.getFlowType().isFallthrough() && i.getFlows().length==0;
            }
            public boolean boundary(long pc) {
                return boundaries.contains(pc) || currentProgram.getFunctionManager().getFunctionAt(addressFromOffset(pc))!=null;
            }
            public boolean trustedEntry(long pc) {
                return trusted.contains(pc) && isExecutableAddress(addressFromOffset(pc)) &&
                    currentProgram.getListing().getInstructionAt(addressFromOffset(pc))!=null;
            }
        });
    }

    private void collectDataFunctionPointerEntries(List<FunctionRecord> records, Set<Long> existingStarts) {
        final long clusterDistanceBytes=32L;
        Set<Long> trusted=new HashSet<>(existingStarts);
        int added=0, rejected=0, rawAdded=0, rawTried=0;
        java.util.Map<String,Integer> rawReasons=new java.util.TreeMap<>();
        for(MemoryBlock block:currentProgram.getMemory().getBlocks()) {
            if(block==null || !block.isInitialized() || block.getSize()<4) continue;
            List<AddressTakenCandidate> shortCandidates=new ArrayList<>();
            List<PointerTablePolicy.Candidate> candidates=new ArrayList<>();
            List<long[]> undecodedCandidates=new ArrayList<>();
            Set<Long> boundaries=new HashSet<>(trusted);
            for(long offset=0;offset+4<=block.getSize() && !monitor.isCancelled();offset+=4) {
                Address source=block.getStart().add(offset);
                Long target=readWord(source);
                if(target==null || (target&3)!=0) continue;
                Address a=addressFromOffset(target);
                if(!isExecutableAddress(a)) continue;
                if(currentProgram.getListing().getInstructionAt(a)==null) {
                    // Executable bytes Ghidra left undecoded: only a routine validated by the raw path may
                    // become an entry, and only when the pointer itself lives in data (see below).
                    if(isDataOnlyNeighbourhood(source)) undecodedCandidates.add(new long[]{offset,target});
                    continue;
                }
                // Decoded but owned by no Function (orphan code): the clustered short-shape rule below may
                // still miss an isolated pointer, so offer it to the raw validators as well.
                if(currentProgram.getFunctionManager().getFunctionContaining(a)==null && isDataOnlyNeighbourhood(source))
                    undecodedCandidates.add(new long[]{offset,target});
                boolean shortCallable=looksLikeCallableEntry(target,true);
                if(shortCallable) shortCandidates.add(new AddressTakenCandidate(offset,target));
                var data=currentProgram.getListing().getDefinedDataAt(source);
                boolean typed=data!=null && data.isPointer() && data.getLength()==4 &&
                    data.getValue() instanceof Address pointer && pointer.equals(a) &&
                    currentProgram.getListing().getInstructionContaining(source)==null;
                candidates.add(new PointerTablePolicy.Candidate(offset,target,shortCallable,typed));
                if(typed) boundaries.add(target);
            }
            // Preserve the original short-shape candidate list and clustering.
            boolean dedicated=isDedicatedFunctionPointerBlock(block.getName());
            for(int i=0;i<shortCandidates.size();i++) {
                var c=shortCandidates.get(i);
                boolean clustered=dedicated || (i>0 && c.sourceOffset-shortCandidates.get(i-1).sourceOffset<=clusterDistanceBytes) ||
                    (i+1<shortCandidates.size() && shortCandidates.get(i+1).sourceOffset-c.sourceOffset<=clusterDistanceBytes);
                if(clustered) addSyntheticEntry(records,existingStarts,c.target);
            }
            for(int i=0;i<candidates.size();i++) {
                var c=candidates.get(i);
                if(c.shortCallable() || existingStarts.contains(c.target()) || !PointerTablePolicy.hasTableEvidence(candidates,i)) continue;
                PointerTablePolicy.Result result=validateTableLeaf(c.target(),boundaries,trusted);
                if(result.accepted()) {
                    int count=records.size();
                    addSyntheticEntry(records,existingStarts,c.target());
                    if(records.size()>count) { added++; println(String.format("TABLE FLOW ACCEPT source=0x%08X target=0x%08X nodes=%d",block.getStart().getOffset()+c.source(),c.target(),result.visited())); }
                } else {
                    rejected++;
                    println(String.format("TABLE FLOW REJECT source=0x%08X target=0x%08X reason=%s",block.getStart().getOffset()+c.source(),c.target(),result.reason()));
                }
            }
            Set<Long> rawSeen=new HashSet<>();
            for(long[] c:undecodedCandidates) {
                long target=c[1];
                if(existingStarts.contains(target) || !rawSeen.add(target)) continue;
                PointerTablePolicy.Result result=validateUndecodedLeaf(target,trusted);
                if(!result.accepted()) {
                    PointerTablePolicy.Result function=PointerTablePolicy.validateRawFunction(target,rawView(trusted));
                    if(function.accepted()) result=function;
                }
                rawTried++;
                if(result.accepted()) {
                    int count=records.size();
                    addSyntheticEntry(records,existingStarts,target);
                    if(records.size()>count) { rawAdded++; rawAcceptedStarts.add(target); println(String.format("UNDECODED LEAF ACCEPT source=0x%08X target=0x%08X nodes=%d",block.getStart().getOffset()+c[0],target,result.visited())); }
                } else {
                    rawReasons.merge(result.reason(),1,Integer::sum);
                }
            }
        }
        println(String.format("TABLE FLOW SUMMARY added=%d rejected=%d",added,rejected));
        println(String.format("UNDECODED LEAF SUMMARY added=%d evaluated=%d rejected-by-reason=%s",rawAdded,rawTried,rawReasons));
    }

    // A pointer found in data must not be surrounded by decoded code: no Ghidra instruction within
    // 64 bytes either side of the pointer slot.
    private boolean isDataOnlyNeighbourhood(Address source) {
        for(int delta=-64;delta<=64;delta+=4) {
            Address a;
            try { a=source.add(delta); } catch(Exception e) { continue; }
            if(currentProgram.getListing().getInstructionContaining(a)!=null) return false;
        }
        return true;
    }

    // Entries accepted by the raw validators: their undecoded words may be read directly.
    private final Set<Long> rawAcceptedStarts=new HashSet<>();

    private PointerTablePolicy.RawView rawView(Set<Long> trusted) {
        return new PointerTablePolicy.RawView() {
            public boolean knownCode(long pc) {
                if((pc&3)!=0) return false;
                Address a=addressFromOffset(pc);
                if(a==null || !isExecutableAddress(a)) return false;
                return trusted.contains(pc) || currentProgram.getFunctionManager().getFunctionContaining(a)!=null;
            }
            public Long word(long pc) {
                Address a=addressFromOffset(pc);
                return a==null?null:readWord(a);
            }
            public boolean undecoded(long pc) {
                if((pc&3)!=0 || trusted.contains(pc)) return false;
                Address a=addressFromOffset(pc);
                if(a==null || !isExecutableAddress(a)) return false;
                var listing=currentProgram.getListing();
                // Bytes no Ghidra Function owns: undecoded, or decoded into orphan instructions. The
                // raw validators read the words themselves, so Ghidra's decoding is not required.
                return listing.getDefinedDataContaining(a)==null &&
                    currentProgram.getFunctionManager().getFunctionContaining(a)==null;
            }
        };
    }

    private PointerTablePolicy.Result validateUndecodedLeaf(long entry, Set<Long> trusted) {
        return PointerTablePolicy.validateRawLeaf(entry,rawView(trusted));
    }

    // End of a synthetic unit exactly as the sizing pass of collectExecutableLabelRecords computes it.
    private long syntheticEndOf(long start, List<FunctionRecord> functionRecords, java.util.TreeSet<Long> starts) {
        long end=0L;
        for(FunctionRecord f:functionRecords) {
            if(start>f.start && start<f.endExclusive) { end=f.endExclusive; break; }
        }
        if(end==0L) {
            Address a=addressFromOffset(start);
            MemoryBlock block=a==null?null:currentProgram.getMemory().getBlock(a);
            if(block!=null) end=block.getEnd().getOffset()+1L;
        }
        Long next=starts.higher(start);
        if(next!=null && (end==0L || next<end)) end=next;
        if(end<=start) end=start+4L;
        return end;
    }

    private List<Long> directExitTargets(long pc, boolean rawUnit) {
        List<Long> targets=new ArrayList<>();
        Address a=addressFromOffset(pc);
        if(a==null) return targets;
        Instruction instruction=currentProgram.getListing().getInstructionAt(a);
        if(instruction!=null) {
            var flow=instruction.getFlowType();
            if(flow.isJump() || flow.isCall()) {
                for(Address flowTarget:instruction.getFlows()) targets.add(flowTarget.getOffset());
            }
            return targets;
        }
        // Words Ghidra never decoded are interpreted only inside units that were accepted as undecoded code.
        if(rawUnit) {
            Long raw=readWord(a);
            if(raw!=null) {
                long target=PointerTablePolicy.rawDirectTarget(pc,raw);
                if(target>=0) targets.add(target);
            }
        }
        return targets;
    }

    // Addresses built by lui+addiu/ori in decoded code that do not look like a callable entry to the
    // original shape test: routines in bytes no Function owns are validated by the raw validators.
    private final Set<Long> materializedRawCandidates=new java.util.LinkedHashSet<>();

    private void evaluateMaterializedRawCandidates(List<FunctionRecord> records, Set<Long> existingStarts) {
        int added=0, shown=0;
        java.util.Map<String,Integer> reasons=new java.util.TreeMap<>();
        for(long target:materializedRawCandidates) {
            if(target==0x11bda8L) println("MATERIALIZED DEBUG 11bda8 existing="+existingStarts.contains(target)+" fn="+(currentProgram.getFunctionManager().getFunctionContaining(addressFromOffset(target))!=null));
            if(existingStarts.contains(target) || (target&3)!=0) continue;
            Address a=addressFromOffset(target);
            if(a==null || !isExecutableAddress(a) || currentProgram.getFunctionManager().getFunctionContaining(a)!=null) continue;
            // Only inside the code region: a Ghidra Function must end within 512 bytes before the entry
            // (orphan routines sit between decoded ones; data/zero-filled areas do not).
            boolean nearCode=false;
            for(int k=1;k<=128 && !nearCode;k++) {
                Address back=addressFromOffset(target-4L*k);
                nearCode=back!=null && currentProgram.getFunctionManager().getFunctionContaining(back)!=null;
            }
            if(!nearCode) { reasons.merge("isolated-from-code",1,Integer::sum); continue; }
            PointerTablePolicy.Result result=validateUndecodedLeaf(target,existingStarts);
            if(!result.accepted()) {
                PointerTablePolicy.Result function=PointerTablePolicy.validateRawFunction(target,rawView(existingStarts));
                if(function.accepted()) result=function;
            }
            if(!result.accepted()) {
                reasons.merge(result.reason(),1,Integer::sum);
                if((!result.reason().equals("not-block-start") && ++shown<=40) || target==0x11bda8L) println(String.format("MATERIALIZED RAW REJECT target=0x%08X reason=%s",target,result.reason()));
                continue;
            }
            int count=records.size();
            addSyntheticEntry(records,existingStarts,target);
            if(records.size()>count) {
                added++;
                rawAcceptedStarts.add(target);
                println(String.format("MATERIALIZED RAW ACCEPT target=0x%08X nodes=%d",target,result.visited()));
            }
        }
        println(String.format("MATERIALIZED RAW SUMMARY added=%d candidates=%d rejected-by-reason=%s",added,materializedRawCandidates.size(),reasons));
    }

    // Static exits of synthetic units that land outside every ordinary function range need their
    // own entry: the recompiler can only attach resume aliases to ordinary decoded owners. Targets
    // inside another synthetic unit split that unit; targets in bytes Ghidra left undecoded must pass
    // the raw fragment validation. Iterates to a fixed point (new units have exits too).
    private void collectSyntheticExitClosure(List<FunctionRecord> functionRecords, List<FunctionRecord> labelRecords, Set<Long> existingStarts) {
        int added=0, rawAdded=0, blocked=0, passes=0;
        java.util.Map<String,Integer> reasons=new java.util.TreeMap<>();
        Set<Long> attempted=new HashSet<>();
        for(int pass=0;pass<64 && !monitor.isCancelled();pass++) {
            passes++;
            java.util.TreeSet<Long> starts=new java.util.TreeSet<>(existingStarts);
            java.util.TreeMap<Long,Long> targets=new java.util.TreeMap<>();
            for(FunctionRecord unit:new ArrayList<>(labelRecords)) {
                long end=syntheticEndOf(unit.start,functionRecords,starts);
                boolean rawUnit=rawAcceptedStarts.contains(unit.start);
                long reach=unit.start;
                for(long pc=unit.start;pc<end;pc+=4) {
                    if(rawUnit) {
                        // A raw unit's real extent ends at the first return (plus delay slot) that no forward
                        // branch jumps past; the words after it up to the next start are padding or foreign data.
                        Address ra=addressFromOffset(pc);
                        if(currentProgram.getFunctionManager().getFunctionContaining(ra)==null) {
                            Long w=readWord(ra);
                            if(w!=null) {
                                long bt=PointerTablePolicy.rawDirectTarget(pc,w);
                                if(bt>pc && bt<end && (w>>>26)!=2L && (w>>>26)!=3L) reach=Math.max(reach,bt);
                                if(w==0x03E00008L && pc+4>=reach) {
                                    for(long target:directExitTargets(pc+4,rawUnit)) {
                                        if(target>=unit.start && target<end) continue;
                                        if(existingStarts.contains(target) || attempted.contains(target) || (target&3)!=0) continue;
                                        Address ta0=addressFromOffset(target);
                                        if(ta0==null || !isExecutableAddress(ta0)) continue;
                                        boolean ordinary0=false;
                                        for(FunctionRecord f:functionRecords) { if(target>=f.start && target<f.endExclusive) { ordinary0=true; break; } }
                                        if(!ordinary0) targets.putIfAbsent(target,unit.start);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                    for(long target:directExitTargets(pc,rawUnit)) {
                        if(target>=unit.start && target<end) continue;
                        if(existingStarts.contains(target) || attempted.contains(target) || (target&3)!=0) continue;
                        Address ta=addressFromOffset(target);
                        if(ta==null || !isExecutableAddress(ta)) continue;
                        boolean ordinary=false;
                        for(FunctionRecord f:functionRecords) {
                            if(target>=f.start && target<f.endExclusive) { ordinary=true; break; }
                        }
                        if(!ordinary) targets.putIfAbsent(target,unit.start);
                    }
                }
            }
            if(targets.isEmpty()) break;
            int before=labelRecords.size();
            for(java.util.Map.Entry<Long,Long> item:targets.entrySet()) {
                long target=item.getKey();
                attempted.add(target);
                Address ta=addressFromOffset(target);
                Instruction instruction=currentProgram.getListing().getInstructionAt(ta);
                boolean raw=false;
                if(instruction==null) {
                    PointerTablePolicy.Result fragment=PointerTablePolicy.validateRawFragment(target,rawView(existingStarts));
                    if(!fragment.accepted()) { reasons.merge("undecoded-"+fragment.reason(),1,Integer::sum); continue; }
                    raw=true;
                }
                if(instruction!=null && instruction.isInDelaySlot()) { reasons.merge("delay-slot-target",1,Integer::sum); continue; }
                int count=labelRecords.size();
                // The target lies outside every exported range. If Ghidra assigns it to a secondary range of
                // a multi-range Function, the export never covered it; like the cross-range collector, add
                // the entry despite the containing Function.
                if(existingStarts.add(target)) {
                    FunctionRecord record=new FunctionRecord();
                    record.name=makeAnonymousEntryName(target);
                    record.start=target;
                    record.syntheticEntry=true;
                    labelRecords.add(record);
                }
                if(labelRecords.size()>count) {
                    added++; if(raw) { rawAdded++; rawAcceptedStarts.add(target); }
                    println(String.format("SYNTHETIC EXIT ACCEPT source-unit=0x%08X target=0x%08X %s",item.getValue(),target,raw?"undecoded":"decoded"));
                } else blocked++;
            }
            if(labelRecords.size()==before) break;
        }
        println(String.format("SYNTHETIC EXIT SUMMARY added=%d undecoded=%d blocked=%d passes=%d skipped-by-reason=%s",added,rawAdded,blocked,passes,reasons));
    }
    private List<FunctionRecord> collectExecutableLabelRecords(List<FunctionRecord> functionRecords) {
        List<FunctionRecord> labelRecords = new ArrayList<>();
        Set<Long> existingStarts = new HashSet<>();
        for (FunctionRecord record : functionRecords) {
            existingStarts.add(record.start);
        }

        SymbolIterator symbols = currentProgram.getSymbolTable().getSymbolIterator(true);
        while (symbols.hasNext() && !monitor.isCancelled()) {
            Symbol symbol = symbols.next();
            if (symbol == null || !symbol.isPrimary()) {
                continue;
            }

            if (symbol.getSymbolType() == SymbolType.FUNCTION) {
                continue;
            }
String symbolName = symbol.getName();

            if (symbolName.startsWith("caseD_") ||
                  symbolName.startsWith("switchD_")) {
                continue;
}

            Address address = symbol.getAddress();
            if (!isExecutableAddress(address)) {
                continue;
            }

            long start = address.getOffset();
            if (existingStarts.contains(start)) {
                continue;
            }

            Instruction instruction = currentProgram.getListing().getInstructionAt(address);
            if (instruction == null) {
                continue;
            }

            if (!hasCallableLabelReference(address)) {
                continue;
            }

            FunctionRecord record = new FunctionRecord();
            record.name = symbol.getName();
            record.start = start;
            record.syntheticEntry = true;
            labelRecords.add(record);
            existingStarts.add(start);
        }
        /*
        AddressSet executableAddresses = new AddressSet();
        for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
            if (block != null && block.isExecute()) {
                executableAddresses.addRange(block.getStart(), block.getEnd());
            }
        }

        InstructionIterator instructions = currentProgram.getListing().getInstructions(executableAddresses, true);
        while (instructions.hasNext() && !monitor.isCancelled()) {
            Instruction instruction = instructions.next();
            if (instruction == null) {
                continue;
            }

            Address address = instruction.getAddress();
            long start = address.getOffset();
            if (existingStarts.contains(start)) {
                continue;
            }

            if (!hasCallableLabelReference(address)) {
                continue;
            }

            FunctionRecord record = new FunctionRecord();
            record.name = makeAnonymousEntryName(start);
            record.start = start;
            record.syntheticEntry = true;
            labelRecords.add(record);
            existingStarts.add(start);
        }
         */
        // Ghidra does not always promote function pointers to CALL references,
        // especially when the low half is produced in a MIPS delay slot. Mirror
        // the stripped-ELF fallback used by ElfParser so the CSV still contains
        // callback and vtable entries that are only address-taken.
          collectMaterializedCodeEntries(labelRecords, existingStarts);
         collectDataFunctionPointerEntries(labelRecords, existingStarts);
		 collectCrossRangeBranchEntries(labelRecords, existingStarts);
        evaluateMaterializedRawCandidates(labelRecords, existingStarts);
        collectSyntheticExitClosure(functionRecords, labelRecords, existingStarts);

        if (labelRecords.isEmpty()) {
            return labelRecords;
        }

        List<Long> boundaries = new ArrayList<>();
        for (FunctionRecord record : functionRecords) {
            boundaries.add(record.start);
        }
        for (FunctionRecord record : labelRecords) {
            boundaries.add(record.start);
        }
        Collections.sort(boundaries);

        functionRecords.sort(Comparator.comparingLong(r -> r.start));
        for (FunctionRecord record : labelRecords) {
            long endExclusive = 0L;

            for (FunctionRecord functionRecord : functionRecords) {
                if (record.start > functionRecord.start && record.start < functionRecord.endExclusive) {
                    endExclusive = functionRecord.endExclusive;
                    break;
                }
            }

            if (endExclusive == 0L) {
                Address startAddress = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(record.start);
                MemoryBlock block = currentProgram.getMemory().getBlock(startAddress);
                if (block != null) {
                    endExclusive = block.getEnd().getOffset() + 1L;
                }
            }

            for (Long boundary : boundaries) {
                if (boundary > record.start && (endExclusive == 0L || boundary < endExclusive)) {
                    endExclusive = boundary;
                    break;
                }
            }

            if (endExclusive <= record.start) {
                endExclusive = record.start + 4L;
            }

            record.endExclusive = endExclusive;
            record.size = record.endExclusive - record.start;
        }

        labelRecords.sort(Comparator.comparingLong(r -> r.start));
        return labelRecords;
    }

    @Override
    public void run() throws Exception {
        File tomlFile = askFile("Choose output TOML config file", "Save");
        if (tomlFile == null) {
            return;
        }

        File csvFile = askFile("Choose output CSV file", "Save");
        if (csvFile == null) {
            return;
        }

        FunctionManager fm = currentProgram.getFunctionManager();
        FunctionIterator it = fm.getFunctions(true);

        List<FunctionRecord> functionRecords = new ArrayList<>();
        Set<String> stubSelectors = new LinkedHashSet<>();
        Set<String> untrackedStubSelectors = new LinkedHashSet<>();
        int uncategorizedCount = 0;

        while (it.hasNext() && !monitor.isCancelled()) {
            Function func = it.next();

            AddressSetView body = func.getBody();
            if (body == null || body.getNumAddresses() == 0) {
                continue;
            }

            FunctionRecord record = new FunctionRecord();
            record.name = func.getName();
            record.start = func.getEntryPoint().getOffset();

Address entryPoint = func.getEntryPoint();
AddressRange entryRange = null;
AddressRangeIterator rangeIterator = body.getAddressRanges();
if (func.getName().equals("FUN_00196fe8")) {
    println("RANGES FUN_00196fe8:");
    AddressRangeIterator debugBadRanges = body.getAddressRanges();

    while (debugBadRanges.hasNext()) {
        AddressRange debugRange = debugBadRanges.next();

        println(String.format(
            "  0x%08X - 0x%08X  size=%d",
            debugRange.getMinAddress().getOffset(),
            debugRange.getMaxAddress().getOffset(),
            debugRange.getLength()
        ));
    }
}
if (func.getName().equals("FUN_00193ef0")) {
    println("RANGES FUN_00193ef0:");
    AddressRangeIterator debugRanges = body.getAddressRanges();

    while (debugRanges.hasNext()) {
        AddressRange debugRange = debugRanges.next();

        println(String.format(
            "  0x%08X - 0x%08X  size=%d",
            debugRange.getMinAddress().getOffset(),
            debugRange.getMaxAddress().getOffset(),
            debugRange.getLength()
        ));
    }
}
if (body.getNumAddressRanges() > 1) {
    println(String.format(
        "MULTI RANGE %s: ranges=%d",
        func.getName(),
        body.getNumAddressRanges()
    ));
}
if (body.getNumAddressRanges() > 1) {
    long firstStart = body.getMinAddress().getOffset();
    long lastEnd = body.getMaxAddress().getOffset();
    long span = lastEnd - firstStart + 1;

    if (span > 0x10000) {
        println(String.format(
            "HUGE MULTI %s: ranges=%d span=0x%X start=0x%08X end=0x%08X",
            func.getName(),
            body.getNumAddressRanges(),
            span,
            firstStart,
            lastEnd
        ));
    }
}
AddressRangeIterator forwardRanges = body.getAddressRanges();
boolean foundEntryRange = false;
long forwardPreviousEnd = -1;

while (forwardRanges.hasNext()) {
    AddressRange r = forwardRanges.next();

    if (!foundEntryRange) {
        if (r.contains(func.getEntryPoint())) {
            foundEntryRange = true;
            forwardPreviousEnd = r.getMaxAddress().getOffset();
        }
        continue;
    }

    long gap = r.getMinAddress().getOffset() - forwardPreviousEnd - 1;

    if (gap > 64) {
        println(String.format(
            "LARGE FORWARD GAP %s: gap=%d from=0x%08X to=0x%08X",
            func.getName(),
            gap,
            forwardPreviousEnd,
            r.getMinAddress().getOffset()
        ));
    }

    forwardPreviousEnd = r.getMaxAddress().getOffset();
}
long previousEnd = -1;

AddressRangeIterator gapRanges = body.getAddressRanges();
while (gapRanges.hasNext()) {
    AddressRange r = gapRanges.next();

    if (previousEnd >= 0) {
        long gap = r.getMinAddress().getOffset() - previousEnd - 1;

        if (gap > 0 && gap <= 64) {
            println(String.format(
                "SMALL GAP %s: gap=%d  next=0x%08X",
                func.getName(),
                gap,
                r.getMinAddress().getOffset()
            ));
        }
    }

    previousEnd = r.getMaxAddress().getOffset();
}
while (rangeIterator.hasNext()) {
    AddressRange range = rangeIterator.next();
    if (range.contains(entryPoint)) {
        entryRange = range;
        break;
    }
}

if (entryRange == null) {
    continue;
}

record.endExclusive = entryRange.getMaxAddress().getOffset() + 1L;
record.size = entryRange.getLength();
functionRecords.add(record);

            ClassificationResult classification = classifyFunction(func);
            if (classification.kind == ClassificationKind.STUB) {
                stubSelectors.add(makeSelector(classification.name, record.start, true));
            } else if (classification.kind == ClassificationKind.UNTRACKED_STUB) {
                untrackedStubSelectors.add(makeSelector(classification.name, record.start, true));
            } else {
                uncategorizedCount++;
            }
        }

        final int functionCount = functionRecords.size();

List<FunctionRecord> labelRecords =
    collectExecutableLabelRecords(functionRecords);

List<FunctionRecord> exportRecords = new ArrayList<>(functionRecords);


exportRecords.addAll(labelRecords);
exportRecords.sort(Comparator.comparingLong(r -> r.start));
   

        try (PrintWriter writer = new PrintWriter(csvFile)) {
            writer.println("Name,Start,End,Size");
            for (FunctionRecord record : exportRecords) {
                writer.printf("%s,0x%08X,0x%08X,%d%n",
                    record.name,
                    record.start,
                    record.endExclusive,
                    record.size
                );
            }
        } 

        String programPath = currentProgram.getExecutablePath();
        if (programPath == null) {
            programPath = "";
        }
        programPath = normalizeWindowsDrivePath(programPath);

        File outputDir = tomlFile.getParentFile() == null ? new File("output") : new File(tomlFile.getParentFile(), "output");
        String ghidraCsvPath = csvFile.getAbsolutePath();

        try (PrintWriter writer = new PrintWriter(tomlFile)) {
            writer.println("# Auto-generated by ExportPS2Functions.java");
            writer.println("#");
            writer.println("# Classification policy (aligned with analyzer intent):");
            writer.println("# - runtime-known names -> [general].stubs");
            writer.println("# - library-like names without runtime handlers -> [general].untracked_stubs");
            writer.println("# - [general].skip is retained empty for legacy compatibility");
            writer.println("# - no SCE symbol database is used by this Ghidra script");
            writer.println();

            writer.println("[general]");
            writer.println("input = " + tomlString(programPath));
            writer.println("output = " + tomlString(outputDir.getAbsolutePath()));
            writer.println("ghidra_output = " + tomlString(ghidraCsvPath));
            writer.println("single_file_output = false");
            writer.println("patch_syscalls = false");
            writer.println("patch_cop0 = true");
            writer.println("patch_cache = true");
            writer.println("stubs = [");
            for (String selector : stubSelectors) {
                writer.println("  " + tomlString(selector) + ",");
            }
            writer.println("]");
            writer.println("untracked_stubs = [");
            for (String selector : untrackedStubSelectors) {
                writer.println("  " + tomlString(selector) + ",");
            }
            writer.println("]");
            writer.println("skip = []");
            writer.println();

            writer.println("[ghidra_export]");
            writer.println("function_count = " + functionCount);
            writer.println("code_label_count = " + labelRecords.size());
            writer.println("csv_record_count = " + exportRecords.size());
            writer.println("stub_count = " + stubSelectors.size());
            writer.println("untracked_stub_count = " + untrackedStubSelectors.size());
            writer.println("skip_count = 0");
            writer.println("uncategorized_count = " + uncategorizedCount);
            writer.println("runtime_call_name_count = " + RUNTIME_HANDLER_NAMES.size());
            writer.println("runtime_call_source = \"embedded_ps2_call_list_snapshot\"");
        }

         println(String.format("Exported %d functions and %d executable labels to %s", functionCount, labelRecords.size(), csvFile.getAbsolutePath()));

        println(String.format("Using %d embedded runtime handler names from ps2_call_list.h snapshot.", RUNTIME_HANDLER_NAMES.size()));
        println(String.format("Exported TOML config to %s", tomlFile.getAbsolutePath()));
    }
}
