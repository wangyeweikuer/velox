/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "velox/common/base/Exceptions.h"
#include "velox/core/ITypedExpr.h"
#include "velox/vector/BaseVector.h"

namespace facebook::velox::core {

class InputTypedExpr : public ITypedExpr {
 public:
  InputTypedExpr(std::shared_ptr<const Type> type)
      : ITypedExpr{std::move(type)} {}

  bool operator==(const ITypedExpr& other) const final {
    const auto* casted = dynamic_cast<const InputTypedExpr*>(&other);
    return casted != nullptr;
  }

  std::string toString() const override {
    return "ROW";
  }

  size_t localHash() const override {
    static const size_t kBaseHash = std::hash<const char*>()("InputTypedExpr");
    return kBaseHash;
  }

  TypedExprPtr rewriteInputNames(
      const std::unordered_map<std::string, TypedExprPtr>& /*mapping*/)
      const override {
    return std::make_shared<InputTypedExpr>(type());
  }

  folly::dynamic serialize() const override;

  static TypedExprPtr create(const folly::dynamic& obj, void* context);
};

class ConstantTypedExpr : public ITypedExpr {
 public:
  // Creates constant expression. For complex types, only
  // variant::null() value is supported.
  ConstantTypedExpr(std::shared_ptr<const Type> type, variant value)
      : ITypedExpr{std::move(type)}, value_{std::move(value)} {}

  // Creates constant expression of scalar or complex type. The value comes from
  // index zero.
  explicit ConstantTypedExpr(const VectorPtr& value)
      : ITypedExpr{value->type()},
        valueVector_{
            value->isConstantEncoding()
                ? value
                : BaseVector::wrapInConstant(1, 0, value)} {}

  std::string toString() const override {
    if (hasValueVector()) {
      return valueVector_->toString(0);
    }
    return value_.toJson(type());
  }

  size_t localHash() const override {
    static const size_t kBaseHash =
        std::hash<const char*>()("ConstantTypedExpr");

    return bits::hashMix(
        kBaseHash,
        hasValueVector() ? valueVector_->hashValueAt(0) : value_.hash());
  }

  bool hasValueVector() const {
    return valueVector_ != nullptr;
  }

  // Returns scalar value as variant if hasValueVector() is false.
  const variant& value() const {
    return value_;
  }

  /// Return constant value vector if hasValueVector() is true. Returns null
  /// otherwise.
  const VectorPtr& valueVector() const {
    return valueVector_;
  }

  VectorPtr toConstantVector(memory::MemoryPool* pool) const {
    if (valueVector_) {
      return valueVector_;
    }
    if (value_.isNull()) {
      return BaseVector::createNullConstant(type(), 1, pool);
    }
    return BaseVector::createConstant(type(), value_, 1, pool);
  }

  const std::vector<TypedExprPtr>& inputs() const {
    static const std::vector<TypedExprPtr> kEmpty{};
    return kEmpty;
  }

  TypedExprPtr rewriteInputNames(
      const std::unordered_map<std::string, TypedExprPtr>& /*mapping*/)
      const override {
    if (hasValueVector()) {
      return std::make_shared<ConstantTypedExpr>(valueVector_);
    } else {
      return std::make_shared<ConstantTypedExpr>(type(), value_);
    }
  }

  bool equals(const ITypedExpr& other) const {
    const auto* casted = dynamic_cast<const ConstantTypedExpr*>(&other);
    if (!casted) {
      return false;
    }

    if (*this->type() != *casted->type()) {
      return false;
    }

    if (this->hasValueVector() != casted->hasValueVector()) {
      return false;
    }

    if (this->hasValueVector()) {
      return this->valueVector_->equalValueAt(casted->valueVector_.get(), 0, 0);
    }

    return this->value_ == casted->value_;
  }

  bool operator==(const ITypedExpr& other) const final {
    return this->equals(other);
  }

  bool operator==(const ConstantTypedExpr& other) const {
    return this->equals(other);
  }

  folly::dynamic serialize() const override;

  static TypedExprPtr create(const folly::dynamic& obj, void* context);

 private:
  const variant value_;
  const VectorPtr valueVector_;
};

class CallTypedExpr : public ITypedExpr {
 public:
  CallTypedExpr(
      std::shared_ptr<const Type> type,
      std::vector<TypedExprPtr> inputs,
      std::string funcName)
      : ITypedExpr{std::move(type), std::move(inputs)},
        name_(std::move(funcName)) {}

