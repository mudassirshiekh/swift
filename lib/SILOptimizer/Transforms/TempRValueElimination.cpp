//===--- TempRValueElimination.cpp ----------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// Eliminate temporary RValues inserted as a result of materialization by
/// SILGen. The key pattern here is that we are looking for alloc_stack that are
/// only written to once and are eventually either destroyed/taken from.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-temp-rvalue-opt"

#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/NodeBits.h"
#include "swift/SIL/OSSALifetimeCompletion.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/Analysis/RCIdentityAnalysis.h"
#include "swift/SILOptimizer/Analysis/SimplifyInstruction.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/ValueLifetime.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                                 Interface
//===----------------------------------------------------------------------===//

namespace {

/// Temporary RValue Optimization
///
/// Peephole optimization to eliminate short-lived immutable temporary copies.
/// This handles a common pattern generated by SILGen where temporary RValues
/// are emitted as copies...
///
///   %temp = alloc_stack $T
///   copy_addr %src to [init] %temp : $*T
///   // no writes to %src or %temp
///   destroy_addr %temp : $*T
///   dealloc_stack %temp : $*T
///
/// This differs from the copy forwarding algorithm because it handles
/// copy source and dest lifetimes that are unavoidably overlapping. Instead,
/// it finds cases in which it is easy to determine that the source is
/// unmodified during the copy destination's lifetime. Thus, the destination can
/// be viewed as a short-lived "rvalue".
///
/// As a second optimization, also stores into temporaries are handled. This is
/// a simple form of redundant-load-elimination (RLE).
///
///   %temp = alloc_stack $T
///   store %src to [init] %temp : $*T
///   // no writes to %temp
///   %v = load [take] %temp : $*T
///   dealloc_stack %temp : $*T
///
/// TODO: Check if we still need to handle stores when RLE supports OSSA.
class TempRValueOptPass : public SILFunctionTransform {
  bool collectLoads(Operand *addressUse, CopyAddrInst *originalCopy,
                    InstructionSetWithSize &loadInsts);
  bool collectLoadsFromProjection(SingleValueInstruction *projection,
                                  CopyAddrInst *originalCopy,
                                  InstructionSetWithSize &loadInsts);

  SILInstruction *getLastUseWhileSourceIsNotModified(
    CopyAddrInst *copyInst, const InstructionSetWithSize &useInsts,
    AliasAnalysis *aa);

  bool
  checkTempObjectDestroy(AllocStackInst *tempObj, CopyAddrInst *copyInst);

  bool extendAccessScopes(CopyAddrInst *copyInst, SILInstruction *lastUseInst,
                          AliasAnalysis *aa);

  void tryOptimizeCopyIntoTemp(CopyAddrInst *copyInst);
  SILBasicBlock::iterator tryOptimizeStoreIntoTemp(StoreInst *si);

  void run() override;
};

} // anonymous namespace

bool TempRValueOptPass::collectLoadsFromProjection(
    SingleValueInstruction *projection, CopyAddrInst *originalCopy,
    InstructionSetWithSize &loadInsts) {
  // Transitively look through projections on stack addresses.
  for (auto *projUseOper : projection->getUses()) {
    auto *user = projUseOper->getUser();
    if (user->isTypeDependentOperand(*projUseOper))
      continue;

    if (!collectLoads(projUseOper, originalCopy, loadInsts))
      return false;
  }
  return true;
}

