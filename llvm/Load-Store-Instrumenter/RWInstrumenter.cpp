//===- AddressSanitizer.cpp - memory error detector -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of AddressSanitizer, an address sanity checker.
// Details of the algorithm:
//  https://github.com/google/sanitizers/wiki/AddressSanitizerAlgorithm
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/AddressSanitizer.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Triple.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/Comdat.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Use.h"
#include "llvm/IR/Value.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/MCSectionMachO.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/Utils/ASanStackFrameLayout.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>

using namespace llvm;

#define DEBUG_TYPE "asan"
#define numAccessesSizes 5

static const uint64_t kDefaultShadowScale = 3;
static const uint64_t kDefaultShadowOffset32 = 1ULL << 29;
static const uint64_t kDefaultShadowOffset64 = 1ULL << 44;
static const uint64_t kDynamicShadowSentinel =
        std::numeric_limits<uint64_t>::max();
static const uint64_t kSmallX86_64ShadowOffsetBase = 0x7FFFFFFF;  // < 2G.
static const uint64_t kSmallX86_64ShadowOffsetAlignMask = ~0xFFFULL;
static const uint64_t kLinuxKasan_ShadowOffset64 = 0xdffffc0000000000;
static const uint64_t kPPC64_ShadowOffset64 = 1ULL << 44;
static const uint64_t kSystemZ_ShadowOffset64 = 1ULL << 52;
static const uint64_t kMIPS32_ShadowOffset32 = 0x0aaa0000;
static const uint64_t kMIPS64_ShadowOffset64 = 1ULL << 37;
static const uint64_t kAArch64_ShadowOffset64 = 1ULL << 36;
static const uint64_t kFreeBSD_ShadowOffset32 = 1ULL << 30;
static const uint64_t kFreeBSD_ShadowOffset64 = 1ULL << 46;
static const uint64_t kNetBSD_ShadowOffset32 = 1ULL << 30;
static const uint64_t kNetBSD_ShadowOffset64 = 1ULL << 46;
static const uint64_t kNetBSDKasan_ShadowOffset64 = 0xdfff900000000000;
static const uint64_t kPS4CPU_ShadowOffset64 = 1ULL << 40;
static const uint64_t kWindowsShadowOffset32 = 3ULL << 28;
static const uint64_t kEmscriptenShadowOffset = 0;

static const uint64_t kMyriadShadowScale = 5;
static const uint64_t kMyriadMemoryOffset32 = 0x80000000ULL;
static const uint64_t kMyriadMemorySize32 = 0x20000000ULL;
// The shadow memory space is dynamically allocated.
static const uint64_t kWindowsShadowOffset64 = kDynamicShadowSentinel;

static const uint64_t kAsanCtorAndDtorPriority = 1;
// On Emscripten, the system needs more than one priorities for constructors.
static const uint64_t kAsanEmscriptenCtorAndDtorPriority = 50;
// Accesses sizes are powers of two: 1, 2, 4, 8, 16.
static const size_t kNumberOfAccessSizes = 5;


// This flag limits the number of instructions to be instrumented
// in any given BB. Normally, this should be set to unlimited (INT_MAX),
// but due to http://llvm.org/bugs/show_bug.cgi?id=12652 we temporary
// set it to 10000.
static int ClMaxInsnsToInstrumentPerBB = 10000;

static cl::opt <std::string> ClMemoryAccessCallbackPrefix(
        "rwinstrumenter-asan-memory-access-callback-prefix",
        cl::desc("Prefix for memory access callbacks"), cl::Hidden,
        cl::init("__asan_"));
// These flags allow to change the shadow mapping.
// The shadow mapping looks like
//    Shadow = (Mem >> scale) + offset

static cl::opt<int> ClMappingScale("rwinstrumenter-asan-mapping-scale",
                                   cl::desc("scale of asan shadow mapping"),
                                   cl::Hidden, cl::init(0));

static cl::opt <uint64_t>
        ClMappingOffset("rwinstrumenter-asan-mapping-offset",
                        cl::desc("offset of asan shadow mapping [EXPERIMENTAL]"),
                        cl::Hidden, cl::init(0));

static int ClForceExperiment =0;

STATISTIC(NumInstrumentedReads,
"Number of instrumented reads");
STATISTIC(NumInstrumentedWrites,
"Number of instrumented writes");
STATISTIC(NumOptimizedAccessesToGlobalVar,
"Number of optimized accesses to global vars");
STATISTIC(NumOptimizedAccessesToStackVar,
"Number of optimized accesses to stack vars");

namespace {

/// This struct defines the shadow mapping using the rule:
///   shadow = (mem >> Scale) ADD-or-OR Offset.
/// If InGlobal is true, then
///   extern char __asan_shadow[];
///   shadow = (mem >> Scale) + &__asan_shadow
    struct ShadowMapping {
        int Scale;
        uint64_t Offset;
        bool OrShadowOffset;
        bool InGlobal;
    };

} // end anonymous namespace

