//===----- ConstantBuilder.h - Builder for LLVM IR constants ----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This class provides a convenient interface for building complex
// global initializers.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_CODEGEN_CONSTANTBUILDER_H
#define LLVM_CLANG_LIB_CODEGEN_CONSTANTBUILDER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"

#include "CodeGenModule.h"

namespace clang {
namespace CodeGen {

class ConstantStructBuilder;
class ConstantArrayBuilder;

/// A convenience builder class for complex constant initializers,
/// especially for anonymous global structures used by various language
/// runtimes.
///
/// The basic usage pattern is expected to be something like:
///    ConstantInitBuilder builder(CGM);
///    auto toplevel = builder.beginStruct();
///    toplevel.addInt(CGM.SizeTy, widgets.size());
///    auto widgetArray = builder.beginArray();
///    for (auto &widget : widgets) {
///      auto widgetDesc = widgetArray.beginStruct();
///      widgetDesc.addInt(CGM.SizeTy, widget.getPower());
///      widgetDesc.add(CGM.GetAddrOfConstantString(widget.getName()));
///      widgetDesc.add(CGM.GetAddrOfGlobal(widget.getInitializerDecl()));
///      widgetArray.add(widgetDesc.finish());
///    }
///    toplevel.add(widgetArray.finish());
///    auto global = toplevel.finishAndCreateGlobal("WIDGET_LIST", Align,
///                                                 /*constant*/ true);
class ConstantInitBuilder {
  CodeGenModule &CGM;
  llvm::SmallVector<llvm::Constant*, 16> Buffer;
  bool Frozen = false;

public:
  explicit ConstantInitBuilder(CodeGenModule &CGM) : CGM(CGM) {}

  ~ConstantInitBuilder() {
    assert(Buffer.empty() && "didn't claim all values out of buffer");
  }

  class AggregateBuilderBase {
  protected:
    ConstantInitBuilder &Builder;
    AggregateBuilderBase *Parent;
    size_t Begin;
    bool Finished = false;
    bool Frozen = false;

    llvm::SmallVectorImpl<llvm::Constant*> &getBuffer() {
      return Builder.Buffer;
    }

    const llvm::SmallVectorImpl<llvm::Constant*> &getBuffer() const {
      return Builder.Buffer;
    }

    AggregateBuilderBase(ConstantInitBuilder &builder,
                         AggregateBuilderBase *parent)
        : Builder(builder), Parent(parent), Begin(builder.Buffer.size()) {
      if (parent) {
        assert(!parent->Frozen && "parent already has child builder active");
        parent->Frozen = true;
      } else {
        assert(!builder.Frozen && "builder already has child builder active");
        builder.Frozen = true;
      }
    }

    ~AggregateBuilderBase() {
      assert(Finished && "didn't claim value from aggregate builder");
    }

    void markFinished() {
      assert(!Frozen && "child builder still active");
      assert(!Finished && "builder already finished");
      Finished = true;
      if (Parent) {
        assert(Parent->Frozen &&
               "parent not frozen while child builder active");
        Parent->Frozen = false;
      } else {
        assert(Builder.Frozen &&
               "builder not frozen while child builder active");
        Builder.Frozen = false;
      }
    }

  public:
    // Not copyable.
    AggregateBuilderBase(const AggregateBuilderBase &) = delete;
    AggregateBuilderBase &operator=(const AggregateBuilderBase &) = delete;

    // Movable, mostly to allow returning.  But we have to write this out
    // properly to satisfy the assert in the destructor.
    AggregateBuilderBase(AggregateBuilderBase &&other)
      : Builder(other.Builder), Parent(other.Parent), Begin(other.Begin),
        Finished(other.Finished), Frozen(other.Frozen) {
      other.Finished = false;
    }
    AggregateBuilderBase &operator=(AggregateBuilderBase &&other) = delete;

