/** @file
 *
 *  Copyright (c) 2018, Andrei Warkentin <andrey.warkentin@gmail.com>
 *
 *  This program and the accompanying materials
 *  are licensed and made available under the terms and conditions of the BSD License
 *  which accompanies this distribution.  The full text of the license may be found at
 *  http://opensource.org/licenses/bsd-license.php
 *
 *  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
 *
 **/

#include "HypDxe.h"
#include "ArmDefs.h"
#include <Library/CacheMaintenanceLib.h>

#define BRK_BREAKPOINT   0xf000
#define BRK_ASSERT       0xf001
#define BRK_SERVICE      0xf002
#define SERVICE_PRINT          1
#define SERVICE_LOAD_SYMBOLS   3
#define SERVICE_UNLOAD_SYMBOLS 4

#define NT_MIN_ILM       0xFFFFF80000000000ul
#define NT_MAX_ILM       0xFFFFF87FFFFFFFFFul
#define WS_BUILD_UNKNOWN 0

/*
 * +/- so many bytes will be examined around
 * _KDDEBUGGER_DATA64 candidates to locate
 * DBGKD_GET_VERSION64.
 */
#define BYTES_AROUND_KDDEBUGGER_DATA64 (10 * EFI_PAGE_SIZE)

#define KPCR_LOCK_ARRAY(KPCR) *((UINT64 *) (U8P((KPCR)) + 0x28))
#define KPCR_VER_MAJOR(KPCR) *((UINT16 *) (U8P((KPCR)) + 0x3c))
#define KPCR_VER_MINOR(KPCR) *((UINT16 *) (U8P((KPCR)) + 0x3e))
#define KPCR_KD_VER_BLOCK(KPCR) *((UINT64 *) (U8P((KPCR)) + 0x80))

typedef BOOLEAN (*ContigGPASearchCheck)(HVA, VOID *);

typedef struct {
  UINT16 Length;
  UINT16 MaximumLength;
  GVA Buffer;
} OEM_STRING;

typedef struct {
  UINT16 MajorVersion;
  UINT16 MinorVersion;
  UINT8 ProtocolVersion;
  UINT8 KdSecondaryVersion;
  UINT16 Flags;

  UINT16 MachineType;
  UINT8 MaxPacketType;
  UINT8 MaxStateChange;
  UINT8 MaxManipulate;
  UINT8 Simulation;
  UINT16 Unused;

  UINT64 KernBase;
  UINT64 PsLoadedModuleList;
  UINT64 DebuggerDataList;
} DBGKD_GET_VERSION64;

STATIC UINTN mWSBuild;
STATIC BOOLEAN mWSCanPatch;
STATIC BOOLEAN mWSDebugHook;
STATIC UINT32 mWSWin2000Mask;
STATIC GVA mWSKernBase;


STATIC HVA
Probe(
  IN  GVA VA
  )
{
  UINT64 Par;
  UINT64 ParSaved;

  /*
   * Preserve EL1 PAR.
   */
  ReadSysReg(ParSaved, par_el1);
  ISB();

  ATS12E1R(VA);
  ISB();

  ReadSysReg(Par, par_el1);
  if (PAR_IS_BAD(Par)) {
    goto done;
  }

  return MPA_2_HVA((PAR_2_ADDR(Par) | X(VA, 0, 11)));
done:
  ISB();
  WriteSysReg(par_el1, ParSaved);
  return INVALID_HVA;
}


STATIC HVA
ProbeIgnZero(
  IN  GVA VA
  )
{
  if (VA == 0) {
    return INVALID_HVA;
  }

  return Probe(VA);
}


