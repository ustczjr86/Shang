//===- Writer.cpp - VTM machine instructions to RTL verilog  ----*- C++ -*-===//
//
// Copyright: 2011 by SYSU EDA Group. all rights reserved.
// IMPORTANT: This software is supplied to you by Hongbin Zheng in consideration
// of your agreement to the following terms, and your use, installation,
// modification or redistribution of this software constitutes acceptance
// of these terms.  If you do not agree with these terms, please do not use,
// install, modify or redistribute this software. You may not redistribute,
// install copy or modify this software without written permission from
// Hongbin Zheng.
//
//===----------------------------------------------------------------------===//
//
// This file implement the VerilogASTBuilder pass, which write VTM machine
// instructions in form of RTL verilog code.
//
//===----------------------------------------------------------------------===//
#include "VASTExprBuilder.h"

#include "vtm/Passes.h"
#include "vtm/VFInfo.h"
#include "vtm/LangSteam.h"
#include "vtm/VRegisterInfo.h"
#include "vtm/VInstrInfo.h"
#include "vtm/Utilities.h"

#include "llvm/Constants.h"
#include "llvm/GlobalVariable.h"
#include "llvm/Type.h"
#include "llvm/Module.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Target/Mangler.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetData.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/SourceMgr.h"
#define DEBUG_TYPE "vtm-rtl-codegen"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(SlotsByPassed, "Number of slots are bypassed");

namespace {
struct MemBusBuilder {
  VASTModule *VM;
  VASTExprBuilder &Builder;
  VFUMemBus *Bus;
  unsigned BusNum;
  VASTWire *MembusEn, *MembusCmd, *MemBusAddr, *MemBusOutData, *MemBusByteEn;
  // Helper class to build the expression.
  VASTExprHelper EnExpr, CmdExpr, AddrExpr, OutDataExpr, BeExpr;

  VASTWire *createOutputPort(const std::string &PortName, unsigned BitWidth,
                              VASTRegister *&LocalEn, VASTExprHelper &Expr) {
    // We need to create multiplexer to allow current module and its submodules
    // share the bus.
    std::string PortReg = PortName + "_r";
    VASTRegister *LocalReg = VM->addRegister(PortReg, BitWidth);
    VASTPort *P = VM->addOutputPort(PortName, BitWidth, VASTModule::Others,
                                    false);
    VASTWire *OutputWire = cast<VASTWire>(P->get());
    // Are we creating the enable port?
    if (LocalEn == 0) {
      // Or all enables together to generate the enable output,
      // we use And Inverter Graph here.
      Expr.init(VASTExpr::dpAnd, OutputWire->getBitWidth(), true);
      // Add the local enable.
      assert(Expr.BuildNot && Expr.Opc == VASTExpr::dpAnd
             && "It is not building an Or Expr!");
      VASTValPtr V = Builder.buildNotExpr(LocalReg);
      Expr.addOperand(V);
      LocalEn = LocalReg;
    } else {
      Expr.init(VASTExpr::dpMux, OutputWire->getBitWidth());
      // Select the local signal if local enable is true.
      Expr.addOperand(LocalEn);
      Expr.addOperand(LocalReg);
    }

    return OutputWire;
  }

  void addSubModuleOutPort(raw_ostream &S, VASTWire *OutputWire,
                            unsigned BitWidth, const std::string &SubModuleName,
                            VASTWire *&SubModEn, VASTExprHelper &Expr) {
    std::string ConnectedWireName = SubModuleName + "_"
                                    + std::string(OutputWire->getName());

    VASTWire *SubModWire = VM->addWire(ConnectedWireName, BitWidth);

    // Are we creating the enable signal from sub module?
    if (SubModEn == 0) {
      // Or all enables together to generate the enable output.
      // we use And Inverter Graph here.
      assert(Expr.BuildNot && Expr.Opc == VASTExpr::dpAnd
             && "It is not building an Or Expr!");
      VASTValPtr V = Builder.buildNotExpr(SubModWire);
      Expr.addOperand(V);
      SubModEn = SubModWire;
    } else {
      // Select the signal from submodule if sub module enable is true.
      Expr.addOperand(SubModEn);
      Expr.addOperand(SubModWire);
    }

    // Write the connection.
    // The corresponding port name of submodule should be the same as current
    // output port name.
    S << '.' << OutputWire->getName() << '(' << ConnectedWireName << "),\n\t";
  }

  void addSubModuleInPort(raw_ostream &S, const std::string &PortName) {
    // Simply connect the input port to the corresponding port of submodule,
    // which suppose to have the same name.
    S << '.' << PortName << '(' <<  PortName << "),\n\t";
  }

  void addSubModule(const std::string &SubModuleName, raw_ostream &S) {
    VASTWire *SubModEn = 0;
    addSubModuleOutPort(S, MembusEn, 1,
                          SubModuleName, SubModEn, EnExpr);
    // Output ports.
    addSubModuleOutPort(S, MembusCmd,
                        VFUMemBus::CMDWidth, SubModuleName, SubModEn, CmdExpr);
    addSubModuleOutPort(S, MemBusAddr,
                        Bus->getAddrWidth(), SubModuleName, SubModEn,
                        AddrExpr);
    addSubModuleOutPort(S, MemBusOutData,
                        Bus->getDataWidth(), SubModuleName, SubModEn,
                        OutDataExpr);
    addSubModuleOutPort(S, MemBusByteEn,
                        Bus->getDataWidth()/8, SubModuleName, SubModEn,
                        BeExpr);

    // Input ports.
    addSubModuleInPort(S, VFUMemBus::getInDataBusName(BusNum));
    addSubModuleInPort(S, VFUMemBus::getReadyName(BusNum));
  }

  MemBusBuilder(VASTModule *VM, VASTExprBuilder &Builder, unsigned N)
    : VM(VM), Builder(Builder), Bus(getFUDesc<VFUMemBus>()), BusNum(N) {
    // Build the ports for current module.
    FuncUnitId ID(VFUs::MemoryBus, BusNum);
    // We need to create multiplexer to allow current module and its submodules
    // share the memory bus.
    VM->setFUPortBegin(ID);
    // The enable signal for local memory bus.
    VASTRegister *LocalEn = 0;
    // Control ports.
    MembusEn =
      createOutputPort(VFUMemBus::getEnableName(BusNum), 1, LocalEn, EnExpr);
    MembusCmd =
      createOutputPort(VFUMemBus::getCmdName(BusNum), VFUMemBus::CMDWidth,
                        LocalEn, CmdExpr);

    // Address port.
    MemBusAddr =
      createOutputPort(VFUMemBus::getAddrBusName(BusNum), Bus->getAddrWidth(),
                        LocalEn, AddrExpr);
    // Data ports.
    VM->addInputPort(VFUMemBus::getInDataBusName(BusNum), Bus->getDataWidth());
    MemBusOutData =
      createOutputPort(VFUMemBus::getOutDataBusName(BusNum),
                        Bus->getDataWidth(), LocalEn, OutDataExpr);
    // Byte enable.
    MemBusByteEn =
      createOutputPort(VFUMemBus::getByteEnableName(BusNum),
                        Bus->getDataWidth() / 8, LocalEn, BeExpr);
    // Bus ready.
    VM->addInputPort(VFUMemBus::getReadyName(BusNum), 1);
  }

  void buildMemBusMux() {
    VM->assign(MembusEn, Builder.buildExpr(EnExpr));
    VM->assign(MembusCmd, Builder.buildExpr(CmdExpr));
    VM->assign(MemBusAddr, Builder.buildExpr(AddrExpr));
    VM->assign(MemBusOutData, Builder.buildExpr(OutDataExpr));
    VM->assign(MemBusByteEn, Builder.buildExpr(BeExpr));
  }
};
class VerilogASTBuilder : public MachineFunctionPass,
                          public VASTExprBuilderContext {
  const Module *M;
  MachineFunction *MF;
  TargetData *TD;
  VRegisterInfo *TRI;
  VFInfo *FInfo;
  MachineRegisterInfo *MRI;
  VASTModule *VM;
  OwningPtr<VASTExprBuilder> Builder;
  MemBusBuilder *MBBuilder;
  StringSet<> EmittedSubModules;

  bool isSubModuleEmitted(StringRef Name) {
    return !EmittedSubModules.insert(Name);
  }

  VASTImmediate *getOrCreateImmediate(uint64_t Value, int8_t BitWidth) {
    return VM->getOrCreateImmediate(Value, BitWidth);
  }

  VASTValPtr createExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                        unsigned UB, unsigned LB) {
    return VM->createExpr(Opc, Ops, UB, LB);
  }

  typedef std::map<unsigned, VASTValPtr> RegIdxMapTy;
  RegIdxMapTy Idx2Reg;
  RegIdxMapTy Idx2Expr;

