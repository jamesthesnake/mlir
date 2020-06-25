//
// Created by martin on 26/11/2019.
//

#include "mlir/Conversion/RiseToImperative/ConvertRiseToImperative.h"
#include "mlir/Dialect/Rise/IR/Dialect.h"
#include "mlir/Dialect/Rise/Passes.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/Builders.h"

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"

#include <iostream>
#include <mlir/EDSC/Builders.h>

using namespace mlir;
using namespace mlir::rise;

namespace {
struct ConvertRiseToImperativePass
    : public RiseToImperativeBase<ConvertRiseToImperativePass> {
  void runOnFunction() override;
};
} // namespace

//===----------------------------------------------------------------------===//
// Patterns
//===----------------------------------------------------------------------===//

struct RiseToImperativePattern : public OpRewritePattern<FuncOp> {
  using OpRewritePattern<FuncOp>::OpRewritePattern;
  LogicalResult match(FuncOp funcOp) const override;
  void rewrite(FuncOp funcOp, PatternRewriter &rewriter) const override;
};

LogicalResult RiseToImperativePattern::match(FuncOp funcOp) const {
  bool riseInside = false;

  if (funcOp.isExternal())
    return failure();

  // Only unlowered rise programs contain RiseInOps
  funcOp.walk([&](Operation *op) {
    if (isa<InOp>(op))
      riseInside = true;
  });

  if (riseInside) {
    return success();
  } else {
    return failure();
  }
}

void RiseToImperativePattern::rewrite(FuncOp funcOp,
                                      PatternRewriter &rewriter) const {
  Block &block = funcOp.getBody().front();
  funcOp.walk([&](Operation *op) {
    if (InOp inOp = dyn_cast<InOp>(op)) {
      rewriter.setInsertionPointAfter(inOp);
    }
  });

  // Start at the back and find the rise.out op
  OutOp outOp;
  for (auto op = block.rbegin(); op != block.rend(); op++) {
    if (isa<OutOp>(*op)) {
      outOp = cast<OutOp>(*op);
      break;
    }
  }

  // not sure how to report an error in this pattern - with just emitError or
  // assertions?
  assert(outOp &&
         "Result operand of rise.out has to be the result of a rise.apply op!");

  // insert cast of out to fit our type system
  Value out = rewriter
                  .create<CastOp>(outOp.getLoc(), outOp.getOperand(1).getType(),
                                  outOp.getOperand(0))
                  .getResult();

  if (ApplyOp apply = dyn_cast<ApplyOp>(outOp.getOperand(1).getDefiningOp())) {
    AccT(apply, out, rewriter);
  } else {
    emitError(outOp.getLoc())
        << "Result of rise.out has to be the result of a rise.apply op!";
    return;
  }

  // Cleanup of leftover ops from AccT
  SmallVector<Operation *, 10> leftoverOps = {};
  funcOp.walk([&leftoverOps](Operation *inst) {
    if (!inst)
      return;
    if (isa<ApplyOp>(inst) || isa<LambdaOp>(inst) || isa<MapSeqOp>(inst) ||
        isa<MapParOp>(inst) || isa<ReduceSeqOp>(inst) || isa<OutOp>(inst) ||
        isa<LiteralOp>(inst) || isa<TransposeOp>(inst) || isa<SplitOp>(inst) ||
        isa<JoinOp>(inst) || isa<SlideOp>(inst) || isa<PadOp>(inst)) {
      if (inst->getParentOfType<LambdaOp>()) {
      } else {
        leftoverOps.push_back(inst);
      }
    }
    return;
  });
  size_t unneededLeftoverOps = leftoverOps.size();
  for (size_t i = 0; i < unneededLeftoverOps; i++) {
    auto op = leftoverOps.pop_back_val();
    op->dropAllUses();
    op->dropAllReferences();
    rewriter.eraseOp(op);
  }

  // Translation to imperative
  emitRemark(funcOp.getLoc()) << "AccT finished. Starting CodeGen.";

  SmallVector<rise::AssignOp, 10> assignOps;
  funcOp.walk([&assignOps](Operation *inst) {
    if (!inst)
      return;
    if (isa<AssignOp>(inst)) {
      assignOps.push_back(cast<AssignOp>(inst));
    }
    return;
  });

  funcOp.dump();
  // Codegen:
  bool doCodegen = true;
  SmallVector<Operation *, 10> erasureList = {};
  if (doCodegen) {
    for (rise::AssignOp assign : assignOps) {
      codeGen(assign, {}, rewriter);
    }
  }
  emitRemark(funcOp.getLoc()) << "CodeGen finished. Starting Cleanup.";

  funcOp.dump();
  //   cleanup:
  //   erase intermediate operations.
  //   We remove them back to front right now,
  //   this should be alright in terms of dependencies.

  if (doCodegen) {
    funcOp.walk([&](Operation *inst) {
      if (!inst->getDialect())
        return;
      if (inst->getDialect()->getNamespace().equals("rise")) {
        if (!(inst->getParentOfType<LambdaOp>() ||
              inst->getParentOfType<EmbedOp>())) {
          erasureList.push_back(inst);
        }
      }
      return;
    });
  }

  size_t unneededOps = erasureList.size();
  for (size_t i = 0; i < unneededOps; i++) {
    auto op = erasureList.pop_back_val();
    op->dropAllUses();
    op->dropAllReferences();
    rewriter.eraseOp(op);
  }

  return;
}

/// Acceptor Translation
void mlir::rise::AccT(ReturnOp returnOp, Value out, PatternRewriter &rewriter) {
  if (!returnOp.getOperand(0).isa<OpResult>()) {
    emitRemark(returnOp.getLoc())
        << "Directly returning an argument is not supported in lowering to "
           "imperative currently";
    return;
  }
  if (ApplyOp apply =
          dyn_cast<ApplyOp>(returnOp.getOperand(0).getDefiningOp())) {
    AccT(apply, out, rewriter);
    return;
  } else if (EmbedOp embedOp =
                 dyn_cast<EmbedOp>(returnOp.getOperand(0).getDefiningOp())) {
    emitRemark(returnOp.getLoc())
        << "AccT of EmbedOp. Copy operations from this block to result.";
    assert(
        embedOp.getNumOperands() ==
            embedOp.region().front().getNumArguments() &&
        "Embed has to have the same number of operands and block arguments!");

    // Translating all operands first
    for (int i = 0; i < embedOp.getOperands().size(); i++) {
      auto operand = embedOp.getOperand(i);
      auto operandCont = ConT(operand, rewriter.getInsertionPoint(), rewriter);
      embedOp.setOperand(i, operandCont);
    }

    auto newEmbed = rewriter.clone(*embedOp.getOperation());

    auto assignment = rewriter.create<AssignOp>(newEmbed->getLoc(),
                                                newEmbed->getResult(0), out);
    //    rewriter.eraseOp(embedOp);

    return;
  } else {
    emitError(returnOp.getLoc()) << "AccT of a .return went wrong!";
    return;
  }
}