STATIC BOOLEAN
HypWSProbeKdVerBlock(
  IN  DBGKD_GET_VERSION64 *KdVer,
  IN  GVA KnownKernBase
  )
{
  if (KdVer == NULL) {
    return FALSE;
  }

  if (KdVer->MajorVersion != 0xf &&
      KdVer->MajorVersion != 0xc) {
    return FALSE;
  }

  if (KdVer->MachineType != 0xAA64) {
    return FALSE;
  }

  if (KnownKernBase != 0 &&
      KnownKernBase != KdVer->KernBase) {
    return FALSE;
  }

  HLOG((HLOG_INFO, "Detected arm64%a build %u, NT base = 0x%lx\n",
        KdVer->MajorVersion == 0xf ? "fre" : "chk",
        KdVer->MinorVersion, KdVer->KernBase));

  mWSBuild = KdVer->MinorVersion;
  mWSKernBase = KdVer->KernBase;
  return TRUE;
}


STATIC GVA
ContigGPASearch(
                IN  GVA Start,
                IN  GVA End,
                IN  VOID *Seq,
                IN  UINTN Length,
                IN  UINTN Alignment,
                IN  ContigGPASearchCheck Check,
                IN  VOID *CheckParam
               )
{
  ASSERT (Length != 0);
  ASSERT (Alignment != 0);
  ASSERT (Length <= EFI_PAGE_SIZE);

  Start = A_UP(Start, Alignment);
  ASSERT (End > Start);

  while (Start + Length < End) {
    GVA SeqLast = Start + Length - 1;
    HVA StartHVA = Probe(Start);
    HVA SeqLastHVA = Probe(SeqLast);

    if (SeqLastHVA == INVALID_HVA) {
      /*
       * Try page after the page SeqLast fell on.
       */
      Start = A_UP(SeqLast + 1, EFI_PAGE_SIZE);
      continue;
    }

    if ((HVA_2_HVPN(SeqLastHVA)- HVA_2_HVPN(StartHVA)) > 1) {
      /*
       * Crossing page boundary and there is no contiguity?
       * Start at the page containing SeqLast.
       */
      Start = A_DOWN(SeqLast, EFI_PAGE_SIZE);
      continue;
    }

    if (Check != NULL) {
      if (!Check(StartHVA, CheckParam)) {
        Start += Alignment;
        continue;
      }

      if (Seq == NULL) {
        return Start;
      }
    }

    if (Seq != NULL) {
      UINTN Index;
      /*
       * Now we know we can safely check length
       * bytes of sequence.
       */
      for (Index = 0; Index < Length; ) {
        UINTN AdjIndex;
        UINTN CompSize = Alignment;

        if (CompSize > (Length - Index)) {
          CompSize = Length - Index;
        }

        if (CompSize != 8 &&
            CompSize != 4 &&
            CompSize != 2) {
          CompSize = 1;
        }

        AdjIndex = Index / CompSize;

        if ((CompSize == 8 &&
             U64P(StartHVA)[AdjIndex] !=
             U64P(Seq)[AdjIndex]) ||
            (CompSize == 4 &&
             U32P(StartHVA)[AdjIndex] !=
             U32P(Seq)[AdjIndex]) ||
            (CompSize == 2 &&
             U16P(StartHVA)[AdjIndex] !=
             U16P(Seq)[AdjIndex]) ||
              (U8P(StartHVA)[AdjIndex] !=
               U8P(Seq)[AdjIndex])) {
          break;
        }

        Index += CompSize;
      }

      if (Index == Length) {
        return Start;
      }

      Start += Alignment;
    }
  }

  return INVALID_GVA;
}