  // Keep the wires are single defined and CSEd.
  typedef std::map<VASTValPtr, VASTWire*> ExprLHSMapTy;
  ExprLHSMapTy ExprLHS;

  VASTValPtr lookupSignal(unsigned RegNum) {
    if (TargetRegisterInfo::isPhysicalRegister(RegNum)) {
      RegIdxMapTy::const_iterator at = Idx2Reg.find(RegNum);
      assert(at != Idx2Reg.end() && "Signal not found!");

      return at->second;
    }

    // Retrieve the expression.
    VASTValPtr Expr = getOrCreateExpr(RegNum);
    VASTExprPtr Ptr = dyn_cast<VASTExprPtr>(Expr);
    // If the expression is inlinalbe, do not create the wire.
    if (!Ptr || Ptr->isInlinable()) return Expr;

    // Try to get the wire.
    VASTWire *&LHSWire = ExprLHS[Expr];    
    if (LHSWire) return  LHSWire;

    // Create the LHS wire if it had not existed yet.
    assert(TargetRegisterInfo::isVirtualRegister(RegNum)
           && "Unexpected physics register as wire!");
    std::string Name =
      "w" + utostr_32(TargetRegisterInfo::virtReg2Index(RegNum)) + "w";

    return (LHSWire = VM->assign(VM->addWire(Name, Expr->getBitWidth()), Expr));
  }

  VASTWire *lookupWire(unsigned WireNum) const {
    assert(TargetRegisterInfo::isVirtualRegister(WireNum)
           && "Expect virtual register!");
    RegIdxMapTy::const_iterator at = Idx2Expr.find(WireNum);
    if(at == Idx2Expr.end()) return 0;
    
    ExprLHSMapTy::const_iterator wire_at = ExprLHS.find(at->second);
    if (wire_at == ExprLHS.end()) return 0;

    return wire_at->second;
  }

  VASTValPtr nameExpr(VASTValPtr V) {
    // Name the expression when necessary.
    if (isa<VASTNamedValue>(V.get()) && cast<VASTNamedValue>(V.get())->getName())
      return V;

    ExprLHSMapTy::iterator at = ExprLHS.find(V);
    if (at != ExprLHS.end()) return at->second;

    std::string Name = "e" + utohexstr(uint64_t(V.get())) + "w";
    // Try to create the temporary wire for the bitslice.
    if (VASTValue *V = VM->lookupSymbol(Name)) return V;

    return VM->assign(VM->addWire(Name, V->getBitWidth()), V);
  }

  VASTValPtr stripName(VASTValPtr V) const {
    // Try to get the underlying expression.
    if (VASTWirePtr Ptr = dyn_cast<VASTWire>(V)) {
      VASTExprPtr ExprPtr = Ptr.getExpr();
      if (ExprPtr.get()) return ExprPtr;
    }

    return V;
  }

 VASTValPtr getOrCreateExpr(unsigned WireNum, MachineInstr *MI = 0) {
    assert(TargetRegisterInfo::isVirtualRegister(WireNum)
           && "Expected virtual register!");
    VASTValPtr &Expr = Idx2Expr[WireNum];
    if (Expr) return Expr;

    // Build the expression if it had not existed yet.
    if (MI == 0) MI = MRI->getVRegDef(WireNum);
    assert(MI && "Virtual register for wire not defined!");
    return (Expr = emitDatapathExpr(MI));
  }

  VASTValPtr indexVASTRegister(unsigned RegNum, VASTValPtr V) {
    assert(TargetRegisterInfo::isPhysicalRegister(RegNum)
           && "Expect physical register!");
    bool inserted = Idx2Reg.insert(std::make_pair(RegNum, V)).second;
    assert(inserted && "RegNum already indexed some value!");

    return V;
  }

  VASTValPtr indexVASTWire(unsigned WireNum, VASTValPtr V) {
    assert(TargetRegisterInfo::isVirtualRegister(WireNum)
           && "Expect physical register!");
    bool inserted = Idx2Expr.insert(std::make_pair(WireNum, V)).second;
    assert(inserted && "RegNum already indexed some value!");

    return V;
  }

  VASTRegister *addDataRegister(unsigned RegNum, unsigned BitWidth,
                                const char *Attr = "") {
    std::string Name = "p" + utostr_32(RegNum) + "r";

    VASTRegister *R = VM->addDataRegister(Name, BitWidth, RegNum, Attr);
    indexVASTRegister(RegNum, R);
    return R;
  }

  VASTSlot *getInstrSlot(MachineInstr *MI) {
    unsigned SlotNum = VInstrInfo::getInstrSlotNum(MI);
    return VM->getSlot(SlotNum - 1);
  }

  VASTSlot *getOrCreateInstrSlot(MachineInstr *MI, unsigned ParentIdx) {
    unsigned SlotNum = VInstrInfo::getInstrSlotNum(MI);
    return VM->getOrCreateSlot(SlotNum - 1, MI->getParent());
  }

  void OrCnd(VASTUse &U, VASTValPtr Cnd) {
    if (U.isInvalid())  U.set(Cnd);
    else                U.replaceUseBy(Builder->buildOrExpr(Cnd, U, 1));
  }

  void addSuccSlot(VASTSlot *S, VASTSlot *NextSlot, VASTValPtr Cnd) {
    OrCnd(S->allocateSuccSlot(NextSlot, VM), Cnd);
  }

  void addSlotDisable(VASTSlot *S, VASTRegister *R, VASTValPtr Cnd) {
    OrCnd(S->allocateDisable(R, VM), Cnd);
  }

  void addSlotReady(VASTSlot *S, VASTValue *V, VASTValPtr Cnd) {
    OrCnd(S->allocateReady(V, VM), Cnd);
  }

  void addSlotEnable(VASTSlot *S, VASTRegister *R, VASTValPtr Cnd) {
    OrCnd(S->allocateEnable(R, VM), Cnd);
  }

  void emitFunctionSignature(const Function *F);
  void emitCommonPort(unsigned FNNum);
  void emitAllocatedFUs();
  void emitSubModule(StringRef CalleeName, unsigned FNNum);
  void emitIdleState();

  void emitBasicBlock(MachineBasicBlock &MBB);

  void emitAllSignals();
  VASTValPtr emitFUAdd(unsigned FUNum, unsigned BitWidth);
  VASTValPtr emitFUMult(unsigned FUNum, unsigned BitWidth, bool HasHi);
  VASTValPtr emitFUShift(unsigned FUNum, unsigned BitWidth,
                         VASTExpr::Opcode Opc);
  VASTValPtr emitFUCmp(unsigned FUNum, unsigned BitWidth, bool isSigned);

  // Mapping success fsm state to their predicate in current state.
  void emitCtrlOp(MachineBasicBlock::instr_iterator ctrl_begin,
                  MachineBasicBlock::instr_iterator ctrl_end,
                  unsigned II, bool Pipelined);

  MachineBasicBlock::iterator emitDatapath(MachineInstr *Bundle);
  VASTValPtr emitDatapathExpr(MachineInstr *MI);
  VASTValPtr emitUnaryOp(MachineInstr *MI, VASTExpr::Opcode Opc);
  VASTValPtr emitInvert(MachineInstr *MI);
  VASTValPtr emitReduceOr(MachineInstr *MI);
  template<typename FnTy>
  VASTValPtr emitBinaryOp(MachineInstr *MI, FnTy F);
  VASTValPtr emitOpLut(MachineInstr *MI);
  VASTValPtr emitOpSel(MachineInstr *MI);
  VASTValPtr emitChainedOpAdd(MachineInstr *MI);
  VASTValPtr emitChainedOpICmp(MachineInstr *MI);
  VASTValPtr emitOpBitSlice(MachineInstr *MI);


  typedef SmallVectorImpl<VASTValPtr> VASTValueVecTy;
  // Emit the operations in the first micro state in the FSM state when we are
  // jumping to it.
  // Return true if the first slot of DstBB is bypassed.
  bool emitFirstCtrlBundle(MachineBasicBlock *DstBB, VASTSlot *Slot,
                           VASTValueVecTy &Cnds);

  void emitBr(MachineInstr *MI, VASTSlot *CurSlot, VASTValueVecTy &Cnds,
              MachineBasicBlock *CurBB, bool Pipelined);

  void emitOpAdd(MachineInstr *MI, VASTSlot *Slot, VASTValueVecTy &Cnds);
  void emitBinaryFUOp(MachineInstr *MI, VASTSlot *Slot, VASTValueVecTy &Cnds);

  // Create a condition from a predicate operand.
  VASTValPtr createCnd(MachineOperand Op);

  VASTValPtr getAsOperand(MachineOperand &Op, bool GetAsInlineOperand = true);

  template <class Ty>
  Ty *getAsLValue(MachineOperand &Op) {
    assert(Op.isReg() && "Bad MO type for LValue!");
    if (VASTValPtr V = lookupSignal(Op.getReg())) {
      assert(!V.isInverted()
             && "Don't know how to handle inverted LValue at the moment!");
      return dyn_cast<Ty>(V);
    }

    return 0;
  }

