//===- Attributor.h --- Module-wide attribute deduction ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Attributor: An inter procedural (abstract) "attribute" deduction framework.
//
// The Attributor framework is an inter procedural abstract analysis (fixpoint
// iteration analysis). The goal is to allow easy deduction of new attributes as
// well as information exchange between abstract attributes in-flight.
//
// The Attributor class is the driver and the link between the various abstract
// attributes. The Attributor will iterate until a fixpoint state is reached by
// all abstract attributes in-flight, or until it will enforce a pessimistic fix
// point because an iteration limit is reached.
//
// Abstract attributes, derived from the AbstractAttribute class, actually
// describe properties of the code. They can correspond to actual LLVM-IR
// attributes, or they can be more general, ultimately unrelated to LLVM-IR
// attributes. The latter is useful when an abstract attributes provides
// information to other abstract attributes in-flight but we might not want to
// manifest the information. The Attributor allows to query in-flight abstract
// attributes through the `Attributor::getAAFor` method (see the method
// description for an example). If the method is used by an abstract attribute
// P, and it results in an abstract attribute Q, the Attributor will
// automatically capture a potential dependence from Q to P. This dependence
// will cause P to be reevaluated whenever Q changes in the future.
//
// The Attributor will only reevaluated abstract attributes that might have
// changed since the last iteration. That means that the Attribute will not
// revisit all instructions/blocks/functions in the module but only query
// an update from a subset of the abstract attributes.
//
// The update method `AbstractAttribute::updateImpl` is implemented by the
// specific "abstract attribute" subclasses. The method is invoked whenever the
// currently assumed state (see the AbstractState class) might not be valid
// anymore. This can, for example, happen if the state was dependent on another
// abstract attribute that changed. In every invocation, the update method has
// to adjust the internal state of an abstract attribute to a point that is
// justifiable by the underlying IR and the current state of abstract attributes
// in-flight. Since the IR is given and assumed to be valid, the information
// derived from it can be assumed to hold. However, information derived from
// other abstract attributes is conditional on various things. If the justifying
// state changed, the `updateImpl` has to revisit the situation and potentially
// find another justification or limit the optimistic assumes made.
//
// Change is the key in this framework. Until a state of no-change, thus a
// fixpoint, is reached, the Attributor will query the abstract attributes
// in-flight to re-evaluate their state. If the (current) state is too
// optimistic, hence it cannot be justified anymore through other abstract
// attributes or the state of the IR, the state of the abstract attribute will
// have to change. Generally, we assume abstract attribute state to be a finite
// height lattice and the update function to be monotone. However, these
// conditions are not enforced because the iteration limit will guarantee
// termination. If an optimistic fixpoint is reached, or a pessimistic fix
// point is enforced after a timeout, the abstract attributes are tasked to
// manifest their result in the IR for passes to come.
//
// Attribute manifestation is not mandatory. If desired, there is support to
// generate a single or multiple LLVM-IR attributes already in the helper struct
// IRAttribute. In the simplest case, a subclass inherits from IRAttribute with
// a proper Attribute::AttrKind as template parameter. The Attributor
// manifestation framework will then create and place a new attribute if it is
// allowed to do so (based on the abstract state). Other use cases can be
// achieved by overloading AbstractAttribute or IRAttribute methods.
//
//
// The "mechanics" of adding a new "abstract attribute":
// - Define a class (transitively) inheriting from AbstractAttribute and one
//   (which could be the same) that (transitively) inherits from AbstractState.
//   For the latter, consider the already available BooleanState and
//   IntegerState if they fit your needs, e.g., you require only a bit-encoding.
// - Implement all pure methods. Also use overloading if the attribute is not
//   conforming with the "default" behavior: A (set of) LLVM-IR attribute(s) for
//   an argument, call site argument, function return value, or function. See
//   the class and method descriptions for more information on the two
//   "Abstract" classes and their respective methods.
// - Register opportunities for the new abstract attribute in the
//   `Attributor::identifyDefaultAbstractAttributes` method if it should be
//   counted as a 'default' attribute.
// - Add sufficient tests.
// - Add a Statistics object for bookkeeping. If it is a simple (set of)
//   attribute(s) manifested through the Attributor manifestation framework, see
//   the bookkeeping function in Attributor.cpp.
// - If instructions with a certain opcode are interesting to the attribute, add
//   that opcode to the switch in `Attributor::identifyAbstractAttributes`. This
//   will make it possible to query all those instructions through the
//   `InformationCache::getOpcodeInstMapForFunction` interface and eliminate the
//   need to traverse the IR repeatedly.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_IPO_ATTRIBUTOR_H
#define LLVM_TRANSFORMS_IPO_ATTRIBUTOR_H

#include "llvm/ADT/SetVector.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

struct AbstractAttribute;
struct InformationCache;
struct AAIsDead;

class Function;

/// Simple enum class that forces the status to be spelled out explicitly.
///
///{
enum class ChangeStatus {
  CHANGED,
  UNCHANGED,
};

ChangeStatus operator|(ChangeStatus l, ChangeStatus r);
ChangeStatus operator&(ChangeStatus l, ChangeStatus r);
///}

/// Helper to describe and deal with positions in the LLVM-IR.
///
/// A position in the IR is described by an anchor value and an "offset" that
/// could be the argument number, for call sites and arguments, or an indicator
/// of the "position kind". The kinds, specified in the Kind enum below, include
/// the locations in the attribute list, i.a., function scope and return value,
/// as well as a distinction between call sites and functions. Finally, there
/// are floating values that do not have a corresponding attribute list
/// position.
struct IRPosition {
  virtual ~IRPosition() {}

  /// The positions we distinguish in the IR.
  ///
  /// The values are chosen such that the KindOrArgNo member has a value >= 1
  /// if it is an argument or call site argument while a value < 1 indicates the
  /// respective kind of that value.
  enum Kind : int {
    IRP_INVALID = -6, ///< An invalid position.
    IRP_FLOAT = -5, ///< A position that is not associated with a spot suitable
                    ///< for attributes. This could be any value or instruction.
    IRP_RETURNED = -4, ///< An attribute for the function return value.
    IRP_CALL_SITE_RETURNED = -3, ///< An attribute for a call site return value.
    IRP_FUNCTION = -2,           ///< An attribute for a function (scope).
    IRP_CALL_SITE = -1, ///< An attribute for a call site (function scope).
    IRP_ARGUMENT = 0,   ///< An attribute for a function argument.
    IRP_CALL_SITE_ARGUMENT = 1, ///< An attribute for a call site argument.
  };