static ShadowMapping getShadowMapping(Triple &TargetTriple, int LongSize,
                                      bool IsKasan) {
    bool IsAndroid = TargetTriple.isAndroid();
    bool IsIOS = TargetTriple.isiOS() || TargetTriple.isWatchOS();
    bool IsFreeBSD = TargetTriple.isOSFreeBSD();
    bool IsNetBSD = TargetTriple.isOSNetBSD();
    bool IsPS4CPU = TargetTriple.isPS4CPU();
    bool IsLinux = TargetTriple.isOSLinux();
    bool IsPPC64 = TargetTriple.getArch() == Triple::ppc64 ||
                   TargetTriple.getArch() == Triple::ppc64le;
    bool IsSystemZ = TargetTriple.getArch() == Triple::systemz;
    bool IsX86_64 = TargetTriple.getArch() == Triple::x86_64;
    bool IsMIPS32 = TargetTriple.isMIPS32();
    bool IsMIPS64 = TargetTriple.isMIPS64();
    bool IsArmOrThumb = TargetTriple.isARM() || TargetTriple.isThumb();
    bool IsAArch64 = TargetTriple.getArch() == Triple::aarch64;
    bool IsWindows = TargetTriple.isOSWindows();
    bool IsFuchsia = TargetTriple.isOSFuchsia();
    bool IsMyriad = TargetTriple.getVendor() == llvm::Triple::Myriad;
    bool IsEmscripten = TargetTriple.isOSEmscripten();

    ShadowMapping Mapping;

    Mapping.Scale = IsMyriad ? kMyriadShadowScale : kDefaultShadowScale;
    if (ClMappingScale.getNumOccurrences() > 0) {
        Mapping.Scale = ClMappingScale;
    }

    if (LongSize == 32) {
        if (IsAndroid)
            Mapping.Offset = kDynamicShadowSentinel;
        else if (IsMIPS32)
            Mapping.Offset = kMIPS32_ShadowOffset32;
        else if (IsFreeBSD)
            Mapping.Offset = kFreeBSD_ShadowOffset32;
        else if (IsNetBSD)
            Mapping.Offset = kNetBSD_ShadowOffset32;
        else if (IsIOS)
            Mapping.Offset = kDynamicShadowSentinel;
        else if (IsWindows)
            Mapping.Offset = kWindowsShadowOffset32;
        else if (IsEmscripten)
            Mapping.Offset = kEmscriptenShadowOffset;
        else if (IsMyriad) {
            uint64_t ShadowOffset = (kMyriadMemoryOffset32 + kMyriadMemorySize32 -
                                     (kMyriadMemorySize32 >> Mapping.Scale));
            Mapping.Offset = ShadowOffset - (kMyriadMemoryOffset32 >> Mapping.Scale);
        } else
            Mapping.Offset = kDefaultShadowOffset32;
    } else {  // LongSize == 64
        // Fuchsia is always PIE, which means that the beginning of the address
        // space is always available.
        if (IsFuchsia)
            Mapping.Offset = 0;
        else if (IsPPC64)
            Mapping.Offset = kPPC64_ShadowOffset64;
        else if (IsSystemZ)
            Mapping.Offset = kSystemZ_ShadowOffset64;
        else if (IsFreeBSD && !IsMIPS64)
            Mapping.Offset = kFreeBSD_ShadowOffset64;
        else if (IsNetBSD) {
            if (IsKasan)
                Mapping.Offset = kNetBSDKasan_ShadowOffset64;
            else
                Mapping.Offset = kNetBSD_ShadowOffset64;
        } else if (IsPS4CPU)
            Mapping.Offset = kPS4CPU_ShadowOffset64;
        else if (IsLinux && IsX86_64) {
            if (IsKasan)
                Mapping.Offset = kLinuxKasan_ShadowOffset64;
            else
                Mapping.Offset = (kSmallX86_64ShadowOffsetBase &
                                  (kSmallX86_64ShadowOffsetAlignMask << Mapping.Scale));
        } else if (IsWindows && IsX86_64) {
            Mapping.Offset = kWindowsShadowOffset64;
        } else if (IsMIPS64)
            Mapping.Offset = kMIPS64_ShadowOffset64;
        else if (IsIOS)
            Mapping.Offset = kDynamicShadowSentinel;
        else if (IsAArch64)
            Mapping.Offset = kAArch64_ShadowOffset64;
        else
            Mapping.Offset = kDefaultShadowOffset64;
    }

    if (ClMappingOffset.getNumOccurrences() > 0) {
        Mapping.Offset = ClMappingOffset;
    }

    // OR-ing shadow offset if more efficient (at least on x86) if the offset
    // is a power of two, but on ppc64 we have to use add since the shadow
    // offset is not necessary 1/8-th of the address space.  On SystemZ,
    // we could OR the constant in a single instruction, but it's more
    // efficient to load it once and use indexed addressing.
    Mapping.OrShadowOffset = !IsAArch64 && !IsPPC64 && !IsSystemZ && !IsPS4CPU &&
                             !(Mapping.Offset & (Mapping.Offset - 1)) &&
                             Mapping.Offset != kDynamicShadowSentinel;
    bool IsAndroidWithIfuncSupport =
            IsAndroid && !TargetTriple.isAndroidVersionLT(21);
    Mapping.InGlobal = IsAndroidWithIfuncSupport && IsArmOrThumb;

    return Mapping;
}