  void printOperand(MachineOperand &Op, raw_ostream &OS);

  void emitOpInternalCall(MachineInstr *MI, VASTSlot *Slot, VASTValueVecTy &Cnds);
  void emitOpReadReturn(MachineInstr *MI, VASTSlot *Slot, VASTValueVecTy &Cnds);
  void emitOpUnreachable(MachineInstr *MI, VASTSlot *Slot, VASTValueVecTy &Cnds);
  void emitOpRetVal(MachineInstr *MI, VASTSlot *Slot, VASTValueVecTy &Cnds);
  void emitOpRet(MachineInstr *MIRet, VASTSlot *CurSlot, VASTValueVecTy &Cnds);
  void emitOpCopy(MachineInstr *MI, VASTSlot *Slot, VASTValueVecTy &Cnds);
  void emitOpReadFU(MachineInstr *MI, VASTSlot *Slot, VASTValueVecTy &Cnds);

  void addSlotReady(MachineInstr *MI, VASTSlot *S);

  void emitOpDisableFU(MachineInstr *MI, VASTSlot *Slot, VASTValueVecTy &Cnds);

  void emitOpMemTrans(MachineInstr *MI, VASTSlot *Slot, VASTValueVecTy &Cnds);
  void emitOpBRamTrans(MachineInstr *MI, VASTSlot *Slot, VASTValueVecTy &Cnds);

  std::string getSubModulePortName(unsigned FNNum,
                                   const std::string PortName) const {
    return "SubMod" + utostr(FNNum) + "_" + PortName;
  }

public:
  /// @name FunctionPass interface
  //{
  static char ID;

  VerilogASTBuilder() : MachineFunctionPass(ID) {
    initializeVerilogASTBuilderPass(*PassRegistry::getPassRegistry());
  }

  ~VerilogASTBuilder();

  bool doInitialization(Module &M);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  void releaseMemory() {
    Builder.reset();
    EmittedSubModules.clear();
    Idx2Reg.clear();
    Idx2Expr.clear();
    ExprLHS.clear();
  }

  bool runOnMachineFunction(MachineFunction &MF);

  virtual void print(raw_ostream &O, const Module *M) const;
  //}
};

}

//===----------------------------------------------------------------------===//
char VerilogASTBuilder::ID = 0;

Pass *llvm::createVerilogASTBuilderPass() {
  return new VerilogASTBuilder();
}

INITIALIZE_PASS_BEGIN(VerilogASTBuilder, "vtm-rtl-info-VerilogASTBuilder",
                      "Build RTL Verilog module for synthesised function.",
                      false, true)
INITIALIZE_PASS_END(VerilogASTBuilder, "vtm-rtl-info-VerilogASTBuilder",
                    "Build RTL Verilog module for synthesised function.",
                    false, true)

bool VerilogASTBuilder::doInitialization(Module &Mod) {
  M = &Mod;
  return false;
}

bool VerilogASTBuilder::runOnMachineFunction(MachineFunction &F) {
  MF = &F;
  TD = getAnalysisIfAvailable<TargetData>();
  FInfo = MF->getInfo<VFInfo>();
  MRI = &MF->getRegInfo();
  VM = FInfo->getRtlMod();
  TargetRegisterInfo *RegInfo
    = const_cast<TargetRegisterInfo*>(MF->getTarget().getRegisterInfo());
  TRI = reinterpret_cast<VRegisterInfo*>(RegInfo);
  Builder.reset(new VASTExprBuilder(*this));

  emitFunctionSignature(F.getFunction());

  // Note: Create the memory bus builder will add the input/output ports of the
  // memory bus implicitly. We should add these ports after function
  // "emitFunctionSignature" is called, which add some other ports that need to
  // be added before input/output ports of memory bus.
  MemBusBuilder MBB(VM, *Builder, 0);
  MBBuilder = &MBB;

  // Emit all function units then emit all register/wires because function units
  // may alias with registers.
  emitAllocatedFUs();
  emitAllSignals();

  // States of the control flow.
  emitIdleState();
  for (MachineFunction::iterator I = MF->begin(), E = MF->end(); I != E; ++I)
    emitBasicBlock(*I);

  // Build the mux for memory bus.
  MBBuilder->buildMemBusMux();

  // Building the Slot active signals.
  VM->buildSlotLogic(*Builder);

  // Release the context.
  releaseMemory();
  return false;
}

void VerilogASTBuilder::print(raw_ostream &O, const Module *M) const {

}

void VerilogASTBuilder::emitFunctionSignature(const Function *F) {
  raw_ostream &S = VM->getDataPathBuffer();
  unsigned FNNum = FInfo->getCalleeFNNum(F->getName());
  for (Function::const_arg_iterator I = F->arg_begin(), E = F->arg_end();
      I != E; ++I) {
    const Argument *Arg = I;
    std::string Name = Arg->getName();
    unsigned BitWidth = TD->getTypeSizeInBits(Arg->getType());
    // Add port declaration.
    if (FNNum == 0)
      VM->addInputPort(Name, BitWidth, VASTModule::ArgPort);
    else {
      std::string RegName = getSubModulePortName(FNNum, Name);
      VM->addRegister(RegName, BitWidth);
      S << "." << Name << '(' << RegName << "),\n\t";
    }
  }

  Type *RetTy = F->getReturnType();
  if (!RetTy->isVoidTy()) {
    assert(RetTy->isIntegerTy() && "Only support return integer now!");
    unsigned BitWidth = TD->getTypeSizeInBits(RetTy);
    if (FNNum == 0)
      VM->addOutputPort("return_value", BitWidth, VASTModule::RetPort);
    else {
      std::string WireName = getSubModulePortName(FNNum, "return_value");
      indexVASTRegister(FNNum, VM->addWire(WireName, BitWidth));
      S << ".return_value(" << WireName << "),\n\t";
    }
  }

  emitCommonPort(FNNum);
}

void VerilogASTBuilder::emitIdleState() {
  // The module is busy now
  MachineBasicBlock *EntryBB =  GraphTraits<MachineFunction*>::getEntryNode(MF);
  VASTSlot *IdleSlot = VM->getOrCreateSlot(0, 0);
  IdleSlot->buildReadyLogic(*VM, *Builder);
  VASTValue *StartPort = VM->getPort(VASTModule::Start);
  IdleSlot->addSuccSlot(IdleSlot, Builder->buildNotExpr(StartPort), VM);

  // Always Disable the finish signal.
  addSlotDisable(IdleSlot, cast<VASTRegister>(VM->getPort(VASTModule::Finish)),
                 VM->getBoolImmediate(true));
  SmallVector<VASTValPtr, 1> Cnds(1, StartPort);
  if (!emitFirstCtrlBundle(EntryBB, IdleSlot, Cnds)) {
    unsigned EntryStartSlot = FInfo->getStartSlotFor(EntryBB);
    addSuccSlot(IdleSlot, VM->getOrCreateSlot(EntryStartSlot, EntryBB),
                StartPort);
  }
}

void VerilogASTBuilder::emitBasicBlock(MachineBasicBlock &MBB) {
  unsigned startSlot = FInfo->getStartSlotFor(&MBB);
  unsigned IISlot = FInfo->getIISlotFor(&MBB);
  unsigned II = IISlot - startSlot;
  unsigned EndSlot = FInfo->getEndSlotFor(&MBB);
  // The alias slots of pipelined BB.
  SmallVector<VASTSlot*, 8> AliasSlots;
  typedef MachineBasicBlock::instr_iterator instr_it;
  typedef MachineBasicBlock::iterator it;
  it I = MBB.getFirstNonPHI();
  // Skip the first bundle, it already emitted by the predecessor bbs.
  ++I;

  // Build the Verilog AST.
  while(!I->isTerminator()) {
    // Emit the datepath of current state.
    I = emitDatapath(I);
    // We are assign the register at the previous slot of this slot, so the
    // datapath op with same slot can read the register schedule to this slot.
    unsigned stateSlot = VInstrInfo::getBundleSlot(I) - 1;

    // Collect slot ready signals.
    instr_it NextI = instr_it(I);
    while ((++NextI)->getOpcode() != VTM::CtrlEnd)
      if (NextI->getOpcode() == VTM::VOpReadFU)
        addSlotReady(NextI, getOrCreateInstrSlot(NextI, startSlot));

    // Create and collect the slots.
    VASTSlot *LeaderSlot = VM->getOrCreateSlot(stateSlot, &MBB);
    AliasSlots.push_back(LeaderSlot);
    // There will be alias slot if the BB is pipelined.
    if (startSlot + II < EndSlot) {
      LeaderSlot->setAliasSlots(stateSlot, EndSlot, II);
      for (unsigned slot = stateSlot + II; slot < EndSlot; slot += II) {
        VASTSlot *S = VM->getOrCreateSlot(slot, &MBB);
        S->setAliasSlots(stateSlot, EndSlot, II);
        AliasSlots.push_back(S);
      }
    }

    // Build the slot ready expression.
    while (!AliasSlots.empty())
      AliasSlots.pop_back_val()->buildReadyLogic(*VM, *Builder);

    // Emit the control operations.
    emitCtrlOp(instr_it(I), NextI, II, IISlot < EndSlot);
    I = it(llvm::next(NextI));
  }
}