  /// Default constructor available to create invalid positions implicitly. All
  /// other positions need to be created explicitly through the appropriate
  /// static member function.
  IRPosition() : AnchorVal(nullptr), KindOrArgNo(IRP_INVALID) { verify(); }

  /// Create a position describing the value of \p V.
  static const IRPosition value(const Value &V) {
    if (auto *Arg = dyn_cast<Argument>(&V))
      return IRPosition::argument(*Arg);
    if (auto *CB = dyn_cast<CallBase>(&V))
      return IRPosition::callsite_returned(*CB);
    return IRPosition(const_cast<Value &>(V), IRP_FLOAT);
  }

  /// Create a position describing the function scope of \p F.
  static const IRPosition function(const Function &F) {
    return IRPosition(const_cast<Function &>(F), IRP_FUNCTION);
  }

  /// Create a position describing the returned value of \p F.
  static const IRPosition returned(const Function &F) {
    return IRPosition(const_cast<Function &>(F), IRP_RETURNED);
  }

  /// Create a position describing the argument \p Arg.
  static const IRPosition argument(const Argument &Arg) {
    return IRPosition(const_cast<Argument &>(Arg), Kind(Arg.getArgNo()));
  }

  /// Create a position describing the function scope of \p CB.
  static const IRPosition callsite_function(const CallBase &CB) {
    return IRPosition(const_cast<CallBase &>(CB), IRP_CALL_SITE);
  }

  /// Create a position describing the returned value of \p CB.
  static const IRPosition callsite_returned(const CallBase &CB) {
    return IRPosition(const_cast<CallBase &>(CB), IRP_CALL_SITE_RETURNED);
  }

  /// Create a position describing the argument of \p CB at position \p ArgNo.
  static const IRPosition callsite_argument(const CallBase &CB,
                                            unsigned ArgNo) {
    return IRPosition(const_cast<CallBase &>(CB), Kind(ArgNo));
  }

  /// Create a position describing the function scope of \p ICS.
  static const IRPosition callsite_function(ImmutableCallSite ICS) {
    return IRPosition::callsite_function(cast<CallBase>(*ICS.getInstruction()));
  }

  /// Create a position describing the returned value of \p ICS.
  static const IRPosition callsite_returned(ImmutableCallSite ICS) {
    return IRPosition::callsite_returned(cast<CallBase>(*ICS.getInstruction()));
  }

  /// Create a position describing the argument of \p ICS at position \p ArgNo.
  static const IRPosition callsite_argument(ImmutableCallSite ICS,
                                            unsigned ArgNo) {
    return IRPosition::callsite_argument(cast<CallBase>(*ICS.getInstruction()),
                                         ArgNo);
  }

  /// Create a position with function scope matching the "context" of \p IRP.
  /// If \p IRP is a call site (see isAnyCallSitePosition()) then the result
  /// will be a call site position, otherwise the function position of the
  /// associated function.
  static const IRPosition function_scope(const IRPosition &IRP) {
    if (IRP.isAnyCallSitePosition()) {
      return IRPosition::callsite_function(
          cast<CallBase>(IRP.getAnchorValue()));
    }
    assert(IRP.getAssociatedFunction());
    return IRPosition::function(*IRP.getAssociatedFunction());
  }

  bool operator==(const IRPosition &RHS) const {
    return (AnchorVal == RHS.AnchorVal) && (KindOrArgNo == RHS.KindOrArgNo);
  }
  bool operator!=(const IRPosition &RHS) const { return !(*this == RHS); }

  /// Return the value this abstract attribute is anchored with.
  ///
  /// The anchor value might not be the associated value if the latter is not
  /// sufficient to determine where arguments will be manifested. This is, so
  /// far, only the case for call site arguments as the value is not sufficient
  /// to pinpoint them. Instead, we can use the call site as an anchor.
  ///
  ///{
  Value &getAnchorValue() {
    assert(KindOrArgNo != IRP_INVALID &&
           "Invalid position does not have an anchor value!");
    return *AnchorVal;
  }
  const Value &getAnchorValue() const {
    return const_cast<IRPosition *>(this)->getAnchorValue();
  }
  ///}

  /// Return the associated function, if any.
  ///
  ///{
  Function *getAssociatedFunction() {
    if (auto *CB = dyn_cast<CallBase>(AnchorVal))
      return CB->getCalledFunction();
    assert(KindOrArgNo != IRP_INVALID &&
           "Invalid position does not have an anchor scope!");
    Value &V = getAnchorValue();
    if (isa<Function>(V))
      return &cast<Function>(V);
    if (isa<Argument>(V))
      return cast<Argument>(V).getParent();
    if (isa<Instruction>(V))
      return cast<Instruction>(V).getFunction();
    return nullptr;
  }
  const Function *getAssociatedFunction() const {
    return const_cast<IRPosition *>(this)->getAssociatedFunction();
  }
  ///}

  /// Return the Function surrounding the anchor value.
  ///
  ///{
  Function *getAnchorScope() {
    Value &V = getAnchorValue();
    if (isa<Function>(V))
      return &cast<Function>(V);
    if (isa<Argument>(V))
      return cast<Argument>(V).getParent();
    if (isa<Instruction>(V))
      return cast<Instruction>(V).getFunction();
    return nullptr;
  }
  const Function *getAnchorScope() const {
    return const_cast<IRPosition *>(this)->getAnchorScope();
  }
  ///}

  /// Return the context instruction, if any.
  ///
  ///{
  Instruction *getCtxI() {
    Value &V = getAnchorValue();
    if (auto *I = dyn_cast<Instruction>(&V))
      return I;
    if (auto *Arg = dyn_cast<Argument>(&V))
      if (!Arg->getParent()->isDeclaration())
        return &Arg->getParent()->getEntryBlock().front();
    return nullptr;
  }
  const Instruction *getCtxI() const {
    return const_cast<IRPosition *>(this)->getCtxI();
  }
  ///}

  /// Return the value this abstract attribute is associated with.
  ///
  ///{
  Value &getAssociatedValue() {
    assert(KindOrArgNo != IRP_INVALID &&
           "Invalid position does not have an associated value!");
    if (getArgNo() < 0 || isa<Argument>(AnchorVal))
      return *AnchorVal;
    assert(isa<CallBase>(AnchorVal) && "Expected a call base!");
    return *cast<CallBase>(AnchorVal)->getArgOperand(getArgNo());
  }
  const Value &getAssociatedValue() const {
    return const_cast<IRPosition *>(this)->getAssociatedValue();
  }
  ///}