void mlir::rise::AccT(ApplyOp apply, Value out, PatternRewriter &rewriter) {
  if (out.getType().getDialect().getNamespace() !=
      RiseDialect::getDialectNamespace()) {
    emitError(apply.getLoc()) << "type of out is wrong! Dumping it";
    out.getType().dump();
  }

  OpBuilder::InsertPoint savedInsertionPoint = rewriter.saveInsertionPoint();

  Operation *appliedFun = apply.getOperand(0).getDefiningOp();
  Location loc = apply.getLoc();

  if (isa<ReduceSeqOp>(appliedFun)) {
    emitRemark(appliedFun->getLoc()) << "AccT of ReduceSeq";

    auto n = appliedFun->getAttrOfType<NatAttr>("n");
    auto s = appliedFun->getAttrOfType<DataTypeAttr>("s");
    auto t = appliedFun->getAttrOfType<DataTypeAttr>("t");

    auto reductionFun = apply.getOperand(1);
    auto initializer = apply.getOperand(2);
    auto array = apply.getOperand(3);

    // Add Continuation for array.
    auto contArray = ConT(array, rewriter.getInsertionPoint(), rewriter);

    auto cst_zero = rewriter.create<ConstantIndexOp>(appliedFun->getLoc(), 0);

    bool defineNewAccumulator = false;
    // Accumulator for Reduction
    // TODO: do this properly! This can be way better structured
    Value accum;
    if (defineNewAccumulator) {

      EmbedOp embedOp = rewriter.create<EmbedOp>(
          appliedFun->getLoc(),
          ScalarType::get(rewriter.getContext(),
                          FloatType::getF32(rewriter.getContext())),
          ValueRange());
      //      Block *embedBlock = new Block();
      //      embedOp.region().push_front(embedBlock);
      rewriter.setInsertionPointToStart(&embedOp.region().front());

      auto contInit = ConT(initializer, rewriter.getInsertionPoint(), rewriter);

      AllocOp alloc = rewriter.create<AllocOp>(
          appliedFun->getLoc(),
          MemRefType::get(ArrayRef<int64_t>{1},
                          FloatType::getF32(rewriter.getContext())));

      rewriter.create<linalg::FillOp>(initializer.getLoc(), alloc.getResult(),
                                      contInit);
      rewriter.create<rise::ReturnOp>(initializer.getLoc(), alloc.getResult());

      rewriter.setInsertionPointAfter(embedOp);
      accum = embedOp.getResult();
    } else {
      // TODO: not sure about this one

      accum = out;
      Value indexedAccum;
      if (out.getType().isa<ArrayType>()) {
        indexedAccum = rewriter.create<IdxOp>(
            accum.getLoc(), accum.getType().cast<ArrayType>().getElementType(),
            accum, cst_zero.getResult());
      } else {
        indexedAccum = accum;
      }

      //      EmbedOp embedOp = rewriter.create<EmbedOp>(
      //          appliedFun->getLoc(),
      //          ScalarType::get(rewriter.getContext(),
      //                          FloatType::getF32(rewriter.getContext())),
      //          ValueRange());
      //      Block *embedBlock = new Block();
      //      embedOp.region().push_front(embedBlock);
      //      rewriter.setInsertionPointToStart(&embedOp.region().front());

      auto contInit = ConT(initializer, rewriter.getInsertionPoint(), rewriter);

      //      rewriter.setInsertionPointAfter(embedOp);
      auto initAccum = rewriter.create<AssignOp>(appliedFun->getLoc(), contInit,
                                                 indexedAccum);
    }
    // zero constant for indexing

    Value loopInductionVar;
    Block *forLoopBody;

    // lowering to a specific loop depending on the lowering target dialect
    std::string loweringTarget;
    if (StringAttr loweringTargetAttr =
            appliedFun->getAttrOfType<StringAttr>("to")) {
      loweringTarget = loweringTargetAttr.getValue().str();
    } else {
      // default lowering target
      loweringTarget = "loop";
    }

    if (loweringTarget == "loop") {
      auto lowerBound =
          rewriter.create<ConstantIndexOp>(appliedFun->getLoc(), 0);
      auto upperBound = rewriter.create<ConstantIndexOp>(
          appliedFun->getLoc(), n.getValue().getIntValue());
      auto step = rewriter.create<ConstantIndexOp>(appliedFun->getLoc(), 1);

      auto forLoop =
          rewriter.create<mlir::scf::ForOp>(loc, lowerBound, upperBound, step);
      loopInductionVar = forLoop.getInductionVar();
      forLoopBody = forLoop.getBody();
    } else if (loweringTarget == "affine") {
      auto forLoop =
          rewriter.create<AffineForOp>(loc, 0, n.getValue().getIntValue(), 1);
      loopInductionVar = forLoop.getInductionVar();
      forLoopBody = forLoop.getBody();
    }

    rewriter.setInsertionPointToStart(forLoopBody);
    LambdaOp reductionLambda = dyn_cast<LambdaOp>(reductionFun.getDefiningOp());

    // TODO: not needed anymore
    //    // index into input
    IdxOp xi;
    //    if (contArray.getType().isa<MemRefType>()) {
    //      ArrayRef<int64_t> inShape =
    //          contArray.getType().dyn_cast<MemRefType>().getShape().drop_back(1);
    //      Type inIndexOpResult;
    //      if (inShape.size() > 0) {
    //        inIndexOpResult =
    //            MemRefType::get(inShape,
    //            FloatType::getF32(rewriter.getContext()));
    //      } else {
    //        inIndexOpResult = FloatType::getF32(rewriter.getContext());
    //      }
    //      xi = rewriter.create<IdxOp>(loc, inIndexOpResult, contArray,
    //                                      loopInductionVar);
    //      //      std::cout << "contArray is of MemrefType";
    //    } else if (isa<IdxOp>(contArray.getDefiningOp())) {
    //      //      std::cout << "contArray is not of MemrefType";
    //    }

    if (contArray.getType().isa<ArrayType>()) {
      xi = rewriter.create<IdxOp>(
          loc, contArray.getType().cast<ArrayType>().getElementType(),
          contArray, loopInductionVar);
    }

    // I think we don't need these anymore. Just pass the result of rise.embed
    // here
    //    IdxOp accumIdx;
    //    if (defineNewAccumulator) {
    //      // index into acc
    //      accumIdx = rewriter.create<IdxOp>(
    //          accum.getLoc(), FloatType::getF32(rewriter.getContext()), accum,
    //          cst_zero.getResult());
    //    } else {
    //      accumIdx = rewriter.create<IdxOp>(
    //          accum.getLoc(), FloatType::getF32(rewriter.getContext()), accum,
    //          cst_zero.getResult());
    //      //      ArrayRef<int64_t> outShape =
    //      //          out.getType().dyn_cast<MemRefType>().getShape();
    //      //      MemRefType outIndexOpResult =
    //      //          MemRefType::get(outShape,
    //      //          FloatType::getF32(rewriter.getContext()));
    //      //      // TODO: at this point we sometimes need the 0 to access a
    //      val and
    //      //      // sometimes not.
    //      //      accumIdx = rewriter.create<IdxOp>(loc, outIndexOpResult,
    //      out,
    //      //                                            cst_zero.getResult());
    //    }
    // operate on a copy of the lambda to avoid generating dependencies.
    LambdaOp lambdaCopy = cast<LambdaOp>(rewriter.clone(*reductionLambda));
    auto fxi = rewriter.create<ApplyOp>(loc, lambdaCopy.getType(),
                                        lambdaCopy.getResult(),
                                        ValueRange{accum, xi.getResult()});

    AccT(fxi, accum, rewriter);
    // Copy of Lambda and corresponding Apply not needed anymore.
    fxi.getResult().dropAllUses();
    rewriter.eraseOp(fxi);
    lambdaCopy.getResult().dropAllUses();
    rewriter.eraseOp(lambdaCopy);

    // copy accumulator to output
    if (defineNewAccumulator) {
      rewriter.setInsertionPointAfter(forLoopBody->getParentOp());

      // index into accumulator
      IdxOp storeAccIdx = rewriter.create<IdxOp>(
          accum.getLoc(),
          accum.getType().dyn_cast<ArrayType>().getElementType(), accum,
          cst_zero.getResult());

      // index into output
      ArrayRef<int64_t> outShape =
          out.getType().dyn_cast<MemRefType>().getShape();
      MemRefType outIndexOpResult =
          MemRefType::get(outShape, FloatType::getF32(rewriter.getContext()));

      auto out0 = rewriter.create<IdxOp>(loc, outIndexOpResult, out,
                                         cst_zero.getResult());

      rewriter.create<AssignOp>(appliedFun->getLoc(), storeAccIdx.getResult(),
                                out0.getResult());

      rewriter.restoreInsertionPoint(savedInsertionPoint);
    }

    return;

  } else if (isa<MapSeqOp>(appliedFun)) {
    emitRemark(appliedFun->getLoc()) << "AccT of MapSeq";

    auto n = appliedFun->getAttrOfType<NatAttr>("n");
    auto s = appliedFun->getAttrOfType<DataTypeAttr>("s");
    auto t = appliedFun->getAttrOfType<DataTypeAttr>("t");

    auto f = apply.getOperand(1);
    auto array = apply.getOperand(2);
    auto contArray = ConT(array, rewriter.getInsertionPoint(), rewriter);

    // zero constant for indexing
    auto cst_zero = rewriter.create<ConstantIndexOp>(appliedFun->getLoc(), 0);

    Value loopInductionVar;
    Block *forLoopBody;

    // lowering to a specific loop depending on the lowering target dialect
    std::string loweringTarget;
    if (StringAttr loweringTargetAttr =
            appliedFun->getAttrOfType<StringAttr>("to")) {
      loweringTarget = loweringTargetAttr.getValue().str();
    } else {
      // default lowering target
      loweringTarget = "loop";
    }

    if (loweringTarget == "loop") {
      auto lowerBound =
          rewriter.create<ConstantIndexOp>(appliedFun->getLoc(), 0);
      auto upperBound = rewriter.create<ConstantIndexOp>(
          appliedFun->getLoc(), n.getValue().getIntValue());
      auto step = rewriter.create<ConstantIndexOp>(appliedFun->getLoc(), 1);

      auto forLoop = rewriter.create<mlir::scf::ForOp>(
          appliedFun->getLoc(), lowerBound, upperBound, step);
      loopInductionVar = forLoop.getInductionVar();
      forLoopBody = forLoop.getBody();
    } else if (loweringTarget == "affine") {
      auto forLoop = rewriter.create<AffineForOp>(
          appliedFun->getLoc(), 0, n.getValue().getIntValue(), 1);
      loopInductionVar = forLoop.getInductionVar();
      forLoopBody = forLoop.getBody();
    }

    rewriter.setInsertionPointToStart(forLoopBody);

    LambdaOp fLambda = dyn_cast<LambdaOp>(f.getDefiningOp());

    IdxOp xi;

    // TODO: not needed anymore
    if (contArray.getType().isa<MemRefType>()) {
      ArrayRef<int64_t> inShape =
          contArray.getType().dyn_cast<MemRefType>().getShape().drop_back(1);

      MemRefType inIndexOpResult =
          MemRefType::get(inShape, FloatType::getF32(rewriter.getContext()));

      xi = rewriter.create<IdxOp>(loc, inIndexOpResult, contArray,
                                  loopInductionVar);
    } else if (isa<IdxOp>(contArray.getDefiningOp())) {
      //      std::cout << "got an idx and not a memref! \n" << std::flush;
    }

    if (contArray.getType().isa<ArrayType>()) {
      xi = rewriter.create<IdxOp>(
          loc, contArray.getType().cast<ArrayType>().getElementType(),
          contArray, loopInductionVar);
    }

    // operate on a copy of the lambda to avoid generating dependencies.
    LambdaOp lambdaCopy = cast<LambdaOp>(rewriter.clone(*fLambda));
    auto fxi = rewriter.create<ApplyOp>(loc, lambdaCopy.getType(),
                                        lambdaCopy.getResult(), xi.getResult());

    //    ArrayRef<int64_t> outShape =
    //        contArray.getType().dyn_cast<MemRefType>().getShape().drop_back(1);
    //    MemRefType outIndexOpResult =
    //        MemRefType::get(outShape,
    //        FloatType::getF32(rewriter.getContext()));

    auto outi = rewriter.create<IdxOp>(
        loc, out.getType().dyn_cast<ArrayType>().getElementType(), out,
        loopInductionVar);

    AccT(fxi, outi.getResult(), rewriter);

    // Copy of Lambda and corresponding Apply not needed anymore.
    fxi.getResult().dropAllUses();
    rewriter.eraseOp(fxi);
    lambdaCopy.getResult().dropAllUses();
    rewriter.eraseOp(lambdaCopy);

    rewriter.setInsertionPointAfter(forLoopBody->getParentOp());
    return;
  } else if (isa<MapParOp>(appliedFun)) {
    emitRemark(appliedFun->getLoc()) << "AccT of MapPar";

    //     For now we treat all maps as mapSeqs
    auto n = appliedFun->getAttrOfType<NatAttr>("n");
    auto s = appliedFun->getAttrOfType<DataTypeAttr>("s");
    auto t = appliedFun->getAttrOfType<DataTypeAttr>("t");

    auto f = apply.getOperand(1);
    auto array = apply.getOperand(2);

    auto contArray = ConT(array, rewriter.getInsertionPoint(), rewriter);

    Value loopInductionVar;
    Block *forLoopBody;

    // lowering to a specific loop depending on the lowering target dialect
    std::string loweringTarget;
    if (StringAttr loweringTargetAttr =
            appliedFun->getAttrOfType<StringAttr>("to")) {
      loweringTarget = loweringTargetAttr.getValue().str();
    } else {
      // default lowering target
      loweringTarget = "loop";
    }

    // TODO: These have no parallel semantics yet
    if (loweringTarget == "loop") {
      auto lowerBound =
          rewriter.create<ConstantIndexOp>(appliedFun->getLoc(), 0);
      auto upperBound = rewriter.create<ConstantIndexOp>(
          appliedFun->getLoc(), n.getValue().getIntValue());
      auto step = rewriter.create<ConstantIndexOp>(appliedFun->getLoc(), 1);

      auto forLoop = rewriter.create<mlir::scf::ForOp>(
          appliedFun->getLoc(), lowerBound, upperBound, step);
      loopInductionVar = forLoop.getInductionVar();
      forLoopBody = forLoop.getBody();
    } else if (loweringTarget == "affine") {
      auto forLoop = rewriter.create<AffineForOp>(
          appliedFun->getLoc(), 0, n.getValue().getIntValue(), 1);

      // TODO: Not working as intended
      //      auto lbMap = AffineMap::getConstantMap(0, rewriter.getContext());
      //      auto ubMap = AffineMap::getConstantMap(n.getValue().getIntValue(),
      //                                             rewriter.getContext());
      //
      //      auto parallelOp =
      //      rewriter.create<AffineParallelOp>(appliedFun->getLoc(),
      //                                                          lbMap,
      //                                                          ValueRange{},
      //                                                          ubMap,
      //                                                          ValueRange{});
      //      loopInductionVar = *parallelOp.getLowerBoundsOperands().begin();
      //      forLoopBody = parallelOp.getBody();
      loopInductionVar = forLoop.getInductionVar();
      forLoopBody = forLoop.getBody();
    }

    rewriter.setInsertionPointToStart(forLoopBody);

    LambdaOp fLambda = dyn_cast<LambdaOp>(f.getDefiningOp());
    //    if (!fLambda) {
    //      fLambda = expandToLambda(f, rewriter);
    //    }

    IdxOp xi;
    // TODO: not needed anymore
    //    if (contArray.getType().isa<MemRefType>()) {
    //      ArrayRef<int64_t> inShape =
    //          contArray.getType().dyn_cast<MemRefType>().getShape().drop_back(1);
    //
    //      MemRefType inIndexOpResult =
    //          MemRefType::get(inShape,
    //          FloatType::getF32(rewriter.getContext()));
    //
    //      xi = rewriter.create<IdxOp>(loc, inIndexOpResult, contArray,
    //                                      loopInductionVar);
    //    } else if (isa<IdxOp>(contArray.getDefiningOp())) {
    //      //      std::cout << "got an idx and not a memref! \n" <<
    //      std::flush;
    //    }
    if (contArray.getType().isa<ArrayType>()) {
      xi = rewriter.create<IdxOp>(
          loc, contArray.getType().cast<ArrayType>().getElementType(),
          contArray, loopInductionVar);
    }

    // operate on a copy of the lambda to avoid generating dependencies.
    LambdaOp lambdaCopy = cast<LambdaOp>(rewriter.clone(*fLambda));
    auto fxi = rewriter.create<ApplyOp>(loc, lambdaCopy.getType(),
                                        lambdaCopy.getResult(), xi.getResult());

    auto outi = rewriter.create<IdxOp>(
        loc, out.getType().dyn_cast<ArrayType>().getElementType(), out,
        loopInductionVar);

    AccT(fxi, outi.getResult(), rewriter);

    // Copy of Lambda and corresponding Apply not needed anymore.
    fxi.getResult().dropAllUses();
    rewriter.eraseOp(fxi);
    lambdaCopy.getResult().dropAllUses();
    rewriter.eraseOp(lambdaCopy);

    rewriter.setInsertionPointAfter(forLoopBody->getParentOp());
    return;
  } else if (FstOp fstOp = dyn_cast<FstOp>(appliedFun)) {
    emitRemark(appliedFun->getLoc()) << "AccT of Fst";

    auto tuple = apply.getOperand(1);
    auto contTuple = ConT(tuple, rewriter.getInsertionPoint(), rewriter);

    auto fstIntermOp = rewriter.create<FstIntermediateOp>(
        fstOp.getLoc(), FloatType::getF32(rewriter.getContext()), contTuple);
    auto assignment = rewriter.create<AssignOp>(appliedFun->getLoc(),
                                                fstIntermOp.getResult(), out);
    return;
  } else if (SndOp sndOp = dyn_cast<SndOp>(appliedFun)) {
    emitRemark(appliedFun->getLoc()) << "AccT of Snd";

    auto tuple = apply.getOperand(1);
    auto contTuple = ConT(tuple, rewriter.getInsertionPoint(), rewriter);

    auto sndIntermOp = rewriter.create<SndIntermediateOp>(
        sndOp.getLoc(), FloatType::getF32(rewriter.getContext()), contTuple);
    auto assignment = rewriter.create<AssignOp>(appliedFun->getLoc(),
                                                sndIntermOp.getResult(), out);
    return;
  } else if (SplitOp splitOp = dyn_cast<SplitOp>(appliedFun)) {
    emitRemark(appliedFun->getLoc()) << "AccT of Split";
    Nat n = splitOp.n();
    Nat m = splitOp.m();
    DataType t = splitOp.t();

    ArrayType splitAccType = ArrayType::get(
        rewriter.getContext(),
        Nat::get(rewriter.getContext(), n.getIntValue() * m.getIntValue()), t);

    auto splitAccInterm = rewriter.create<SplitAccIntermediateOp>(
        splitOp.getLoc(), splitAccType, out,
        splitOp.getAttrOfType<NatAttr>("n"),
        splitOp.getAttrOfType<NatAttr>("m"),
        splitOp.getAttrOfType<DataTypeAttr>("t"));

    if (isa<ApplyOp>(apply.getOperand(1).getDefiningOp())) {
      AccT(dyn_cast<ApplyOp>(apply.getOperand(1).getDefiningOp()),
           splitAccInterm.getResult(), rewriter);
    } else {
      emitError(appliedFun->getLoc())
          << "input to Split has to be result of ApplyOp"; // Is this actually a
                                                           // requirement?
    }
    return;
  } else if (JoinOp joinOp = dyn_cast<JoinOp>(appliedFun)) {
    emitRemark(appliedFun->getLoc()) << "AccT of Join";
    Nat n = joinOp.n();
    Nat m = joinOp.m();
    DataType t = joinOp.t();

    ArrayType joinAccType = ArrayType::get(
        rewriter.getContext(), n, ArrayType::get(rewriter.getContext(), m, t));

    auto joinAccInterm = rewriter.create<JoinAccIntermediateOp>(
        joinOp.getLoc(), joinAccType, out, joinOp.getAttrOfType<NatAttr>("n"),
        joinOp.getAttrOfType<NatAttr>("m"),
        joinOp.getAttrOfType<DataTypeAttr>("t"));

    if (isa<ApplyOp>(apply.getOperand(1).getDefiningOp())) {
      AccT(dyn_cast<ApplyOp>(apply.getOperand(1).getDefiningOp()),
           joinAccInterm.getResult(), rewriter);
    } else {
      emitError(appliedFun->getLoc())
          << "input to Join has to be result of ApplyOp"; // Is this actually a
      // requirement?
    }
    return;
  } else if (isa<LambdaOp>(appliedFun)) {
    emitRemark(appliedFun->getLoc()) << "AccT of Lambda";

    LambdaOp lambda = cast<LambdaOp>(appliedFun);

    SmallVector<Value, 10> args = SmallVector<Value, 10>();
    for (int i = apply.getNumOperands() - 1; i > 0; i--) {
      args.push_back(apply.getOperand(i));
    }

    Substitute(lambda, args);

    // Find return in Lambda Region to start new AccT
    rise::ReturnOp returnOp;
    for (auto op = lambda.region().front().rbegin();
         op != lambda.region().front().rend(); op++) {
      if (isa<ReturnOp>(*op)) {
        returnOp = cast<ReturnOp>(*op);
        break;
      }
    }

    //    assert(returnOp.getOperand(0) && "ReturnOp in Lambda missing!");

    AccT(returnOp, out, rewriter);
    return;
  } else if (isa<AddOp>(appliedFun)) {
    emitRemark(appliedFun->getLoc()) << "AccT of Add";

    auto summand0 = apply.getOperand(1);
    auto summand1 = apply.getOperand(2);

    auto contSummand0 = ConT(summand0, rewriter.getInsertionPoint(), rewriter);
    auto contSummand1 = ConT(summand1, rewriter.getInsertionPoint(), rewriter);

    auto newAddOp = rewriter.create<BinaryOp>(
        appliedFun->getLoc(), FloatType::getF32(rewriter.getContext()),
        StringAttr::get("add", rewriter.getContext()), contSummand0,
        contSummand1);
    auto assignment = rewriter.create<AssignOp>(appliedFun->getLoc(),
                                                newAddOp.getResult(), out);

    return;
  } else if (isa<MulOp>(appliedFun)) {
    emitRemark(appliedFun->getLoc()) << "AccT of Mul";

    auto factor0 = apply.getOperand(1);
    auto factor1 = apply.getOperand(2);

    auto contFactor0 = ConT(factor0, rewriter.getInsertionPoint(), rewriter);
    auto contFactor1 = ConT(factor1, rewriter.getInsertionPoint(), rewriter);

    auto newMulOp = rewriter.create<BinaryOp>(
        appliedFun->getLoc(), FloatType::getF32(rewriter.getContext()),
        StringAttr::get("mul", rewriter.getContext()), contFactor0,
        contFactor1);

    auto assignment = rewriter.create<AssignOp>(appliedFun->getLoc(),
                                                newMulOp.getResult(), out);

    return;
  } else {
    emitRemark(appliedFun->getLoc())
        << "Can't lower the application of op: " << appliedFun->getName();
  }
}