STATIC BOOLEAN
HypWSFindKdVerBlock(
  IN  HVA KPCR,
  IN  GVA KPCRVA
  )
{
  GVA ProbeVA = KPCRVA;
  const UINT32 Sig = UN('K') << 0 |
    UN('D') << 8 |
    UN('B') << 16 |
    UN('G') << 24;

  do {
    UINT32 *Page;
    GVA KernBase;
    GVA VerProbeVA;

    ProbeVA = ContigGPASearch(ProbeVA, NT_MAX_ILM,
                              VP(&Sig), sizeof(Sig),
                              sizeof(UINT64), NULL, NULL);
    if (ProbeVA == INVALID_GVA) {
      /*
       * Signature not found at all?
       */
      return FALSE;
    }

    Page = HVA_2_P(Probe(ProbeVA));
    KernBase = *(UINT64 *) (Page + 2);
    if ((KernBase & NT_MIN_ILM) != NT_MIN_ILM) {
      /*
       * Go to the next thing that looks like
       * _KDDEBUGGER_DATA64.
       */
      goto next;
    }

    VerProbeVA = ContigGPASearch(ProbeVA -
                                 BYTES_AROUND_KDDEBUGGER_DATA64,
                                 ProbeVA +
                                 BYTES_AROUND_KDDEBUGGER_DATA64,
                                 NULL,
                                 sizeof(DBGKD_GET_VERSION64),
                                 sizeof(UINT64),
                                 VP(HypWSProbeKdVerBlock),
                                 VP(KernBase));
    if (VerProbeVA == INVALID_GVA) {
      /*
       * Go to the next thing that looks like
       * _KDDEBUGGER_DATA64.
       */
      goto next;
    }

    HLOG((HLOG_VERBOSE,
          "Matched DBGKD_GET_VERSION64 @ %lx relative to %lx\n",
          VerProbeVA, ProbeVA));
    return TRUE;

  next:
    ProbeVA++;
    continue;
  } while (ProbeVA < NT_MAX_ILM);

  return FALSE;
}


STATIC BOOLEAN
HypWSSupported(
  IN  EFI_SYSTEM_CONTEXT_AARCH64 *Context
  )
{
  HVA KPCR;
  GVA KPCRVA;
  VOID *KdVersionBlock;

  /*
   * ntddk.h defines KIPCR() by masking
   * off low bits.
   */
  ReadSysReg(KPCRVA, tpidr_el1);
  KPCRVA &= ~0xFFFul;
  if (KPCRVA != Context->X18) {
    /*
     * Windows uses TPIDR_EL1 and x18 in kernel
     * mode to point to the KPCR.
     */
    return FALSE;
  }

  if (KPCRVA < NT_MIN_ILM ||
      KPCRVA > NT_MAX_ILM) {
    /*
     * Outside of the initial loader mapping ranges.
     */
    return FALSE;
  }

  KPCR = Probe(KPCRVA);
  if (KPCR == INVALID_HVA) {
    HLOG((HLOG_ERROR, "Couldn't probe KPCR\n"));
    return FALSE;
  }

  if (KPCR_VER_MAJOR(KPCR) != 1 ||
      KPCR_VER_MINOR(KPCR) != 1) {
    /*
     * The two fields that never change...
     */
    return FALSE;
  }

  HLOG((HLOG_INFO, "Probing for DBGKD_GET_VERSION64...\n"));
  KdVersionBlock = HVA_2_P(ProbeIgnZero(KPCR_KD_VER_BLOCK(KPCR)));
  if (HypWSProbeKdVerBlock(KdVersionBlock, 0)) {
    return TRUE;
  }

  HLOG((HLOG_INFO, "Searching for DBGKD_GET_VERSION64...\n"));
  if (!HypWSFindKdVerBlock(KPCR, KPCRVA)) {
    HLOG((HLOG_ERROR, "Cannot identify build, try with debug enabled in BCD\n"));
  }

  /*
   * While some patches may rely on knowing the build, others
   * may try matching a known instruction sequence, and still
   * work.
   */
  return TRUE;
}