  /// Return the argument number of the associated value if it is an argument or
  /// call site argument, otherwise a negative value.
  int getArgNo() const { return KindOrArgNo; }

  /// Return the index in the attribute list for this position.
  unsigned getAttrIdx() const {
    switch (getPositionKind()) {
    case IRPosition::IRP_INVALID:
    case IRPosition::IRP_FLOAT:
      break;
    case IRPosition::IRP_FUNCTION:
    case IRPosition::IRP_CALL_SITE:
      return AttributeList::FunctionIndex;
    case IRPosition::IRP_RETURNED:
    case IRPosition::IRP_CALL_SITE_RETURNED:
      return AttributeList::ReturnIndex;
    case IRPosition::IRP_ARGUMENT:
    case IRPosition::IRP_CALL_SITE_ARGUMENT:
      return KindOrArgNo + AttributeList::FirstArgIndex;
    }
    llvm_unreachable(
        "There is no attribute index for a floating or invalid position!");
  }

  /// Return the associated position kind.
  Kind getPositionKind() const {
    if (getArgNo() >= 0) {
      assert(((isa<Argument>(getAnchorValue()) &&
               isa<Argument>(getAssociatedValue())) ||
              isa<CallBase>(getAnchorValue())) &&
             "Expected argument or call base due to argument number!");
      if (isa<CallBase>(getAnchorValue()))
        return IRP_CALL_SITE_ARGUMENT;
      return IRP_ARGUMENT;
    }

    assert(KindOrArgNo < 0 &&
           "Expected (call site) arguments to never reach this point!");
    assert(!isa<Argument>(getAnchorValue()) &&
           "Expected arguments to have an associated argument position!");
    return Kind(KindOrArgNo);
  }

  /// TODO: Figure out if the attribute related helper functions should live
  ///       here or somewhere else.

  /// Return true if any kind in \p AKs existing in the IR at a position that
  /// will affect this one. See also getAttrs(...).
  bool hasAttr(ArrayRef<Attribute::AttrKind> AKs) const;

  /// Return the attributes of any kind in \p AKs existing in the IR at a
  /// position that will affect this one. While each position can only have a
  /// single attribute of any kind in \p AKs, there are "subsuming" positions
  /// that could have an attribute as well. This method returns all attributes
  /// found in \p Attrs.
  void getAttrs(ArrayRef<Attribute::AttrKind> AKs,
                SmallVectorImpl<Attribute> &Attrs) const;

  /// Return the attribute of kind \p AK existing in the IR at this position.
  Attribute getAttr(Attribute::AttrKind AK) const {
    if (getPositionKind() == IRP_INVALID || getPositionKind() == IRP_FLOAT)
      return Attribute();

    AttributeList AttrList;
    if (ImmutableCallSite ICS = ImmutableCallSite(&getAnchorValue()))
      AttrList = ICS.getAttributes();
    else
      AttrList = getAssociatedFunction()->getAttributes();

    if (AttrList.hasAttribute(getAttrIdx(), AK))
      return AttrList.getAttribute(getAttrIdx(), AK);
    return Attribute();
  }

  bool isAnyCallSitePosition() const {
    switch (getPositionKind()) {
    case IRPosition::IRP_CALL_SITE:
    case IRPosition::IRP_CALL_SITE_RETURNED:
    case IRPosition::IRP_CALL_SITE_ARGUMENT:
      return true;
    default:
      return false;
    }
  }

  /// Special DenseMap key values.
  ///
  ///{
  static const IRPosition EmptyKey;
  static const IRPosition TombstoneKey;
  ///}

private:
  /// Private constructor for special values only!
  explicit IRPosition(int KindOrArgNo)
      : AnchorVal(0), KindOrArgNo(KindOrArgNo) {}

  /// IRPosition anchored at \p AnchorVal with kind/argument numbet \p PK.
  explicit IRPosition(Value &AnchorVal, Kind PK)
      : AnchorVal(&AnchorVal), KindOrArgNo(PK) {
    verify();
  }

  /// Verify internal invariants.
  void verify();

  /// The value this position is anchored at.
  Value *AnchorVal;

  /// The argument number, if non-negative, or the position "kind".
  int KindOrArgNo;
};

/// Helper that allows IRPosition as a key in a DenseMap.
template <> struct DenseMapInfo<IRPosition> {
  static inline IRPosition getEmptyKey() { return IRPosition::EmptyKey; }
  static inline IRPosition getTombstoneKey() {
    return IRPosition::TombstoneKey;
  }
  static unsigned getHashValue(const IRPosition &IRP) {
    return (DenseMapInfo<Value *>::getHashValue(&IRP.getAnchorValue()) << 4) ^
           (unsigned(IRP.getArgNo()));
  }
  static bool isEqual(const IRPosition &LHS, const IRPosition &RHS) {
    return LHS == RHS;
  }
};

/// A visitor class for IR positions.
///
/// Given a position P, the SubsumingPositionIterator allows to visit "subsuming
/// positions" wrt. attributes/information. Thus, if a piece of information
/// holds for a subsuming position, it also holds for the position P.
///
/// The subsuming positions always include the initial position and then,
/// depending on the position kind, additionally the following ones:
/// - for IRP_RETURNED:
///   - the function (IRP_FUNCTION)
/// - for IRP_ARGUMENT:
///   - the function (IRP_FUNCTION)
/// - for IRP_CALL_SITE:
///   - the callee (IRP_FUNCTION), if known
/// - for IRP_CALL_SITE_RETURNED:
///   - the callee (IRP_RETURNED), if known
///   - the call site (IRP_FUNCTION)
///   - the callee (IRP_FUNCTION), if known
/// - for IRP_CALL_SITE_ARGUMENT:
///   - the argument of the callee (IRP_ARGUMENT), if known
///   - the callee (IRP_FUNCTION), if known
///   - the position the call site argument is associated with if it is not
///     anchored to the call site, e.g., if it is an arugment then the argument
///     (IRP_ARGUMENT)
class SubsumingPositionIterator {
  SmallVector<IRPosition, 4> IRPositions;
  using iterator = decltype(IRPositions)::iterator;

public:
  SubsumingPositionIterator(const IRPosition &IRP);
  iterator begin() { return IRPositions.begin(); }
  iterator end() { return IRPositions.end(); }
};