/// Continuation Translation
mlir::Value mlir::rise::ConT(mlir::Value contValue,
                             Block::iterator contLocation,
                             PatternRewriter &rewriter) {
  Location loc = contValue.getLoc();
  auto oldInsertPoint = rewriter.saveInsertionPoint();

  if (contValue.isa<OpResult>()) {
    if (isa<LiteralOp>(contValue.getDefiningOp())) {
      emitRemark(contValue.getLoc()) << "ConT of Literal";
      std::string literalValue = dyn_cast<LiteralOp>(contValue.getDefiningOp())
                                     .literalAttr()
                                     .getValue();

      emitRemark(contValue.getLoc()) << "Literal value: " << literalValue;

      if (LiteralOp op = dyn_cast<LiteralOp>(contValue.getDefiningOp())) {
        if (op.literalAttr()
                .getType()
                .isa<ScalarType>()) { // TODO: use contained type for generating
                                      // this
          EmbedOp embedOp = rewriter.create<EmbedOp>(
              loc,
              ScalarType::get(rewriter.getContext(),
                              FloatType::getF32(rewriter.getContext())),
              ValueRange());
          rewriter.setInsertionPointToStart(&embedOp.region().front());

          auto fillOp = rewriter.create<ConstantFloatOp>(
              loc, llvm::APFloat(std::stof(literalValue)),
              FloatType::getF32(rewriter.getContext()));
          rewriter.create<rise::ReturnOp>(loc, fillOp.getResult());

          rewriter.restoreInsertionPoint(oldInsertPoint);
          return embedOp.getResult();
        } else if (ArrayType arrayType =
                       op.literalAttr().getType().dyn_cast<ArrayType>()) {
          SmallVector<int64_t, 4> shape = {};

          shape.push_back(arrayType.getSize().getIntValue());
          while (arrayType.getElementType().isa<ArrayType>()) {
            arrayType = arrayType.getElementType().dyn_cast<ArrayType>();
            shape.push_back(arrayType.getSize().getIntValue());
          }

          Type memrefElementType = FloatType::getF32(rewriter.getContext());

          auto array = rewriter.create<AllocOp>(
              loc, MemRefType::get(shape, memrefElementType));

          // For now just fill the array with one value
          StringRef litStr = literalValue;
          litStr = litStr.substr(0, litStr.find_first_of(','));
          litStr = litStr.trim('[');
          litStr = litStr.trim(']');
          float fillValue = std::stof(litStr.str() + ".0f");

          auto filler = rewriter.create<ConstantFloatOp>(
              loc, llvm::APFloat(fillValue),
              FloatType::getF32(rewriter.getContext()));

          rewriter.create<linalg::FillOp>(loc, array.getResult(),
                                          filler.getResult());
          //        std::cout << "\nHouston, we have a ArrayType Literal" <<
          //        std::flush;
          rewriter.restoreInsertionPoint(oldInsertPoint);
          return array.getResult();
        } else {
          emitError(op.getLoc())
              << "Lowering literals of this type not supported right now!";
        }
      }
    } else if (isa<LambdaOp>(contValue.getDefiningOp())) {
      emitRemark(contValue.getLoc()) << "ConT of Lambda";

      emitError(loc)
          << "We dont lower Lambdas using the function ConT right now.";
      // A Lambda has only one block
      Block &block = cast<LambdaOp>(contValue.getDefiningOp()).region().front();
      // For now start at the back and just find the first apply
      ApplyOp lastApply;
      for (auto op = block.rbegin(); op != block.rend(); op++) {
        if (isa<ApplyOp>(*op)) {
          lastApply = cast<ApplyOp>(*op);
          break;
        }
      }

      // Finding the return from the chunk of rise IR
      rise::ReturnOp returnOp = dyn_cast<rise::ReturnOp>(block.getTerminator());

    } else if (ApplyOp apply = dyn_cast<ApplyOp>(contValue.getDefiningOp())) {

      if (ZipOp zipOp = dyn_cast<ZipOp>(apply.fun().getDefiningOp())) {
        emitRemark(contValue.getLoc()) << "ConT of Applied Zip";

        auto lhs = apply.getOperand(1);
        auto rhs = apply.getOperand(2);

        auto contLhs = ConT(lhs, rewriter.getInsertionPoint(), rewriter);
        auto contRhs = ConT(rhs, rewriter.getInsertionPoint(), rewriter);

        // usually this is an Array of tuples. But at the end it always has to
        // be projected to fst or snd. For now I will keep the type as
        // memref<...xf32>

        ArrayType lhType = lhs.getType().dyn_cast<ArrayType>();
        ArrayType rhType = rhs.getType().dyn_cast<ArrayType>();
        assert(lhType && rhType && "Inputs to zip have to be Arrays!");

        ArrayType zipType = ArrayType::get(
            rewriter.getContext(), lhType.getSize(),
            rise::Tuple::get(rewriter.getContext(), lhType.getElementType(),
                             rhType.getElementType()));
        auto zipped = rewriter.create<ZipIntermediateOp>(
            zipOp.getLoc(), zipType, contLhs, contRhs);

        return zipped;
      } else if (FstOp fst = dyn_cast<FstOp>(apply.fun().getDefiningOp())) {
        emitRemark(contValue.getLoc()) << "ConT of Applied Fst";

        auto tuple = apply.getOperand(1);

        auto tupleCont = ConT(tuple, rewriter.getInsertionPoint(), rewriter);

        auto fstInterm = rewriter.create<FstIntermediateOp>(
            fst.getLoc(), FloatType::getF32(rewriter.getContext()), tupleCont);
        return fstInterm;
      } else if (SndOp snd = dyn_cast<SndOp>(apply.fun().getDefiningOp())) {
        emitRemark(contValue.getLoc()) << "ConT of Applied Snd";

        auto tuple = apply.getOperand(1);

        auto tupleCont = ConT(tuple, rewriter.getInsertionPoint(), rewriter);

        auto sndInterm = rewriter.create<SndIntermediateOp>(
            snd.getLoc(), FloatType::getF32(rewriter.getContext()), tupleCont);
        return sndInterm;
      } else if (SplitOp splitOp =
                     dyn_cast<SplitOp>(apply.fun().getDefiningOp())) {
        emitRemark(contValue.getLoc()) << "ConT of Applied split";
        auto arrayCont =
            ConT(apply.getOperand(1), rewriter.getInsertionPoint(), rewriter);

        auto splitInterm = rewriter.create<SplitIntermediateOp>(
            splitOp.getLoc(), apply.getType(), arrayCont,
            splitOp.getAttrOfType<NatAttr>("n"),
            splitOp.getAttrOfType<NatAttr>("m"),
            splitOp.getAttrOfType<DataTypeAttr>("t"));
        return splitInterm;
      } else if (JoinOp joinOp =
                     dyn_cast<JoinOp>(apply.fun().getDefiningOp())) {
        emitRemark(contValue.getLoc()) << "ConT of Applied join";
        auto arrayCont =
            ConT(apply.getOperand(1), rewriter.getInsertionPoint(), rewriter);
        auto joinInterm = rewriter.create<JoinIntermediateOp>(
            joinOp.getLoc(), apply.getType(), arrayCont,
            joinOp.getAttrOfType<NatAttr>("n"),
            joinOp.getAttrOfType<NatAttr>("m"),
            joinOp.getAttrOfType<DataTypeAttr>("t"));
        return joinInterm;
      } else if (TransposeOp transposeOp =
                     dyn_cast<TransposeOp>(apply.fun().getDefiningOp())) {
        emitRemark(contValue.getLoc()) << "ConT of Applied transpose";
        auto arrayCont =
            ConT(apply.getOperand(1), rewriter.getInsertionPoint(), rewriter);
        auto transposeInterm = rewriter.create<TransposeIntermediateOp>(
            transposeOp.getLoc(), apply.getType(), arrayCont,
            transposeOp.getAttrOfType<NatAttr>("n"),
            transposeOp.getAttrOfType<NatAttr>("m"),
            transposeOp.getAttrOfType<DataTypeAttr>("t"));
        return transposeInterm;
      } else if (SlideOp slideOp =
                     dyn_cast<SlideOp>(apply.fun().getDefiningOp())) {
        emitRemark(contValue.getLoc()) << "ConT of Applied Slide";
        auto arrayCont =
            ConT(apply.getOperand(1), rewriter.getInsertionPoint(), rewriter);
        auto slideInterm = rewriter.create<SlideIntermediateOp>(
            slideOp.getLoc(), apply.getType(), arrayCont,
            slideOp.getAttrOfType<NatAttr>("n"),
            slideOp.getAttrOfType<NatAttr>("sz"),
            slideOp.getAttrOfType<NatAttr>("sp"),
            slideOp.getAttrOfType<DataTypeAttr>("t"));
        return slideInterm;
      } else if (PadOp padOp = dyn_cast<PadOp>(apply.fun().getDefiningOp())) {
        emitRemark(contValue.getLoc()) << "ConT of Applied Pad";
        auto padValCont =
            ConT(apply.getOperand(1), rewriter.getInsertionPoint(), rewriter);
        auto arrayCont =
            ConT(apply.getOperand(2), rewriter.getInsertionPoint(), rewriter);
        auto padInterm = rewriter.create<PadIntermediateOp>(
            padOp.getLoc(), apply.getType(), padValCont, arrayCont,
            padOp.getAttrOfType<NatAttr>("n"),
            padOp.getAttrOfType<NatAttr>("l"),
            padOp.getAttrOfType<NatAttr>("r"),
            padOp.getAttrOfType<DataTypeAttr>("t"));
        return padInterm;
      } else if (MapSeqOp mapOp =
                     dyn_cast<MapSeqOp>(apply.fun().getDefiningOp())) {
        emitRemark(contValue.getLoc()) << "ConT of Applied MapSeq";

        // introduce tmp Array of length n:
        EmbedOp embedOp = rewriter.create<EmbedOp>(
            mapOp.getLoc(),
            ArrayType::get(
                rewriter.getContext(), mapOp.n(),
                ScalarType::get(rewriter.getContext(),
                                FloatType::getF32(rewriter.getContext()))),
            ValueRange());

        //        Block *embedBlock = new Block();
        //        embedOp.region().push_front(embedBlock);
        rewriter.setInsertionPointToStart(&embedOp.region().front());
        auto tmpArray = rewriter.create<AllocOp>(
            loc, MemRefType::get(ArrayRef<int64_t>{mapOp.n().getIntValue()},
                                 FloatType::getF32(rewriter.getContext())));
        rewriter.create<rise::ReturnOp>(tmpArray.getLoc(),
                                        tmpArray.getResult());

        rewriter.setInsertionPointAfter(embedOp);

        AccT(apply, embedOp.getResult(), rewriter);

        return embedOp.getResult();
      } else if (MapParOp mapOp =
                     dyn_cast<MapParOp>(apply.fun().getDefiningOp())) {
        emitRemark(contValue.getLoc()) << "ConT of Applied MapPar";

        // introduce tmp Array of length n:
        EmbedOp embedOp = rewriter.create<EmbedOp>(
            mapOp.getLoc(),
            ArrayType::get(
                rewriter.getContext(), mapOp.n(),
                ScalarType::get(rewriter.getContext(),
                                FloatType::getF32(rewriter.getContext()))),
            ValueRange());

        //        Block *embedBlock = new Block();
        //        embedOp.region().push_front(embedBlock);
        rewriter.setInsertionPointToStart(&embedOp.region().front());
        auto tmpArray = rewriter.create<AllocOp>(
            loc, MemRefType::get(ArrayRef<int64_t>{mapOp.n().getIntValue()},
                                 FloatType::getF32(rewriter.getContext())));
        rewriter.create<rise::ReturnOp>(tmpArray.getLoc(),
                                        tmpArray.getResult());

        rewriter.setInsertionPointAfter(embedOp);

        AccT(apply, embedOp.getResult(), rewriter);

        return embedOp.getResult();
      } else {
        emitError(apply.getLoc()) << "Cannot perform ConT for this apply!";
      }
    } else if (EmbedOp embedOp = dyn_cast<EmbedOp>(contValue.getDefiningOp())) {
      emitRemark(contValue.getLoc()) << "ConT of Embed";

      // Translating all operands
      for (int i = 0; i < embedOp.getOperands().size(); i++) {
        auto operand = embedOp.getOperand(i);
        auto operandCont =
            ConT(operand, rewriter.getInsertionPoint(), rewriter);
        embedOp.setOperand(i, operandCont);
      }

      // This will be inlined later
      return embedOp.getResult();
    } else if (InOp inOp = dyn_cast<InOp>(contValue.getDefiningOp())) {
      emitRemark(contValue.getLoc()) << "ConT of In";

      // we dont return the ConT of this to stay in our type system. This will
      // be resolved in the codeGen stage of the translation.
      return contValue;
    } else {
      emitRemark(contValue.getLoc())
          << "cannot perform continuation "
             "translation for "
          << contValue.getDefiningOp()->getName().getStringRef().str()
          << " leaving Value as is.";

      rewriter.restoreInsertionPoint(oldInsertPoint);
      return contValue;
    }
  } else {
    emitRemark(contValue.getLoc())
        << "cannot perform continuation for BlockArg, leaving as is.";
    return contValue;
  }
}