/// Transitively explore all data flow uses of the given \p address until
/// reaching a load or returning false.
///
/// Any user opcode recognized by collectLoads must be replaced correctly later
/// during tryOptimizeCopyIntoTemp. If it is possible for any use to destroy the
/// value in \p address, then that use must be removed or made non-destructive
/// after the copy is removed and its operand is replaced.
///
/// Warning: To preserve the original object lifetime, tryOptimizeCopyIntoTemp
/// must assume that there are no holes in lifetime of the temporary stack
/// location at \address. The temporary must be initialized by the original copy
/// and never written to again. Therefore, collectLoads disallows any operation
/// that may write to memory at \p address.
bool TempRValueOptPass::
collectLoads(Operand *addressUse, CopyAddrInst *originalCopy,
             InstructionSetWithSize &loadInsts) {
  SILInstruction *user = addressUse->getUser();
  SILValue address = addressUse->get();

  // All normal uses (loads) must be in the initialization block.
  // (The destroy and dealloc are commonly in a different block though.)
  SILBasicBlock *block = originalCopy->getParent();
  if (user->getParent() != block)
    return false;

  // Only allow uses that cannot destroy their operand. We need to be sure
  // that replacing all this temporary's uses with the copy source doesn't
  // destroy the source. This way, we know that the destroy_addr instructions
  // that we recorded cover all the temporary's lifetime termination points.
  //
  // Currently this includes address projections, loads, and in_guaranteed uses
  // by an apply.
  //
  // TODO: handle non-destructive projections of enums
  // (unchecked_take_enum_data_addr of Optional is nondestructive.)
  switch (user->getKind()) {
  default:
    LLVM_DEBUG(llvm::dbgs()
               << "  Temp use may write/destroy its source" << *user);
    return false;
  case SILInstructionKind::BeginAccessInst: {
    auto *beginAccess = cast<BeginAccessInst>(user);
    if (beginAccess->getAccessKind() != SILAccessKind::Read)
      return false;

    // We don't have to recursively call collectLoads for the beginAccess
    // result, because a SILAccessKind::Read already guarantees that there are
    // no writes to the beginAccess result address (or any projection from it).
    // But we have to register the end-accesses as loads to correctly mark the
    // end-of-lifetime of the tempObj.
    //
    //   %addr = begin_access [read]
    //      ... // there can be no writes to %addr here
    //   end_access %addr   // <- This is where the use actually ends.
    for (EndAccessInst *endAccess : beginAccess->getEndAccesses()) {
      if (endAccess->getParent() != block)
        return false;
      loadInsts.insert(endAccess);
    }
    return true;
  }
  case SILInstructionKind::MarkDependenceInst: {
    auto mdi = cast<MarkDependenceInst>(user);
    // If the user is the base operand of the MarkDependenceInst we can return
    // true, because this would be the end of this dataflow chain
    if (mdi->getBase() == address) {
      return true;
    }
    // If the user is the value operand of the MarkDependenceInst we have to
    // transitively explore its uses until we reach a load or return false
    for (auto *mdiUseOper : mdi->getUses()) {
      if (!collectLoads(mdiUseOper, originalCopy, loadInsts))
        return false;
    }
    return true;
  }
  case SILInstructionKind::PartialApplyInst:
    if (!cast<PartialApplyInst>(user)->isOnStack())
      return false;
     LLVM_FALLTHROUGH;
  case SILInstructionKind::ApplyInst:
  case SILInstructionKind::TryApplyInst:
  case SILInstructionKind::BeginApplyInst: {
    auto convention = ApplySite(user).getArgumentParameterInfo(*addressUse);
    if (!convention.isGuaranteed())
      return false;

    loadInsts.insert(user);
    if (auto *beginApply = dyn_cast<BeginApplyInst>(user)) {
      // Register 'end_apply'/'abort_apply' as loads as well
      // 'checkNoSourceModification' should check instructions until
      // 'end_apply'/'abort_apply'.
      for (auto tokenUse : beginApply->getTokenResult()->getUses()) {
        SILInstruction *tokenUser = tokenUse->getUser();
        if (tokenUser->getParent() != block)
          return false;
        loadInsts.insert(tokenUser);
      }
    }
    return true;
  }
  case SILInstructionKind::YieldInst: {
    auto *yield = cast<YieldInst>(user);
    auto convention = yield->getArgumentConventionForOperand(*addressUse);
    if (!convention.isGuaranteedConvention())
      return false;

    loadInsts.insert(user);
    return true;
  }
  case SILInstructionKind::OpenExistentialAddrInst: {
    // We only support open existential addr if the access is immutable.
    auto *oeai = cast<OpenExistentialAddrInst>(user);
    if (oeai->getAccessKind() != OpenedExistentialAccess::Immutable) {
      LLVM_DEBUG(llvm::dbgs() << "  Temp consuming use may write/destroy "
                                 "its source"
                              << *user);
      return false;
    }
    return collectLoadsFromProjection(oeai, originalCopy, loadInsts);
  }
  case SILInstructionKind::UncheckedTakeEnumDataAddrInst: {
    // In certain cases, unchecked_take_enum_data_addr invalidates the
    // underlying memory, so by default we can not look through it... but this
    // is not true in the case of Optional. This is an important case for us to
    // handle, so handle it here.
    auto *utedai = cast<UncheckedTakeEnumDataAddrInst>(user);
    if (!utedai->getOperand()->getType().getOptionalObjectType()) {
      LLVM_DEBUG(llvm::dbgs()
                 << "  Temp use may write/destroy its source" << *utedai);
      return false;
    }

    return collectLoadsFromProjection(utedai, originalCopy, loadInsts);
  }
  case SILInstructionKind::StructElementAddrInst:
  case SILInstructionKind::TupleElementAddrInst:
  case SILInstructionKind::UncheckedAddrCastInst:
    return collectLoadsFromProjection(cast<SingleValueInstruction>(user),
                                      originalCopy, loadInsts);

  case SILInstructionKind::LoadInst: {
    // Loads are the end of the data flow chain. The users of the load can't
    // access the temporary storage.
    //
    // That being said, if we see a load [take] here then we must have had a
    // load [take] of a projection of our temporary stack location since we skip
    // all the load [take] of the top level allocation in the caller of this
    // function. So if we have such a load [take], we /must/ have a
    // reinitialization or an alloc_stack that does not fit the pattern we are
    // expecting from SILGen. Be conservative and return false.
    auto *li = cast<LoadInst>(user);
    if (li->getOwnershipQualifier() == LoadOwnershipQualifier::Take &&
        // Only accept load [take] if it takes the whole temporary object.
        // load [take] from a projection would destroy only a part of the
        // temporary and we don't handle this.
        address != originalCopy->getDest()) {
      return false;
    }
    loadInsts.insert(user);
    return true;
  }
  case SILInstructionKind::LoadBorrowInst: {
    loadInsts.insert(user);
    BorrowedValue borrow(cast<LoadBorrowInst>(user));
    auto visitEndScope = [&](Operand *op) -> bool {
      auto *opUser = op->getUser();
      if (auto *endBorrow = dyn_cast<EndBorrowInst>(opUser)) {
        if (endBorrow->getParent() != block)
          return false;
        loadInsts.insert(endBorrow);
        return true;
      }
      // Don't look further if we see a reborrow.
      assert(cast<BranchInst>(opUser));
      return false;
    };
    auto res = borrow.visitLocalScopeEndingUses(visitEndScope);
    return res;
  }
  case SILInstructionKind::FixLifetimeInst:
    // If we have a fixed lifetime on our alloc_stack, we can just treat it like
    // a load and re-write it so that it is on the old memory or old src object.
    loadInsts.insert(user);
    return true;
  case SILInstructionKind::CopyAddrInst: {
    // copy_addr which read from the temporary are like loads.
    auto *copyFromTmp = cast<CopyAddrInst>(user);
    if (copyFromTmp->getDest() == address) {
      LLVM_DEBUG(llvm::dbgs() << "  Temp written or taken" << *user);
      return false;
    }
    // As with load [take], only accept copy_addr [take] if it takes the whole
    // temporary object.
    if (copyFromTmp->isTakeOfSrc() && address != originalCopy->getDest())
      return false;
    loadInsts.insert(copyFromTmp);
    return true;
  }
  }
}