void VerilogASTBuilder::addSlotReady(MachineInstr *MI, VASTSlot *S) {
  FuncUnitId Id = VInstrInfo::getPreboundFUId(MI);
  VASTValue *ReadyPort = 0;

  switch (Id.getFUType()) {
  case VFUs::MemoryBus:
    ReadyPort = VM->getSymbol(VFUMemBus::getReadyName(Id.getFUNum()));
    break;
  case VFUs::CalleeFN: {
    // The register representing the function unit is store in the src operand
    // of VOpReadFU.
    unsigned FNNum = MI->getOperand(1).getReg();
    ReadyPort = VM->getSymbol(getSubModulePortName(FNNum, "fin"));
    break;
  }
  default: return;
  }
  // TODO: Assert not in first slot.
  addSlotReady(S, ReadyPort, createCnd(*VInstrInfo::getPredOperand(MI)));
}

void VerilogASTBuilder::emitCommonPort(unsigned FNNum) {
  if (FNNum == 0) { // If F is current function.
    VM->addInputPort("clk", 1, VASTModule::Clk);
    VM->addInputPort("rstN", 1, VASTModule::RST);
    VM->addInputPort("start", 1, VASTModule::Start);
    VM->addOutputPort("fin", 1, VASTModule::Finish);
  } else { // It is a callee function, emit the signal for the sub module.
    std::string StartPortName = getSubModulePortName(FNNum, "start");
    indexVASTRegister(FNNum + 1, VM->addRegister(StartPortName, 1));
    std::string FinPortName = getSubModulePortName(FNNum, "fin");
    VM->addWire(FinPortName, 1);
    // Connect to the ports
    raw_ostream &S = VM->getDataPathBuffer();
    S << ".clk(clk),\n\t.rstN(rstN),\n\t";
    S << ".start(" << StartPortName << "),\n\t";
    S << ".fin(" <<FinPortName << ")";
  }
}

void VerilogASTBuilder::emitAllocatedFUs() {
  raw_ostream &S = VM->getDataPathBuffer();
  VFUBRAM *BlockRam = getFUDesc<VFUBRAM>();
  for (VFInfo::const_bram_iterator I = FInfo->bram_begin(), E = FInfo->bram_end();
       I != E; ++I) {
    const VFInfo::BRamInfo &Info = I->second;
    unsigned BramNum = Info.PhyRegNum;
    //const Value* Initializer = Info.Initializer;
    unsigned NumElem = Info.NumElem;
    unsigned AddrWidth = std::max(Log2_32_Ceil(NumElem), 1u);
    unsigned DataWidth = Info.ElemSizeInBytes * 8;
    std::string InitFilePath = "";
    if (const GlobalVariable *Initializer =
        dyn_cast_or_null<GlobalVariable>(Info.Initializer))
      InitFilePath = VBEMangle(Initializer->getName()) + "_init.txt";

    // Create the enable signal for bram.
    VM->addRegister(VFUBRAM::getEnableName(BramNum), 1);
    VM->addRegister(VFUBRAM::getWriteEnableName(BramNum), 1);
    VM->addRegister(VFUBRAM::getInDataBusName(BramNum), DataWidth);
    VM->addRegister(VFUBRAM::getAddrBusName(BramNum), AddrWidth);
    VASTWire *BRAMOut = VM->addWire(VFUBRAM::getOutDataBusName(BramNum),
                                    DataWidth);
    // Used in template.
    BRAMOut->Pin();
    S << "// Addrspace: " << I->first;
    if (Info.Initializer) S << *Info.Initializer;
    S << '\n';
    indexVASTRegister(BramNum, BRAMOut);
    // FIXME: Get the file name from the initializer name.
    S << BlockRam->generateCode(VM->getPortName(VASTModule::Clk), BramNum,
                                DataWidth, AddrWidth, InitFilePath)
      << '\n';
  }
}


void VerilogASTBuilder::emitSubModule(StringRef CalleeName, unsigned FNNum) {
  // Do not emit a submodule more than once.
  if (isSubModuleEmitted(CalleeName)) return;

  raw_ostream &S = VM->getDataPathBuffer();

  if (const Function *Callee = M->getFunction(CalleeName)) {
    if (!Callee->isDeclaration()) {
      S << getSynSetting(Callee->getName())->getModName() << ' '
        << CalleeName << "_inst" << "(\n\t";
      MBBuilder->addSubModule(getSubModulePortName(FNNum, "_inst"), S);
      emitFunctionSignature(Callee);
      S << ");\n";
      return;
    }
  }


  std::string Ports[5] = {
    VM->getPortName(VASTModule::Clk),
    VM->getPortName(VASTModule::RST),
    getSubModulePortName(FNNum, "start"),
    getSubModulePortName(FNNum, "fin"),
    getSubModulePortName(FNNum, "return_value")
  };
  // Else ask the constraint about how to instantiates this submodule.
  S << "// External module: " << CalleeName << '\n';
  S << VFUs::instantiatesModule(CalleeName, FNNum, Ports);

  // Add the start/finsh signal and return_value to the signal list.
  indexVASTRegister(FNNum + 1, VM->addRegister(Ports[2], 1));
  VM->getOrCreateSymbol(Ports[3], 1, false);
  unsigned RetPortIdx = FNNum;
  // Dose the submodule have a return port?
  VRegisterInfo::PhyRegInfo Info = TRI->getPhyRegInfo(RetPortIdx);
  if (Info.getBitWidth()) {
    SmallVector<VFUs::ModOpInfo, 4> OpInfo;
    unsigned Latency = VFUs::getModuleOperands(CalleeName, FNNum, OpInfo);

    if (Latency == 0) {
      VASTValPtr PortName = VM->getOrCreateSymbol(Ports[4],
        Info.getBitWidth(),
        false);
      indexVASTRegister(RetPortIdx, PortName);
      return;
    }

    VASTWire *ResultWire = VM->addWire(Ports[4], Info.getBitWidth());
    indexVASTRegister(RetPortIdx, ResultWire);

    SmallVector<VASTValPtr, 4> Ops;
    for (unsigned i = 0, e = OpInfo.size(); i < e; ++i)
      Ops.push_back(VM->addOpRegister(OpInfo[i].first, OpInfo[i].second, FNNum));

    VASTValPtr Expr = Builder->buildExpr(VASTExpr::dpBlackBox, Ops,
                                         Info.getBitWidth());
    VM->assignWithExtraDelay(ResultWire, Expr, Latency);
    return;
  }

  // Else do not has return port.
  indexVASTRegister(RetPortIdx, 0);
}


VASTValPtr VerilogASTBuilder::emitFUAdd(unsigned FUNum, unsigned BitWidth) {
  // Write the datapath for function unit.
  std::string ResultName = "addsub" + utostr_32(FUNum) + "o";
  unsigned OperandWidth = BitWidth - 1;
  VASTRegister *LHS = VM->addOpRegister(ResultName + "_a", OperandWidth, FUNum),
               *RHS = VM->addOpRegister(ResultName + "_b", OperandWidth, FUNum),
               *C = VM->addOpRegister(ResultName + "_c", 1, FUNum);
  return VM->assign(VM->addWire(ResultName, BitWidth),
                    Builder->buildExpr(VASTExpr::dpAdd, LHS, RHS, C, BitWidth));
}

VASTValPtr VerilogASTBuilder::emitFUMult(unsigned FUNum, unsigned BitWidth, bool HasHi){
  std::string ResultName = "mult" + utostr_32(FUNum) + "o";
  VASTWire *Result = VM->addWire(ResultName, BitWidth);

  // No need to include the high part is included in the operand register.
  unsigned OperandWidth = BitWidth;
  if (HasHi) OperandWidth /= 2;
  
  VASTRegister *LHS = VM->addOpRegister(ResultName + "_a", OperandWidth, FUNum),
               *RHS = VM->addOpRegister(ResultName + "_b", OperandWidth, FUNum);
  return VM->assign(Result,
                    Builder->buildExpr(VASTExpr::dpMul, LHS, RHS, BitWidth));
}