Value mlir::rise::codeGen(Operation *op, SmallVector<OutputPathType, 10> path,
                          PatternRewriter &rewriter) {
  if (!op) {
    emitError(rewriter.getUnknownLoc()) << "codegen started with nullptr!";
  }
  if (AssignOp assign = dyn_cast<AssignOp>(op)) {
    emitRemark(op->getLoc()) << "Codegen for Assign";

    if (assign.value().isa<OpResult>()) {
      // Should be to the idx generated usually right beside this one
      // but I have no handle to that here
      rewriter.setInsertionPoint(assign.assignee().getDefiningOp());
    } else {
      rewriter.setInsertionPointToStart(
          &assign.value().getParentRegion()->front());
    }
    auto writeValue = codeGen(assign.value(), {}, rewriter);
    if (!writeValue)
      emitError(op->getLoc()) << "Assignment has no Value to write.";

    rewriter.setInsertionPointAfter(assign);
    auto leftPath = codeGenStore(assign.assignee(), writeValue, {}, rewriter);

  } else {
    emitRemark(op->getLoc())
        << "Codegen for " << op->getName().getStringRef().str()
        << " unsupported!";
  }
  return nullptr;
}
SmallVector<OutputPathType, 10>
mlir::rise::codeGenStore(Value storeLocation, Value val,
                         SmallVector<OutputPathType, 10> path,
                         PatternRewriter &rewriter) {
  if (storeLocation.isa<OpResult>()) {
    if (IdxOp idx = dyn_cast<IdxOp>(storeLocation.getDefiningOp())) {
      emitRemark(val.getLoc()) << "CodegenStore for idx";

      path.push_back(idx.arg1());

      return codeGenStore(idx.arg0(), val, path, rewriter);
    } else if (CastOp castOp =
                   dyn_cast<CastOp>(storeLocation.getDefiningOp())) {
      emitRemark(val.getLoc()) << "CodegenStore for cast";
      return codeGenStore(castOp.getOperand(), val, path, rewriter);
    } else if (JoinAccIntermediateOp joinAccOp =
                   dyn_cast<JoinAccIntermediateOp>(
                       storeLocation.getDefiningOp())) {
      emitRemark(val.getLoc()) << "CodegenStore for joinAcc";
      auto i = mpark::get<Value>(path.pop_back_val());
      auto j = mpark::get<Value>(path.pop_back_val());

      auto cstM = rewriter
                      .create<ConstantIndexOp>(joinAccOp.getLoc(),
                                               joinAccOp.m().getIntValue())
                      .getResult();
      auto i_times_m =
          rewriter.create<MulIOp>(joinAccOp.getLoc(), i, cstM).getResult();
      auto newIndex =
          rewriter.create<AddIOp>(joinAccOp.getLoc(), i_times_m, j).getResult();

      path.push_back(newIndex);
      return codeGenStore(joinAccOp.getOperand(), val, path, rewriter);
    } else if (SplitAccIntermediateOp splitAccOp =
                   dyn_cast<SplitAccIntermediateOp>(
                       storeLocation.getDefiningOp())) {
      emitRemark(val.getLoc()) << "CodegenStore for splitAcc";
      auto loc = splitAccOp.getLoc();
      auto lhs = mpark::get<Value>(path.pop_back_val());
      auto rhs =
          rewriter.create<ConstantIndexOp>(loc, splitAccOp.n().getIntValue())
              .getResult();

      // modulo op taken from AffineToStandard
      Value remainder = rewriter.create<SignedRemIOp>(loc, lhs, rhs);
      Value zeroCst = rewriter.create<ConstantIndexOp>(loc, 0);
      Value isRemainderNegative =
          rewriter.create<CmpIOp>(loc, CmpIPredicate::slt, remainder, zeroCst);
      Value correctedRemainder = rewriter.create<AddIOp>(loc, remainder, rhs);
      Value result = rewriter.create<SelectOp>(loc, isRemainderNegative,
                                               correctedRemainder, remainder);
      Value divResult = rewriter.create<UnsignedDivIOp>(loc, lhs, rhs);

      path.push_back(result);
      path.push_back(divResult);
      return codeGenStore(splitAccOp.getOperand(), val, path, rewriter);
    } else if (EmbedOp embedOp =
                   dyn_cast<EmbedOp>(storeLocation.getDefiningOp())) {
      emitRemark(val.getLoc()) << "CodegenStore for embed";
      assert(embedOp.getNumOperands() == 0 &&
             "codegenstore for embed with operands not handled yet.");

      auto oldInsertPoint = rewriter.saveInsertionPoint();
      rewriter.setInsertionPointAfter(embedOp);

      // replace uses of embed value with returned value
      rise::ReturnOp embedReturn = dyn_cast<rise::ReturnOp>(
          embedOp.getRegion().front().getOperations().back());
      assert(embedReturn &&
             "Region of EmbedOp has to be terminated using rise.return!");
      embedOp.getResult().replaceAllUsesWith(embedReturn.getOperand(0));

      // inline all operations of the embedOp region except for return
      rewriter.getInsertionBlock()->getOperations().splice(
          rewriter.getInsertionPoint(),
          embedOp.getRegion().front().getOperations(),
          embedOp.getRegion().front().begin(), Block::iterator(embedReturn));

      rewriter.restoreInsertionPoint(oldInsertPoint);
      generateReadAccess(path, val, embedReturn.getOperand(0), rewriter);
    } else {

      emitRemark(val.getLoc())
          << "CodegenStore for "
          << val.getDefiningOp()->getName().getStringRef().str();

      generateReadAccess(path, val, storeLocation, rewriter);
    }
  } else {
    // We have reached a BlockArgument
    // call to reverse here.
    emitRemark(val.getLoc()) << "CodegenStore for BlockArg";
    generateReadAccess(path, val, storeLocation, rewriter);
  }
  return path;
}