/// Checks if the source of \p copyInst is not modified within the temporary's
/// lifetime, i.e. is not modified before the last use of \p useInsts.
///
/// If there are no source modifications with the lifetime, returns the last
/// user (or copyInst if there are no uses at all).
/// Otherwise, returns a nullptr.
///
/// Unfortunately, we cannot simply use the destroy points as the lifetime end,
/// because they can be in a different basic block (that's what SILGen
/// generates). Instead we guarantee that all normal uses are within the block
/// of the temporary and look for the last use, which effectively ends the
/// lifetime.
SILInstruction *TempRValueOptPass::getLastUseWhileSourceIsNotModified(
    CopyAddrInst *copyInst, const InstructionSetWithSize &useInsts,
    AliasAnalysis *aa) {
  if (useInsts.empty())
    return copyInst;
  unsigned numLoadsFound = 0;
  SILValue copySrc = copyInst->getSrc();

  // We already checked that the useful lifetime of the temporary ends in
  // the initialization block. Iterate over the instructions of the block,
  // starting at copyInst, until we get to the last user.
  auto iter = std::next(copyInst->getIterator());
  auto iterEnd = copyInst->getParent()->end();
  for (; iter != iterEnd; ++iter) {
    SILInstruction *inst = &*iter;

    if (useInsts.contains(inst))
      ++numLoadsFound;

    // If this is the last use of the temp we are ok. After this point,
    // modifications to the source don't matter anymore.
    // Note that we are assuming here that if an instruction loads and writes
    // to copySrc at the same time (like a copy_addr could do), the write
    // takes effect after the load.
    if (numLoadsFound == useInsts.size()) {
      // Function calls are an exception: in a called function a potential
      // modification of copySrc could occur _before_ the read of the temporary.
      if ((FullApplySite::isa(inst) || isa<YieldInst>(inst)) &&
          aa->mayWriteToMemory(inst, copySrc)) {
        return nullptr;
      }

      return inst;
    }

    if (aa->mayWriteToMemory(inst, copySrc)) {
      LLVM_DEBUG(llvm::dbgs() << "  Source modified by" << *iter);
      return nullptr;
    }
  }
  // For some reason, not all normal uses have been seen between the copy and
  // the end of the initialization block. We should never reach here.
  return nullptr;
}