VASTValPtr VerilogASTBuilder::emitFUShift(unsigned FUNum, unsigned BitWidth,
                                   VASTExpr::Opcode Opc) {
  std::string ResultName = "shift" + utostr_32(FUNum) + "o";


  VASTRegister *LHS = VM->addOpRegister(ResultName + "_a", BitWidth, FUNum),
               *RHS = VM->addOpRegister(ResultName + "_b",
                                             Log2_32_Ceil(BitWidth), FUNum);
  return VM->assign(VM->addWire(ResultName, BitWidth),
                    Builder->buildExpr(Opc, LHS, RHS, BitWidth));
}

VASTValPtr VerilogASTBuilder::emitFUCmp(unsigned FUNum, unsigned BitWidth,
                                 bool isSigned) {
  std::string ResultName = "cmp" + utostr_32(FUNum) + "o";
  if (isSigned)  ResultName = "s" + ResultName;
  else           ResultName = "u" + ResultName;

  VASTRegister *LHS = VM->addOpRegister(ResultName + "_a", BitWidth, FUNum),
               *RHS = VM->addOpRegister(ResultName + "_b", BitWidth, FUNum);
  // Comparer have 4 output port.
  return VM->assign(VM->addWire(ResultName, 8),
                    Builder->buildExpr(isSigned ? VASTExpr::dpSCmp
                                                : VASTExpr::dpUCmp,
                                       LHS, RHS, 5));
}

void VerilogASTBuilder::emitAllSignals() {
  for (unsigned i = 0, e = TRI->num_phyreg(); i != e; ++i) {
    unsigned RegNum = i + 1;
    VRegisterInfo::PhyRegInfo Info = TRI->getPhyRegInfo(RegNum);
    if (!Info.isTopLevelReg(RegNum)
        // Sub-register for RCFNRegClass already handled in
        // emitFunctionSignature called by emitAllocatedFUs;
        && Info.getRegClass() != VTM::RCFNRegClassID) {
      VASTValPtr Parent = lookupSignal(Info.getParentRegister());
      indexVASTRegister(RegNum,
                        Builder->buildBitSliceExpr(Parent.getAsInlineOperand(),
                                                   Info.getUB(), Info.getLB()));
      continue;
    }

    switch (Info.getRegClass()) {
    case VTM::DRRegClassID:
      addDataRegister(RegNum, Info.getBitWidth());
      break;
    case VTM::RADDRegClassID:
      indexVASTRegister(RegNum, emitFUAdd(RegNum, Info.getBitWidth()));
      break;
    case VTM::RUCMPRegClassID:
      indexVASTRegister(RegNum, emitFUCmp(RegNum, Info.getBitWidth(), false));
      break;
    case VTM::RSCMPRegClassID:
      indexVASTRegister(RegNum, emitFUCmp(RegNum, Info.getBitWidth(), true));
      break;
    case VTM::RMULRegClassID:
      indexVASTRegister(RegNum, emitFUMult(RegNum, Info.getBitWidth(), false));
      break;
    case VTM::RMULLHRegClassID:
      indexVASTRegister(RegNum, emitFUMult(RegNum, Info.getBitWidth(), true));
      break;
    case VTM::RASRRegClassID: {
      VASTValPtr V = emitFUShift(RegNum, Info.getBitWidth(), VASTExpr::dpSRA);
      indexVASTRegister(RegNum, V);
      break;
    }
    case VTM::RLSRRegClassID:{
      VASTValPtr V = emitFUShift(RegNum, Info.getBitWidth(), VASTExpr::dpSRL);
      indexVASTRegister(RegNum, V);
      break;
    }
    case VTM::RSHLRegClassID:{
      VASTValPtr V = emitFUShift(RegNum, Info.getBitWidth(), VASTExpr::dpShl);
      indexVASTRegister(RegNum, V);
      break;
    }
    case VTM::RINFRegClassID: {
      // FIXME: Do not use such magic number!
      // The offset of data input port is 3
      unsigned DataInIdx = VM->getFUPortOf(FuncUnitId(VFUs::MemoryBus, 0)) + 3;
      VASTValue *V = VM->getPort(DataInIdx);
      indexVASTRegister(RegNum, V);
      break;
    }
    case VTM::RBRMRegClassID:
    case VTM::RCFNRegClassID:
      /*Nothing to do, it is allocated on the fly*/
      break;
    case VTM::RMUXRegClassID: {
      std::string Name = "dstmux" + utostr_32(RegNum) + "r";
      indexVASTRegister(RegNum, VM->addDataRegister(Name, Info.getBitWidth(),
                        RegNum));
      break;
    }
    default: llvm_unreachable("Unexpected register class!"); break;
    }
  }
}

VerilogASTBuilder::~VerilogASTBuilder() {}

//===----------------------------------------------------------------------===//
void VerilogASTBuilder::emitCtrlOp(MachineBasicBlock::instr_iterator ctrl_begin,
                                   MachineBasicBlock::instr_iterator ctrl_end,
                                   unsigned II, bool Pipelined) {
  MachineBasicBlock *CurBB = ctrl_begin->getParent();
  assert(ctrl_begin->getOpcode() == VTM::CtrlStart && "Expect control bundle!");
  SmallVector<VASTValPtr, 4> Cnds;

  typedef MachineBasicBlock::instr_iterator instr_it;
  for (instr_it I = llvm::next(ctrl_begin); I != ctrl_end; ++I) {
    MachineInstr *MI = I;

    VASTSlot *CurSlot = getInstrSlot(MI);
    assert(VInstrInfo::getInstrSlotNum(MI) !=
             FInfo->getStartSlotFor(CurSlot->getParentBB())
           && "Unexpected first slot!");

    Cnds.clear();
    Cnds.push_back(createCnd(*VInstrInfo::getPredOperand(MI)));

    // Emit the operations.
    switch (MI->getOpcode()) {
    case VTM::VOpDstMux:
    case VTM::VOpMoveArg:
    case VTM::VOpMove:
    case VTM::VOpMvPhi:
    case VTM::VOpMvPipe:        emitOpCopy(MI, CurSlot, Cnds);            break;
    case VTM::VOpAdd:           emitOpAdd(MI, CurSlot, Cnds);             break;
    case VTM::VOpICmp:
    case VTM::VOpMultLoHi:
    case VTM::VOpMult:
    case VTM::VOpSHL:
    case VTM::VOpSRL:
    case VTM::VOpSRA:           emitBinaryFUOp(MI, CurSlot, Cnds);        break;
    case VTM::VOpReadFU:        emitOpReadFU(MI, CurSlot, Cnds);          break;
    case VTM::VOpDisableFU:     emitOpDisableFU(MI, CurSlot, Cnds);       break;
    case VTM::VOpInternalCall:  emitOpInternalCall(MI, CurSlot, Cnds);    break;
    case VTM::VOpRetVal:        emitOpRetVal(MI, CurSlot, Cnds);          break;
    case VTM::VOpRet_nt:        emitOpRet(MI, CurSlot, Cnds);             break;
    case VTM::VOpMemTrans:      emitOpMemTrans(MI, CurSlot, Cnds);        break;
    case VTM::VOpBRAMTrans:     emitOpBRamTrans(MI, CurSlot, Cnds);       break;
    case VTM::VOpToState_nt: emitBr(MI, CurSlot, Cnds, CurBB, Pipelined); break;
    case VTM::VOpReadReturn:    emitOpReadReturn(MI, CurSlot, Cnds);      break;
    case VTM::VOpUnreachable:   emitOpUnreachable(MI, CurSlot, Cnds);     break;
    default:  assert(0 && "Unexpected opcode!");                          break;
    }
  }
}

bool VerilogASTBuilder::emitFirstCtrlBundle(MachineBasicBlock *DstBB,
                                            VASTSlot *Slot,
                                            VASTValueVecTy &Cnds) {
  // TODO: Emit PHINodes if necessary.
  MachineInstr *FirstBundle = DstBB->instr_begin();
  assert(FInfo->getStartSlotFor(DstBB) == VInstrInfo::getBundleSlot(FirstBundle)
         && FirstBundle->getOpcode() == VTM::CtrlStart && "Broken Slot!");
  bool Bypassed = false;

  typedef MachineBasicBlock::instr_iterator instr_it;
  instr_it I = FirstBundle;
  while ((++I)->isInsideBundle()) {
    MachineInstr *MI = I;

    switch (I->getOpcode()) {
    case VTM::VOpDstMux:
    case VTM::VOpMoveArg:
    case VTM::VOpMove:
    case VTM::VOpMvPhi:
    case VTM::COPY:             emitOpCopy(MI, Slot, Cnds);   break;
    case VTM::VOpDefPhi:                                      break;
    case VTM::CtrlEnd:          /*Not need to handle*/        break;
    case VTM::VOpToState_nt:
      emitBr(MI, Slot, Cnds, DstBB, false);
      ++SlotsByPassed;
      Bypassed = true;
      break;
    case VTM::VOpRetVal:        emitOpRetVal(MI, Slot, Cnds); break;
    case VTM::VOpInternalCall:  emitOpInternalCall(MI, Slot, Cnds);    break;
    case VTM::VOpRet_nt:
      emitOpRet(MI, Slot, Cnds);
      ++SlotsByPassed;
      Bypassed = true;
      break;
    case VTM::VOpMemTrans:      emitOpMemTrans(MI, Slot, Cnds);        break;
    case VTM::VOpBRAMTrans:     emitOpBRamTrans(MI, Slot, Cnds);       break;
    default:  llvm_unreachable("Unexpected opcode!");         break;
    }
  }

  return Bypassed;
}