static size_t RedzoneSizeForScale(int MappingScale) {
    // Redzone used for stack and globals is at least 32 bytes.
    // For scales 6 and 7, the redzone has to be 64 and 128 bytes respectively.
    return std::max(32U, 1U << MappingScale);
}

static uint64_t GetCtorAndDtorPriority(Triple &TargetTriple) {
    if (TargetTriple.isOSEmscripten()) {
        return kAsanEmscriptenCtorAndDtorPriority;
    } else {
        return kAsanCtorAndDtorPriority;
    }
}

namespace {

    /// Module analysis for getting various metadata about the module.
    class RWIGlobalsMetadataWrapperPass : public ModulePass {
    public:
        static char ID;

        RWIGlobalsMetadataWrapperPass() : ModulePass(ID) {
            initializeRWIGlobalsMetadataWrapperPassPass(
                    *PassRegistry::getPassRegistry());
        }

        bool runOnModule(Module &M) override {
            GlobalsMD = GlobalsMetadata(M);
            return false;
        }

        StringRef getPassName() const override {
            return "RWIGlobalsMetadataWrapperPass";
        }

        void getAnalysisUsage(AnalysisUsage &AU) const override {
            AU.setPreservesAll();
        }

        GlobalsMetadata &getGlobalsMD() { return GlobalsMD; }

    private:
        GlobalsMetadata GlobalsMD;
    };

    char RWIGlobalsMetadataWrapperPass::ID = 0;


    /// AddressSanitizer: instrument the code in module to find memory bugs.
    struct AddressSanitizer {
        AddressSanitizer(Module &M, const GlobalsMetadata *GlobalsMD,
                         bool CompileKernel = false, bool Recover = false,
                         bool UseAfterScope = false)
                : UseAfterScope(UseAfterScope), GlobalsMD(*GlobalsMD) {
            this->Recover = Recover;
            this->CompileKernel = CompileKernel;

            C = &(M.getContext());
            LongSize = M.getDataLayout().getPointerSizeInBits();
            IntptrTy = Type::getIntNTy(*C, LongSize);
            TargetTriple = Triple(M.getTargetTriple());

            Mapping = getShadowMapping(TargetTriple, LongSize, this->CompileKernel);
        }

        uint64_t getAllocaSizeInBytes(const AllocaInst &AI) const {
            uint64_t ArraySize = 1;
            if (AI.isArrayAllocation()) {
                const ConstantInt *CI = dyn_cast<ConstantInt>(AI.getArraySize());
                assert(CI && "non-constant array size");
                ArraySize = CI->getZExtValue();
            }
            Type *Ty = AI.getAllocatedType();
            uint64_t SizeInBytes =
                    AI.getModule()->getDataLayout().getTypeAllocSize(Ty);
            return SizeInBytes * ArraySize;
        }

        /// Check if we want (and can) handle this alloca.
        bool isInterestingAlloca(const AllocaInst &AI);

        /// If it is an interesting memory access, return the PointerOperand
        /// and set IsWrite/Alignment. Otherwise return nullptr.
        /// MaybeMask is an output parameter for the mask Value, if we're looking at a
        /// masked load/store.
        Value *isInterestingMemoryAccess(Instruction *I, bool *IsWrite,
                                         uint64_t *TypeSize, unsigned *Alignment,
                                         Value **MaybeMask = nullptr);

        void instrumentMop(ObjectSizeOffsetVisitor &ObjSizeVis, Instruction *I,
                           bool UseCalls, const DataLayout &DL);

        void instrumentAddress(Instruction *OrigIns, Instruction *InsertBefore,
                               Value *Addr, uint32_t TypeSize, bool IsWrite,
                               Value *SizeArgument, bool UseCalls, uint32_t Exp);

        void instrumentUnusualSizeOrAlignment(Instruction *I,
                                              Instruction *InsertBefore, Value *Addr,
                                              uint32_t TypeSize, bool IsWrite,
                                              Value *SizeArgument, bool UseCalls,
                                              uint32_t Exp);


        Value *memToShadow(Value *Shadow, IRBuilder<> &IRB);

        bool instrumentFunction(Function &F, const TargetLibraryInfo *TLI);

        void markEscapedLocalAllocas(Function &F);

    private:
        void initializeCallbacks(Module &M);

        bool LooksLikeCodeInBug11395(Instruction *I);

        bool GlobalIsLinkerInitialized(GlobalVariable *G);