VOID
HypWSTryBRK(
  IN  EFI_SYSTEM_CONTEXT_AARCH64 *SystemContext
  )
{
  BOOLEAN GiveUp = FALSE;
  GVA PreferredReturn = SystemContext->ELR + 4;
  UINTN BrkCall = ESR_2_ISS(SystemContext->ESR);

  if (!mWSDebugHook) {
    /*
     * Another CPU gave up on the hooking.
     */
    WriteSysReg(mdcr_el2, 0);
    return;
  }

  if (BrkCall == BRK_BREAKPOINT) {
    HLOG((HLOG_INFO, "DbgBreakPointWithStatus(0x%x)\n",
          SystemContext->X0));
    GiveUp = TRUE;
  } else if (BrkCall == BRK_SERVICE) {
    UINTN Service = SystemContext->X16;
    switch (Service) {
    case SERVICE_PRINT: {
      CHAR8 *String = HVA_2_P(Probe(SystemContext->X0));

      if (String != NULL) {
        /*
         * Yeah yeah, I know :-/.
         */
        UINTN Len = AsciiStrLen(String);
        if (String[Len - 1] == '\r' ||
            String[Len - 1] == '\n') {
          Len--;
        }
        HLOG((HLOG_VM, "%.*a\n", Len, String));
      }
    } break;
    case SERVICE_LOAD_SYMBOLS:
    case SERVICE_UNLOAD_SYMBOLS: {
      CHAR8 *String = NULL;
      OEM_STRING *StringStruct = HVA_2_P(Probe(SystemContext->X0));

      if (StringStruct != NULL) {
        String = HVA_2_P(Probe(StringStruct->Buffer));
      }

      if (String != NULL) {
        HLOG((HLOG_VM, "%aoading module '%.*a'\n",
              Service == SERVICE_LOAD_SYMBOLS ? "L" : "Unl",
              StringStruct->Length, String));
      } else {
        HLOG((HLOG_VM, "%aoading unknown module\n",
              Service == SERVICE_LOAD_SYMBOLS ? "L" : "Unl"));
      }
    } break;
    default:
      HLOG((HLOG_ERROR, "Unsupported NT debug service %u\n",
            Service));
      GiveUp = TRUE;
    }

    /*
     * Services must ELR + 8 overall to jump over the following
     * brk #F000 guard.
     */
    PreferredReturn += 4;
  } else {
    HLOG((HLOG_ERROR, "Ignoring unknown BRK #0x%x\n", BrkCall));
  }

  if (GiveUp) {
    /*
     * May simulate BRK delivery into EL1 in the future,
     * but right now just bail on trapping. This replays
     * the BRK inside EL1 but means we'll miss any future
     * ones.
     */
    HLOG((HLOG_ERROR, "Giving up on debug hooking\n"));
    mWSDebugHook = FALSE;
    WriteSysReg(mdcr_el2, 0);
  } else {
    /*
     * Report no debugger break-in.
     */
    SystemContext->X0 = 0;
    SystemContext->ELR = PreferredReturn;
  }
}


STATIC GVA
FindHalpInterruptRegisterControllerLoc(
  IN  GVA From
  )
{
  /*
   * See comment in PatchHalpInterruptRegisterController.
   */
  static UINT32 Seq[] = {
    0x51000da8, 0x7100051f, 0x54000069
  };

  GVA ProbeVA = A_DOWN(From, EFI_PAGE_SIZE);
  /*
   * Hypothetically, the matching could be fuzzier,
   * accepting any registers and branch offsets, under
   * the theory that small additions or removals to the
   * function may have changed the register assignments
   * or branch distances, but won't change the sequence
   * of opcodes themselves or the immediates used.
   */
  ProbeVA = ContigGPASearch(ProbeVA, NT_MAX_ILM,
                            VP(Seq), sizeof(Seq),
                            sizeof(*Seq),
                            NULL, NULL);
  if (ProbeVA != INVALID_GVA) {
    ProbeVA += sizeof(Seq) - 4;
  }

  return ProbeVA;
}