Value mlir::rise::codeGen(Value val, SmallVector<OutputPathType, 10> path,
                          PatternRewriter &rewriter) {

  if (val.isa<OpResult>()) {
    if (EmbedOp embedOp = dyn_cast<EmbedOp>(val.getDefiningOp())) {
      emitRemark(embedOp.getLoc()) << "Codegen for Embed";

      auto oldInsertPoint = rewriter.saveInsertionPoint();

      rewriter.setInsertionPointAfter(embedOp);

      // Do codegen for all operands of embed first
      int i = 0;
      for (auto operand : (val.getDefiningOp()->getOperands())) {
        embedOp.setOperand(i, codeGen(operand, path, rewriter));
        i++;
      }
      // replace blockArgs in the region with results of the codegen for the
      // operands
      for (int i = 0; i < embedOp.getOperands().size(); i++) {
        embedOp.region().front().getArgument(i).replaceAllUsesWith(
            embedOp.getOperand(i));
      }
      // replace uses of embed value with returned value
      rise::ReturnOp embedReturn = dyn_cast<rise::ReturnOp>(
          embedOp.getRegion().front().getOperations().back());
      assert(embedReturn &&
             "Region of EmbedOp has to be terminated using rise.return!");
      embedOp.getResult().replaceAllUsesWith(embedReturn.getOperand(0));

      // inline all operations of the embedOp region except for return
      rewriter.getInsertionBlock()->getOperations().splice(
          rewriter.getInsertionPoint(),
          embedOp.getRegion().front().getOperations(),
          embedOp.getRegion().front().begin(), Block::iterator(embedReturn));

      rewriter.restoreInsertionPoint(oldInsertPoint);

      return embedReturn.getOperand(0);

    } else if (IdxOp idx = dyn_cast<IdxOp>(val.getDefiningOp())) {
      emitRemark(idx.getLoc()) << "Codegen for idx";

      Value arg1 = idx.arg1();
      path.push_back(arg1);
      return codeGen(idx.arg0(), path, rewriter);
    } else if (BinaryOp binOp = dyn_cast<BinaryOp>(val.getDefiningOp())) {
      emitRemark(binOp.getLoc()) << "Codegen for binOp";

      auto arg0 = codeGen(binOp.arg0(), {}, rewriter);
      auto arg1 = codeGen(binOp.arg1(), {}, rewriter);
      if (binOp.op().equals("add")) {
        return rewriter.create<AddFOp>(val.getLoc(), arg0.getType(), arg0, arg1)
            .getResult();
      } else if (binOp.op().equals("mul")) {
        return rewriter.create<MulFOp>(val.getLoc(), arg0.getType(), arg0, arg1)
            .getResult();
      } else {
        emitError(binOp.getLoc())
            << "Cannot create code for binOp:" << binOp.op();
        return binOp.getResult();
      }
    } else if (AllocOp alloc = dyn_cast<AllocOp>(val.getDefiningOp())) {
      emitRemark(alloc.getLoc()) << "Codegen for alloc";

      // call to reverse here.
      return generateWriteAccess(path, alloc.getResult(), rewriter);
    } else if (ZipIntermediateOp zipIntermOp =
                   dyn_cast<ZipIntermediateOp>(val.getDefiningOp())) {
      emitRemark(zipIntermOp.getLoc()) << "Codegen for zip";
      OutputPathType sndLastElem = path[path.size() - 2];
      int *fst = mpark::get_if<int>(&sndLastElem);

      // delete snd value on the path.
      auto tmp = path.pop_back_val();
      path.pop_back();
      path.push_back(tmp);

      if (*fst) {
        return codeGen(zipIntermOp.lhs(), path, rewriter);
      } else {
        return codeGen(zipIntermOp.rhs(), path, rewriter);
      }
    } else if (FstIntermediateOp fstIntermOp =
                   dyn_cast<FstIntermediateOp>(val.getDefiningOp())) {
      emitRemark(fstIntermOp.getLoc()) << "Codegen for fst";

      path.push_back(true);
      return codeGen(fstIntermOp.value(), path, rewriter);

    } else if (SndIntermediateOp sndIntermOp =
                   dyn_cast<SndIntermediateOp>(val.getDefiningOp())) {
      emitRemark(sndIntermOp.getLoc()) << "Codegen for snd";

      path.push_back(false);
      return codeGen(sndIntermOp.value(), path, rewriter);

    } else if (isa<LoadOp>(val.getDefiningOp()) ||
               isa<AffineLoadOp>(val.getDefiningOp())) {
      emitRemark(val.getLoc()) << "Codegen for Load";
      return val;
    } else if (SplitIntermediateOp splitIntermediateOp =
                   dyn_cast<SplitIntermediateOp>(val.getDefiningOp())) {
      emitRemark(val.getLoc()) << "Codegen for Split";

      auto i = mpark::get<Value>(path.pop_back_val());
      auto j = mpark::get<Value>(path.pop_back_val());

      auto cstN =
          rewriter
              .create<ConstantIndexOp>(splitIntermediateOp.getLoc(),
                                       splitIntermediateOp.n().getIntValue())
              .getResult();
      auto i_times_n =
          rewriter.create<MulIOp>(splitIntermediateOp.getLoc(), i, cstN)
              .getResult();
      auto newIndex =
          rewriter.create<AddIOp>(splitIntermediateOp.getLoc(), i_times_n, j)
              .getResult();
      path.push_back(newIndex);

      return codeGen(splitIntermediateOp.value(), path, rewriter);
    } else if (JoinIntermediateOp joinIntermediateOp =
                   dyn_cast<JoinIntermediateOp>(val.getDefiningOp())) {
      emitRemark(val.getLoc()) << "Codegen for Join";

      auto loc = joinIntermediateOp.getLoc();
      auto lhs = mpark::get<Value>(path.pop_back_val());
      auto rhs = rewriter
                     .create<ConstantIndexOp>(
                         loc, joinIntermediateOp.m().getIntValue())
                     .getResult();

      // modulo op taken from AffineToStandard
      Value remainder = rewriter.create<SignedRemIOp>(loc, lhs, rhs);
      Value zeroCst = rewriter.create<ConstantIndexOp>(loc, 0);
      Value isRemainderNegative =
          rewriter.create<CmpIOp>(loc, CmpIPredicate::slt, remainder, zeroCst);
      Value correctedRemainder = rewriter.create<AddIOp>(loc, remainder, rhs);
      Value result = rewriter.create<SelectOp>(loc, isRemainderNegative,
                                               correctedRemainder, remainder);
      Value divResult = rewriter.create<SignedDivIOp>(loc, lhs, rhs);

      path.push_back(result);
      path.push_back(divResult);

      return codeGen(joinIntermediateOp.value(), path, rewriter);
    } else if (TransposeIntermediateOp transposeIntermediateOp =
                   dyn_cast<TransposeIntermediateOp>(val.getDefiningOp())) {
      emitRemark(val.getLoc()) << "Codegen for Transpose";
      auto n = path.pop_back_val();
      auto m = path.pop_back_val();

      path.push_back(n);
      path.push_back(m);

      return codeGen(transposeIntermediateOp.getOperand(), path, rewriter);
    } else if (SlideIntermediateOp slideIntermediateOp =
                   dyn_cast<SlideIntermediateOp>(val.getDefiningOp())) {
      emitRemark(val.getLoc()) << "Codegen for Slide";
      Value i = mpark::get<Value>(path.pop_back_val());
      Value j = mpark::get<Value>(path.pop_back_val());

      Value s2 =
          rewriter
              .create<ConstantIndexOp>(slideIntermediateOp.getLoc(),
                                       slideIntermediateOp.sp().getIntValue())
              .getResult();
      Value i_times_s2 =
          rewriter.create<MulIOp>(slideIntermediateOp.getLoc(), i, s2)
              .getResult();
      Value newIndex =
          rewriter.create<AddIOp>(slideIntermediateOp.getLoc(), i_times_s2, j)
              .getResult();

      path.push_back(newIndex);

      return codeGen(slideIntermediateOp.value(), path, rewriter);
    } else if (PadIntermediateOp padIntermediateOp =
                   dyn_cast<PadIntermediateOp>(val.getDefiningOp())) {
      emitRemark(val.getLoc()) << "Codegen for Pad";
      Location loc = padIntermediateOp.getLoc();

      Value i = mpark::get<Value>(path.pop_back_val());

      // I will do padclamp first
      //      Value padVal = codeGen(padIntermediateOp.padvalue(),
      //      path,rewriter);

      Value l =
          rewriter
              .create<ConstantIndexOp>(loc, padIntermediateOp.l().getIntValue())
              .getResult();
      Value r =
          rewriter
              .create<ConstantIndexOp>(loc, padIntermediateOp.r().getIntValue())
              .getResult();
      Value n =
          rewriter
              .create<ConstantIndexOp>(loc, padIntermediateOp.n().getIntValue())
              .getResult();

      Value cst0 = rewriter.create<ConstantIndexOp>(loc, 0).getResult();
      Value cst1 = rewriter.create<ConstantIndexOp>(loc, 1).getResult();

      Value n_minus_1 = rewriter.create<SubIOp>(loc, n, cst1).getResult();
//      Value n_minus_r_minus_1 = rewriter.create<SubIOp>(loc, n_minus_r, cst1).getResult();

      Value l_plus_n = rewriter.create<AddIOp>(loc, l, n).getResult();

      Value index = rewriter.create<SubIOp>(loc, i, l);

      // Conditions whether index is in bounds.
      // We can just base the conditions on i, not on i+l as done in shine_rise.
      Value isIndexSmallerLb =
          rewriter.create<CmpIOp>(loc, CmpIPredicate::slt, i, l);
      Value isIndexSmallerRb =
          rewriter.create<CmpIOp>(loc, CmpIPredicate::slt, i, l_plus_n);

      // (i < l) ? 0 : (i < l+n) ? index : n-1
      Value thenelseVal =
          rewriter.create<SelectOp>(loc, isIndexSmallerRb, index, n_minus_1);
      Value ifthenVal =
          rewriter.create<SelectOp>(loc, isIndexSmallerLb, cst0, thenelseVal);

      path.push_back(ifthenVal);

      // TODO: bronching!

      return codeGen(padIntermediateOp.array(), path, rewriter);
    } else if (InOp inOp = dyn_cast<InOp>(val.getDefiningOp())) {
      emitRemark(val.getLoc())
          << "Codegen for In, generating read operation for operand";
      return generateWriteAccess(path, inOp.input(), rewriter);
    } else if (CastOp castOp = dyn_cast<CastOp>(val.getDefiningOp())) {
      emitRemark(val.getLoc()) << "Codegen for Cast, reversing";
      return generateWriteAccess(path, castOp.getOperand(), rewriter);
    } else {
      emitRemark(val.getLoc())
          << "I don't know how to do codegen for: "
          << val.getDefiningOp()->getName().getStringRef().str()
          << " this is prob. an operation from another dialect. We walk "
             "recursively through the operands until we hit something we can "
             "do codegen for.";
      int i = 0;
      for (auto operand : (val.getDefiningOp()->getOperands())) {
        val.getDefiningOp()->setOperand(i, codeGen(operand, path, rewriter));
        i++;
      }
      return val;
      // go through all the operands until we hit an idx
    }
  } else {
    // val is a BlockArg
    emitRemark(val.getLoc()) << "reached a blockArg in Codegen, reversing";
    return generateWriteAccess(path, val, rewriter);
  }
}