void VerilogASTBuilder::emitBr(MachineInstr *MI, VASTSlot *CurSlot,
                               VASTValueVecTy &Cnds, MachineBasicBlock *CurBB,
                               bool Pipelined) {
  MachineOperand &CndOp = MI->getOperand(0);
  Cnds.push_back(createCnd(CndOp));

  MachineBasicBlock *TargetBB = MI->getOperand(1).getMBB();
  assert(VInstrInfo::getPredOperand(MI)->getReg() == 0 &&
    "Cannot handle predicated BrCnd");

  // Emit control operation for next state.
  if (TargetBB == CurBB && Pipelined)
    // The loop op of pipelined loop enable next slot explicitly.
    addSuccSlot(CurSlot, VM->getOrCreateNextSlot(CurSlot),
                VM->getBoolImmediate(true));

  // Emit the first micro state of the target state.
  if (!emitFirstCtrlBundle(TargetBB, CurSlot, Cnds)) {
    // Build the edge if the edge is not bypassed.
    unsigned TargetSlotNum = FInfo->getStartSlotFor(TargetBB);

    VASTSlot *TargetSlot = VM->getOrCreateSlot(TargetSlotNum, TargetBB);
    VASTValPtr Cnd = Builder->buildExpr(VASTExpr::dpAnd, Cnds, 1);
    addSuccSlot(CurSlot, TargetSlot, Cnd);
  }
}

void VerilogASTBuilder::emitOpUnreachable(MachineInstr *MI, VASTSlot *Slot,
                                          VASTValueVecTy &Cnds) {
  vlang_raw_ostream &OS = VM->getControlBlockBuffer();
  std::string PredStr;
  raw_string_ostream SS(PredStr);
  VASTRegister::printCondition(SS, Slot, Cnds);
  SS.flush();
  OS.if_begin(PredStr);
  OS << "$display(\"BAD BAD BAD BAD! Run to unreachable\");\n";
  OS << "$finish();\n";
  OS.exit_block();

  addSuccSlot(Slot, VM->getOrCreateSlot(0, 0),
              VM->getBoolImmediate(true));
}

void VerilogASTBuilder::emitOpAdd(MachineInstr *MI, VASTSlot *Slot,
                                  VASTValueVecTy &Cnds) {
  VASTWire *Result = getAsLValue<VASTWire>(MI->getOperand(0));
  assert(Result && "FU result port replaced?");
  VASTRegister *R = cast<VASTRegister>(Result->getExpr()->getOperand(0));
  VM->addAssignment(R, getAsOperand(MI->getOperand(1)), Slot, Cnds, *Builder);
  R = cast<VASTRegister>(Result->getExpr()->getOperand(1));
  VM->addAssignment(R, getAsOperand(MI->getOperand(2)), Slot, Cnds, *Builder);
  R = cast<VASTRegister>(Result->getExpr()->getOperand(2));
  VM->addAssignment(R, getAsOperand(MI->getOperand(3)), Slot, Cnds, *Builder);
}

void VerilogASTBuilder::emitBinaryFUOp(MachineInstr *MI, VASTSlot *Slot,
                                       VASTValueVecTy &Cnds) {
  VASTWirePtr Result = getAsLValue<VASTWire>(MI->getOperand(0));
  assert(Result && "FU result port replaced?");
  VASTRegister *R = cast<VASTRegister>(Result->getExpr()->getOperand(0));
  VM->addAssignment(R, getAsOperand(MI->getOperand(1)), Slot, Cnds, *Builder);
  R = cast<VASTRegister>(Result->getExpr()->getOperand(1));
  VM->addAssignment(R, getAsOperand(MI->getOperand(2)), Slot, Cnds, *Builder);
}

void VerilogASTBuilder::emitOpCopy(MachineInstr *MI, VASTSlot *Slot,
                                   VASTValueVecTy &Cnds) {
  MachineOperand &Dst = MI->getOperand(0), &Src = MI->getOperand(1);
  // Ignore the identical copy.
  if (Src.isReg() && Dst.getReg() == Src.getReg()) return;

  VASTRegister *R = getAsLValue<VASTRegister>(Dst);
  VM->addAssignment(R, getAsOperand(Src), Slot, Cnds, *Builder);
}

void VerilogASTBuilder::emitOpReadFU(MachineInstr *MI, VASTSlot *CurSlot,
                                     VASTValueVecTy &Cnds) {
  // The dst operand of ReadFU change to immediate if it is dead.
  if (MI->getOperand(0).isReg() && MI->getOperand(0).getReg())
    emitOpCopy(MI, CurSlot, Cnds);
}

void VerilogASTBuilder::emitOpDisableFU(MachineInstr *MI, VASTSlot *Slot,
                                        VASTValueVecTy &Cnds) {
  FuncUnitId Id = VInstrInfo::getPreboundFUId(MI);
  unsigned FUNum = Id.getFUNum();
  VASTValPtr EnablePort = 0;

  switch (Id.getFUType()) {
  case VFUs::MemoryBus:
    EnablePort = VM->getSymbol(VFUMemBus::getEnableName(FUNum) + "_r");
    break;
  case VFUs::CalleeFN:
    EnablePort =  lookupSignal(MI->getOperand(0).getReg() + 1);
    break;
  case VFUs::BRam:
    EnablePort =
      VM->getSymbol(VFUBRAM::getEnableName(MI->getOperand(0).getReg()));
    break;
  default:
    llvm_unreachable("Unexpected FU to disable!");
    break;
  }

  VASTValPtr Pred = Builder->buildExpr(VASTExpr::dpAnd, Cnds, 1);
  addSlotDisable(Slot, cast<VASTRegister>(EnablePort), Pred);
}

void VerilogASTBuilder::emitOpReadReturn(MachineInstr *MI, VASTSlot *Slot,
                                         VASTValueVecTy &Cnds) {
  VASTRegister *R = getAsLValue<VASTRegister>(MI->getOperand(0));
  // Dirty Hack: Do not trust the bitwidth information of the operand
  // representing the return port.
  VM->addAssignment(R, lookupSignal(MI->getOperand(1).getReg()), Slot,
                    Cnds, *Builder);
}

void VerilogASTBuilder::emitOpInternalCall(MachineInstr *MI, VASTSlot *Slot,
                                           VASTValueVecTy &Cnds) {
  // Assign input port to some register.
  const char *CalleeName = MI->getOperand(1).getSymbolName();
  unsigned FNNum = FInfo->getCalleeFNNum(CalleeName);

  // Emit the submodule on the fly.
  emitSubModule(CalleeName, FNNum);

  VASTValPtr Pred = Builder->buildAndExpr(Cnds, 1);

  std::string StartPortName = getSubModulePortName(FNNum, "start");
  VASTValPtr StartSignal = VM->getSymbol(StartPortName);
  addSlotEnable(Slot, cast<VASTRegister>(StartSignal), Pred);

  const Function *FN = M->getFunction(CalleeName);
  if (FN && !FN->isDeclaration()) {
    Function::const_arg_iterator ArgIt = FN->arg_begin();
    for (unsigned i = 0, e = FN->arg_size(); i != e; ++i) {
      VASTRegister *R =
        VM->getSymbol<VASTRegister>(getSubModulePortName(FNNum, ArgIt->getName()));
      VM->addAssignment(R, getAsOperand(MI->getOperand(4 + i)), Slot,
                        Cnds, *Builder);
      ++ArgIt;
    }
    return;
  }

  typedef PtrInvPair<VASTExpr> VASTExprPtr;
  // Is the function have latency information not captured by schedule?
  if (VASTWire *RetPort = getAsLValue<VASTWire>(MI->getOperand(0))) {
    if (VASTExpr *Expr = RetPort->getExpr().getAsLValue<VASTExpr>()) {
      for (unsigned i = 0, e = Expr->NumOps; i < e; ++i) {
        VASTRegister *R = cast<VASTRegister>(Expr->getOperand(i));
        VM->addAssignment(R, getAsOperand(MI->getOperand(4 + i)), Slot, Cnds,
                          *Builder);
      }
      return;
    }
  }

  // Else we had to write the control code to the control block.
  vlang_raw_ostream &OS = VM->getControlBlockBuffer();
  OS << "// Calling function: " << CalleeName << ";\n";
  std::string PredStr;
  raw_string_ostream SS(PredStr);
  VASTRegister::printCondition(SS, Slot, Cnds);
  SS.flush();

  OS.if_begin(PredStr);
  if (FN /*&& FN->isDeclaration()*/) {
    // Dirty Hack.
    // TODO: Extract these to some special instruction?
    OS << "$c(\"" << FN->getName() << "(\",";
    for (unsigned i = 4, e = MI->getNumOperands(); i != e; ++i) {
      MachineOperand &Operand = MI->getOperand(i);
      if (Operand.isReg() && (Operand.getReg() == 0 || Operand.isImplicit()))
        continue;

      if (i != 4) OS << ",\",\", ";

      // It is the format string?
      StringRef FmtStr;
      if (Operand.isGlobal()
          && getConstantStringInfo(Operand.getGlobal(), FmtStr)) {
        std::string s;
        raw_string_ostream SS(s);
        SS << '"';
        PrintEscapedString(FmtStr, SS);
        SS << '"';
        SS.flush();
        OS << '"';
        PrintEscapedString(s, OS);
        OS << '"';
        continue;
      }
      printOperand(Operand, OS);
    }

    OS << ", \");\""; // Enclose the c function call.
    OS << ");\n";
  } else {
    // Else ask the constraint about how to handle this call.
    SmallVector<std::string, 8> InPorts;
    std::string s;
    raw_string_ostream SS(s);
    for (unsigned i = 4, e = MI->getNumOperands(); i != e; ++i) {
      printOperand(MI->getOperand(i), SS);
      SS.flush();
      InPorts.push_back(SS.str());
      s.clear();
    }

    std::string Name = CalleeName;
    OS << VFUs::startModule(Name, FInfo->getCalleeFNNum(CalleeName), InPorts);
  }
  OS.exit_block();
}