/// Tries to move an end_access down to extend the access scope over all uses
/// of the temporary. For example:
///
///   %a = begin_access %src
///   copy_addr %a to [init] %temp : $*T
///   end_access %a
///   use %temp
///
/// We must not replace %temp with %a after the end_access. Instead we try to
/// move the end_access after "use %temp".
bool TempRValueOptPass::extendAccessScopes(
    CopyAddrInst *copyInst, SILInstruction *lastUseInst, AliasAnalysis *aa) {

  SILValue copySrc = copyInst->getSrc();
  EndAccessInst *endAccessToMove = nullptr;
  auto begin = std::next(copyInst->getIterator());
  auto end = std::next(lastUseInst->getIterator());

  for (SILInstruction &inst : make_range(begin, end)) {
    if (auto *endAccess = dyn_cast<EndAccessInst>(&inst)) {
      // To keep things simple, we can just move a single end_access. Also, we
      // cannot move an end_access over a (non-aliasing) end_access.
      if (endAccessToMove)
        return false;
      // Is this the end of an access scope of the copy-source?
      if (!aa->isNoAlias(copySrc, endAccess->getSource()) &&

          // There cannot be any aliasing modifying accesses within the
          // liverange of the temporary, because we would have cought this in
          // `getLastUseWhileSourceIsNotModified`.
          // But there are cases where `AliasAnalysis::isNoAlias` is less
          // precise than `AliasAnalysis::mayWriteToMemory`. Therefore, just
          // ignore any non-read accesses.
          endAccess->getBeginAccess()->getAccessKind() == SILAccessKind::Read) {

        // Don't move instructions beyond the block's terminator.
        if (isa<TermInst>(lastUseInst))
          return false;

        endAccessToMove = endAccess;
      }
    } else if (endAccessToMove) {
      // We cannot move an end_access over a begin_access. This would destroy
      // the proper nesting of accesses.
      if (isa<BeginAccessInst>(&inst) || isa<BeginUnpairedAccessInst>(inst))
        return false;
      // Don't extend a read-access scope over a (potential) write.
      // Note that inst can be a function call containing other access scopes.
      // But doing the mayWriteToMemory check, we know that the function can
      // only contain read accesses (to the same memory location). So it's fine
      // to move endAccessToMove even over such a function call.
      if (aa->mayWriteToMemory(&inst, endAccessToMove->getSource()))
        return false;
    }
  }
  if (endAccessToMove)
    endAccessToMove->moveAfter(lastUseInst);

  return true;
}

/// Return true if the \p tempObj, which is initialized by \p copyInst, is
/// destroyed in an orthodox way.
///
/// When tryOptimizeCopyIntoTemp replaces all of tempObj's uses, it assumes that
/// the object is initialized by the original copy and directly destroyed on all
/// paths by one of the recognized 'destroy_addr' or 'copy_addr [take]'
/// operations. This assumption must be checked. For example, in non-OSSA,
/// it is legal to destroy an in-memory object by loading the value and
/// releasing it. Rather than detecting unbalanced load releases, simply check
/// that tempObj is destroyed directly on all paths.
bool TempRValueOptPass::checkTempObjectDestroy(
    AllocStackInst *tempObj, CopyAddrInst *copyInst) {
  // ValueLifetimeAnalysis is not normally used for address types. It does not
  // reason about the lifetime of the in-memory object. However the utility can
  // be abused here to check that the address is directly destroyed on all
  // paths. collectLoads has already guaranteed that tempObj's lifetime has no
  // holes/reinitializations.
  SmallVector<SILInstruction *, 8> users;
  for (auto result : tempObj->getResults()) {
    for (Operand *operand : result->getUses()) {
      SILInstruction *user = operand->getUser();
      if (user == copyInst)
        continue;
      if (isa<DeallocStackInst>(user))
        continue;
      users.push_back(user);
    }
  }
  // Find the boundary of tempObj's address lifetime, starting at copyInst.
  ValueLifetimeAnalysis vla(copyInst, users);
  ValueLifetimeAnalysis::Frontier tempAddressFrontier;
  if (!vla.computeFrontier(tempAddressFrontier,
                           ValueLifetimeAnalysis::DontModifyCFG)) {
    return false;
  }
  // Check that the lifetime boundary ends at direct destroy points.
  for (SILInstruction *frontierInst : tempAddressFrontier) {
    auto pos = frontierInst->getIterator();
    // If the frontier is at the head of a block, then either it is an
    // unexpected lifetime exit, or the lifetime ended at a
    // terminator. TempRValueOptPass does not handle either case.
    if (pos == frontierInst->getParent()->begin())
      return false;

    // Look for a known destroy point as described in the function level
    // comment. This allowlist can be expanded as more cases are handled in
    // tryOptimizeCopyIntoTemp during copy replacement.
    SILInstruction *lastUser = &*std::prev(pos);
    if (isa<DestroyAddrInst>(lastUser))
      continue;

    if (auto *cai = dyn_cast<CopyAddrInst>(lastUser)) {
      assert(cai->getSrc() == tempObj && "collectLoads checks for writes");
      if (cai->isTakeOfSrc())
        continue;
    }
    return false;
  }
  return true;
}