/// Data structure to hold cached (LLVM-IR) information.
///
/// All attributes are given an InformationCache object at creation time to
/// avoid inspection of the IR by all of them individually. This default
/// InformationCache will hold information required by 'default' attributes,
/// thus the ones deduced when Attributor::identifyDefaultAbstractAttributes(..)
/// is called.
///
/// If custom abstract attributes, registered manually through
/// Attributor::registerAA(...), need more information, especially if it is not
/// reusable, it is advised to inherit from the InformationCache and cast the
/// instance down in the abstract attributes.
struct InformationCache {
  InformationCache(const DataLayout &DL) : DL(DL) {}

  /// A map type from opcodes to instructions with this opcode.
  using OpcodeInstMapTy = DenseMap<unsigned, SmallVector<Instruction *, 32>>;

  /// Return the map that relates "interesting" opcodes with all instructions
  /// with that opcode in \p F.
  OpcodeInstMapTy &getOpcodeInstMapForFunction(const Function &F) {
    return FuncInstOpcodeMap[&F];
  }

  /// A vector type to hold instructions.
  using InstructionVectorTy = std::vector<Instruction *>;

  /// Return the instructions in \p F that may read or write memory.
  InstructionVectorTy &getReadOrWriteInstsForFunction(const Function &F) {
    return FuncRWInstsMap[&F];
  }

private:
  /// A map type from functions to opcode to instruction maps.
  using FuncInstOpcodeMapTy = DenseMap<const Function *, OpcodeInstMapTy>;

  /// A map type from functions to their read or write instructions.
  using FuncRWInstsMapTy = DenseMap<const Function *, InstructionVectorTy>;

  /// A nested map that remembers all instructions in a function with a certain
  /// instruction opcode (Instruction::getOpcode()).
  FuncInstOpcodeMapTy FuncInstOpcodeMap;

  /// A map from functions to their instructions that may read or write memory.
  FuncRWInstsMapTy FuncRWInstsMap;

  /// The datalayout used in the module.
  const DataLayout &DL;

  /// Give the Attributor access to the members so
  /// Attributor::identifyDefaultAbstractAttributes(...) can initialize them.
  friend struct Attributor;
};

/// The fixpoint analysis framework that orchestrates the attribute deduction.
///
/// The Attributor provides a general abstract analysis framework (guided
/// fixpoint iteration) as well as helper functions for the deduction of
/// (LLVM-IR) attributes. However, also other code properties can be deduced,
/// propagated, and ultimately manifested through the Attributor framework. This
/// is particularly useful if these properties interact with attributes and a
/// co-scheduled deduction allows to improve the solution. Even if not, thus if
/// attributes/properties are completely isolated, they should use the
/// Attributor framework to reduce the number of fixpoint iteration frameworks
/// in the code base. Note that the Attributor design makes sure that isolated
/// attributes are not impacted, in any way, by others derived at the same time
/// if there is no cross-reasoning performed.
///
/// The public facing interface of the Attributor is kept simple and basically
/// allows abstract attributes to one thing, query abstract attributes
/// in-flight. There are two reasons to do this:
///    a) The optimistic state of one abstract attribute can justify an
///       optimistic state of another, allowing to framework to end up with an
///       optimistic (=best possible) fixpoint instead of one based solely on
///       information in the IR.
///    b) This avoids reimplementing various kinds of lookups, e.g., to check
///       for existing IR attributes, in favor of a single lookups interface
///       provided by an abstract attribute subclass.
///
/// NOTE: The mechanics of adding a new "concrete" abstract attribute are
///       described in the file comment.
struct Attributor {
  Attributor(InformationCache &InfoCache) : InfoCache(InfoCache) {}
  ~Attributor() { DeleteContainerPointers(AllAbstractAttributes); }

  /// Run the analyses until a fixpoint is reached or enforced (timeout).
  ///
  /// The attributes registered with this Attributor can be used after as long
  /// as the Attributor is not destroyed (it owns the attributes now).
  ///
  /// \Returns CHANGED if the IR was changed, otherwise UNCHANGED.
  ChangeStatus run();

  /// Lookup an abstract attribute of type \p AAType at position \p IRP. While
  /// no abstract attribute is found equivalent positions are checked, see
  /// SubsumingPositionIterator. Thus, the returned abstract attribute
  /// might be anchored at a different position, e.g., the callee if \p IRP is a
  /// call base.
  ///
  /// This method is the only (supported) way an abstract attribute can retrieve
  /// information from another abstract attribute. As an example, take an
  /// abstract attribute that determines the memory access behavior for a
  /// argument (readnone, readonly, ...). It should use `getAAFor` to get the
  /// most optimistic information for other abstract attributes in-flight, e.g.
  /// the one reasoning about the "captured" state for the argument or the one
  /// reasoning on the memory access behavior of the function as a whole.
  template <typename AAType>
  const AAType &getAAFor(const AbstractAttribute &QueryingAA,
                         const IRPosition &IRP) {
    static_assert(std::is_base_of<AbstractAttribute, AAType>::value,
                  "Cannot query an attribute with a type not derived from "
                  "'AbstractAttribute'!");

    // Lookup the abstract attribute of type AAType. If found, return it after
    // registering a dependence of QueryingAA on the one returned attribute.
    const auto &KindToAbstractAttributeMap =
        AAMap.lookup(const_cast<IRPosition &>(IRP));
    if (AAType *AA = static_cast<AAType *>(
            KindToAbstractAttributeMap.lookup(&AAType::ID))) {
      // Do not registr a dependence on an attribute with an invalid state.
      if (AA->getState().isValidState())
        QueryMap[AA].insert(const_cast<AbstractAttribute *>(&QueryingAA));
      return *AA;
    }

    // No matching attribute found, create one.
    auto &AA = AAType::createForPosition(IRP, *this);
    registerAA(AA);
    if (AA.getState().isValidState())
      QueryMap[&AA].insert(const_cast<AbstractAttribute *>(&QueryingAA));
    return AA;
  }

  /// Introduce a new abstract attribute into the fixpoint analysis.
  ///
  /// Note that ownership of the attribute is given to the Attributor. It will
  /// invoke delete for the Attributor on destruction of the Attributor.
  ///
  /// Attributes are identified by their IR position (AAType::getIRPosition())
  /// and the address of their static member (see AAType::ID).
  template <typename AAType> AAType &registerAA(AAType &AA) {
    static_assert(std::is_base_of<AbstractAttribute, AAType>::value,
                  "Cannot register an attribute with a type not derived from "
                  "'AbstractAttribute'!");
    // Put the attribute in the lookup map structure and the container we use to
    // keep track of all attributes.
    IRPosition &IRP = AA.getIRPosition();
    AAMap[IRP][&AAType::ID] = &AA;
    AllAbstractAttributes.push_back(&AA);
    return AA;
  }