Value mlir::rise::generateWriteAccess(SmallVector<OutputPathType, 10> path,
                                      Value accessVal,
                                      PatternRewriter &rewriter) {
  int index;
  SmallVector<Value, 10> indexValues = {};
  //  for (OutputPathType element : path) {
  for (auto element = path.rbegin(); element != path.rend(); ++element) {
    if (auto i = mpark::get_if<int>(&*element)) {
      index = *i;
    } else if (auto val = mpark::get_if<Value>(&*element)) {
      indexValues.push_back(*val);
    }
  }
  //  // handle problem originating from translation of reduce (accessing
  //  element) int rank = accessVal.getType().dyn_cast<MemRefType>().getRank();
  //  if (indexValues.size() != rank) {
  //    indexValues.erase(indexValues.begin());
  //  }
  if (isa<AffineForOp>(rewriter.getBlock()->getParent()->getParentOp())) {
    return rewriter
        .create<AffineLoadOp>(accessVal.getLoc(), accessVal, indexValues)
        .getResult();
  } else {
    return rewriter.create<LoadOp>(accessVal.getLoc(), accessVal, indexValues)
        .getResult();
  }
}

void mlir::rise::generateReadAccess(SmallVector<OutputPathType, 10> path,
                                    Value storeVal, Value storeLoc,
                                    PatternRewriter &rewriter) {
  int index;
  SmallVector<Value, 10> indexValues = {};
  for (auto element = path.rbegin(); element != path.rend(); ++element) {
    if (auto i = mpark::get_if<int>(&*element)) {
      index = *i;
    } else if (auto val = mpark::get_if<Value>(&*element)) {
      indexValues.push_back(*val);
    }
  }
  int rank = storeLoc.getType().dyn_cast<MemRefType>().getRank();
  if (indexValues.size() != rank) {
    indexValues.erase(indexValues.begin());
  }
  ValueRange valRange = ValueRange(indexValues);
  if (isa<AffineForOp>(rewriter.getBlock()->getParent()->getParentOp())) {
    rewriter.create<AffineStoreOp>(storeLoc.getLoc(), storeVal, storeLoc,
                                   llvm::makeArrayRef(indexValues));
    return;
  } else {
    rewriter.create<StoreOp>(storeLoc.getLoc(), storeVal, storeLoc,
                             llvm::makeArrayRef(indexValues));

    return;
  }
}

