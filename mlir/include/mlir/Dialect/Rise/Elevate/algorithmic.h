//
// Created by martin on 9/25/20.
//

#ifndef LLVM_ELEVATE_ALGORITHMIC_H
#define LLVM_ELEVATE_ALGORITHMIC_H

#include "mlir/Elevate/core.h"
#include "mlir/Dialect/Rise/IR/Dialect.h"
#include "mlir/Dialect/Rise/EDSC/Builders.h"
#include "mlir/Dialect/Rise/EDSC/HighLevel.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Types.h"
#include "mlir/IR/Value.h"

using namespace mlir;
using namespace mlir::rise;
using namespace mlir::edsc::abstraction;
using namespace mlir::edsc::op;
using namespace mlir::edsc::type;
using namespace mlir::elevate;

struct FuseReduceMapStrategy : Strategy {
  FuseReduceMapStrategy() {};

  RewriteResult rewrite(Expr &expr) const override {
    PatternRewriter *rewriter = ElevateRewriter::getInstance().rewriter;

    if (!isa<ApplyOp>(expr)) return Failure();
    auto applyReduction = cast<ApplyOp>(expr);

    if (!isa<ReduceSeqOp>(applyReduction.getOperand(0).getDefiningOp())) return Failure();
    auto reduction = cast<ReduceSeqOp>(applyReduction.getOperand(0).getDefiningOp());
    auto reductionLambda = applyReduction.getOperand(1).getDefiningOp();
    auto initializer = applyReduction.getOperand(2).getDefiningOp();

    if (!isa<ApplyOp>(applyReduction.getOperand(3).getDefiningOp())) return Failure();
    auto reductionInput = cast<ApplyOp>(applyReduction.getOperand(3).getDefiningOp());

    if (!isa<MapSeqOp>(reductionInput.getOperand(0).getDefiningOp())) return Failure();
    // successful match
    auto mapSeq = cast<MapSeqOp>(reductionInput.getOperand(0).getDefiningOp());
    auto mapLambda = reductionInput.getOperand(1).getDefiningOp();
    Value mapInput = reductionInput.getOperand(2);

    ScopedContext scope(*rewriter, expr.getLoc());
    rewriter->setInsertionPointAfter(applyReduction);

    Value newReduceApplication = reduceSeq(scalarF32Type(), [&](Value y, Value acc){
          Value mapped = apply(scalarF32Type(), mapLambda->getResult(0), y);
          return apply(scalarF32Type(), reductionLambda->getResult(0), {mapped, acc});
    },initializer->getResult(0), mapInput);

    Operation *result = newReduceApplication.getDefiningOp();
    return success(*result);
  };
};

auto fuseReduceMap = FuseReduceMapStrategy();


struct SplitJoinStrategy : Strategy {
  int n;
  SplitJoinStrategy(const int n) : n{n} {};

  RewriteResult rewrite(Expr &expr) const override {
    PatternRewriter *rewriter = ElevateRewriter::getInstance().rewriter;

    if (!isa<ApplyOp>(expr)) return Failure();
    auto applyMap = cast<ApplyOp>(expr);

    if (!isa<MapSeqOp>(applyMap.getOperand(0).getDefiningOp())) return Failure();
    auto mapSeqOp = cast<MapSeqOp>(applyMap.getOperand(0).getDefiningOp());
    if (mapSeqOp.n().getIntValue() % n != 0) return Failure();
    // successful match

    auto mapLambda = applyMap.getOperand(1).getDefiningOp();
    Value mapInput = applyMap.getOperand(2);

    ScopedContext scope(*rewriter, expr.getLoc());
    rewriter->setInsertionPointAfter(applyMap);

    Value result = join(mapSeq2D(mapSeqOp.t(), mapLambda->getResult(0), split(natType(n), mapInput)));

    return success(*result.getDefiningOp());
  };
};

auto splitJoin = [](const auto n) {
  return SplitJoinStrategy(n);
};

struct AddIdAfterStrategy : Strategy {

  AddIdAfterStrategy() {};

  RewriteResult rewrite(Expr &expr) const override {
    PatternRewriter *rewriter = ElevateRewriter::getInstance().rewriter;

    if (!isa<ApplyOp>(expr)) return Failure();
    auto apply = cast<ApplyOp>(expr);

    // successful match
    ScopedContext scope(*rewriter, expr.getLoc());
    rewriter->setInsertionPointAfter(apply);

    auto newApply = rewriter->clone(*apply);
    Value result = mlir::edsc::op::id(newApply->getResult(0));

    return success(*result.getDefiningOp());
  };
};

auto addIdAfter = AddIdAfterStrategy();

struct CreateTransposePairStrategy : Strategy {
  CreateTransposePairStrategy() {};

  RewriteResult rewrite(Expr &expr) const override {
    PatternRewriter *rewriter = ElevateRewriter::getInstance().rewriter;

    if (!isa<ApplyOp>(expr)) return Failure();
    auto apply = cast<ApplyOp>(expr);
    if (!isa<IdOp>(apply.getOperand(0).getDefiningOp())) return Failure();
    auto id = cast<IdOp>(apply.getOperand(0).getDefiningOp());
    if (!apply.getResult().getType().isa<ArrayType>()) return Failure();
    if (!apply.getResult().getType().dyn_cast<ArrayType>().getElementType().isa<ArrayType>()) return Failure();

    // successful match
    ScopedContext scope(*rewriter, expr.getLoc());
    rewriter->setInsertionPointAfter(apply);
    Value result = transpose(transpose(apply.getOperand(1)));

    return success(*result.getDefiningOp());
  };
};