STATIC
VOID
HypWSPatchHalpInterruptRegisterController(
  IN  EFI_SYSTEM_CONTEXT_AARCH64 *Context
  )
{
  UINT32 *Insn;
  GVA PC = Context->ELR;
  BOOLEAN Status = FALSE;

  /*
   * hal!HalpPowerInvokeSmc:
   * 0x0: d10043ff sub  sp, sp #0x10
   * 0x4: f506b004 str  x7, [sp]
   * 0x8: d4000003 smc  #0            <-- PC
   * 0xa: f94003e7 ldr  x7, [sp]
   * ...
   */

  if (mWSBuild >= 17125 &&
      mWSBuild <= 17134) {
    PC += 0x7684;
  } else if (mWSBuild == 17672) {
    PC += 0x7d4c;
  } else {
    /*
     * Unknown build Let's try finding
     * the area of interest..
     */
    HLOG((HLOG_ERROR, "Unknown build %u, trying to match\n",
          mWSBuild));
    PC = FindHalpInterruptRegisterControllerLoc(PC);
  }

  if (PC == INVALID_GVA) {
    HLOG((HLOG_ERROR, "Couldn't locate hal!HalpInterruptRegisterController patch site\n"));
    goto done;
  }

  /*
   * hal!HalpInterruptRegisterController:
   * ...
   * 0x64: 51000da8 sub  w8, w13, #3
   * 0x68: 7100051f cmp  w8, #1
   * 0x6c: 54000069 bls  <func>+0x78  <-- PC
   */

  Insn = HVA_2_P(Probe(PC));
  if (*Insn == 0x14000003) {
    HLOG((HLOG_ERROR, "Already patched?\n"));
    Status = TRUE;
    goto done;
  } else if (*Insn != 0x54000069) {
    HLOG((HLOG_ERROR, "Invalid hal!HalpInterruptRegisterController patch site (0x%x)\n", *Insn));
    goto done;
  }

  /*
   * Patch the `bls` (54000069) to `b` (14000003).
   */
  *Insn = 0x14000003;
  InvalidateInstructionCacheRange(Insn, 4);
  Status = TRUE;
done:
  if (!Status) {
    HLOG((HLOG_ERROR, "Couldn't patch HAL for InterruptControllerBcm\n"));
  }
}


STATIC VOID
HypWSPatchWin2000Mask(
  VOID
  )
{
  UINT32 *Mask;
  GVA MaskGVA = mWSKernBase;

  if (mWSBuild == 17125) {
    /*
     * nt!HalDispatchTable+0x100+0x260.
     */
    MaskGVA += 0x336260;
  } else if (mWSBuild == 17672) {
    /*
     * nt!HalPrivateDispatchTable+0x3d10+0xd30.
     */
    MaskGVA += 0x329d30;
  } else {
    HLOG((HLOG_ERROR, "Kd_Win2000_Mask patch doesn't support build %u\n",
          mWSBuild));
    return;
  }

  Mask = HVA_2_P(Probe(MaskGVA));
  *Mask = mWSWin2000Mask;
}


VOID
HypWSTryPatch(
  IN  EFI_SYSTEM_CONTEXT_AARCH64 *Context
  )
{
  if (!mWSCanPatch) {
    return;
  }

  if (!HypWSSupported(Context)) {
    /*
     * Not Windows.
     */
    goto done;
  }

  HypWSPatchHalpInterruptRegisterController(Context);

  if (mWSWin2000Mask != 0) {
    HypWSPatchWin2000Mask();
  }

  if (mWSDebugHook) {
    /*
     * Trap all debug registers and route
     * debugging exceptions.
     */
    WriteSysReg(mdcr_el2, MDCR_TDE);
  }
done:
  mWSCanPatch = FALSE;
}


VOID
HypWSInit(
  VOID
  )
{
  mWSBuild = WS_BUILD_UNKNOWN;
  mWSDebugHook = PcdGet32(PcdHypWindowsDebugHook);
  mWSWin2000Mask = PcdGet32(PcdHypWin2000Mask);
  mWSCanPatch = TRUE;
}