/// This is obviously not really working.
/// For some Values it prints ints.
void mlir::rise::printPath(SmallVector<OutputPathType, 10> input) {
  struct {
    void operator()(Value val) {
      if (val.isa<OpResult>()) {
        std::cout << "val: "
                  << val.getDefiningOp()->getName().getStringRef().str() << " ";
      } else {
        std::cout << "blockArg, ";
      }
    }
    void operator()(Value *val) {
      if (val->isa<OpResult>()) {
        std::cout << "val: "
                  << val->getDefiningOp()->getName().getStringRef().str()
                  << " ";
      } else {
        std::cout << "blockArg, ";
      }
    }
    void operator()(int i) { std::cout << "int!" << i << ", "; }
    void operator()(std::string const &) { std::cout << "string!"; }
    void operator()(bool b) { std::cout << "bool: " << b << ", "; }
  } visitor;
  std::cout << "path: {";
  for (OutputPathType element : input) {
    mpark::visit(visitor, input[0]);
  }
  std::cout << "}\n" << std::flush;
}

void mlir::rise::printUses(Value val) {
  std::cout << val.getDefiningOp()->getName().getStringRef().str()
            << " has uses: \n"
            << std::flush;

  auto uses = val.getUses().begin();
  while (true) {
    if (uses != val.getUses().end()) {
      std::cout << "    " << uses.getUser()->getName().getStringRef().str()
                << "\n"
                << std::flush;
      uses++;
    } else {
      break;
    }
  }
}