  /// Return the internal information cache.
  InformationCache &getInfoCache() { return InfoCache; }

  /// Determine opportunities to derive 'default' attributes in \p F and create
  /// abstract attribute objects for them.
  ///
  /// \param F The function that is checked for attribute opportunities.
  /// \param Whitelist If not null, a set limiting the attribute opportunities.
  ///
  /// Note that abstract attribute instances are generally created even if the
  /// IR already contains the information they would deduce. The most important
  /// reason for this is the single interface, the one of the abstract attribute
  /// instance, which can be queried without the need to look at the IR in
  /// various places.
  void identifyDefaultAbstractAttributes(
      Function &F, DenseSet<const char *> *Whitelist = nullptr);

  /// Return true if \p AA (or its context instruction) is assumed dead.
  ///
  /// If \p LivenessAA is not provided it is queried.
  bool isAssumedDead(const AbstractAttribute &AA, const AAIsDead *LivenessAA);

  /// Check \p Pred on all function call sites.
  ///
  /// This method will evaluate \p Pred on call sites and return
  /// true if \p Pred holds in every call sites. However, this is only possible
  /// all call sites are known, hence the function has internal linkage.
  bool checkForAllCallSites(const function_ref<bool(CallSite)> &Pred,
                            const AbstractAttribute &QueryingAA,
                            bool RequireAllCallSites);

  /// Check \p Pred on all values potentially returned by \p F.
  ///
  /// This method will evaluate \p Pred on all values potentially returned by
  /// the function associated with \p QueryingAA. The returned values are
  /// matched with their respective return instructions. Returns true if \p Pred
  /// holds on all of them.
  bool checkForAllReturnedValuesAndReturnInsts(
      const function_ref<bool(Value &, const SmallPtrSetImpl<ReturnInst *> &)>
          &Pred,
      const AbstractAttribute &QueryingAA);

  /// Check \p Pred on all values potentially returned by the function
  /// associated with \p QueryingAA.
  ///
  /// This is the context insensitive version of the method above.
  bool checkForAllReturnedValues(const function_ref<bool(Value &)> &Pred,
                                 const AbstractAttribute &QueryingAA);

  /// Check \p Pred on all instructions with an opcode present in \p Opcodes.
  ///
  /// This method will evaluate \p Pred on all instructions with an opcode
  /// present in \p Opcode and return true if \p Pred holds on all of them.
  bool checkForAllInstructions(const function_ref<bool(Instruction &)> &Pred,
                               const AbstractAttribute &QueryingAA,
                               const ArrayRef<unsigned> &Opcodes);

  /// Check \p Pred on all call-like instructions (=CallBased derived).
  ///
  /// See checkForAllCallLikeInstructions(...) for more information.
  bool
  checkForAllCallLikeInstructions(const function_ref<bool(Instruction &)> &Pred,
                                  const AbstractAttribute &QueryingAA) {
    return checkForAllInstructions(Pred, QueryingAA,
                                   {(unsigned)Instruction::Invoke,
                                    (unsigned)Instruction::CallBr,
                                    (unsigned)Instruction::Call});
  }

  /// Check \p Pred on all Read/Write instructions.
  ///
  /// This method will evaluate \p Pred on all instructions that read or write
  /// to memory present in the information cache and return true if \p Pred
  /// holds on all of them.
  bool checkForAllReadWriteInstructions(
      const llvm::function_ref<bool(Instruction &)> &Pred,
      AbstractAttribute &QueryingAA);

  /// Return the data layout associated with the anchor scope.
  const DataLayout &getDataLayout() const { return InfoCache.DL; }

private:
  /// The set of all abstract attributes.
  ///{
  using AAVector = SmallVector<AbstractAttribute *, 64>;
  AAVector AllAbstractAttributes;
  ///}

  /// A nested map to lookup abstract attributes based on the argument position
  /// on the outer level, and the addresses of the static member (AAType::ID) on
  /// the inner level.
  ///{
  using KindToAbstractAttributeMap =
      DenseMap<const char *, AbstractAttribute *>;
  DenseMap<IRPosition, KindToAbstractAttributeMap> AAMap;
  ///}

  /// A map from abstract attributes to the ones that queried them through calls
  /// to the getAAFor<...>(...) method.
  ///{
  using QueryMapTy =
      DenseMap<AbstractAttribute *, SetVector<AbstractAttribute *>>;
  QueryMapTy QueryMap;
  ///}

  /// The information cache that holds pre-processed (LLVM-IR) information.
  InformationCache &InfoCache;
};

/// An interface to query the internal state of an abstract attribute.
///
/// The abstract state is a minimal interface that allows the Attributor to
/// communicate with the abstract attributes about their internal state without
/// enforcing or exposing implementation details, e.g., the (existence of an)
/// underlying lattice.
///
/// It is sufficient to be able to query if a state is (1) valid or invalid, (2)
/// at a fixpoint, and to indicate to the state that (3) an optimistic fixpoint
/// was reached or (4) a pessimistic fixpoint was enforced.
///
/// All methods need to be implemented by the subclass. For the common use case,
/// a single boolean state or a bit-encoded state, the BooleanState and
/// IntegerState classes are already provided. An abstract attribute can inherit
/// from them to get the abstract state interface and additional methods to
/// directly modify the state based if needed. See the class comments for help.
struct AbstractState {
  virtual ~AbstractState() {}

  /// Return if this abstract state is in a valid state. If false, no
  /// information provided should be used.
  virtual bool isValidState() const = 0;

  /// Return if this abstract state is fixed, thus does not need to be updated
  /// if information changes as it cannot change itself.
  virtual bool isAtFixpoint() const = 0;

  /// Indicate that the abstract state should converge to the optimistic state.
  ///
  /// This will usually make the optimistically assumed state the known to be
  /// true state.
  ///
  /// \returns ChangeStatus::UNCHANGED as the assumed value should not change.
  virtual ChangeStatus indicateOptimisticFixpoint() = 0;

  /// Indicate that the abstract state should converge to the pessimistic state.
  ///
  /// This will usually revert the optimistically assumed state to the known to
  /// be true state.
  ///
  /// \returns ChangeStatus::CHANGED as the assumed value may change.
  virtual ChangeStatus indicatePessimisticFixpoint() = 0;
};