  virtual const std::string& name() const {
    return name_;
  }

  TypedExprPtr rewriteInputNames(
      const std::unordered_map<std::string, TypedExprPtr>& mapping)
      const override {
    return std::make_shared<CallTypedExpr>(
        type(), rewriteInputsRecursive(mapping), name_);
  }

  std::string toString() const override {
    std::string str{};
    str += name();
    str += "(";
    for (size_t i = 0; i < inputs().size(); ++i) {
      auto& input = inputs().at(i);
      if (i != 0) {
        str += ",";
      }
      str += input->toString();
    }
    str += ")";
    return str;
  }

  size_t localHash() const override {
    static const size_t kBaseHash = std::hash<const char*>()("CallTypedExpr");
    return bits::hashMix(kBaseHash, std::hash<std::string>()(name_));
  }

  bool operator==(const ITypedExpr& other) const override {
    const auto* casted = dynamic_cast<const CallTypedExpr*>(&other);
    if (!casted) {
      return false;
    }
    if (casted->name() != this->name()) {
      return false;
    }
    if (*casted->type() != *this->type()) {
      return false;
    }
    return std::equal(
        this->inputs().begin(),
        this->inputs().end(),
        casted->inputs().begin(),
        casted->inputs().end(),
        [](const auto& p1, const auto& p2) { return *p1 == *p2; });
  }

  folly::dynamic serialize() const override;

  static TypedExprPtr create(const folly::dynamic& obj, void* context);

 private:
  const std::string name_;
};

using CallTypedExprPtr = std::shared_ptr<const CallTypedExpr>;

/// Represents a leaf in an expression tree specifying input column by name.
class FieldAccessTypedExpr : public ITypedExpr {
 public:
  /// Used as a leaf in an expression tree specifying input column by name.
  FieldAccessTypedExpr(TypePtr type, std::string name)
      : ITypedExpr{std::move(type)},
        name_(std::move(name)),
        isInputColumn_(true) {}

  /// Used as a dereference expression which selects a subfield in a struct by
  /// name.
  FieldAccessTypedExpr(TypePtr type, TypedExprPtr input, std::string name)
      : ITypedExpr{std::move(type), {std::move(input)}},
        name_(std::move(name)),
        isInputColumn_(dynamic_cast<const InputTypedExpr*>(inputs()[0].get())) {
  }

  const std::string& name() const {
    return name_;
  }

  TypedExprPtr rewriteInputNames(
      const std::unordered_map<std::string, TypedExprPtr>& mapping)
      const override {
    if (inputs().empty()) {
      auto it = mapping.find(name_);
      return it != mapping.end()
          ? it->second
          : std::make_shared<FieldAccessTypedExpr>(type(), name_);
    }

    auto newInputs = rewriteInputsRecursive(mapping);
    VELOX_CHECK_EQ(1, newInputs.size());
    // Only rewrite name if input in InputTypedExpr. Rewrite in other
    // cases(like dereference) is unsound.
    if (!std::dynamic_pointer_cast<const InputTypedExpr>(newInputs[0])) {
      return std::make_shared<FieldAccessTypedExpr>(
          type(), newInputs[0], name_);
    }
    auto it = mapping.find(name_);
    auto newName = name_;
    if (it != mapping.end()) {
      if (auto name = std::dynamic_pointer_cast<const FieldAccessTypedExpr>(
              it->second)) {
        newName = name->name();
      }
    }
    return std::make_shared<FieldAccessTypedExpr>(
        type(), newInputs[0], newName);
  }

  std::string toString() const override {
    if (inputs().empty()) {
      return fmt::format("{}", std::quoted(name(), '"', '"'));
    }

    return fmt::format(
        "{}[{}]", inputs()[0]->toString(), std::quoted(name(), '"', '"'));
  }

  size_t localHash() const override {
    static const size_t kBaseHash =
        std::hash<const char*>()("FieldAccessTypedExpr");
    return bits::hashMix(kBaseHash, std::hash<std::string>()(name_));
  }