void VerilogASTBuilder::emitOpRet(MachineInstr *MI, VASTSlot *CurSlot,
                                  VASTValueVecTy &Cnds) {
  // Go back to the idle slot.
  VASTValPtr Pred = Builder->buildAndExpr(Cnds, 1);
  addSuccSlot(CurSlot, VM->getOrCreateSlot(0, 0), Pred);
  addSlotEnable(CurSlot, cast<VASTRegister>(VM->getPort(VASTModule::Finish)),
                Pred);
}

void VerilogASTBuilder::emitOpRetVal(MachineInstr *MI, VASTSlot *Slot,
                                     VASTValueVecTy &Cnds) {
  VASTRegister &RetReg = cast<VASTRegister>(*VM->getRetPort());
  unsigned retChannel = MI->getOperand(1).getImm();
  assert(retChannel == 0 && "Only support Channel 0!");
  VM->addAssignment(&RetReg, getAsOperand(MI->getOperand(0)), Slot,
                    Cnds, *Builder);
}

void VerilogASTBuilder::emitOpMemTrans(MachineInstr *MI, VASTSlot *Slot,
                                       VASTValueVecTy &Cnds) {
  unsigned FUNum = VInstrInfo::getPreboundFUId(MI).getFUNum();

  // Emit Address.
  std::string RegName = VFUMemBus::getAddrBusName(FUNum) + "_r";
  VASTRegister *R = VM->getSymbol<VASTRegister>(RegName);
  VM->addAssignment(R, getAsOperand(MI->getOperand(1)), Slot, Cnds, *Builder);
  // Assign store data.
  RegName = VFUMemBus::getOutDataBusName(FUNum) + "_r";
  R = VM->getSymbol<VASTRegister>(RegName);
  VM->addAssignment(R, getAsOperand(MI->getOperand(2)), Slot, Cnds, *Builder);
  // And write enable.
  RegName = VFUMemBus::getCmdName(FUNum) + "_r";
  R = VM->getSymbol<VASTRegister>(RegName);
  VM->addAssignment(R, getAsOperand(MI->getOperand(3)), Slot, Cnds, *Builder);
  // The byte enable.
  RegName = VFUMemBus::getByteEnableName(FUNum) + "_r";
  R = VM->getSymbol<VASTRegister>(RegName);
  VM->addAssignment(R, getAsOperand(MI->getOperand(4)), Slot, Cnds, *Builder);

  // Remember we enabled the memory bus at this slot.
  std::string EnableName = VFUMemBus::getEnableName(FUNum) + "_r";
  VASTValPtr MemEn = VM->getSymbol(EnableName);
  VASTValPtr Pred = Builder->buildAndExpr(Cnds, 1);
  addSlotEnable(Slot, cast<VASTRegister>(MemEn), Pred);
}

void VerilogASTBuilder::emitOpBRamTrans(MachineInstr *MI, VASTSlot *Slot,
                                         VASTValueVecTy &Cnds) {
  unsigned FUNum = MI->getOperand(0).getReg();
  unsigned BRamID = MI->getOperand(5).getImm();
  unsigned SizeInBytes = FInfo->getBRamInfo(BRamID).ElemSizeInBytes;
  unsigned Alignment = Log2_32_Ceil(SizeInBytes);

  std::string RegName = VFUBRAM::getAddrBusName(FUNum);
  VASTRegister *R = VM->getSymbol<VASTRegister>(RegName);
  VASTValPtr Addr = getAsOperand(MI->getOperand(1));
  Addr = Builder->buildBitSliceExpr(Addr, Addr->getBitWidth(), Alignment);
  VM->addAssignment(R, Addr, Slot, Cnds, *Builder);
  // Assign store data.
  RegName = VFUBRAM::getInDataBusName(FUNum);
  R = VM->getSymbol<VASTRegister>(RegName);
  VM->addAssignment(R, getAsOperand(MI->getOperand(2)), Slot, Cnds, *Builder);
  // And write enable.
  RegName = VFUBRAM::getWriteEnableName(FUNum);
  R = VM->getSymbol<VASTRegister>(RegName);
  VM->addAssignment(R, getAsOperand(MI->getOperand(3)), Slot, Cnds, *Builder);
  // The byte enable.
  //RegName = VFUBRAM::getByteEnableName(FUNum) + "_r";
  //R = VM->getSymbol<VASTRegister>(RegName);
  //VM->addAssignment(R, getAsOperand(MI->getOperand(4)), Slot, Cnds);

  // Remember we enabled the memory bus at this slot.
  std::string EnableName = VFUBRAM::getEnableName(FUNum);
  VASTValPtr MemEn = VM->getSymbol(EnableName);

  VASTValPtr Pred = Builder->buildAndExpr(Cnds, 1);
  addSlotEnable(Slot, cast<VASTRegister>(MemEn), Pred);

  // Remember the output register of block ram is defined at next slot.
}

MachineBasicBlock::iterator
VerilogASTBuilder::emitDatapath(MachineInstr *Bundle) {
  typedef MachineBasicBlock::instr_iterator instr_it;
  assert(Bundle->getOpcode() == VTM::Datapath
         && "Expect data-path bundle start!");

  instr_it I = Bundle;
   while ((++I)->isInsideBundle())
    getOrCreateExpr(I->getOperand(0).getReg(), I);

  return I;
}

VASTValPtr VerilogASTBuilder::emitDatapathExpr(MachineInstr *MI) {
  switch (MI->getOpcode()) {
  case VTM::VOpBitSlice:  return emitOpBitSlice(MI);
  case VTM::VOpBitCat:
    return emitBinaryOp(MI, VASTExprBuilder::buildExpr<VASTExpr::dpBitCat>);
  case VTM::VOpBitRepeat:
    return emitBinaryOp(MI, VASTExprBuilder::buildExpr<VASTExpr::dpBitRepeat>);

  case VTM::VOpAdd_c:     return emitChainedOpAdd(MI);
  case VTM::VOpICmp_c:    return emitChainedOpICmp(MI);

  case VTM::VOpSHL_c:
    return emitBinaryOp(MI, VASTExprBuilder::buildExpr<VASTExpr::dpShl>);
  case VTM::VOpSRA_c:
    return emitBinaryOp(MI, VASTExprBuilder::buildExpr<VASTExpr::dpSRA>);
  case VTM::VOpSRL_c:
    return emitBinaryOp(MI, VASTExprBuilder::buildExpr<VASTExpr::dpSRL>);

  case VTM::VOpMultLoHi_c:
  case VTM::VOpMult_c:
    return emitBinaryOp(MI, VASTExprBuilder::buildExpr<VASTExpr::dpMul>);

  case VTM::VOpSel:       return emitOpSel(MI);

  case VTM::VOpLUT:       return emitOpLut(MI);

  case VTM::VOpXor:       return emitBinaryOp(MI, VASTExprBuilder::buildXor);
  case VTM::VOpAnd:
    return emitBinaryOp(MI, VASTExprBuilder::buildExpr<VASTExpr::dpAnd>);
  case VTM::VOpOr:        return emitBinaryOp(MI, VASTExprBuilder::buildOr);
  case VTM::VOpNot:       return emitInvert(MI);
  case VTM::VOpROr:       return emitReduceOr(MI);
  case VTM::VOpRAnd:      return emitUnaryOp(MI, VASTExpr::dpRAnd);
  case VTM::VOpRXor:      return emitUnaryOp(MI, VASTExpr::dpRXor);
  case VTM::VOpPipelineStage: return emitUnaryOp(MI, VASTExpr::dpAssign);
  default:  assert(0 && "Unexpected opcode!");    break;
  }

  return VASTValPtr(0);
}