        bool isSafeAccess(ObjectSizeOffsetVisitor &ObjSizeVis, Value *Addr,
                          uint64_t TypeSize) const;

        /// Helper to cleanup per-function state.
        struct FunctionStateRAII {
            AddressSanitizer *Pass;

            FunctionStateRAII(AddressSanitizer *Pass) : Pass(Pass) {
                assert(Pass->ProcessedAllocas.empty() &&
                       "last pass forgot to clear cache");
                assert(!Pass->LocalDynamicShadow);
            }

            ~FunctionStateRAII() {
                Pass->LocalDynamicShadow = nullptr;
                Pass->ProcessedAllocas.clear();
            }
        };

        LLVMContext *C;
        Triple TargetTriple;
        int LongSize;
        bool CompileKernel;
        bool Recover;
        bool UseAfterScope;
        Type *IntptrTy;
        ShadowMapping Mapping;
        Constant *AsanShadowGlobal;

        // These arrays is indexed by AccessIsWrite, Experiment and log2(AccessSize).
        FunctionCallee InstrumenterMemoryAccessCallback[2][kNumberOfAccessSizes];

        InlineAsm *EmptyAsm;
        Value *LocalDynamicShadow = nullptr;
        const GlobalsMetadata &GlobalsMD;
        DenseMap<const AllocaInst *, bool> ProcessedAllocas;
    };

    class RWInstrumenter : public FunctionPass {
    public:
        static char ID;

        explicit RWInstrumenter(bool CompileKernel = false,
                                bool Recover = false,
                                bool UseAfterScope = false)
                : FunctionPass(ID), CompileKernel(CompileKernel), Recover(Recover),
                  UseAfterScope(UseAfterScope) {
            initializeRWInstrumenterPass(*PassRegistry::getPassRegistry());
        }

        StringRef getPassName() const override {
            return "RWInstrumenterFunctionPass";
        }

        void getAnalysisUsage(AnalysisUsage &AU) const override {
            AU.addRequired<RWIGlobalsMetadataWrapperPass>();
            AU.addRequired<TargetLibraryInfoWrapperPass>();
        }

        bool runOnFunction(Function &F) override {
            // errs() << "Function name: " << F.getName() << "\n";
            GlobalsMetadata &GlobalsMD =
                    getAnalysis<RWIGlobalsMetadataWrapperPass>().getGlobalsMD();
            const TargetLibraryInfo *TLI =
                    &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
            AddressSanitizer ASan(*F.getParent(), &GlobalsMD, CompileKernel, Recover,
                                  UseAfterScope);
            return ASan.instrumentFunction(F, TLI);
        }

    private:
        bool CompileKernel;
        bool Recover;
        bool UseAfterScope;
    };
} // end anonymous namespace

INITIALIZE_PASS(RWIGlobalsMetadataWrapperPass,
"rw-instrumenter-globals-md",
"Read metadata to mark which globals should be instrumented "
"when running rw-instrumenter.",
false, true)

char RWInstrumenter::ID = 0;

INITIALIZE_PASS_BEGIN(
        RWInstrumenter,
"rw-asan",
"RWInstrumenter: detects use-after-free and out-of-bounds bugs.", false,
false)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(RWIGlobalsMetadataWrapperPass)
INITIALIZE_PASS_END(
        RWInstrumenter,
"rw-asan",
"RWInstrumenter: detects use-after-free and out-of-bounds bugs.", false,
false)

FunctionPass *llvm::createRWInstrumenterFunctionPass(bool CompileKernel,
                                                     bool Recover,
                                                     bool UseAfterScope) {
    assert(!CompileKernel || Recover);
     errs() << "RWInstrumenter created" << F.getName() << "\n";
    return new RWInstrumenter(CompileKernel, Recover, UseAfterScope);
}

static size_t TypeSizeToSizeIndex(uint32_t TypeSize) {
    size_t Res = countTrailingZeros(TypeSize / 8);
    assert(Res < kNumberOfAccessSizes);
    return Res;
}

/// Check if we want (and can) handle this alloca.
bool AddressSanitizer::isInterestingAlloca(const AllocaInst &AI) {
    auto PreviouslySeenAllocaInfo = ProcessedAllocas.find(&AI);

    if (PreviouslySeenAllocaInfo != ProcessedAllocas.end())
        return PreviouslySeenAllocaInfo->getSecond();

    bool IsInteresting =
            (AI.getAllocatedType()->isSized() &&
             // alloca() may be called with 0 size, ignore it.
             ((!AI.isStaticAlloca()) || getAllocaSizeInBytes(AI) > 0) &&
             // We are only interested in allocas not promotable to registers.
             // Promotable allocas are common under -O0.
             (!isAllocaPromotable(&AI)) &&
             // inalloca allocas are not treated as static, and we don't want
             // dynamic alloca instrumentation for them as well.
             !AI.isUsedWithInAlloca() &&
             // swifterror allocas are register promoted by ISel
             !AI.isSwiftError());

    ProcessedAllocas[&AI] = IsInteresting;
    return IsInteresting;
}