  bool operator==(const ITypedExpr& other) const final {
    const auto* casted = dynamic_cast<const FieldAccessTypedExpr*>(&other);
    if (!casted) {
      return false;
    }
    if (casted->name_ != this->name_) {
      return false;
    }
    if (*casted->type() != *this->type()) {
      return false;
    }
    return std::equal(
        this->inputs().begin(),
        this->inputs().end(),
        casted->inputs().begin(),
        casted->inputs().end(),
        [](const auto& p1, const auto& p2) { return *p1 == *p2; });
  }

  /// Is this FieldAccess accessing an input column or a field in a struct.
  bool isInputColumn() const {
    return isInputColumn_;
  }

  folly::dynamic serialize() const override;

  static TypedExprPtr create(const folly::dynamic& obj, void* context);

 private:
  const std::string name_;
  const bool isInputColumn_;
};

using FieldAccessTypedExprPtr = std::shared_ptr<const FieldAccessTypedExpr>;

/// Represents a dereference expression which selects a subfield in a struct by
/// name.
class DereferenceTypedExpr : public ITypedExpr {
 public:
  DereferenceTypedExpr(TypePtr type, TypedExprPtr input, uint32_t index)
      : ITypedExpr{std::move(type), {std::move(input)}}, index_(index) {
    // Make sure this isn't being used to access a top level column.
    VELOX_USER_CHECK_NULL(
        std::dynamic_pointer_cast<const InputTypedExpr>(inputs()[0]));
  }

  uint32_t index() const {
    return index_;
  }

  const std::string& name() const {
    return inputs()[0]->type()->asRow().nameOf(index_);
  }

  TypedExprPtr rewriteInputNames(
      const std::unordered_map<std::string, TypedExprPtr>& mapping)
      const override {
    auto newInputs = rewriteInputsRecursive(mapping);
    VELOX_CHECK_EQ(1, newInputs.size());

    return std::make_shared<DereferenceTypedExpr>(type(), newInputs[0], index_);
  }

  std::string toString() const override {
    return fmt::format(
        "{}[{}]", inputs()[0]->toString(), std::quoted(name(), '"', '"'));
  }

  size_t localHash() const override {
    static const size_t kBaseHash =
        std::hash<const char*>()("DereferenceTypedExpr");
    return bits::hashMix(kBaseHash, index_);
  }

  bool operator==(const ITypedExpr& other) const final {
    const auto* casted = dynamic_cast<const DereferenceTypedExpr*>(&other);
    if (!casted) {
      return false;
    }
    if (casted->index_ != this->index_) {
      return false;
    }
    return std::equal(
        this->inputs().begin(),
        this->inputs().end(),
        casted->inputs().begin(),
        casted->inputs().end(),
        [](const auto& p1, const auto& p2) { return *p1 == *p2; });
  }

  folly::dynamic serialize() const override;

  static TypedExprPtr create(const folly::dynamic& obj, void* context);

 private:
  const uint32_t index_;
};

using DereferenceTypedExprPtr = std::shared_ptr<const DereferenceTypedExpr>;

/*
 * Evaluates a list of expressions to produce a row.
 */
class ConcatTypedExpr : public ITypedExpr {
 public:
  ConcatTypedExpr(
      const std::vector<std::string>& names,
      const std::vector<TypedExprPtr>& inputs)
      : ITypedExpr{toType(names, inputs), inputs} {}

  TypedExprPtr rewriteInputNames(
      const std::unordered_map<std::string, TypedExprPtr>& mapping)
      const override {
    return std::make_shared<ConcatTypedExpr>(
        type()->asRow().names(), rewriteInputsRecursive(mapping));
  }

  std::string toString() const override {
    std::string str{};
    str += "CONCAT(";
    for (size_t i = 0; i < inputs().size(); ++i) {
      auto& input = inputs().at(i);
      if (i != 0) {
        str += ",";
      }
      str += input->toString();
    }
    str += ")";
    return str;
  }

  size_t localHash() const override {
    static const size_t kBaseHash = std::hash<const char*>()("ConcatTypedExpr");
    return kBaseHash;
  }

  bool operator==(const ITypedExpr& other) const override {
    const auto* casted = dynamic_cast<const ConcatTypedExpr*>(&other);
    if (!casted) {
      return false;
    }
    if (*casted->type() != *this->type()) {
      return false;
    }
    return std::equal(
        this->inputs().begin(),
        this->inputs().end(),
        casted->inputs().begin(),
        casted->inputs().end(),
        [](const auto& p1, const auto& p2) { return *p1 == *p2; });
  }