/// Simple state with integers encoding.
///
/// The interface ensures that the assumed bits are always a subset of the known
/// bits. Users can only add known bits and, except through adding known bits,
/// they can only remove assumed bits. This should guarantee monotoniticy and
/// thereby the existence of a fixpoint (if used corretly). The fixpoint is
/// reached when the assumed and known state/bits are equal. Users can
/// force/inidicate a fixpoint. If an optimistic one is indicated, the known
/// state will catch up with the assumed one, for a pessimistic fixpoint it is
/// the other way around.
struct IntegerState : public AbstractState {
  /// Underlying integer type, we assume 32 bits to be enough.
  using base_t = uint32_t;

  /// Initialize the (best) state.
  IntegerState(base_t BestState = ~0) : Assumed(BestState) {}

  /// Return the worst possible representable state.
  static constexpr base_t getWorstState() { return 0; }

  /// See AbstractState::isValidState()
  /// NOTE: For now we simply pretend that the worst possible state is invalid.
  bool isValidState() const override { return Assumed != getWorstState(); }

  /// See AbstractState::isAtFixpoint()
  bool isAtFixpoint() const override { return Assumed == Known; }

  /// See AbstractState::indicateOptimisticFixpoint(...)
  ChangeStatus indicateOptimisticFixpoint() override {
    Known = Assumed;
    return ChangeStatus::UNCHANGED;
  }

  /// See AbstractState::indicatePessimisticFixpoint(...)
  ChangeStatus indicatePessimisticFixpoint() override {
    Assumed = Known;
    return ChangeStatus::CHANGED;
  }

  /// Return the known state encoding
  base_t getKnown() const { return Known; }

  /// Return the assumed state encoding.
  base_t getAssumed() const { return Assumed; }

  /// Return true if the bits set in \p BitsEncoding are "known bits".
  bool isKnown(base_t BitsEncoding) const {
    return (Known & BitsEncoding) == BitsEncoding;
  }

  /// Return true if the bits set in \p BitsEncoding are "assumed bits".
  bool isAssumed(base_t BitsEncoding) const {
    return (Assumed & BitsEncoding) == BitsEncoding;
  }

  /// Add the bits in \p BitsEncoding to the "known bits".
  IntegerState &addKnownBits(base_t Bits) {
    // Make sure we never miss any "known bits".
    Assumed |= Bits;
    Known |= Bits;
    return *this;
  }

  /// Remove the bits in \p BitsEncoding from the "assumed bits" if not known.
  IntegerState &removeAssumedBits(base_t BitsEncoding) {
    // Make sure we never loose any "known bits".
    Assumed = (Assumed & ~BitsEncoding) | Known;
    return *this;
  }

  /// Keep only "assumed bits" also set in \p BitsEncoding but all known ones.
  IntegerState &intersectAssumedBits(base_t BitsEncoding) {
    // Make sure we never loose any "known bits".
    Assumed = (Assumed & BitsEncoding) | Known;
    return *this;
  }

  /// Take minimum of assumed and \p Value.
  IntegerState &takeAssumedMinimum(base_t Value) {
    // Make sure we never loose "known value".
    Assumed = std::max(std::min(Assumed, Value), Known);
    return *this;
  }

  /// Take maximum of known and \p Value.
  IntegerState &takeKnownMaximum(base_t Value) {
    // Make sure we never loose "known value".
    Assumed = std::max(Value, Assumed);
    Known = std::max(Value, Known);
    return *this;
  }

  /// Equality for IntegerState.
  bool operator==(const IntegerState &R) const {
    return this->getAssumed() == R.getAssumed() &&
           this->getKnown() == R.getKnown();
  }

  /// Inequality for IntegerState.
  bool operator!=(const IntegerState &R) const { return !(*this == R); }

  /// "Clamp" this state with \p R. The result is the minimum of the assumed
  /// information but not less than what was known before.
  ///
  /// TODO: Consider replacing the operator with a call or using it only when
  ///       we can also take the maximum of the known information, thus when
  ///       \p R is not dependent on additional assumed state.
  IntegerState operator^=(const IntegerState &R) {
    takeAssumedMinimum(R.Assumed);
    return *this;
  }

  /// Make this the minimum, known and assumed, of this state and \p R.
  IntegerState operator&=(const IntegerState &R) {
    Known = std::min(Known, R.Known);
    Assumed = std::min(Assumed, R.Assumed);
    return *this;
  }

  /// Make this the maximum, known and assumed, of this state and \p R.
  IntegerState operator|=(const IntegerState &R) {
    Known = std::max(Known, R.Known);
    Assumed = std::max(Assumed, R.Assumed);
    return *this;
  }

private:
  /// The known state encoding in an integer of type base_t.
  base_t Known = getWorstState();

  /// The assumed state encoding in an integer of type base_t.
  base_t Assumed;
};

/// Simple wrapper for a single bit (boolean) state.
struct BooleanState : public IntegerState {
  BooleanState() : IntegerState(1){};
};

/// Helper struct necessary as the modular build fails if the virtual method
/// IRAttribute::manifest is defined in the Attributor.cpp.
struct IRAttributeManifest {
  static ChangeStatus manifestAttrs(Attributor &A, IRPosition &IRP,
                                    const ArrayRef<Attribute> &DeducedAttrs);
};

/// Helper to tie a abstract state implementation to an abstract attribute.
template <typename StateTy, typename Base>
struct StateWrapper : public StateTy, public Base {
  /// Provide static access to the type of the state.
  using StateType = StateTy;

  /// See AbstractAttribute::getState(...).
  StateType &getState() override { return *this; }

  /// See AbstractAttribute::getState(...).
  const AbstractState &getState() const override { return *this; }
};

/// Helper class that provides common functionality to manifest IR attributes.
template <Attribute::AttrKind AK, typename Base>
struct IRAttribute : public IRPosition, public Base {
  IRAttribute(const IRPosition &IRP) : IRPosition(IRP) {}
  ~IRAttribute() {}

  /// See AbstractAttribute::manifest(...).
  ChangeStatus manifest(Attributor &A) override {
    SmallVector<Attribute, 4> DeducedAttrs;
    getDeducedAttributes(getAnchorValue().getContext(), DeducedAttrs);
    return IRAttributeManifest::manifestAttrs(A, getIRPosition(), DeducedAttrs);
  }

  /// Return the kind that identifies the abstract attribute implementation.
  Attribute::AttrKind getAttrKind() const { return AK; }