Value *AddressSanitizer::isInterestingMemoryAccess(Instruction *I,
                                                   bool *IsWrite,
                                                   uint64_t *TypeSize,
                                                   unsigned *Alignment,
                                                   Value **MaybeMask) {
    // Skip memory accesses inserted by another instrumentation.
    if (I->hasMetadata("nosanitize")) return nullptr;

    // Do not instrument the load fetching the dynamic shadow address.
    if (LocalDynamicShadow == I)
        return nullptr;

    Value *PtrOperand = nullptr;
    const DataLayout &DL = I->getModule()->getDataLayout();
    if (LoadInst * LI = dyn_cast<LoadInst>(I)) {
        *IsWrite = false;
        *TypeSize = DL.getTypeStoreSizeInBits(LI->getType());
        *Alignment = LI->getAlignment();
        PtrOperand = LI->getPointerOperand();
    } else if (StoreInst * SI = dyn_cast<StoreInst>(I)) {
        *IsWrite = true;
        *TypeSize = DL.getTypeStoreSizeInBits(SI->getValueOperand()->getType());
        *Alignment = SI->getAlignment();
        PtrOperand = SI->getPointerOperand();
    } else if (AtomicRMWInst * RMW = dyn_cast<AtomicRMWInst>(I)) {
        *IsWrite = true;
        *TypeSize = DL.getTypeStoreSizeInBits(RMW->getValOperand()->getType());
        *Alignment = 0;
        PtrOperand = RMW->getPointerOperand();
    } else if (AtomicCmpXchgInst * XCHG = dyn_cast<AtomicCmpXchgInst>(I)) {
        *IsWrite = true;
        *TypeSize = DL.getTypeStoreSizeInBits(XCHG->getCompareOperand()->getType());
        *Alignment = 0;
        PtrOperand = XCHG->getPointerOperand();
    } else if (auto CI = dyn_cast<CallInst>(I)) {
        auto *F = dyn_cast<Function>(CI->getCalledValue());
        if (F && (F->getName().startswith("llvm.masked.load.") ||
                  F->getName().startswith("llvm.masked.store."))) {
            unsigned OpOffset = 0;
            if (F->getName().startswith("llvm.masked.store.")) {
                // Masked store has an initial operand for the value.
                OpOffset = 1;
                *IsWrite = true;
            } else {
                *IsWrite = false;
            }

            auto BasePtr = CI->getOperand(0 + OpOffset);
            auto Ty = cast<PointerType>(BasePtr->getType())->getElementType();
            *TypeSize = DL.getTypeStoreSizeInBits(Ty);
            if (auto AlignmentConstant =
                    dyn_cast<ConstantInt>(CI->getOperand(1 + OpOffset)))
                *Alignment = (unsigned) AlignmentConstant->getZExtValue();
            else
                *Alignment = 1; // No alignment guarantees. We probably got Undef
            if (MaybeMask)
                *MaybeMask = CI->getOperand(2 + OpOffset);
            PtrOperand = BasePtr;
        }
    }

    if (PtrOperand) {
        // Do not instrument acesses from different address spaces; we cannot deal
        // with them.
        Type *PtrTy = cast<PointerType>(PtrOperand->getType()->getScalarType());
        if (PtrTy->getPointerAddressSpace() != 0)
            return nullptr;

        // Ignore swifterror addresses.
        // swifterror memory addresses are mem2reg promoted by instruction
        // selection. As such they cannot have regular uses like an instrumentation
        // function and it makes no sense to track them as memory.
        if (PtrOperand->isSwiftError())
            return nullptr;
    }

    // Treat memory accesses to promotable allocas as non-interesting since they
    // will not cause memory violations. This greatly speeds up the instrumented
    // executable at -O0.
//    if (ClSkipPromotableAllocas)
        if (auto AI = dyn_cast_or_null<AllocaInst>(PtrOperand))
            return isInterestingAlloca(*AI) ? AI : nullptr;

    return PtrOperand;
}

static bool isPointerOperand(Value *V) {
    return V->getType()->isPointerTy() || isa<PtrToIntInst>(V);
}

bool AddressSanitizer::GlobalIsLinkerInitialized(GlobalVariable *G) {
    // If a global variable does not have dynamic initialization we don't
    // have to instrument it.  However, if a global does not have initializer
    // at all, we assume it has dynamic initializer (in other TU).
    //
    // FIXME: Metadata should be attched directly to the global directly instead
    // of being added to llvm.asan.globals.
    return G->hasInitializer() && !GlobalsMD.get(G).IsDynInit;
}

