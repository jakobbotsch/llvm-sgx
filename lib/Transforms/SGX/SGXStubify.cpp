#include "SGXInternal.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <tuple>

#define DEBUG_TYPE "sgx-stubify"

using namespace llvm;

namespace {

enum ENCLU_LEAF {
  ENCLU_EENTER = 2,
  ENCLU_EEXIT = 4,
};

struct SecureFuncAdapterInfo {
  uint32_t FuncIndex;
  Function *OriginalFunction;
  Function *InsecureAdapter;
  Type *FrameTy;
};

struct SGXStubify : public ModulePass {
  static char ID;
  SGXStubify() : ModulePass(ID) {
    initializeSGXStubifyPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;

private:
  Module *M;
  LLVMContext *C;
  Constant *EncTcsGlobal;
  Constant *EncInit;
  Constant *EncEH;
  Type *Int8Ty;
  Type *Int32Ty;
  Type *Int64Ty;
  Type *Int8PtrTy;
  Type *VoidTy;

  SecureFuncAdapterInfo createInsecureAdapter(Function &F, uint32_t FuncIndex,
                                              Function &SecureAdapter);
  void fillSecureAdapter(Function *SecureAdapter,
                         ArrayRef<SecureFuncAdapterInfo> Adapters);
};
}

bool SGXStubify::runOnModule(Module &M) {
  this->M = &M;
  C = &M.getContext();
  Int8Ty = Type::getInt8Ty(*C);
  Int32Ty = Type::getInt32Ty(*C);
  Int64Ty = Type::getInt64Ty(*C);
  Int8PtrTy = Type::getInt8PtrTy(*C);
  VoidTy = Type::getVoidTy(*C);

  // LLVMSGX runtime externals
  EncTcsGlobal =
    M.getOrInsertGlobal("__llvmsgx_enclave_tcs", Int8PtrTy);
  EncInit = M.getOrInsertFunction("__llvmsgx_enclave_init", VoidTy, Int8PtrTy);
  // OpenSGX EH support (TODO: remove the need for this)
  EncEH = M.getOrInsertGlobal("exception_handler", Int8Ty);

  SmallVector<Function *, 16> SecureFuncs;
  for (Function &F : M.functions()) {
    if (F.hasFnAttribute(SGX_SECURE_ATTR))
      SecureFuncs.push_back(&F);
  }

  SmallVector<SecureFuncAdapterInfo, 16> AdapterInfos;
  uint32_t CurrentAdapterIndex = 0;
  Function *SecureAdapter = nullptr;
  for (Function *F : SecureFuncs) {
    F->setSection(SGX_SECURE_SECTION);

    // If a secure function has insecure callers we need to split the function
    // into 3 different functions:
    // 1. The insecure adapter stub, which can be called from insecure functions
    //    and is in regular '.text'. It packs args/return value and uses EENTER
    //    to get into the secure adapter.
    // 2. A secure adapter stub, which is placed in 'sgxtext'. It unpacks args
    //    and the return value, calls the actual implementation and leaves with EEXIT.
    // 3. The actual function, placed in 'sgxtext'.
    // If there are only secure callers then we can just move it into sgxtext
    // directly.
    SmallVector<Instruction *, 16> InsecureCalls;
    for (Use &U : F->uses()) {
      User *FU = U.getUser();
      if (!isa<CallInst>(FU) && !isa<InvokeInst>(FU))
        continue;

      // Ensure the function is actually called and not used as an arg
      Instruction *Inst = cast<Instruction>(FU);
      ImmutableCallSite CS(Inst);
      if (!CS.isCallee(&U))
        continue;

      // Do not modify secure-to-secure calls at all.
      // Check both 'secure' attribute but also section in case someone
      // wants to add functions manually in the future.
      if (CS.getCaller()->hasFnAttribute(SGX_SECURE_ATTR) ||
          CS.getCaller()->getSection() == SGX_SECURE_SECTION)
        continue;

      InsecureCalls.push_back(Inst);
    }

    if (InsecureCalls.empty())
      continue; // no adapter necessary

    // A secure adapter will be needed, so create it now if necessary.
    if (SecureAdapter == nullptr) {
      FunctionType *SecureAdapterFTy =
        FunctionType::get(VoidTy, {Int64Ty, Int8PtrTy}, false);

      SecureAdapter = Function::Create(SecureAdapterFTy, Function::PrivateLinkage,
                                       "__llvmsgx_secadapt");

      M.getFunctionList().insert(F->getIterator(), SecureAdapter);
    }

    SecureFuncAdapterInfo Adapter =
      createInsecureAdapter(*F, CurrentAdapterIndex, *SecureAdapter);
    CurrentAdapterIndex++;

    for (Instruction *I : InsecureCalls) {
      if (CallInst *Call = dyn_cast<CallInst>(I))
        Call->setCalledFunction(Adapter.InsecureAdapter);
      if (InvokeInst *Invoke = dyn_cast<InvokeInst>(I))
        Invoke->setCalledFunction(Adapter.InsecureAdapter);
    }

    AdapterInfos.push_back(Adapter);
  }

  fillSecureAdapter(SecureAdapter, AdapterInfos);

  bool AnyGV = false;
  for (GlobalVariable& GV : M.globals()) {
    if (!GV.hasAttribute(SGX_SECURE_ATTR))
      continue;

    GV.setSection(SGX_SECURE_SECTION);
    AnyGV = true;
  }

  return !SecureFuncs.empty() || AnyGV;
}

// Creates an adapter that when used from an insecure function
// calls the specified secure function.
SecureFuncAdapterInfo SGXStubify::createInsecureAdapter(Function &F, uint32_t FuncIndex,
                                                        Function &SecureAdapter) {
  bool ReturnsVoid = F.getReturnType() == VoidTy;
  // Create a type containing the return value and all args.
  // Since secure functions only support 1 argument we will
  // use this to pass arbitrary arguments.
  SmallVector<Type *, 8> FrameTypes;
  if (!ReturnsVoid)
    FrameTypes.push_back(F.getReturnType());
  for (const auto &Arg : F.args())
    FrameTypes.push_back(Arg.getType());

  StructType *FrameTy = StructType::get(*C, FrameTypes);

  Function *InsecureAdapter =
    Function::Create(F.getFunctionType(), Function::PrivateLinkage,
                     Twine("__llvmsgx_insadapt_") + F.getName());
  M->getFunctionList().insert(F.getIterator(), InsecureAdapter);
  InsecureAdapter->copyAttributesFrom(&F);
  InsecureAdapter->setSection("");

  // Add the following code.
  // secure TRet Func(T1 arg1, T2 arg2, ...) {
  //   FrameTy f;
  //   f.arg1 = arg1;
  //   f.arg2 = arg2;
  //   ...
  //   if (!__llvmsgx_enclave_tcs)
  //     __llvmsgx_enclave_init(&SecureAdapter);
  //   EENTER(__llvmsgx_enclave_tcs, exception_handler, FuncIndex, &f);
  //   return f.ret;
  // }

  IRBuilder<> IRB(BasicBlock::Create(*C, "", InsecureAdapter));
  AllocaInst *FrameAlloc = IRB.CreateAlloca(FrameTy);
  uint32_t InsertIndex = ReturnsVoid ? 0 : 1;
  for (auto &Arg : InsecureAdapter->args()) {
    Value *StoredArgAddr = IRB.CreateStructGEP(nullptr, FrameAlloc, InsertIndex);
    IRB.CreateStore(&Arg, StoredArgAddr);
    InsertIndex++;
  }

  Value *TcsIsNull = IRB.CreateICmpEQ(IRB.CreateLoad(EncTcsGlobal),
                                      Constant::getNullValue(Int8PtrTy));

  const char *EnterConstraints =
    // First get inputs:
    // rdi <- first arg (function index)
    // rsi <- second arg (pointer to frame)
    // rbx <- TCS
    // rcx <- AEP
    // rax <- leaf func (EENTER)
    "{di},{si},{bx},{cx},{ax},"
    // Now clobbers. Since this is a call we actually clobber
    // most regs, however here we just take a subset since
    // there is an internal limit on the number of clobbers.
    // Clobbered by returning EEXIT
    "~{bx},"
    // Scratch registers
    "~{ax},~{cx},~{dx},~{si},~{di},"
    "~{r8},~{r9},~{r10},~{r11},"
    "~{flags},~{memory}";

  FunctionType *EnterFTy =
    FunctionType::get(VoidTy, {Int64Ty, Int8PtrTy, Int8PtrTy, Int8PtrTy, Int64Ty}, false);
  InlineAsm *EnterIA = InlineAsm::get(EnterFTy, "enclu", EnterConstraints,
                                      /*hasSideEffect*/true,
                                      /*isAlignStack*/false,
                                      InlineAsm::AD_Intel);

  LoadInst *TailLoad = IRB.CreateLoad(EncTcsGlobal);

  Constant *ConstEEnter = IRB.getInt64(ENCLU_EENTER);
  Value *FuncIndexConst = IRB.getInt64(FuncIndex);
  Value *FrameAsI8Ptr = IRB.CreateBitCast(FrameAlloc, Int8PtrTy);
  CallInst *EnterCall =
    IRB.CreateCall(EnterIA, {FuncIndexConst, FrameAsI8Ptr, TailLoad, EncEH, ConstEEnter});
  EnterCall->addAttribute(AttributeList::FunctionIndex, Attribute::NoUnwind);

  if (ReturnsVoid)
    IRB.CreateRetVoid();
  else {
    Value *RetValAddr = IRB.CreateStructGEP(nullptr, FrameAlloc, 0);

    Value *RetVal = IRB.CreateLoad(RetValAddr);
    IRB.CreateRet(RetVal);
  }

  Instruction *Then = SplitBlockAndInsertIfThen(TcsIsNull, TailLoad, false);

  IRB.SetInsertPoint(Then);

  IRB.CreateCall(EncInit, {IRB.CreateBitCast(&SecureAdapter, Int8PtrTy)});

  return SecureFuncAdapterInfo
  {
    FuncIndex,
    &F,
    InsecureAdapter,
    FrameTy,
  };
}

// The secure adapter is responsible for unpacking arguments, jumping to the correct
// secure function, and packing the returned value. It allows us to support arbitrary
// args and return values (secure functions can only support up to 2 args and no returns),
// and also allows us multiple entry points (OpenSGX only supports a single TCS).
// The secure adapter has code looking like the following:
// void SecureAdapter(int FuncIndex, char *Frame) {
//   switch (FuncIndex) {
//   case 0:
//     Func0FrameTy *Func0Frame = (Func0FrameTy *)Frame;
//     Func0Frame->Ret = Func0(Func0Frame->Arg1, Func0Frame->Arg2, ...);
//     break;
//   case 1:
//     Func1FrameTy *Func1Frame = (Func1FrameTy *)Frame;
//     Func1Frame->Ret = Func1(Func1Frame->Arg1, Func1Frame->Arg2, ...);
//     break;
//   }
// }
// Where a case is generated for each secure function called by insecure code.
// When an insecure adapter needs to enter its function, it uses EENTER with the
// proper index and a pointer to a (stack allocated) frame structure containing
// args and space for the return value.
void SGXStubify::fillSecureAdapter(Function *SecureAdapter,
                                   ArrayRef<SecureFuncAdapterInfo> Adapters) {
  if (Adapters.empty())
    return;

  SecureAdapter->setSection(SGX_SECURE_SECTION);
  SecureAdapter->addFnAttr(SGX_SECURE_ATTR);

  auto ArgIt = SecureAdapter->arg_begin();
  Value &FuncIndexArg = *ArgIt;
  ArgIt++;
  Value &FrameArg = *ArgIt;
  ArgIt++;

  BasicBlock *SwitchBB = BasicBlock::Create(*C, "", SecureAdapter);
  // We always need a default BB. Just mark it unreachable.
  BasicBlock *SwitchDefaultBB = BasicBlock::Create(*C, "", SecureAdapter);
  BasicBlock *EExitBB = BasicBlock::Create(*C, "", SecureAdapter);

  IRBuilder<> IRB(SwitchBB);
  SwitchInst *Switch = IRB.CreateSwitch(&FuncIndexArg, SwitchDefaultBB, Adapters.size());

  for (const auto& AI : Adapters) {
    BasicBlock *Case = BasicBlock::Create(*C, "", SecureAdapter);
    Switch->addCase(IRB.getInt64(AI.FuncIndex), Case);

    IRB.SetInsertPoint(Case);
    // Add this code:
    //   f->ret = SecureFunc(f->arg1, f->arg2, ...);
    //   EEXIT();
    SmallVector<Value *, 8> SecureToImplArgs;

    // Dereference all args from frame
    Type *RetTy = AI.OriginalFunction->getReturnType();
    for (unsigned i = 0; i < AI.OriginalFunction->getFunctionType()->getNumParams(); i++) {
      unsigned ArgIndex = RetTy == VoidTy ? i : (1 + i);
      Value *LoadedArgAddr = IRB.CreateStructGEP(AI.FrameTy, &FrameArg, ArgIndex);
      Value *LoadedArg = IRB.CreateLoad(LoadedArgAddr);
      SecureToImplArgs.push_back(LoadedArg);
    }

    CallInst *ImplCall = IRB.CreateCall(AI.OriginalFunction, SecureToImplArgs);
    // Store return into frame
    if (RetTy != VoidTy) {
      Value *RetValAddr = IRB.CreateStructGEP(AI.FrameTy, &FrameArg, 0);
      IRB.CreateStore(ImplCall, RetValAddr);
    }

    IRB.CreateBr(EExitBB);
  }

  IRB.SetInsertPoint(SwitchDefaultBB);
  IRB.CreateUnreachable();

  IRB.SetInsertPoint(EExitBB);
  FunctionType *ExitFTy = FunctionType::get(VoidTy, {Int64Ty, Int64Ty}, false);
  InlineAsm *ExitIA = InlineAsm::get(ExitFTy, "enclu",
                                     // Does not return, so no clobbers necessary
                                     "{bx},{ax}",
                                     /*hasSideEffect*/true,
                                     /*isAlignStack*/false,
                                     InlineAsm::AD_Intel);
  Constant *Const0 = IRB.getInt64(0);
  Constant *ConstEExit = IRB.getInt64(ENCLU_EEXIT);

  CallInst *ExitCall = IRB.CreateCall(ExitIA, {Const0, ConstEExit});
  ExitCall->addAttribute(AttributeList::FunctionIndex, Attribute::NoUnwind);
  ExitCall->addAttribute(AttributeList::FunctionIndex, Attribute::NoReturn);

  IRB.CreateUnreachable();
}

char SGXStubify::ID = 0;
INITIALIZE_PASS(
    SGXStubify, "sgx-stubify",
    "Stubify SGX secure functions", false, false)

Pass *llvm::createSGXStubifyPass() { return new SGXStubify(); }