void mlir::rise::Substitute(LambdaOp lambda,
                            llvm::SmallVector<Value, 10> args) {
  if (lambda.region().front().getArguments().size() < args.size()) {
    emitError(lambda.getLoc())
        << "Too many arguments given for Lambda substitution";
  }
  for (int i = 0; i < args.size(); i++) {
    lambda.region().front().getArgument(i).replaceAllUsesWith(args[i]);
  }
  return;
}

/// gather all patterns
void mlir::populateRiseToImpConversionPatterns(
    OwningRewritePatternList &patterns, MLIRContext *ctx) {
  patterns.insert<RiseToImperativePattern>(ctx);
}
//===----------------------------------------------------------------------===//
// Pass
//===----------------------------------------------------------------------===//

/// The pass:
void ConvertRiseToImperativePass::runOnFunction() {
  auto module = getOperation();
  OwningRewritePatternList patterns;

  populateRiseToImpConversionPatterns(patterns, &getContext());

  ConversionTarget target(getContext());

  //  target.addLegalOp<
  //      CallOp, FuncOp, ModuleOp, ModuleTerminatorOp, linalg::MatmulOp,
  //      scf::ForOp, scf::ParallelOp, scf::TerminatorOp, AffineForOp,
  //      AffineParallelOp, AffineTerminatorOp, AffineStoreOp, AffineLoadOp,
  //      ConstantIndexOp, AllocOp, LoadOp, StoreOp, AddFOp, MulFOp,
  //      linalg::FillOp, mlir::ReturnOp, mlir::rise::LambdaOp,
  //      mlir::rise::IdxOp, mlir::rise::BinaryOp,
  //      mlir::rise::FstIntermediateOp, mlir::rise::SndIntermediateOp,
  //      mlir::rise::ZipIntermediateOp, mlir::rise::AssignOp,
  //      mlir::rise::WrapOp, mlir::rise::UnwrapOp, mlir::rise::ApplyOp,
  //      RiseContinuationTranslation>();

  target.addLegalOp<ModuleOp, ModuleTerminatorOp>();

  target.addLegalDialect<StandardOpsDialect>();
  target.addLegalDialect<scf::SCFDialect>();
  target.addLegalDialect<AffineDialect>();
  target.addLegalDialect<linalg::LinalgDialect>();
  target.addLegalDialect<rise::RiseDialect>(); // for debugging purposes

  // Ops we don't want in our output
  target.addDynamicallyLegalOp<FuncOp>([](FuncOp funcOp) {
    bool riseInside = false;
    if (funcOp.isExternal())
      return true;
    funcOp.walk([&](Operation *op) {
      if (op->getDialect()->getNamespace().equals(
              rise::RiseDialect::getDialectNamespace()))
        riseInside = true;
    });
    return !riseInside;
  });

  //  if (failed(applyPartialConversion(module, target, patterns)))
  //    signalPassFailure();

  //        if (!applyPatternsGreedily(this->getOperation(), patterns))

  //  std::cout << "here I am! \n" << std::flush;

  bool erased;
  applyOpPatternsAndFold(module, patterns, &erased);

  //  std::cout << "here I am! erased:" << erased << "\n" << std::flush;

  return;
}

std::unique_ptr<OperationPass<FuncOp>>
mlir::rise::createConvertRiseToImperativePass() {
  return std::make_unique<ConvertRiseToImperativePass>();
}