static void doInstrumentAddress(AddressSanitizer *Pass, Instruction *I,
                                Instruction *InsertBefore, Value *Addr,
                                unsigned Alignment, unsigned Granularity,
                                uint32_t TypeSize, bool IsWrite,
                                Value *SizeArgument, bool UseCalls,
                                uint32_t Exp) {
    // Instrument a 1-, 2-, 4-, 8-, or 16- byte access with one check
    // if the data is properly aligned.
    if ((TypeSize == 8 || TypeSize == 16 || TypeSize == 32 || TypeSize == 64 ||
         TypeSize == 128) &&
        (Alignment >= Granularity || Alignment == 0 || Alignment >= TypeSize / 8))
        return Pass->instrumentAddress(I, InsertBefore, Addr, TypeSize, IsWrite,
                                       nullptr, UseCalls, Exp);
    Pass->instrumentUnusualSizeOrAlignment(I, InsertBefore, Addr, TypeSize,
                                           IsWrite, nullptr, UseCalls, Exp);
}

static void instrumentMaskedLoadOrStore(AddressSanitizer *Pass,
                                        const DataLayout &DL, Type *IntptrTy,
                                        Value *Mask, Instruction *I,
                                        Value *Addr, unsigned Alignment,
                                        unsigned Granularity, uint32_t TypeSize,
                                        bool IsWrite, Value *SizeArgument,
                                        bool UseCalls, uint32_t Exp) {
    auto *VTy = cast<PointerType>(Addr->getType())->getElementType();
    uint64_t ElemTypeSize = DL.getTypeStoreSizeInBits(VTy->getScalarType());
    unsigned Num = VTy->getVectorNumElements();
    auto Zero = ConstantInt::get(IntptrTy, 0);
    for (unsigned Idx = 0; Idx < Num; ++Idx) {
        Value *InstrumentedAddress = nullptr;
        Instruction *InsertBefore = I;
        if (auto *Vector = dyn_cast<ConstantVector>(Mask)) {
            // dyn_cast as we might get UndefValue
            if (auto *Masked = dyn_cast<ConstantInt>(Vector->getOperand(Idx))) {
                if (Masked->isZero())
                    // Mask is constant false, so no instrumentation needed.
                    continue;
                // If we have a true or undef value, fall through to doInstrumentAddress
                // with InsertBefore == I
            }
        } else {
            IRBuilder<> IRB(I);
            Value *MaskElem = IRB.CreateExtractElement(Mask, Idx);
            Instruction *ThenTerm = SplitBlockAndInsertIfThen(MaskElem, I, false);
            InsertBefore = ThenTerm;
        }

        IRBuilder<> IRB(InsertBefore);
        InstrumentedAddress =
                IRB.CreateGEP(VTy, Addr, {Zero, ConstantInt::get(IntptrTy, Idx)});
        doInstrumentAddress(Pass, I, InsertBefore, InstrumentedAddress, Alignment,
                            Granularity, ElemTypeSize, IsWrite, SizeArgument,
                            UseCalls, Exp);
    }
}

void AddressSanitizer::instrumentMop(ObjectSizeOffsetVisitor &ObjSizeVis,
                                     Instruction *I, bool UseCalls,
                                     const DataLayout &DL) {
    bool IsWrite = false;
    unsigned Alignment = 0;
    uint64_t TypeSize = 0;
    Value *MaybeMask = nullptr;
    Value *Addr =
            isInterestingMemoryAccess(I, &IsWrite, &TypeSize, &Alignment, &MaybeMask);
    assert(Addr);

    // Optimization experiments.
    // The experiments can be used to evaluate potential optimizations that remove
    // instrumentation (assess false negatives). Instead of completely removing
    // some instrumentation, you set Exp to a non-zero value (mask of optimization
    // experiments that want to remove instrumentation of this instruction).
    // If Exp is non-zero, this pass will emit special calls into runtime
    // (e.g. __asan_report_exp_load1 instead of __asan_report_load1). These calls
    // make runtime terminate the program in a special way (with a different
    // exit status). Then you run the new compiler on a buggy corpus, collect
    // the special terminations (ideally, you don't see them at all -- no false
    // negatives) and make the decision on the optimization.
    uint32_t Exp = ClForceExperiment;

    //if (ClOpt && ClOptGlobals) {
    // If initialization order checking is disabled, a simple access to a
    // dynamically initialized global is always valid.
#if 0
    GlobalVariable *G = dyn_cast<GlobalVariable>(GetUnderlyingObject(Addr, DL));
    if (G && ( GlobalIsLinkerInitialized(G)) &&
        isSafeAccess(ObjSizeVis, Addr, TypeSize)) {
        NumOptimizedAccessesToGlobalVar++;
        return;
    }
#endif
    //}

    //if (ClOpt && ClOptStack) {
    // A direct inbounds access to a stack variable is always valid.
    if (isa<AllocaInst>(GetUnderlyingObject(Addr, DL)) &&
        isSafeAccess(ObjSizeVis, Addr, TypeSize)) {
        NumOptimizedAccessesToStackVar++;
        return;
    }

    if (IsWrite)
        NumInstrumentedWrites++;
    else
        NumInstrumentedReads++;

    unsigned Granularity = 1 << Mapping.Scale;
    if (MaybeMask) {
        instrumentMaskedLoadOrStore(this, DL, IntptrTy, MaybeMask, I, Addr,
                                    Alignment, Granularity, TypeSize, IsWrite,
                                    nullptr, UseCalls, Exp);
    } else {
        doInstrumentAddress(this, I, I, Addr, Alignment, Granularity, TypeSize,
                            IsWrite, nullptr, UseCalls, Exp);
    }
}

