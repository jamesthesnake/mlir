//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#ifndef MLIR_RISE_TYPES_H
#define MLIR_RISE_TYPES_H

#include "mlir/IR/Dialect.h"
#include "mlir/IR/TypeSupport.h"
#include "mlir/IR/Types.h"

////////////////////////////////////////////////////////////////////////////////
/////////////////////// Custom Types for the Rise Dialect //////////////////////
////////////////////////////////////////////////////////////////////////////////

namespace mlir {
namespace rise {

namespace detail {
struct ArrayTypeStorage;
struct RiseFunTypeStorage;
struct RiseDataTypeWrapperStorage;
struct RiseTupleTypeStorage;
struct RiseNatStorage;

struct RiseScalarStorage;
} // namespace detail

/// RISE type structure:
///                      +----------+
///                      |mlir::Type|
///                      +-+---+--+-+
///                        ^   ^  ^
///              +---------+   |  +----------+
///              |             |             |
///          +---+----+      +-+-+      +----+---+
///          |DataType|      |Nat|      |RiseType|
///          ++------++      +---+      +--+--+--+
///           ^      ^                     ^  ^
///           |      |                     |  |
///    +-------+     +-------+          +--+  +-------+
///    |            |       |          |             |
/// +--+---+     +--+--+ +--+--+   +---+---+ +-------+-------+
/// |Scalar|     |Array| |Tuple|   |FunType| |DataTypeWrapper|
/// +------+     +-----+ +-----+   +-------+ +---------------+
///
/// RISE types are divided into three categories that all inherit from
/// mlir::Type:
///    Data types: include Array and Tuple and Scalar types.
///
///    Natural numbers: Used for tracking the length of the array in the type.
///
///    Rise types: Every RISE value has this type which is either a FunType,
///                representing a RISE function, or a DataTypeWrapper, wrapping
///                a DataType.
///
/// These types follow the type system of the Rise language, but exclude -- for
/// now -- type variables modelled as dependent function types. This type system
/// prevents function types (which are a sub-type of RiseType) to be treated
/// like DataTypes and, for example, be stored in an array.
///

class DataType : public Type::TypeBase<DataType, Type, TypeStorage> {
public:
  /// Inherit some necessary constructors from 'TypeBase'.
  using Base::Base;
};

class Nat : public mlir::Type::TypeBase<Nat, Type, detail::RiseNatStorage> {
public:
  /// Inherit some necessary constructors from 'TypeBase'.
  using Base::Base;

  /// This method is used to get an instance of Nat.
  static Nat get(mlir::MLIRContext *context, int intValue);

  int getIntValue();
};

class RiseType : public Type::TypeBase<RiseType, Type, TypeStorage> {
public:
  /// Inherit some necessary constructors from 'TypeBase'.
  using Base::Base;
};


class ScalarType : public mlir::Type::TypeBase<ScalarType, DataType,
    detail::RiseScalarStorage> {
public:
  /// Inherit some necessary constructors from 'TypeBase'.
  using Base::Base;

  /// This method is used to get an instance of ArrayType.
  static ScalarType get(mlir::MLIRContext *context, Type wrappedType);

  Type getWrappedType();
};

class ArrayType : public mlir::Type::TypeBase<ArrayType, DataType,
                                              detail::ArrayTypeStorage> {
public:
  /// Inherit some necessary constructors from 'TypeBase'.
  using Base::Base;

  /// This method is used to get an instance of ArrayType.
  static ArrayType get(mlir::MLIRContext *context, Nat size,
                       DataType elementType);

  Nat getSize();
  DataType getElementType();
};

class Tuple : public mlir::Type::TypeBase<Tuple, DataType,
                                          detail::RiseTupleTypeStorage> {
public:
  /// Inherit some necessary constructors from 'TypeBase'.
  using Base::Base;

  /// This method is used to get an instance of Tuple.
  static Tuple get(mlir::MLIRContext *context, DataType first, DataType second);

  DataType getFirst();
  DataType getSecond();
};

class FunType
    : public Type::TypeBase<FunType, RiseType, detail::RiseFunTypeStorage> {
public:
  /// Inherit some necessary constructors from 'TypeBase'.
  using Base::Base;

  /// This method is used to get an instance of FunType
  static FunType get(mlir::MLIRContext *context, Type input,
                     Type output);

  Type getInput();
  Type getOutput();
};

class DataTypeWrapper
    : public Type::TypeBase<DataTypeWrapper, RiseType,
                            detail::RiseDataTypeWrapperStorage> {
public:
  /// Inherit some necessary constructors from 'TypeBase'.
  using Base::Base;

  /// This method is used to get an instance of DataTypeWrapper.
  static DataTypeWrapper get(mlir::MLIRContext *context, DataType data);

  DataType getDataType();
};

} // end namespace rise
} // end namespace mlir

#endif // MLIR_RISE_TYPES_H
