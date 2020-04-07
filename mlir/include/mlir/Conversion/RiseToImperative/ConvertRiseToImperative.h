//
// Created by martin on 26/11/2019.
//

#ifndef MLIR_CONVERTRISETOIMPERATIVE_H
#define MLIR_CONVERTRISETOIMPERATIVE_H

#include "mlir/Dialect/Rise/IR/Types.h"
#include "mlir/Transforms/DialectConversion.h"
#include <mlir/Dialect/LLVMIR/LLVMDialect.h>

#include "mlir/IR/Builders.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/Functional.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/Passes.h"
#include "mlir/Transforms/Utils.h"

#include "mlir/Dialect/Rise/IR/Dialect.h"
#include "mlir/Dialect/Rise/Passes.h"
#include "mlir/Dialect/StandardOps/Ops.h"

#include "mlir/Dialect/Rise/variant.hpp"

namespace mlir {
namespace rise {


using OutputPathType = mpark::variant<int, Value>;

mlir::Value AccT(ApplyOp apply, Value out, PatternRewriter &rewriter);
mlir::Value AccT(ReturnOp returnOp, Value out, PatternRewriter &rewriter);

mlir::Value ConT(mlir::Value contValue, Block::iterator contLocation,
                 PatternRewriter &rewriter);
Value codeGen(Operation *op, SmallVector<OutputPathType, 10> path,
              PatternRewriter &rewriter);
Value codeGen(Value val, SmallVector<OutputPathType, 10> path,
              PatternRewriter &rewriter);
SmallVector<OutputPathType, 10>
codeGenStore(Value storeLocation, Value val,
             SmallVector<OutputPathType, 10> path, PatternRewriter &rewriter);
Value generateWriteAccess(SmallVector<OutputPathType, 10> path, Value accessVal,
                          PatternRewriter &rewriter);
void generateReadAccess(SmallVector<OutputPathType, 10> path, Value storeVal,
                        Value storeLoc, PatternRewriter &rewriter);

void Substitute(LambdaOp lambda, llvm::SmallVector<Value, 10> args);
LambdaOp expandToLambda(mlir::Value value, PatternRewriter &rewriter);
Value createLoopForMap(Value mapOpValue, IndexType &inductionVar, PatternRewriter &rewriter);
void printPath(SmallVector<OutputPathType, 10> input);
void printUses(Value val);
} // namespace rise
} // namespace mlir

#endif // MLIR_CONVERTRISETOIMPERATIVE_H