void AddressSanitizer::instrumentAddress(Instruction *OrigIns,
                                         Instruction *InsertBefore, Value *Addr,
                                         uint32_t TypeSize, bool IsWrite,
                                         Value *SizeArgument, bool UseCalls,
                                         uint32_t Exp) {
    bool IsMyriad = TargetTriple.getVendor() == llvm::Triple::Myriad;

    IRBuilder<> IRB(InsertBefore);
    Value *AddrLong = IRB.CreatePointerCast(Addr, IntptrTy);
    size_t AccessSizeIndex = TypeSizeToSizeIndex(TypeSize);

    if (UseCalls) {
        IRB.CreateCall(InstrumenterMemoryAccessCallback[IsWrite][AccessSizeIndex],
                       AddrLong);
    }
    return;
}

// Instrument unusual size or unusual alignment.
// We can not do it with a single check, so we do 1-byte check for the first
// and the last bytes. We call __asan_report_*_n(addr, real_size) to be able
// to report the actual access size.
void AddressSanitizer::instrumentUnusualSizeOrAlignment(
        Instruction *I, Instruction *InsertBefore, Value *Addr, uint32_t TypeSize,
        bool IsWrite, Value *SizeArgument, bool UseCalls, uint32_t Exp) {
    IRBuilder<> IRB(InsertBefore);
    Value *Size = ConstantInt::get(IntptrTy, TypeSize / 8);
    Value *AddrLong = IRB.CreatePointerCast(Addr, IntptrTy);
    if (UseCalls) {
        // since we do not need size here, so ..
        IRB.CreateCall(InstrumenterMemoryAccessCallback[IsWrite][0],
                       AddrLong);
    }
}


void AddressSanitizer::initializeCallbacks(Module &M) {
    IRBuilder<> IRB(*C);
    // Create __asan_report* callbacks.
    // IsWrite, TypeSize and Exp are encoded in the function name.
    for (size_t AccessIsWrite = 0; AccessIsWrite <= 1; AccessIsWrite++) {
        const std::string TypeStr = AccessIsWrite ? "store" : "load";

        SmallVector < Type * , 2 > Args1{1, IntptrTy};

        for (size_t AccessSizeIndex = 0; AccessSizeIndex < kNumberOfAccessSizes;
             AccessSizeIndex++) {

            InstrumenterMemoryAccessCallback[AccessIsWrite][AccessSizeIndex] =
                    M.getOrInsertFunction(
                            //"__instrumenter__",
                            TypeStr + "_" + itostr(1ULL << AccessSizeIndex) + "bytes",
                            FunctionType::get(IRB.getVoidTy(), Args1, false));
        }
    }

    const std::string MemIntrinCallbackPrefix =
            CompileKernel ? std::string("") : ClMemoryAccessCallbackPrefix;

    // We insert an empty inline asm after __asan_report* to avoid callback merge.
    EmptyAsm = InlineAsm::get(FunctionType::get(IRB.getVoidTy(), false),
                              StringRef(""), StringRef(""),
            /*hasSideEffects=*/true);
    if (Mapping.InGlobal)
        AsanShadowGlobal = M.getOrInsertGlobal("__asan_shadow",
                                               ArrayType::get(IRB.getInt8Ty(), 0));
}

void AddressSanitizer::markEscapedLocalAllocas(Function &F) {
    // Find the one possible call to llvm.localescape and pre-mark allocas passed
    // to it as uninteresting. This assumes we haven't started processing allocas
    // yet. This check is done up front because iterating the use list in
    // isInterestingAlloca would be algorithmically slower.
    assert(ProcessedAllocas.empty() && "must process localescape before allocas");

    // Try to get the declaration of llvm.localescape. If it's not in the module,
    // we can exit early.
    if (!F.getParent()->getFunction("llvm.localescape")) return;

    // Look for a call to llvm.localescape call in the entry block. It can't be in
    // any other block.
    for (Instruction &I : F.getEntryBlock()) {
        IntrinsicInst *II = dyn_cast<IntrinsicInst>(&I);
        if (II && II->getIntrinsicID() == Intrinsic::localescape) {
            // We found a call. Mark all the allocas passed in as uninteresting.
            for (Value *Arg : II->arg_operands()) {
                AllocaInst *AI = dyn_cast<AllocaInst>(Arg->stripPointerCasts());
                assert(AI && AI->isStaticAlloca() &&
                       "non-static alloca arg to localescape");
                ProcessedAllocas[AI] = false;
            }
            break;
        }
    }
}