/// Tries to perform the temporary rvalue copy elimination for \p copyInst
void TempRValueOptPass::tryOptimizeCopyIntoTemp(CopyAddrInst *copyInst) {
  if (!copyInst->isInitializationOfDest())
    return;

  auto *tempObj = dyn_cast<AllocStackInst>(copyInst->getDest());
  if (!tempObj)
    return;

  // If the temporary storage is lexical, it either came from a source-level var
  // or was marked lexical because it was passed to a function that has been
  // inlined.
  // TODO: [begin_borrow_addr] Once we can mark addresses as being borrowed, we
  //       won't need to mark alloc_stacks lexical during inlining.  At that
  //       point, the above comment should change, but the implementation
  //       remains the same.
  //
  // In either case, we can eliminate the temporary if the source of the copy is
  // lexical and it is live for longer than the temporary.
  if (tempObj->isLexical()) {
    // TODO: Determine whether the base of the copy_addr's source is lexical and
    //       its live range contains the range in which the alloc_stack
    //       contains the value copied into it via the copy_addr.
    //
    // For now, only look for guaranteed arguments.
    auto storage = AccessStorageWithBase::compute(copyInst->getSrc());
    if (!storage.base)
      return;
    if (auto *arg = dyn_cast<SILFunctionArgument>(storage.base))
      if (arg->getOwnershipKind() != OwnershipKind::Guaranteed)
        return;
  }

  bool isOSSA = copyInst->getFunction()->hasOwnership();
  
  SILValue copySrc = copyInst->getSrc();
  assert(tempObj != copySrc && "can't initialize temporary with itself");

  // If the source of the copyInst is taken, it must be deinitialized (via
  // destroy_addr, load [take], copy_addr [take]).  This must be done at the
  // right spot: after the last use tempObj, but before any (potential)
  // re-initialization of the source.
  bool needFinalDeinit = copyInst->isTakeOfSrc();

  // Scan all uses of the temporary storage (tempObj) to verify they all refer
  // to the value initialized by this copy. It is sufficient to check that the
  // only users that modify memory are the copy_addr [initialization] and
  // destroy_addr.
  InstructionSetWithSize loadInsts(getFunction());
  // Set of tempObj users
  InstructionSet userSet(getFunction());
  for (auto *useOper : tempObj->getUses()) {
    SILInstruction *user = useOper->getUser();

    userSet.insert(user);

    if (user == copyInst)
      continue;

    // Deallocations are allowed to be in a different block.
    if (isa<DeallocStackInst>(user))
      continue;

    // Also, destroys are allowed to be in a different block.
    if (isa<DestroyAddrInst>(user)) {
      if (!isOSSA && needFinalDeinit) {
        // In non-OSSA mode, for the purpose of inserting the destroy of
        // copySrc, we have to be conservative and assume that the lifetime of
        // tempObj goes beyond it's last use - until the final destroy_addr.
        // Otherwise we would risk of inserting the destroy too early.
        // So we just treat the destroy_addr as any other use of tempObj.
        if (user->getParent() != copyInst->getParent())
          return;
        loadInsts.insert(user);
      }
      continue;
    }

    if (!collectLoads(useOper, copyInst, loadInsts))
      return;
  }

  // Check and return without optimization if we have any users of tempObj that
  // precede the copyInst.
  // This can happen with projections.
  // TODO: We can enable this case if we clone the projections at "load" uses

  // All instructions in userSet are in the same block as copyInst. collectLoads
  // ensures of this.
  for (SILInstruction &inst : llvm::make_range(copyInst->getParent()->begin(),
                                               copyInst->getIterator())) {
    if (userSet.contains(&inst)) {
      return;
    }
  }

  AliasAnalysis *aa = getPassManager()->getAnalysis<AliasAnalysis>(getFunction());

  // Check if the source is modified within the lifetime of the temporary.
  SILInstruction *lastLoadInst =
    getLastUseWhileSourceIsNotModified(copyInst, loadInsts, aa);
  if (!lastLoadInst)
    return;

  // We cannot insert the destroy of copySrc after lastLoadInst if copySrc is
  // re-initialized by exactly this instruction.
  // This is a corner case, but can happen if lastLoadInst is a copy_addr.
  // Example:
  //   copy_addr [take] %copySrc to [init] %tempObj   // copyInst
  //   copy_addr [take] %tempObj to [init] %copySrc   // lastLoadInst
  if (needFinalDeinit && lastLoadInst != copyInst &&
      !isa<DestroyAddrInst>(lastLoadInst) &&
      aa->mayWriteToMemory(lastLoadInst, copySrc))
    return;

  if (!isOSSA && !checkTempObjectDestroy(tempObj, copyInst))
    return;

  if (!extendAccessScopes(copyInst, lastLoadInst, aa))
    return;

  LLVM_DEBUG(llvm::dbgs() << "  Success: replace temp" << *tempObj);

  // If copyInst's source must be deinitialized, whether that must be done via
  // a newly created destroy_addr.
  //
  // If lastLoadInst is a load or a copy_addr, then the deinitialization can be
  // done in that instruction.
  //
  // This is necessary for correctness: otherwise, copies of move-only values
  // would be introduced.
  bool needToInsertDestroy = [&]() {
    if (!needFinalDeinit)
      return false;
    if (lastLoadInst == copyInst)
      return true;
    if (auto *cai = dyn_cast<CopyAddrInst>(lastLoadInst)) {
      if (cai->getSrc() == tempObj && cai->isTakeOfSrc()) {
        // This copy_addr [take] will perform the final deinitialization.
        return false;
      }
      assert(!tempObj->getType().isMoveOnly() &&
             "introducing copy of move-only value!?");
      return true;
    }
    if (auto *li = dyn_cast<LoadInst>(lastLoadInst)) {
      if (li->getOperand() == tempObj &&
          li->getOwnershipQualifier() == LoadOwnershipQualifier::Take) {
        // This load [take] will perform the final deinitialization.
        return false;
      }
      assert(!tempObj->getType().isMoveOnly() &&
             "introducing copy of move-only value!?");
      return true;
    }
    return true;
  }();
  if (needToInsertDestroy) {
    // Compensate the [take] of the original copyInst.
    SILBuilderWithScope::insertAfter(lastLoadInst, [&] (SILBuilder &builder) {
      builder.createDestroyAddr(builder.getInsertionPoint()->getLoc(), copySrc);
    });
  }

  // * Replace all uses of the tempObj with the copySrc.
  //
  // * Delete the destroy(s) of tempObj (to compensate the removal of the
  //   original copyInst): either by erasing the destroy_addr or by converting
  //   load/copy_addr [take] into copying instructions.
  //
  // Note: we must not delete the original copyInst because it would crash the
  // instruction iteration in run(). Instead the copyInst gets identical Src and
  // Dest operands.
  while (!tempObj->use_empty()) {
    Operand *use = *tempObj->use_begin();
    SILInstruction *user = use->getUser();

    switch (user->getKind()) {
    case SILInstructionKind::DestroyAddrInst:
    case SILInstructionKind::DeallocStackInst:
      user->eraseFromParent();
      break;
    case SILInstructionKind::CopyAddrInst: {
      auto *cai = cast<CopyAddrInst>(user);
      if (cai != copyInst) {
        assert(cai->getSrc() == tempObj);
        if (cai->isTakeOfSrc() && (!needFinalDeinit || lastLoadInst != cai)) {
          cai->setIsTakeOfSrc(IsNotTake);
        }
      }
      use->set(copySrc);
      break;
    }
    case SILInstructionKind::LoadInst: {
      auto *li = cast<LoadInst>(user);
      if (li->getOwnershipQualifier() == LoadOwnershipQualifier::Take &&
          (!needFinalDeinit || li != lastLoadInst)) {
        li->setOwnershipQualifier(LoadOwnershipQualifier::Copy);
      }
      use->set(copySrc);
      break;
    }

    // ASSUMPTION: no operations that may be handled by this default clause can
    // destroy tempObj. This includes operations that load the value from memory
    // and release it or cast the address before destroying it.
    default:
      use->set(copySrc);
      break;
    }
  }

  tempObj->eraseFromParent();
  invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
}