  /// Return the deduced attributes in \p Attrs.
  virtual void getDeducedAttributes(LLVMContext &Ctx,
                                    SmallVectorImpl<Attribute> &Attrs) const {
    Attrs.emplace_back(Attribute::get(Ctx, getAttrKind()));
  }

  /// Return an IR position, see struct IRPosition.
  ///
  ///{
  IRPosition &getIRPosition() override { return *this; }
  const IRPosition &getIRPosition() const override { return *this; }
  ///}
};

/// Base struct for all "concrete attribute" deductions.
///
/// The abstract attribute is a minimal interface that allows the Attributor to
/// orchestrate the abstract/fixpoint analysis. The design allows to hide away
/// implementation choices made for the subclasses but also to structure their
/// implementation and simplify the use of other abstract attributes in-flight.
///
/// To allow easy creation of new attributes, most methods have default
/// implementations. The ones that do not are generally straight forward, except
/// `AbstractAttribute::updateImpl` which is the location of most reasoning
/// associated with the abstract attribute. The update is invoked by the
/// Attributor in case the situation used to justify the current optimistic
/// state might have changed. The Attributor determines this automatically
/// by monitoring the `Attributor::getAAFor` calls made by abstract attributes.
///
/// The `updateImpl` method should inspect the IR and other abstract attributes
/// in-flight to justify the best possible (=optimistic) state. The actual
/// implementation is, similar to the underlying abstract state encoding, not
/// exposed. In the most common case, the `updateImpl` will go through a list of
/// reasons why its optimistic state is valid given the current information. If
/// any combination of them holds and is sufficient to justify the current
/// optimistic state, the method shall return UNCHAGED. If not, the optimistic
/// state is adjusted to the situation and the method shall return CHANGED.
///
/// If the manifestation of the "concrete attribute" deduced by the subclass
/// differs from the "default" behavior, which is a (set of) LLVM-IR
/// attribute(s) for an argument, call site argument, function return value, or
/// function, the `AbstractAttribute::manifest` method should be overloaded.
///
/// NOTE: If the state obtained via getState() is INVALID, thus if
///       AbstractAttribute::getState().isValidState() returns false, no
///       information provided by the methods of this class should be used.
/// NOTE: The Attributor currently has certain limitations to what we can do.
///       As a general rule of thumb, "concrete" abstract attributes should *for
///       now* only perform "backward" information propagation. That means
///       optimistic information obtained through abstract attributes should
///       only be used at positions that precede the origin of the information
///       with regards to the program flow. More practically, information can
///       *now* be propagated from instructions to their enclosing function, but
///       *not* from call sites to the called function. The mechanisms to allow
///       both directions will be added in the future.
/// NOTE: The mechanics of adding a new "concrete" abstract attribute are
///       described in the file comment.
struct AbstractAttribute {
  using StateType = AbstractState;

  /// Virtual destructor.
  virtual ~AbstractAttribute() {}

  /// Initialize the state with the information in the Attributor \p A.
  ///
  /// This function is called by the Attributor once all abstract attributes
  /// have been identified. It can and shall be used for task like:
  ///  - identify existing knowledge in the IR and use it for the "known state"
  ///  - perform any work that is not going to change over time, e.g., determine
  ///    a subset of the IR, or attributes in-flight, that have to be looked at
  ///    in the `updateImpl` method.
  virtual void initialize(Attributor &A) {}

  /// Return the internal abstract state for inspection.
  virtual StateType &getState() = 0;
  virtual const StateType &getState() const = 0;

  /// Return an IR position, see struct IRPosition.
  virtual const IRPosition &getIRPosition() const = 0;

  /// Helper functions, for debug purposes only.
  ///{
  virtual void print(raw_ostream &OS) const;
  void dump() const { print(dbgs()); }

  /// This function should return the "summarized" assumed state as string.
  virtual const std::string getAsStr() const = 0;
  ///}

  /// Allow the Attributor access to the protected methods.
  friend struct Attributor;

protected:
  /// Hook for the Attributor to trigger an update of the internal state.
  ///
  /// If this attribute is already fixed, this method will return UNCHANGED,
  /// otherwise it delegates to `AbstractAttribute::updateImpl`.
  ///
  /// \Return CHANGED if the internal state changed, otherwise UNCHANGED.
  ChangeStatus update(Attributor &A);

  /// Hook for the Attributor to trigger the manifestation of the information
  /// represented by the abstract attribute in the LLVM-IR.
  ///
  /// \Return CHANGED if the IR was altered, otherwise UNCHANGED.
  virtual ChangeStatus manifest(Attributor &A) {
    return ChangeStatus::UNCHANGED;
  }

  /// Hook to enable custom statistic tracking, called after manifest that
  /// resulted in a change if statistics are enabled.
  ///
  /// We require subclasses to provide an implementation so we remember to
  /// add statistics for them.
  virtual void trackStatistics() const = 0;

  /// Return an IR position, see struct IRPosition.
  virtual IRPosition &getIRPosition() = 0;

  /// The actual update/transfer function which has to be implemented by the
  /// derived classes.
  ///
  /// If it is called, the environment has changed and we have to determine if
  /// the current information is still valid or adjust it otherwise.
  ///
  /// \Return CHANGED if the internal state changed, otherwise UNCHANGED.
  virtual ChangeStatus updateImpl(Attributor &A) = 0;
};

/// Forward declarations of output streams for debug purposes.
///
///{
raw_ostream &operator<<(raw_ostream &OS, const AbstractAttribute &AA);
raw_ostream &operator<<(raw_ostream &OS, ChangeStatus S);
raw_ostream &operator<<(raw_ostream &OS, IRPosition::Kind);
raw_ostream &operator<<(raw_ostream &OS, const IRPosition &);
raw_ostream &operator<<(raw_ostream &OS, const AbstractState &State);
raw_ostream &operator<<(raw_ostream &OS, const IntegerState &S);
///}

struct AttributorPass : public PassInfoMixin<AttributorPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

Pass *createAttributorLegacyPass();

/// ----------------------------------------------------------------------------
///                       Abstract Attribute Classes
/// ----------------------------------------------------------------------------