    void add(llvm::Constant *value) {
      assert(!Finished && "cannot add more values after finishing builder");
      Builder.Buffer.push_back(value);
    }

    void addSize(CharUnits size) {
      add(Builder.CGM.getSize(size));
    }

    void addInt(llvm::IntegerType *intTy, uint64_t value,
                bool isSigned = false) {
      add(llvm::ConstantInt::get(intTy, value, isSigned));
    }

    void addNullPointer(llvm::PointerType *ptrTy) {
      add(llvm::ConstantPointerNull::get(ptrTy));
    }

    ArrayRef<llvm::Constant*> getGEPIndicesToCurrentPosition(
                             llvm::SmallVectorImpl<llvm::Constant*> &indices) {
      getGEPIndicesTo(indices, Builder.Buffer.size());
      return indices;
    }

    ConstantArrayBuilder beginArray(llvm::Type *eltTy = nullptr);
    ConstantStructBuilder beginStruct(llvm::StructType *structTy = nullptr);

  private:
    void getGEPIndicesTo(llvm::SmallVectorImpl<llvm::Constant*> &indices,
                         size_t position) const {
      // Recurse on the parent builder if present.
      if (Parent) {
        Parent->getGEPIndicesTo(indices, Begin);

      // Otherwise, add an index to drill into the first level of pointer. 
      } else {
        assert(indices.empty());
        indices.push_back(llvm::ConstantInt::get(Builder.CGM.SizeTy, 0));
      }

      assert(position >= Begin);
      indices.push_back(llvm::ConstantInt::get(Builder.CGM.SizeTy,
                                               position - Begin));
    }
  };

  template <class Impl>
  class AggregateBuilder : public AggregateBuilderBase {
  protected:
    AggregateBuilder(ConstantInitBuilder &builder,
                     AggregateBuilderBase *parent)
      : AggregateBuilderBase(builder, parent) {}

    Impl &asImpl() { return *static_cast<Impl*>(this); }

  public:
    /// Given that this builder was created by beginning an array or struct
    /// component on the given parent builder, finish the array/struct
    /// component and add it to the parent.
    ///
    /// It is an intentional choice that the parent is passed in explicitly
    /// despite it being redundant with information already kept in the
    /// builder.  This aids in readability by making it easier to find the
    /// places that add components to a builder, as well as "bookending"
    /// the sub-builder more explicitly.
    void finishAndAddTo(AggregateBuilderBase &parent) {
      assert(Parent == &parent && "adding to non-parent builder");
      parent.add(asImpl().finishImpl());
    }

    /// Given that this builder was created by beginning an array or struct
    /// directly on a ConstantInitBuilder, finish the array/struct and
    /// create a global variable with it as the initializer.
    template <class... As>
    llvm::GlobalVariable *finishAndCreateGlobal(As &&...args) {
      assert(!Parent && "finishing non-root builder");
      return Builder.createGlobal(asImpl().finishImpl(),
                                  std::forward<As>(args)...);
    }

    /// Given that this builder was created by beginning an array or struct
    /// directly on a ConstantInitBuilder, finish the array/struct and
    /// set it as the initializer of the given global variable.
    void finishAndSetAsInitializer(llvm::GlobalVariable *global) {
      assert(!Parent && "finishing non-root builder");
      return Builder.setGlobalInitializer(global, asImpl().finishImpl());
    }
  };

  ConstantArrayBuilder beginArray(llvm::Type *eltTy = nullptr);

  ConstantStructBuilder beginStruct(llvm::StructType *structTy = nullptr);

private:
  llvm::GlobalVariable *createGlobal(llvm::Constant *initializer,
                                     StringRef name,
                                     CharUnits alignment,
                                     bool constant = false,
                                     llvm::GlobalValue::LinkageTypes linkage
                                       = llvm::GlobalValue::InternalLinkage,
                                     unsigned addressSpace = 0) {
    auto GV = new llvm::GlobalVariable(CGM.getModule(),
                                       initializer->getType(),
                                       constant,
                                       linkage,
                                       initializer,
                                       name,
                                       /*insert before*/ nullptr,
                                       llvm::GlobalValue::NotThreadLocal,
                                       addressSpace);
    GV->setAlignment(alignment.getQuantity());
    return GV;
  }