SILBasicBlock::iterator
TempRValueOptPass::tryOptimizeStoreIntoTemp(StoreInst *si) {
  // If our store is an assign, bail.
  if (si->getOwnershipQualifier() == StoreOwnershipQualifier::Assign)
    return std::next(si->getIterator());

  auto *tempObj = dyn_cast<AllocStackInst>(si->getDest());
  if (!tempObj) {
    return std::next(si->getIterator());
  }

  // If the temporary storage is lexical, it either came from a source-level var
  // or was marked lexical because it was passed to a function that has been
  // inlined.
  // TODO: [begin_borrow_addr] Once we can mark addresses as being borrowed, we
  //       won't need to mark alloc_stacks lexical during inlining.  At that
  //       point, the above comment should change, but the implementation
  //       remains the same.
  //
  // In either case, we can eliminate the temporary if the source of the store
  // is lexical and it is live for longer than the temporary.
  if (tempObj->isLexical()) {
    // TODO: Find the lexical root of the source, if any, and allow optimization
    //       if its live range contains the range in which the alloc_stack
    //       contains the value stored into it.
    return std::next(si->getIterator());
  }

  // If our tempObj has a dynamic lifetime (meaning it is conditionally
  // initialized, conditionally taken, etc), we can not convert its uses to SSA
  // while eliminating it simply. So bail.
  if (tempObj->hasDynamicLifetime()) {
    return std::next(si->getIterator());
  }

  // Scan all uses of the temporary storage (tempObj) to verify they all refer
  // to the value initialized by this copy. It is sufficient to check that the
  // only users that modify memory are the copy_addr [initialization] and
  // destroy_addr.
  for (auto *useOper : tempObj->getUses()) {
    SILInstruction *user = useOper->getUser();

    if (user == si)
      continue;

    // Bail if there is any kind of user which is not handled in the code below.
    switch (user->getKind()) {
      case SILInstructionKind::DestroyAddrInst:
      case SILInstructionKind::DeallocStackInst:
      case SILInstructionKind::LoadInst:
      case SILInstructionKind::FixLifetimeInst:
        break;
      case SILInstructionKind::CopyAddrInst:
        if (cast<CopyAddrInst>(user)->getDest() == tempObj)
          return std::next(si->getIterator());
        break;
      case SILInstructionKind::MarkDependenceInst:
        if (cast<MarkDependenceInst>(user)->getValue() == tempObj)
          return std::next(si->getIterator());
        break;
      default:
        return std::next(si->getIterator());
    }
  }

  // Since store is always a consuming operation, we do not need to worry about
  // any lifetime constraints and can just replace all of the uses here. This
  // contrasts with the copy_addr implementation where we need to consider the
  // possibility that the source address is written to.
  LLVM_DEBUG(llvm::dbgs() << "  Success: replace temp" << *tempObj);

  // Do a "replaceAllUses" by either deleting the users or replacing them with
  // the appropriate operation on the source value.
  SmallVector<SILInstruction *, 4> toDelete;
  for (auto *use : tempObj->getUses()) {
    // If our store is the user, just skip it.
    if (use->getUser() == si) {
      continue;
    }

    SILInstruction *user = use->getUser();
    switch (user->getKind()) {
    case SILInstructionKind::DestroyAddrInst: {
      SILBuilderWithScope builder(user);
      builder.emitDestroyValueOperation(user->getLoc(), si->getSrc());
      toDelete.push_back(user);
      break;
    }
    case SILInstructionKind::DeallocStackInst:
      toDelete.push_back(user);
      break;
    case SILInstructionKind::CopyAddrInst: {
      auto *cai = cast<CopyAddrInst>(user);
      assert(cai->getSrc() == tempObj);
      SILBuilderWithScope builder(user);
      auto qualifier = cai->isInitializationOfDest()
                           ? StoreOwnershipQualifier::Init
                           : StoreOwnershipQualifier::Assign;
      SILValue src = si->getSrc();
      if (!cai->isTakeOfSrc()) {
        src = builder.emitCopyValueOperation(cai->getLoc(), src);
      }
      builder.emitStoreValueOperation(cai->getLoc(), src, cai->getDest(),
                                      qualifier);
      toDelete.push_back(cai);
      break;
    }
    case SILInstructionKind::LoadInst: {
      // Since store is always forwarding, we know that we should have our own
      // value here. So, we should be able to just RAUW any load [take] and
      // insert a copy + RAUW for any load [copy].
      auto *li = cast<LoadInst>(user);
      SILValue srcObject = si->getSrc();
      if (li->getOwnershipQualifier() == LoadOwnershipQualifier::Copy) {
        SILBuilderWithScope builder(li);
        srcObject = builder.emitCopyValueOperation(li->getLoc(), srcObject);
      }
      li->replaceAllUsesWith(srcObject);
      toDelete.push_back(li);
      break;
    }
    case SILInstructionKind::FixLifetimeInst: {
      auto *fli = cast<FixLifetimeInst>(user);
      SILBuilderWithScope builder(fli);
      builder.createFixLifetime(fli->getLoc(), si->getSrc());
      toDelete.push_back(fli);
      break;
    }
    case SILInstructionKind::MarkDependenceInst: {
      auto mdi = cast<MarkDependenceInst>(user);
      assert(mdi->getBase() == tempObj);
      SILBuilderWithScope builder(user);
      auto newInst = builder.createMarkDependence(user->getLoc(),
                                                  mdi->getValue(),
                                                  si->getSrc(),
                                                  mdi->dependenceKind());
      mdi->replaceAllUsesWith(newInst);
      toDelete.push_back(mdi);
      break;
    }
    // ASSUMPTION: no operations that may be handled by this default clause can
    // destroy tempObj. This includes operations that load the value from memory
    // and release it.
    default:
      llvm::errs() << "Unhandled user: " << *user;
      llvm_unreachable("Unhandled case?!");
      break;
    }
  }

  while (!toDelete.empty()) {
    auto *inst = toDelete.pop_back_val();
    inst->dropAllReferences();
    inst->eraseFromParent();
  }
  auto nextIter = std::next(si->getIterator());
  si->eraseFromParent();
  tempObj->eraseFromParent();
  invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);

  return nextIter;
}