auto createTransposePair = CreateTransposePairStrategy();

struct RemoveTransposePairStrategy : Strategy {
  RemoveTransposePairStrategy() {};

  RewriteResult rewrite(Expr &expr) const override {
    PatternRewriter *rewriter = ElevateRewriter::getInstance().rewriter;
    if (!isa<ApplyOp>(expr)) return Failure();
    auto apply1 = cast<ApplyOp>(expr);
    if (!isa<mlir::rise::TransposeOp>(apply1.getOperand(0).getDefiningOp())) return Failure();
    auto transpose1 = cast<mlir::rise::TransposeOp>(apply1.getOperand(0).getDefiningOp());
    if (!apply1.getOperand(1).isa<OpResult>()) return Failure();
    if (!isa<ApplyOp>(apply1.getOperand(1).getDefiningOp())) return Failure();
    auto apply2 = cast<ApplyOp>(apply1.getOperand(1).getDefiningOp());
    if (!isa<mlir::rise::TransposeOp>(apply2.getOperand(0).getDefiningOp())) return Failure();
    auto transpose2 = cast<mlir::rise::TransposeOp>(apply2.getOperand(0).getDefiningOp());
    if (!apply2.getOperand(1).isa<OpResult>()) return Failure();
    // successful match

    Value result = apply2.getOperand(1);

    return success(*result.getDefiningOp());
  };
};

auto removeTransposePair = RemoveTransposePairStrategy();

// movement
struct MapMapFBeforeTransposeStrategy : Strategy {
  MapMapFBeforeTransposeStrategy() {};

  RewriteResult rewrite(Expr &expr) const override {
    PatternRewriter *rewriter = ElevateRewriter::getInstance().rewriter;
    // match for
    //    App(
    //        App(map(), lamA @ Lambda(_, App(
    //        App(map(), lamB @ Lambda(_, App(f, _))), _))),
    //    arg)

    if (!isa<ApplyOp>(expr)) return Failure();
    auto apply1 = cast<ApplyOp>(expr);
    if (!isa<mlir::rise::TransposeOp>(apply1.getOperand(0).getDefiningOp())) return Failure();
    auto transpose1 = cast<mlir::rise::TransposeOp>(apply1.getOperand(0).getDefiningOp());
    if (!apply1.getOperand(1).isa<OpResult>()) return Failure();
    if (!isa<ApplyOp>(apply1.getOperand(1).getDefiningOp())) return Failure();
    auto apply2 = cast<ApplyOp>(apply1.getOperand(1).getDefiningOp());
    if (!isa<MapSeqOp>(apply2.getOperand(0).getDefiningOp())) return Failure();
    auto mapSeqOp1 = cast<MapSeqOp>(apply2.getOperand(0).getDefiningOp());
    if (!apply2.getOperand(1).isa<OpResult>()) return Failure();
    if (!isa<LambdaOp>(apply2.getOperand(1).getDefiningOp())) return Failure();
    auto lamA = cast<LambdaOp>(apply2.getOperand(1).getDefiningOp()); // %2
    if (!isa<ApplyOp>(lamA.region().front().getTerminator()->getOperand(0).getDefiningOp())) return Failure();
    auto apply3 = cast<ApplyOp>(lamA.region().front().getTerminator()->getOperand(0).getDefiningOp());
    if (!isa<MapSeqOp>(apply3.getOperand(0).getDefiningOp())) return Failure();
    auto mapSeqOp2 = cast<MapSeqOp>(apply3.getOperand(0).getDefiningOp());
    if (!isa<LambdaOp>(apply3.getOperand(1).getDefiningOp())) return Failure();
    auto lamB = cast<LambdaOp>(apply3.getOperand(1).getDefiningOp()); // %9
    // successful match

    auto rrLamAEtaReducible = etaReducible(*lamA.getOperation());
    if (std::get_if<Failure>(&rrLamAEtaReducible)) return rrLamAEtaReducible;

    auto rrLamBEtaReducible = etaReducible(*lamB.getOperation());
    if (std::get_if<Failure>(&rrLamBEtaReducible)) return rrLamBEtaReducible;

    llvm::dbgs() << "Both lambdas are eta reducible\n";

    ScopedContext scope(*rewriter, expr.getLoc());
    rewriter->setInsertionPointAfter(apply1);

    apply1.getParentOfType<FuncOp>().dump();

    Operation *lambdaCopy = rewriter->clone(*lamB);

//    apply1.getParentOfType<FuncOp>().dump();

    Value result = mapSeq("affine", mapSeqOp1.t(), [&](Value elem){
          return mapSeq("affine", mapSeqOp2.t(), lambdaCopy->getResult(0), apply3.getOperand(2));
                   }, transpose(apply2.getOperand(2)));

    return success(*result.getDefiningOp());
  };
};

auto mapMapFBeforeTranspose = MapMapFBeforeTransposeStrategy();

#endif // LLVM_ELEVATE_ALGORITHMIC_H