  folly::dynamic serialize() const override;

  static TypedExprPtr create(const folly::dynamic& obj, void* context);

 private:
  static std::shared_ptr<const Type> toType(
      const std::vector<std::string>& names,
      const std::vector<TypedExprPtr>& expressions) {
    std::vector<std::shared_ptr<const Type>> children{};
    std::vector<std::string> namesCopy{};
    for (size_t i = 0; i < names.size(); ++i) {
      namesCopy.push_back(names.at(i));
      children.push_back(expressions.at(i)->type());
    }
    return ROW(std::move(namesCopy), std::move(children));
  }
};

class LambdaTypedExpr : public ITypedExpr {
 public:
  LambdaTypedExpr(RowTypePtr signature, TypedExprPtr body)
      : ITypedExpr(std::make_shared<FunctionType>(
            std::vector<std::shared_ptr<const Type>>(signature->children()),
            body->type())),
        signature_(signature),
        body_(body) {}

  const RowTypePtr& signature() const {
    return signature_;
  }

  const TypedExprPtr& body() const {
    return body_;
  }

  TypedExprPtr rewriteInputNames(
      const std::unordered_map<std::string, TypedExprPtr>& mapping)
      const override {
    for (const auto& name : signature_->names()) {
      if (mapping.count(name)) {
        VELOX_USER_FAIL("Ambiguous variable: {}", name);
      }
    }
    return std::make_shared<LambdaTypedExpr>(
        signature_, body_->rewriteInputNames(mapping));
  }

  std::string toString() const override {
    return fmt::format(
        "lambda {} -> {}", signature_->toString(), body_->toString());
  }

  size_t localHash() const override {
    static const size_t kBaseHash = std::hash<const char*>()("LambdaTypedExpr");
    return bits::hashMix(kBaseHash, body_->hash());
  }

  bool operator==(const ITypedExpr& other) const override {
    const auto* casted = dynamic_cast<const LambdaTypedExpr*>(&other);
    if (!casted) {
      return false;
    }
    if (*casted->type() != *this->type()) {
      return false;
    }
    return *signature_ == *casted->signature_ && *body_ == *casted->body_;
  }

  folly::dynamic serialize() const override;

  static TypedExprPtr create(const folly::dynamic& obj, void* context);

 private:
  const RowTypePtr signature_;
  const TypedExprPtr body_;
};

class CastTypedExpr : public ITypedExpr {
 public:
  CastTypedExpr(
      const std::shared_ptr<const Type>& type,
      const std::vector<TypedExprPtr>& inputs,
      bool nullOnFailure)
      : ITypedExpr{type, inputs}, nullOnFailure_(nullOnFailure) {}

  TypedExprPtr rewriteInputNames(
      const std::unordered_map<std::string, TypedExprPtr>& mapping)
      const override {
    return std::make_shared<CastTypedExpr>(
        type(), rewriteInputsRecursive(mapping), nullOnFailure_);
  }

  std::string toString() const override {
    if (nullOnFailure_) {
      return fmt::format(
          "try_cast {} as {}", inputs()[0]->toString(), type()->toString());
    } else {
      return fmt::format(
          "cast {} as {}", inputs()[0]->toString(), type()->toString());
    }
  }

  size_t localHash() const override {
    static const size_t kBaseHash = std::hash<const char*>()("CastTypedExpr");
    return bits::hashMix(kBaseHash, std::hash<bool>()(nullOnFailure_));
  }

  bool operator==(const ITypedExpr& other) const override {
    const auto* otherCast = dynamic_cast<const CastTypedExpr*>(&other);
    if (!otherCast) {
      return false;
    }
    if (inputs().empty()) {
      return type() == otherCast->type() && otherCast->inputs().empty() &&
          nullOnFailure_ == otherCast->nullOnFailure();
    }
    return *type() == *otherCast->type() &&
        *inputs()[0] == *otherCast->inputs()[0] &&
        nullOnFailure_ == otherCast->nullOnFailure();
  }

  bool nullOnFailure() const {
    return nullOnFailure_;
  }

  folly::dynamic serialize() const override;

  static TypedExprPtr create(const folly::dynamic& obj, void* context);

 private:
  // This flag prevents throws and instead returns
  // null on cast failure
  const bool nullOnFailure_;
};

} // namespace facebook::velox::core