//===----------------------------------------------------------------------===//
//                           High Level Entrypoint
//===----------------------------------------------------------------------===//

/// The main entry point of the pass.
void TempRValueOptPass::run() {
  auto *function = getFunction();

  auto *da = PM->getAnalysis<DominanceAnalysis>();

  LLVM_DEBUG(llvm::dbgs() << "Copy Peephole in Func " << function->getName()
                          << "\n");

  SmallVector<SILValue> valuesToComplete;

  // Find all copy_addr instructions.
  llvm::SmallSetVector<CopyAddrInst *, 8> deadCopies;

  for (auto &block : *function) {
    // Increment the instruction iterator only after calling
    // tryOptimizeCopyIntoTemp because the instruction after CopyInst might be
    // deleted, but copyInst itself won't be deleted until later.
    for (auto ii = block.begin(); ii != block.end();) {
      if (auto *copyInst = dyn_cast<CopyAddrInst>(&*ii)) {
        // In case of success, this may delete instructions, but not the
        // CopyInst itself.
        tryOptimizeCopyIntoTemp(copyInst);
        // Remove identity copies which either directly result from successfully
        // calling tryOptimizeCopyIntoTemp or was created by an earlier
        // iteration, where another copy_addr copied the temporary back to the
        // source location.
        if (copyInst->getSrc() == copyInst->getDest()) {
          deadCopies.insert(copyInst);
        }
        ++ii;
        continue;
      }

      if (auto *si = dyn_cast<StoreInst>(&*ii)) {
        auto stored = si->getSrc();
        bool isOrHasEnum = stored->getType().isOrHasEnum();
        auto nextIter = std::next(si->getIterator());

        ii = tryOptimizeStoreIntoTemp(si);

        // If the optimization was successful, and the stack loc was an enum
        // type, collect the stored value for lifetime completion.
        // This is needed because we can have incomplete address lifetimes on
        // none/trivial paths for an enum type. Once we convert to value form,
        // this will cause incomplete value lifetimes which can raise ownership
        // verification errors, because we rely on linear lifetimes in OSSA.
        if (ii == nextIter && isOrHasEnum) {
          valuesToComplete.push_back(stored);
        }
        continue;
      }

      ++ii;
    }
  }

  auto callbacks = InstModCallbacks().onDelete(
    [](SILInstruction *instToKill) {
      // SimplifyInstruction is not in the business of removing
      // copy_addr. If it were, then we would need to update deadCopies.
      assert(!isa<CopyAddrInst>(instToKill));
      instToKill->eraseFromParent();
    }
  );

  DeadEndBlocks deBlocks(function);
  for (auto *deadCopy : deadCopies) {
    auto *srcInst = deadCopy->getSrc()->getDefiningInstruction();
    deadCopy->eraseFromParent();
    // Simplify any access scope markers that were only used by the dead
    // copy_addr and other potentially unused addresses.
    if (srcInst) {
      simplifyAndReplaceAllSimplifiedUsesAndErase(srcInst, callbacks, &deBlocks);
    }
  }
  if (!deadCopies.empty()) {
    invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }

  // Call the utlity to complete ossa lifetime.
  OSSALifetimeCompletion completion(function, da->get(function));
  for (auto it : valuesToComplete) {
    completion.completeOSSALifetime(it,
                                    OSSALifetimeCompletion::Boundary::Liveness);
  }
}

SILTransform *swift::createTempRValueOpt() { return new TempRValueOptPass(); }