template<typename FnTy>
VASTValPtr VerilogASTBuilder::emitBinaryOp(MachineInstr *MI, FnTy F) {
  return  F(getAsOperand(MI->getOperand(1)), getAsOperand(MI->getOperand(2)),
            VInstrInfo::getBitWidth(MI->getOperand(0)), Builder.get());
}

VASTValPtr VerilogASTBuilder::emitChainedOpAdd(MachineInstr *MI) {
  return Builder->buildExpr(VASTExpr::dpAdd,
                            getAsOperand(MI->getOperand(1)),
                            getAsOperand(MI->getOperand(2)),
                            getAsOperand(MI->getOperand(3)),
                            VInstrInfo::getBitWidth(MI->getOperand(0)));
}

VASTValPtr VerilogASTBuilder::emitChainedOpICmp(MachineInstr *MI) {
  unsigned CndCode = MI->getOperand(3).getImm();
  if (CndCode == VFUs::CmpSigned)
    return emitBinaryOp(MI, VASTExprBuilder::buildExpr<VASTExpr::dpSCmp>);

  // else
  return emitBinaryOp(MI, VASTExprBuilder::buildExpr<VASTExpr::dpUCmp>);
}

VASTValPtr VerilogASTBuilder::emitInvert(MachineInstr *MI) {
  return Builder->buildNotExpr(getAsOperand(MI->getOperand(1)));
}

VASTValPtr VerilogASTBuilder::emitReduceOr(MachineInstr *MI) {
  // A | B .. | Z = ~(~A & ~B ... & ~Z).
  VASTValPtr V = Builder->buildNotExpr(getAsOperand(MI->getOperand(1)));
  V = Builder->buildNotExpr(Builder->buildExpr(VASTExpr::dpRAnd, V, 1));
  return V;
}

VASTValPtr VerilogASTBuilder::emitUnaryOp(MachineInstr *MI,
                                          VASTExpr::Opcode Opc) {
  return Builder->buildExpr(Opc, getAsOperand(MI->getOperand(1)),
                            VInstrInfo::getBitWidth(MI->getOperand(0)));
}

VASTValPtr VerilogASTBuilder::emitOpSel(MachineInstr *MI) {
  VASTValPtr Ops[] = { createCnd(MI->getOperand(1)),
                       getAsOperand(MI->getOperand(2)),
                       getAsOperand(MI->getOperand(3)) };

  return Builder->buildExpr(VASTExpr::dpSel, Ops,
                            VInstrInfo::getBitWidth(MI->getOperand(0)));
}

VASTValPtr VerilogASTBuilder::emitOpLut(MachineInstr *MI) {
  unsigned SizeInBits = VInstrInfo::getBitWidth(MI->getOperand(0));

  SmallVector<VASTValPtr, 8> Operands;
  for (unsigned i = 4, e = MI->getNumOperands(); i < e; ++i)
    Operands.push_back(getAsOperand(MI->getOperand(i)));
  unsigned NumInputs = Operands.size();

  // Interpret the sum of product table.
  const char *p = MI->getOperand(1).getSymbolName();
  SmallVector<VASTValPtr, 8> ProductOps, SumOps;
  bool isComplement = false;
  
  while (*p) {
    // Interpret the product.
    ProductOps.clear();
    for (unsigned i = 0; i < NumInputs; ++i) {
      char c = *p++;
      switch (c) {
      default: llvm_unreachable("Unexpected SOP char!");
      case '-': /*Dont care*/ break;
      case '1': ProductOps.push_back(Operands[i]); break;
      case '0':
        ProductOps.push_back(Builder->buildNotExpr(Operands[i]));
        break;
      }
    }

    // Inputs and outputs are seperated by blank space.
    assert(*p == ' ' && "Expect the blank space!");
    ++p;

    // Create the product.
    // Add the product to the operand list of the sum.
    SumOps.push_back(Builder->buildAndExpr(ProductOps, SizeInBits));

    // Is the output inverted?
    char c = *p++;
    assert((c == '0' || c == '1') && "Unexpected SOP char!");
    isComplement = (c == '0');

    // Products are separated by new line.
    assert(*p == '\n' && "Expect the new line!");
    ++p;
  }

  // Or the products together to build the SOP (Sum of Product).
  VASTValPtr SOP = Builder->buildOrExpr(SumOps, SizeInBits);

  if (isComplement) SOP = Builder->buildNotExpr(SOP);

  // Build the sum;
  return SOP;
}

VASTValPtr VerilogASTBuilder::emitOpBitSlice(MachineInstr *MI) {
  // Get the range of the bit slice, Note that the
  // bit at upper bound is excluded in VOpBitSlice
  unsigned UB = MI->getOperand(2).getImm(),
           LB = MI->getOperand(3).getImm();

  // RHS should be a register.
  MachineOperand &MO = MI->getOperand(1);
  return Builder->buildBitSliceExpr(getAsOperand(MO), UB, LB);
}


VASTValPtr VerilogASTBuilder::createCnd(MachineOperand Op) {
  // Is there an always true predicate?
  if (VInstrInfo::isAlwaysTruePred(Op)) return VM->getBoolImmediate(true);

  bool isInverted = VInstrInfo::isPredicateInverted(Op);
  // Fix the bitwidth, the bitwidth of condition is always 1.
  VInstrInfo::setBitWidth(Op, 1);

  // Otherwise it must be some signal.
  VASTValPtr C = getAsOperand(Op);

  if (isInverted) C = Builder->buildNotExpr(C);

  return C;
}

static void printOperandImpl(raw_ostream &OS, const MachineOperand &MO,
                             unsigned UB = 64, unsigned LB = 0) {
  switch (MO.getType()) {
  case MachineOperand::MO_ExternalSymbol:
    UB = std::min(VInstrInfo::getBitWidth(MO), UB);
    OS << MO.getSymbolName();
    OS << VASTValue::printBitRange(UB, LB, VInstrInfo::getBitWidth(MO) != 1);
    return;
  case MachineOperand::MO_GlobalAddress:
    OS << "(`gv" << VBEMangle(MO.getGlobal()->getName());
    if (int64_t Offset = MO.getOffset())
      OS  << " + " << Offset;
    OS << ')';
    return;
  default: break;
  }

  MO.print(OS);
}

VASTValPtr VerilogASTBuilder::getAsOperand(MachineOperand &Op,
                                           bool GetAsInlineOperand) {
  unsigned BitWidth = VInstrInfo::getBitWidth(Op);
  switch (Op.getType()) {
  case MachineOperand::MO_Register: {
    if (unsigned Reg = Op.getReg())
      if (VASTValPtr V = lookupSignal(Reg)) {
        // The operand may only use a sub bitslice of the signal.
        V = Builder->buildBitSliceExpr(V, BitWidth, 0);
        // Try to inline the operand.
        if (GetAsInlineOperand) V = V.getAsInlineOperand();
        return V;
      }
    return 0;
  }
  case MachineOperand::MO_Immediate:
    return VM->getOrCreateImmediate(Op.getImm(), BitWidth);
  default: break;
  }

  // DirtyHack: simply create a symbol.
  std::string Name;
  raw_string_ostream SS(Name);
  printOperandImpl(SS, Op);
  SS.flush();

  bool NeedWrapper = false;
  unsigned SymbolWidth = 0;
  if (Op.isGlobal()) { // GlobalValues are addresses.
    if (Op.getGlobal()->getType()->getAddressSpace())
      // Not in generic address space, this is the base address of block rams.
      // The base address is 0 as we do not merge block ram at the moment.
      return VM->getOrCreateImmediate(0 + Op.getOffset(), BitWidth);

    SymbolWidth = std::max(TD->getPointerSizeInBits(), BitWidth);
    NeedWrapper = true;
  }

  VASTValPtr Symbol = VM->getOrCreateSymbol(Name, SymbolWidth, NeedWrapper);
  if (SymbolWidth && GetAsInlineOperand)
    Symbol = Builder->buildBitSliceExpr(Symbol, BitWidth, 0).getAsInlineOperand();

  return Symbol;
}

void VerilogASTBuilder::printOperand(MachineOperand &Op, raw_ostream &OS) {
  if(Op.isReg() || Op.isImm()){
    VASTValPtr U = getAsOperand(Op);
    U.printAsOperand(OS);
    //U.PinUser();
    return;
  }

  printOperandImpl(OS, Op);
}