bool AddressSanitizer::instrumentFunction(Function &F,
                                          const TargetLibraryInfo *TLI) {
    if (F.getLinkage() == GlobalValue::AvailableExternallyLinkage) return false;
    if (F.getName().startswith("__asan_")) return false;
    if (F.getName().startswith("load_")) return false;
    if (F.getName().startswith("store_")) return false;

    bool FunctionModified = false;

    // If needed, insert __asan_init before checking for SanitizeAddress attr.
    // This function needs to be called even if the function body is not
    // instrumented.
    //if (maybeInsertAsanInitAtFunctionEntry(F))
    //  FunctionModified = true;

    // Leave if the function doesn't need instrumentation.
    //if (!F.hasFnAttribute(Attribute::SanitizeAddress)) return FunctionModified;

    LLVM_DEBUG(dbgs() << "ASAN instrumenting:\n" << F << "\n");

    initializeCallbacks(*F.getParent());

    FunctionStateRAII CleanupObj(this);

    //maybeInsertDynamicShadowAtFunctionEntry(F);

    // We can't instrument allocas used with llvm.localescape. Only static allocas
    // can be passed to that intrinsic.
    markEscapedLocalAllocas(F);

    // We want to instrument every address only once per basic block (unless there
    // are calls between uses).
    SmallPtrSet < Value * , 16 > TempsToInstrument;
    SmallVector < Instruction * , 16 > ToInstrument;
    SmallVector < Instruction * , 8 > NoReturnCalls;
    SmallVector < BasicBlock * , 16 > AllBlocks;
    SmallVector < Instruction * , 16 > PointerComparisonsOrSubtracts;
    int NumAllocas = 0;
    bool IsWrite;
    unsigned Alignment;
    uint64_t TypeSize;

    // Fill the set of memory operations to instrument.
    for (auto &BB : F) {
        AllBlocks.push_back(&BB);
        TempsToInstrument.clear();
        int NumInsnsPerBB = 0;
        for (auto &Inst : BB) {
            if (LooksLikeCodeInBug11395(&Inst)) return false;
            Value *MaybeMask = nullptr;
            if (Value * Addr = isInterestingMemoryAccess(&Inst, &IsWrite, &TypeSize,
                                                         &Alignment, &MaybeMask)) {
                ToInstrument.push_back(&Inst);
                NumInsnsPerBB++;
            }
            if (NumInsnsPerBB >= ClMaxInsnsToInstrumentPerBB) {
                errs() << "NumInsnsPerBB >= ClMaxInsnsToInstrumentPerBB\n";
                break;
            }
        }
    }

    bool UseCalls = true;
    const DataLayout &DL = F.getParent()->getDataLayout();
    ObjectSizeOpts ObjSizeOpts;
    ObjSizeOpts.RoundToAlign = true;
    ObjectSizeOffsetVisitor ObjSizeVis(DL, TLI, F.getContext(), ObjSizeOpts);

    // Instrument.
    int NumInstrumented = 0;
    for (auto Inst : ToInstrument) {
        if (isInterestingMemoryAccess(Inst, &IsWrite, &TypeSize, &Alignment)) {
            instrumentMop(ObjSizeVis, Inst, UseCalls,
                          F.getParent()->getDataLayout());
            NumInstrumented++;
        }
    }

    if (NumInstrumented > 0)
        FunctionModified = true;

    LLVM_DEBUG(dbgs() << "ASAN done instrumenting: " << FunctionModified << " "
                      << F << "\n");

    return FunctionModified;
}

// Workaround for bug 11395: we don't want to instrument stack in functions
// with large assembly blobs (32-bit only), otherwise reg alloc may crash.
// FIXME: remove once the bug 11395 is fixed.
bool AddressSanitizer::LooksLikeCodeInBug11395(Instruction *I) {
    if (LongSize != 32) return false;
    CallInst *CI = dyn_cast<CallInst>(I);
    if (!CI || !CI->isInlineAsm()) return false;
    if (CI->getNumArgOperands() <= 5) return false;
    // We have inline assembly with quite a few arguments.
    return true;
}


// isSafeAccess returns true if Addr is always inbounds with respect to its
// base object. For example, it is a field access or an array access with
// constant inbounds index.
bool AddressSanitizer::isSafeAccess(ObjectSizeOffsetVisitor &ObjSizeVis,
                                    Value *Addr, uint64_t TypeSize) const {
    SizeOffsetType SizeOffset = ObjSizeVis.compute(Addr);
    if (!ObjSizeVis.bothKnown(SizeOffset)) return false;
    uint64_t Size = SizeOffset.first.getZExtValue();
    int64_t Offset = SizeOffset.second.getSExtValue();
    // Three checks are required to ensure safety:
    // . Offset >= 0  (since the offset is given from the base ptr)
    // . Size >= Offset  (unsigned)
    // . Size - Offset >= NeededSize  (unsigned)
    return Offset >= 0 && Size >= uint64_t(Offset) &&
           Size - uint64_t(Offset) >= TypeSize / 8;
}