  void setGlobalInitializer(llvm::GlobalVariable *GV,
                            llvm::Constant *initializer) {
    GV->setInitializer(initializer);
  }
};

/// A helper class of ConstantInitBuilder, used for building constant
/// array initializers.
class ConstantArrayBuilder
    : public ConstantInitBuilder::AggregateBuilder<ConstantArrayBuilder> {
  llvm::Type *EltTy;
  friend class ConstantInitBuilder;
  template <class Impl> friend class ConstantInitBuilder::AggregateBuilder;
  ConstantArrayBuilder(ConstantInitBuilder &builder,
                       AggregateBuilderBase *parent, llvm::Type *eltTy)
    : AggregateBuilder(builder, parent), EltTy(eltTy) {}
public:
  size_t size() const {
    assert(!Finished);
    assert(!Frozen);
    assert(Begin <= getBuffer().size());
    return getBuffer().size() - Begin;
  }

private:
  /// Form an array constant from the values that have been added to this
  /// builder.
  llvm::Constant *finishImpl() {
    markFinished();

    auto &buffer = getBuffer();
    assert((Begin < buffer.size() ||
            (Begin == buffer.size() && EltTy))
           && "didn't add any array elements without element type");
    auto elts = llvm::makeArrayRef(buffer).slice(Begin);
    auto eltTy = EltTy ? EltTy : elts[0]->getType();
    auto type = llvm::ArrayType::get(eltTy, elts.size());
    auto constant = llvm::ConstantArray::get(type, elts);
    buffer.erase(buffer.begin() + Begin, buffer.end());
    return constant;
  }
};

inline ConstantArrayBuilder
ConstantInitBuilder::beginArray(llvm::Type *eltTy) {
  return ConstantArrayBuilder(*this, nullptr, eltTy);
}

inline ConstantArrayBuilder
ConstantInitBuilder::AggregateBuilderBase::beginArray(llvm::Type *eltTy) {
  return ConstantArrayBuilder(Builder, this, eltTy);
}

/// A helper class of ConstantInitBuilder, used for building constant
/// struct initializers.
class ConstantStructBuilder
    : public ConstantInitBuilder::AggregateBuilder<ConstantStructBuilder> {
  llvm::StructType *Ty;
  friend class ConstantInitBuilder;
  template <class Impl> friend class ConstantInitBuilder::AggregateBuilder;
  ConstantStructBuilder(ConstantInitBuilder &builder,
                        AggregateBuilderBase *parent, llvm::StructType *ty)
    : AggregateBuilder(builder, parent), Ty(ty) {}

  /// Finish the struct.
  llvm::Constant *finishImpl() {
    markFinished();

    auto &buffer = getBuffer();
    assert(Begin < buffer.size() && "didn't add any struct elements?");
    auto elts = llvm::makeArrayRef(buffer).slice(Begin);

    llvm::Constant *constant;
    if (Ty) {
      constant = llvm::ConstantStruct::get(Ty, elts);
    } else {
      constant = llvm::ConstantStruct::getAnon(elts, /*packed*/ false);
    }

    buffer.erase(buffer.begin() + Begin, buffer.end());
    return constant;
  }
};

inline ConstantStructBuilder
ConstantInitBuilder::beginStruct(llvm::StructType *structTy) {
  return ConstantStructBuilder(*this, nullptr, structTy);
}

inline ConstantStructBuilder
ConstantInitBuilder::AggregateBuilderBase::beginStruct(
                                                  llvm::StructType *structTy) {
  return ConstantStructBuilder(Builder, this, structTy);
}

}  // end namespace CodeGen
}  // end namespace clang

#endif