/// An abstract attribute for the returned values of a function.
struct AAReturnedValues
    : public IRAttribute<Attribute::Returned, AbstractAttribute> {
  AAReturnedValues(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Return an assumed unique return value if a single candidate is found. If
  /// there cannot be one, return a nullptr. If it is not clear yet, return the
  /// Optional::NoneType.
  Optional<Value *> getAssumedUniqueReturnValue(Attributor &A) const;

  /// Check \p Pred on all returned values.
  ///
  /// This method will evaluate \p Pred on returned values and return
  /// true if (1) all returned values are known, and (2) \p Pred returned true
  /// for all returned values.
  ///
  /// Note: Unlike the Attributor::checkForAllReturnedValuesAndReturnInsts
  /// method, this one will not filter dead return instructions.
  virtual bool checkForAllReturnedValuesAndReturnInsts(
      const function_ref<bool(Value &, const SmallPtrSetImpl<ReturnInst *> &)>
          &Pred) const = 0;

  using iterator = DenseMap<Value *, SmallPtrSet<ReturnInst *, 2>>::iterator;
  using const_iterator =
      DenseMap<Value *, SmallPtrSet<ReturnInst *, 2>>::const_iterator;
  virtual llvm::iterator_range<iterator> returned_values() = 0;
  virtual llvm::iterator_range<const_iterator> returned_values() const = 0;

  virtual size_t getNumReturnValues() const = 0;
  virtual const SmallPtrSetImpl<CallBase *> &getUnresolvedCalls() const = 0;

  /// Create an abstract attribute view for the position \p IRP.
  static AAReturnedValues &createForPosition(const IRPosition &IRP,
                                             Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

struct AANoUnwind
    : public IRAttribute<Attribute::NoUnwind,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANoUnwind(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Returns true if nounwind is assumed.
  bool isAssumedNoUnwind() const { return getAssumed(); }

  /// Returns true if nounwind is known.
  bool isKnownNoUnwind() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoUnwind &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

struct AANoSync
    : public IRAttribute<Attribute::NoSync,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANoSync(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Returns true if "nosync" is assumed.
  bool isAssumedNoSync() const { return getAssumed(); }

  /// Returns true if "nosync" is known.
  bool isKnownNoSync() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoSync &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for all nonnull attributes.
struct AANonNull
    : public IRAttribute<Attribute::NonNull,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANonNull(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Return true if we assume that the underlying value is nonnull.
  bool isAssumedNonNull() const { return getAssumed(); }

  /// Return true if we know that underlying value is nonnull.
  bool isKnownNonNull() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANonNull &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract attribute for norecurse.
struct AANoRecurse
    : public IRAttribute<Attribute::NoRecurse,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANoRecurse(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Return true if "norecurse" is assumed.
  bool isAssumedNoRecurse() const { return getAssumed(); }

  /// Return true if "norecurse" is known.
  bool isKnownNoRecurse() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoRecurse &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract attribute for willreturn.
struct AAWillReturn
    : public IRAttribute<Attribute::WillReturn,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AAWillReturn(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Return true if "willreturn" is assumed.
  bool isAssumedWillReturn() const { return getAssumed(); }

  /// Return true if "willreturn" is known.
  bool isKnownWillReturn() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AAWillReturn &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for all noalias attributes.
struct AANoAlias
    : public IRAttribute<Attribute::NoAlias,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANoAlias(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Return true if we assume that the underlying value is alias.
  bool isAssumedNoAlias() const { return getAssumed(); }

  /// Return true if we know that underlying value is noalias.
  bool isKnownNoAlias() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoAlias &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An AbstractAttribute for nofree.
struct AANoFree
    : public IRAttribute<Attribute::NoFree,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANoFree(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Return true if "nofree" is assumed.
  bool isAssumedNoFree() const { return getAssumed(); }

  /// Return true if "nofree" is known.
  bool isKnownNoFree() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoFree &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An AbstractAttribute for noreturn.
struct AANoReturn
    : public IRAttribute<Attribute::NoReturn,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANoReturn(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Return true if the underlying object is assumed to never return.
  bool isAssumedNoReturn() const { return getAssumed(); }

  /// Return true if the underlying object is known to never return.
  bool isKnownNoReturn() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoReturn &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for liveness abstract attribute.
struct AAIsDead : public StateWrapper<BooleanState, AbstractAttribute>,
                  public IRPosition {
  AAIsDead(const IRPosition &IRP) : IRPosition(IRP) {}

  /// Returns true if \p BB is assumed dead.
  virtual bool isAssumedDead(const BasicBlock *BB) const = 0;

  /// Returns true if \p BB is known dead.
  virtual bool isKnownDead(const BasicBlock *BB) const = 0;

  /// Returns true if \p I is assumed dead.
  virtual bool isAssumedDead(const Instruction *I) const = 0;

  /// Returns true if \p I is known dead.
  virtual bool isKnownDead(const Instruction *I) const = 0;

  /// This method is used to check if at least one instruction in a collection
  /// of instructions is live.
  template <typename T> bool isLiveInstSet(T begin, T end) const {
    for (const auto &I : llvm::make_range(begin, end)) {
      assert(I->getFunction() == getIRPosition().getAssociatedFunction() &&
             "Instruction must be in the same anchor scope function.");

      if (!isAssumedDead(I))
        return true;
    }

    return false;
  }

  /// Return an IR position, see struct IRPosition.
  ///
  ///{
  IRPosition &getIRPosition() { return *this; }
  const IRPosition &getIRPosition() const { return *this; }
  ///}

  /// Create an abstract attribute view for the position \p IRP.
  static AAIsDead &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for all dereferenceable attribute.
struct AADereferenceable
    : public IRAttribute<Attribute::Dereferenceable, AbstractAttribute> {
  AADereferenceable(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Return true if we assume that the underlying value is nonnull.
  virtual bool isAssumedNonNull() const = 0;

  /// Return true if we assume that underlying value is
  /// dereferenceable(_or_null) globally.
  virtual bool isAssumedGlobal() const = 0;

  /// Return true if we know that underlying value is
  /// dereferenceable(_or_null) globally.
  virtual bool isKnownGlobal() const = 0;

  /// Return assumed dereferenceable bytes.
  virtual uint32_t getAssumedDereferenceableBytes() const = 0;

  /// Return known dereferenceable bytes.
  virtual uint32_t getKnownDereferenceableBytes() const = 0;

  /// Create an abstract attribute view for the position \p IRP.
  static AADereferenceable &createForPosition(const IRPosition &IRP,
                                              Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for all align attributes.
struct AAAlign
    : public IRAttribute<Attribute::Alignment,
                         StateWrapper<IntegerState, AbstractAttribute>> {
  AAAlign(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Return assumed alignment.
  unsigned getAssumedAlign() const { return getAssumed(); }

  /// Return known alignemnt.
  unsigned getKnownAlign() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AAAlign &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_IPO_FUNCTIONATTRS_H
