//===--- TypeCheckRequests.cpp - Type Checking Requests ------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticsCommon.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeLoc.h"
#include "swift/AST/TypeRepr.h"
#include "swift/AST/Types.h"
#include "swift/Subsystems.h"

using namespace swift;

namespace swift {
// Implement the type checker type zone (zone 10).
#define SWIFT_TYPEID_ZONE TypeChecker
#define SWIFT_TYPEID_HEADER "swift/AST/TypeCheckerTypeIDZone.def"
#include "swift/Basic/ImplementTypeIDZone.h"
#undef SWIFT_TYPEID_ZONE
#undef SWIFT_TYPEID_HEADER
}

void swift::simple_display(
       llvm::raw_ostream &out,
       const llvm::PointerUnion<TypeDecl *, ExtensionDecl *> &value) {
  if (auto type = value.dyn_cast<TypeDecl *>()) {
    type->dumpRef(out);
    return;
  }

  auto ext = value.get<ExtensionDecl *>();
  simple_display(out, ext);
}

void swift::simple_display(llvm::raw_ostream &out,
                           const TypeResolutionStage &value) {
  switch (value) {
  case TypeResolutionStage::Structural:
    out << "structural";
    break;

  case TypeResolutionStage::Interface:
    out << "interface";
    break;

  case TypeResolutionStage::Contextual:
    out << "contextual";
    break;
  }
}

void swift::simple_display(llvm::raw_ostream &out, Type type) {
  if (type)
    type.print(out);
  else
    out << "null";
}

void swift::simple_display(llvm::raw_ostream &out, const TypeRepr *TyR) {
  if (TyR)
    TyR->print(out);
  else
    out << "null";
}

void swift::simple_display(llvm::raw_ostream &out, const TypeLoc source) {
  out << "(";
  simple_display(out, source.getType());
  out << ", ";
  simple_display(out, source.getTypeRepr());
  out << ")";
}

//----------------------------------------------------------------------------//
// Inherited type computation.
//----------------------------------------------------------------------------//

SourceLoc InheritedTypeRequest::getNearestLoc() const {
  const auto &storage = getStorage();
  auto &typeLoc = getInheritedTypeLocAtIndex(std::get<0>(storage),
                                             std::get<1>(storage));
  return typeLoc.getLoc();
}

bool InheritedTypeRequest::isCached() const {
  return std::get<2>(getStorage()) == TypeResolutionStage::Interface;
}

Optional<Type> InheritedTypeRequest::getCachedResult() const {
  const auto &storage = getStorage();
  auto &typeLoc = getInheritedTypeLocAtIndex(std::get<0>(storage),
                                             std::get<1>(storage));
  if (typeLoc.wasValidated())
    return typeLoc.getType();

  return None;
}

void InheritedTypeRequest::cacheResult(Type value) const {
  const auto &storage = getStorage();
  auto &typeLoc = getInheritedTypeLocAtIndex(std::get<0>(storage),
                                             std::get<1>(storage));
  typeLoc.setType(value);
}

//----------------------------------------------------------------------------//
// Superclass computation.
//----------------------------------------------------------------------------//
void SuperclassTypeRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  // FIXME: Improve this diagnostic.
  auto nominalDecl = std::get<0>(getStorage());
  diags.diagnose(nominalDecl, diag::circular_class_inheritance,
                 nominalDecl->getName());
}

bool SuperclassTypeRequest::isCached() const {
  return std::get<1>(getStorage()) == TypeResolutionStage::Interface;
}

Optional<Type> SuperclassTypeRequest::getCachedResult() const {
  auto nominalDecl = std::get<0>(getStorage());

  if (auto *classDecl = dyn_cast<ClassDecl>(nominalDecl))
    if (classDecl->LazySemanticInfo.SuperclassType.getInt())
      return classDecl->LazySemanticInfo.SuperclassType.getPointer();

  if (auto *protocolDecl = dyn_cast<ProtocolDecl>(nominalDecl))
    if (protocolDecl->LazySemanticInfo.SuperclassType.getInt())
      return protocolDecl->LazySemanticInfo.SuperclassType.getPointer();

  return None;
}

void SuperclassTypeRequest::cacheResult(Type value) const {
  auto nominalDecl = std::get<0>(getStorage());

  if (auto *classDecl = dyn_cast<ClassDecl>(nominalDecl))
    classDecl->LazySemanticInfo.SuperclassType.setPointerAndInt(value, true);

  if (auto *protocolDecl = dyn_cast<ProtocolDecl>(nominalDecl))
    protocolDecl->LazySemanticInfo.SuperclassType.setPointerAndInt(value, true);
}

evaluator::DependencySource SuperclassTypeRequest::readDependencySource(
    const evaluator::DependencyCollector &e) const {
  const auto access = std::get<0>(getStorage())->getFormalAccess();
  return {
    e.getActiveDependencySourceOrNull(),
    evaluator::getScopeForAccessLevel(access)
  };
}

void SuperclassTypeRequest::writeDependencySink(
    evaluator::DependencyCollector &tracker, Type value) const {
  if (!value)
    return;

  // FIXME: This is compatible with the existing name tracking scheme, but
  // ignoring this name when we fail to look up a class is bogus.
  ClassDecl *Super = value->getClassOrBoundGenericClass();
  if (!Super)
    return;
  tracker.addPotentialMember(Super);
}

//----------------------------------------------------------------------------//
// Enum raw type computation.
//----------------------------------------------------------------------------//
void EnumRawTypeRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  // FIXME: Improve this diagnostic.
  auto enumDecl = std::get<0>(getStorage());
  diags.diagnose(enumDecl, diag::circular_enum_inheritance, enumDecl->getName());
}

bool EnumRawTypeRequest::isCached() const {
  return std::get<1>(getStorage()) == TypeResolutionStage::Interface;
}

Optional<Type> EnumRawTypeRequest::getCachedResult() const {
  auto enumDecl = std::get<0>(getStorage());
  if (enumDecl->LazySemanticInfo.hasRawType())
    return enumDecl->LazySemanticInfo.RawTypeAndFlags.getPointer();

  return None;
}

void EnumRawTypeRequest::cacheResult(Type value) const {
  auto enumDecl = std::get<0>(getStorage());
  enumDecl->LazySemanticInfo.cacheRawType(value);
}

//----------------------------------------------------------------------------//
// isObjC computation.
//----------------------------------------------------------------------------//

Optional<bool> IsObjCRequest::getCachedResult() const {
  auto decl = std::get<0>(getStorage());
  if (decl->LazySemanticInfo.isObjCComputed)
    return decl->LazySemanticInfo.isObjC;

  return None;
}

void IsObjCRequest::cacheResult(bool value) const {
  auto decl = std::get<0>(getStorage());
  decl->setIsObjC(value);
}

//----------------------------------------------------------------------------//
// requiresClass computation.
//----------------------------------------------------------------------------//

void ProtocolRequiresClassRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  auto decl = std::get<0>(getStorage());
  diags.diagnose(decl, diag::circular_protocol_def, decl->getName());
}

void ProtocolRequiresClassRequest::noteCycleStep(DiagnosticEngine &diags) const {
  auto requirement = std::get<0>(getStorage());
  diags.diagnose(requirement, diag::kind_declname_declared_here,
                 DescriptiveDeclKind::Protocol,
                 requirement->getName());
}

Optional<bool> ProtocolRequiresClassRequest::getCachedResult() const {
  auto decl = std::get<0>(getStorage());
  return decl->getCachedRequiresClass();
}

void ProtocolRequiresClassRequest::cacheResult(bool value) const {
  auto decl = std::get<0>(getStorage());
  decl->setCachedRequiresClass(value);
}

//----------------------------------------------------------------------------//
// existentialConformsToSelf computation.
//----------------------------------------------------------------------------//

void ExistentialConformsToSelfRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  auto decl = std::get<0>(getStorage());
  diags.diagnose(decl, diag::circular_protocol_def, decl->getName());
}

void ExistentialConformsToSelfRequest::noteCycleStep(DiagnosticEngine &diags) const {
  auto requirement = std::get<0>(getStorage());
  diags.diagnose(requirement, diag::kind_declname_declared_here,
                 DescriptiveDeclKind::Protocol, requirement->getName());
}

Optional<bool> ExistentialConformsToSelfRequest::getCachedResult() const {
  auto decl = std::get<0>(getStorage());
  return decl->getCachedExistentialConformsToSelf();
}

void ExistentialConformsToSelfRequest::cacheResult(bool value) const {
  auto decl = std::get<0>(getStorage());
  decl->setCachedExistentialConformsToSelf(value);
}

//----------------------------------------------------------------------------//
// existentialTypeSupported computation.
//----------------------------------------------------------------------------//

void ExistentialTypeSupportedRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  auto decl = std::get<0>(getStorage());
  diags.diagnose(decl, diag::circular_protocol_def, decl->getName());
}

void ExistentialTypeSupportedRequest::noteCycleStep(DiagnosticEngine &diags) const {
  auto requirement = std::get<0>(getStorage());
  diags.diagnose(requirement, diag::kind_declname_declared_here,
                 DescriptiveDeclKind::Protocol, requirement->getName());
}

Optional<bool> ExistentialTypeSupportedRequest::getCachedResult() const {
  auto decl = std::get<0>(getStorage());
  return decl->getCachedExistentialTypeSupported();
}

void ExistentialTypeSupportedRequest::cacheResult(bool value) const {
  auto decl = std::get<0>(getStorage());
  decl->setCachedExistentialTypeSupported(value);
}

//----------------------------------------------------------------------------//
// isFinal computation.
//----------------------------------------------------------------------------//

Optional<bool> IsFinalRequest::getCachedResult() const {
  auto decl = std::get<0>(getStorage());
  if (decl->LazySemanticInfo.isFinalComputed)
    return decl->LazySemanticInfo.isFinal;

  return None;
}

void IsFinalRequest::cacheResult(bool value) const {
  auto decl = std::get<0>(getStorage());
  decl->LazySemanticInfo.isFinalComputed = true;
  decl->LazySemanticInfo.isFinal = value;

  // Add an attribute for printing
  if (value && !decl->getAttrs().hasAttribute<FinalAttr>())
    decl->getAttrs().add(new (decl->getASTContext()) FinalAttr(/*Implicit=*/true));
}

//----------------------------------------------------------------------------//
// isDynamic computation.
//----------------------------------------------------------------------------//

Optional<bool> IsDynamicRequest::getCachedResult() const {
  auto decl = std::get<0>(getStorage());
  if (decl->LazySemanticInfo.isDynamicComputed)
    return decl->LazySemanticInfo.isDynamic;

  return None;
}

void IsDynamicRequest::cacheResult(bool value) const {
  auto decl = std::get<0>(getStorage());
  decl->setIsDynamic(value);

  // Add an attribute for printing
  if (value && !decl->getAttrs().hasAttribute<DynamicAttr>())
    decl->getAttrs().add(new (decl->getASTContext()) DynamicAttr(/*Implicit=*/true));
}

//----------------------------------------------------------------------------//
// RequirementSignatureRequest computation.
//----------------------------------------------------------------------------//

Optional<ArrayRef<Requirement>> RequirementSignatureRequest::getCachedResult() const {
  auto proto = std::get<0>(getStorage());
  if (proto->isRequirementSignatureComputed())
    return proto->getCachedRequirementSignature();

  return None;
}

void RequirementSignatureRequest::cacheResult(ArrayRef<Requirement> value) const {
  auto proto = std::get<0>(getStorage());
  proto->setRequirementSignature(value);
}

//----------------------------------------------------------------------------//
// Requirement computation.
//----------------------------------------------------------------------------//

WhereClauseOwner::WhereClauseOwner(GenericContext *genCtx): dc(genCtx) {
  if (const auto whereClause = genCtx->getTrailingWhereClause())
    source = whereClause;
  else
    source = genCtx->getGenericParams();
}

WhereClauseOwner::WhereClauseOwner(AssociatedTypeDecl *atd)
    : dc(atd->getInnermostDeclContext()),
      source(atd->getTrailingWhereClause()) {}

SourceLoc WhereClauseOwner::getLoc() const {
  if (auto where = source.dyn_cast<TrailingWhereClause *>())
    return where->getWhereLoc();

  if (auto attr = source.dyn_cast<SpecializeAttr *>())
    return attr->getLocation();

  return source.get<GenericParamList *>()->getWhereLoc();
}

void swift::simple_display(llvm::raw_ostream &out,
                           const WhereClauseOwner &owner) {
  if (owner.source.is<TrailingWhereClause *>()) {
    simple_display(out, owner.dc->getAsDecl());
  } else if (owner.source.is<SpecializeAttr *>()) {
    out << "@_specialize";
  } else {
    out << "(SIL generic parameter list)";
  }
}

SourceLoc RequirementRequest::getNearestLoc() const {
  auto owner = std::get<0>(getStorage());
  return owner.getLoc();
}

void RequirementRequest::noteCycleStep(DiagnosticEngine &diags) const {
  // For now, the GSB does a better job of describing the exact structure of
  // the cycle.
  //
  // FIXME: We should consider merging the circularity handling the GSB does
  // into this request.  See rdar://55263708
}

MutableArrayRef<RequirementRepr> WhereClauseOwner::getRequirements() const {
  if (const auto genericParams = source.dyn_cast<GenericParamList *>()) {
    return genericParams->getRequirements();
  } else if (const auto attr = source.dyn_cast<SpecializeAttr *>()) {
    if (auto whereClause = attr->getTrailingWhereClause())
      return whereClause->getRequirements();
  } else if (const auto attr = source.dyn_cast<DifferentiableAttr *>()) {
    if (auto whereClause = attr->getWhereClause())
      return whereClause->getRequirements();
  } else if (const auto whereClause = source.get<TrailingWhereClause *>()) {
    return whereClause->getRequirements();
  }

  return { };
}

bool WhereClauseOwner::visitRequirements(
    TypeResolutionStage stage,
    llvm::function_ref<bool(Requirement, RequirementRepr *)> callback)
    const && {
  auto &evaluator = dc->getASTContext().evaluator;
  auto requirements = getRequirements();
  for (unsigned index : indices(requirements)) {
    // Resolve to a requirement.
    auto req = evaluator(RequirementRequest{*this, index, stage});
    if (req) {
      // Invoke the callback. If it returns true, we're done.
      if (callback(*req, &requirements[index]))
        return true;

      continue;
    }

    llvm::handleAllErrors(
        req.takeError(), [](const CyclicalRequestError<RequirementRequest> &E) {
          // cycle detected
        });
  }

  return false;
}

RequirementRepr &RequirementRequest::getRequirement() const {
  auto owner = std::get<0>(getStorage());
  auto index = std::get<1>(getStorage());
  return owner.getRequirements()[index];
}

bool RequirementRequest::isCached() const {
  return std::get<2>(getStorage()) == TypeResolutionStage::Interface;
}

Optional<Requirement> RequirementRequest::getCachedResult() const {
  auto &reqRepr = getRequirement();
  switch (reqRepr.getKind()) {
  case RequirementReprKind::TypeConstraint:
    if (!reqRepr.getSubjectLoc().wasValidated() ||
        !reqRepr.getConstraintLoc().wasValidated())
      return None;

    return Requirement(reqRepr.getConstraint()->getClassOrBoundGenericClass()
                         ? RequirementKind::Superclass
                         : RequirementKind::Conformance,
                       reqRepr.getSubject(),
                       reqRepr.getConstraint());

  case RequirementReprKind::SameType:
    if (!reqRepr.getFirstTypeLoc().wasValidated() ||
        !reqRepr.getSecondTypeLoc().wasValidated())
      return None;

    return Requirement(RequirementKind::SameType, reqRepr.getFirstType(),
                       reqRepr.getSecondType());

  case RequirementReprKind::LayoutConstraint:
    if (!reqRepr.getSubjectLoc().wasValidated())
      return None;

    return Requirement(RequirementKind::Layout, reqRepr.getSubject(),
                       reqRepr.getLayoutConstraint());
  }
  llvm_unreachable("unhandled kind");
}

void RequirementRequest::cacheResult(Requirement value) const {
  auto &reqRepr = getRequirement();
  switch (value.getKind()) {
  case RequirementKind::Conformance:
  case RequirementKind::Superclass:
    reqRepr.getSubjectLoc().setType(value.getFirstType());
    reqRepr.getConstraintLoc().setType(value.getSecondType());
    break;

  case RequirementKind::SameType:
    reqRepr.getFirstTypeLoc().setType(value.getFirstType());
    reqRepr.getSecondTypeLoc().setType(value.getSecondType());
    break;

  case RequirementKind::Layout:
    reqRepr.getSubjectLoc().setType(value.getFirstType());
    reqRepr.getLayoutConstraintLoc()
      .setLayoutConstraint(value.getLayoutConstraint());
    break;
  }
}

//----------------------------------------------------------------------------//
// DefaultTypeRequest.
//----------------------------------------------------------------------------//

void swift::simple_display(llvm::raw_ostream &out,
                           const KnownProtocolKind kind) {
  out << getProtocolName(kind);
}

//----------------------------------------------------------------------------//
// DefaultTypeRequest caching.
//----------------------------------------------------------------------------//

Optional<Type> DefaultTypeRequest::getCachedResult() const {
  auto *DC = std::get<1>(getStorage());
  auto knownProtocolKind = std::get<0>(getStorage());
  const auto &cachedType = DC->getASTContext().getDefaultTypeRequestCache(
      DC->getParentSourceFile(), knownProtocolKind);
  return cachedType ? Optional<Type>(cachedType) : None;
}

void DefaultTypeRequest::cacheResult(Type value) const {
  auto *DC = std::get<1>(getStorage());
  auto knownProtocolKind = std::get<0>(getStorage());
  auto &cacheEntry = DC->getASTContext().getDefaultTypeRequestCache(
                         DC->getParentSourceFile(), knownProtocolKind);
  cacheEntry = value;
}

bool PropertyWrapperTypeInfoRequest::isCached() const {
  auto nominal = std::get<0>(getStorage());
  return nominal->getAttrs().hasAttribute<PropertyWrapperAttr>();;
}

bool AttachedPropertyWrappersRequest::isCached() const {
  auto var = std::get<0>(getStorage());
  return !var->getAttrs().isEmpty();
}

bool AttachedPropertyWrapperTypeRequest::isCached() const {
  auto var = std::get<0>(getStorage());
  return !var->getAttrs().isEmpty();
}

bool PropertyWrapperBackingPropertyTypeRequest::isCached() const {
  auto var = std::get<0>(getStorage());
  return !var->getAttrs().isEmpty();
}

bool PropertyWrapperBackingPropertyInfoRequest::isCached() const {
  auto var = std::get<0>(getStorage());
  return !var->getAttrs().isEmpty();
}

bool PropertyWrapperMutabilityRequest::isCached() const {
  auto var = std::get<0>(getStorage());
  return !var->getAttrs().isEmpty();
}

bool PropertyWrapperLValuenessRequest::isCached() const {
  auto var = std::get<0>(getStorage());
  return !var->getAttrs().isEmpty();
}

void swift::simple_display(
    llvm::raw_ostream &out, const PropertyWrapperTypeInfo &propertyWrapper) {
  out << "{ ";
  if (propertyWrapper.valueVar)
    out << propertyWrapper.valueVar->printRef();
  else
    out << "null";
  out << " }";
}

void swift::simple_display(
    llvm::raw_ostream &out,
    const PropertyWrapperBackingPropertyInfo &backingInfo) {
  out << "{ ";
  if (backingInfo.backingVar)
    backingInfo.backingVar->dumpRef(out);
  out << " }";
}

void swift::simple_display(
  llvm::raw_ostream &out, const CtorInitializerKind initKind) {
  out << "{ ";
  switch (initKind) {
  case CtorInitializerKind::Designated:
    out << "designated"; break;
  case CtorInitializerKind::Convenience:
    out << "convenience"; break;
  case CtorInitializerKind::ConvenienceFactory:
    out << "convenience_factory"; break;
  case CtorInitializerKind::Factory:
    out << "factory"; break;
  }
  out << " }";
}

void swift::simple_display(llvm::raw_ostream &os, PropertyWrapperMutability m) {
  static const char *names[] =
    {"is nonmutating", "is mutating", "doesn't exist"};
  
  os << "getter " << names[m.Getter] << ", setter " << names[m.Setter];
}

void swift::simple_display(llvm::raw_ostream &out, PropertyWrapperLValueness l) {
  out << "is lvalue for get: {";
  simple_display(out, l.isLValueForGetAccess);
  out << "}, is lvalue for set: {";
  simple_display(out, l.isLValueForSetAccess);
  out << "}";
}

void swift::simple_display(llvm::raw_ostream &out,
                           ResilienceExpansion value) {
  switch (value) {
  case ResilienceExpansion::Minimal:
    out << "minimal";
    break;
  case ResilienceExpansion::Maximal:
    out << "maximal";
    break;
  }
}

void swift::simple_display(llvm::raw_ostream &out,
                           FragileFunctionKind value) {
  switch (value.kind) {
  case FragileFunctionKind::Transparent:
    out << "transparent";
    break;
  case FragileFunctionKind::Inlinable:
    out << "inlinable";
    break;
  case FragileFunctionKind::AlwaysEmitIntoClient:
    out << "alwaysEmitIntoClient";
    break;
  case FragileFunctionKind::DefaultArgument:
    out << "defaultArgument";
    break;
  case FragileFunctionKind::PropertyInitializer:
    out << "propertyInitializer";
    break;
  case FragileFunctionKind::None:
    out << "none";
    break;
  }

  out << ", allowUsableFromInline: "
      << (value.allowUsableFromInline ? "true" : "false");
}

//----------------------------------------------------------------------------//
// FunctionBuilder-related requests.
//----------------------------------------------------------------------------//

bool AttachedFunctionBuilderRequest::isCached() const {
  // Only needs to be cached if there are any custom attributes.
  auto var = std::get<0>(getStorage());
  return var->getAttrs().hasAttribute<CustomAttr>();
}

//----------------------------------------------------------------------------//
// SelfAccessKindRequest computation.
//----------------------------------------------------------------------------//

Optional<SelfAccessKind> SelfAccessKindRequest::getCachedResult() const {
  auto *funcDecl = std::get<0>(getStorage());
  return funcDecl->getCachedSelfAccessKind();
}

void SelfAccessKindRequest::cacheResult(SelfAccessKind value) const {
  auto *funcDecl = std::get<0>(getStorage());
  funcDecl->setSelfAccessKind(value);
}

//----------------------------------------------------------------------------//
// IsGetterMutatingRequest computation.
//----------------------------------------------------------------------------//

Optional<bool> IsGetterMutatingRequest::getCachedResult() const {
  auto *storage = std::get<0>(getStorage());
  if (storage->LazySemanticInfo.IsGetterMutatingComputed)
    return storage->LazySemanticInfo.IsGetterMutating;
  return None;
}

void IsGetterMutatingRequest::cacheResult(bool value) const {
  auto *storage = std::get<0>(getStorage());
  storage->setIsGetterMutating(value);
}

//----------------------------------------------------------------------------//
// IsSetterMutatingRequest computation.
//----------------------------------------------------------------------------//

Optional<bool> IsSetterMutatingRequest::getCachedResult() const {
  auto *storage = std::get<0>(getStorage());
  if (storage->LazySemanticInfo.IsSetterMutatingComputed)
    return storage->LazySemanticInfo.IsSetterMutating;
  return None;
}

void IsSetterMutatingRequest::cacheResult(bool value) const {
  auto *storage = std::get<0>(getStorage());
  storage->setIsSetterMutating(value);
}

//----------------------------------------------------------------------------//
// OpaqueReadOwnershipRequest computation.
//----------------------------------------------------------------------------//

Optional<OpaqueReadOwnership>
OpaqueReadOwnershipRequest::getCachedResult() const {
  auto *storage = std::get<0>(getStorage());
  if (storage->LazySemanticInfo.OpaqueReadOwnershipComputed)
    return OpaqueReadOwnership(storage->LazySemanticInfo.OpaqueReadOwnership);
  return None;
}

void OpaqueReadOwnershipRequest::cacheResult(OpaqueReadOwnership value) const {
  auto *storage = std::get<0>(getStorage());
  storage->setOpaqueReadOwnership(value);
}

//----------------------------------------------------------------------------//
// StorageImplInfoRequest computation.
//----------------------------------------------------------------------------//

Optional<StorageImplInfo>
StorageImplInfoRequest::getCachedResult() const {
  auto *storage = std::get<0>(getStorage());
  if (storage->LazySemanticInfo.ImplInfoComputed)
    return storage->ImplInfo;
  return None;
}

void StorageImplInfoRequest::cacheResult(StorageImplInfo value) const {
  auto *storage = std::get<0>(getStorage());
  storage->setImplInfo(value);
}

//----------------------------------------------------------------------------//
// RequiresOpaqueAccessorsRequest computation.
//----------------------------------------------------------------------------//

Optional<bool>
RequiresOpaqueAccessorsRequest::getCachedResult() const {
  auto *storage = std::get<0>(getStorage());
  if (storage->LazySemanticInfo.RequiresOpaqueAccessorsComputed)
    return storage->LazySemanticInfo.RequiresOpaqueAccessors;
  return None;
}

void RequiresOpaqueAccessorsRequest::cacheResult(bool value) const {
  auto *storage = std::get<0>(getStorage());
  storage->LazySemanticInfo.RequiresOpaqueAccessorsComputed = 1;
  storage->LazySemanticInfo.RequiresOpaqueAccessors = value;
}

//----------------------------------------------------------------------------//
// RequiresOpaqueModifyCoroutineRequest computation.
//----------------------------------------------------------------------------//

Optional<bool>
RequiresOpaqueModifyCoroutineRequest::getCachedResult() const {
  auto *storage = std::get<0>(getStorage());
  if (storage->LazySemanticInfo.RequiresOpaqueModifyCoroutineComputed)
    return storage->LazySemanticInfo.RequiresOpaqueModifyCoroutine;
  return None;
}

void RequiresOpaqueModifyCoroutineRequest::cacheResult(bool value) const {
  auto *storage = std::get<0>(getStorage());
  storage->LazySemanticInfo.RequiresOpaqueModifyCoroutineComputed = 1;
  storage->LazySemanticInfo.RequiresOpaqueModifyCoroutine = value;
}

//----------------------------------------------------------------------------//
// IsAccessorTransparentRequest computation.
//----------------------------------------------------------------------------//

Optional<bool>
IsAccessorTransparentRequest::getCachedResult() const {
  auto *accessor = std::get<0>(getStorage());
  return accessor->getCachedIsTransparent();
}

void IsAccessorTransparentRequest::cacheResult(bool value) const {
  auto *accessor = std::get<0>(getStorage());
  accessor->setIsTransparent(value);

  // For interface printing, API diff, etc.
  if (value) {
    auto &attrs = accessor->getAttrs();
    if (!attrs.hasAttribute<TransparentAttr>()) {
      auto &ctx = accessor->getASTContext();
      attrs.add(new (ctx) TransparentAttr(/*IsImplicit=*/true));
    }
  }
}

//----------------------------------------------------------------------------//
// SynthesizeAccessorRequest computation.
//----------------------------------------------------------------------------//

Optional<AccessorDecl *>
SynthesizeAccessorRequest::getCachedResult() const {
  auto *storage = std::get<0>(getStorage());
  auto kind = std::get<1>(getStorage());
  auto *accessor = storage->getAccessor(kind);
  if (accessor)
    return accessor;
  return None;
}

void SynthesizeAccessorRequest::cacheResult(AccessorDecl *accessor) const {
  auto *storage = std::get<0>(getStorage());
  auto kind = std::get<1>(getStorage());

  storage->setSynthesizedAccessor(kind, accessor);
}

//----------------------------------------------------------------------------//
// EmittedMembersRequest computation.
//----------------------------------------------------------------------------//

Optional<DeclRange>
EmittedMembersRequest::getCachedResult() const {
  auto *classDecl = std::get<0>(getStorage());
  if (classDecl->hasForcedEmittedMembers())
    return classDecl->getMembers();
  return None;
}

void EmittedMembersRequest::cacheResult(DeclRange result) const {
  auto *classDecl = std::get<0>(getStorage());
  classDecl->setHasForcedEmittedMembers();
}

//----------------------------------------------------------------------------//
// IsImplicitlyUnwrappedOptionalRequest computation.
//----------------------------------------------------------------------------//

Optional<bool>
IsImplicitlyUnwrappedOptionalRequest::getCachedResult() const {
  auto *decl = std::get<0>(getStorage());
  if (decl->LazySemanticInfo.isIUOComputed)
    return decl->LazySemanticInfo.isIUO;
  return None;
}

void IsImplicitlyUnwrappedOptionalRequest::cacheResult(bool value) const {
  auto *decl = std::get<0>(getStorage());
  decl->setImplicitlyUnwrappedOptional(value);
}

//----------------------------------------------------------------------------//
// GenericSignatureRequest computation.
//----------------------------------------------------------------------------//

Optional<GenericSignature> GenericSignatureRequest::getCachedResult() const {
  auto *GC = std::get<0>(getStorage());
  if (GC->GenericSigAndBit.getInt()) {
    return GC->GenericSigAndBit.getPointer();
  }
  return None;
}

void GenericSignatureRequest::cacheResult(GenericSignature value) const {
  auto *GC = std::get<0>(getStorage());
  GC->GenericSigAndBit.setPointerAndInt(value, true);
}

//----------------------------------------------------------------------------//
// InferredGenericSignatureRequest computation.
//----------------------------------------------------------------------------//

void InferredGenericSignatureRequest::noteCycleStep(DiagnosticEngine &d) const {
  // For now, the GSB does a better job of describing the exact structure of
  // the cycle.
  //
  // FIXME: We should consider merging the circularity handling the GSB does
  // into this request.  See rdar://55263708
}

//----------------------------------------------------------------------------//
// UnderlyingTypeRequest computation.
//----------------------------------------------------------------------------//

Optional<Type>
UnderlyingTypeRequest::getCachedResult() const {
  auto *typeAlias = std::get<0>(getStorage());
  if (auto type = typeAlias->UnderlyingTy.getType())
    return type;
  return None;
}

void UnderlyingTypeRequest::cacheResult(Type value) const {
  auto *typeAlias = std::get<0>(getStorage());
  typeAlias->UnderlyingTy.setType(value);
}

void UnderlyingTypeRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  auto aliasDecl = std::get<0>(getStorage());
  diags.diagnose(aliasDecl, diag::recursive_decl_reference,
                 aliasDecl->getDescriptiveKind(),
                 aliasDecl->getName());
}

//----------------------------------------------------------------------------//
// EnumRawValuesRequest computation.
//----------------------------------------------------------------------------//

bool EnumRawValuesRequest::isCached() const {
  return std::get<1>(getStorage()) == TypeResolutionStage::Interface;
}

Optional<evaluator::SideEffect> EnumRawValuesRequest::getCachedResult() const {
  auto *ED = std::get<0>(getStorage());
  if (ED->LazySemanticInfo.hasCheckedRawValues())
    return std::make_tuple<>();
  return None;
}

void EnumRawValuesRequest::cacheResult(evaluator::SideEffect) const {
  auto *ED = std::get<0>(getStorage());
  auto flags = ED->LazySemanticInfo.RawTypeAndFlags.getInt() |
      EnumDecl::HasFixedRawValues |
      EnumDecl::HasFixedRawValuesAndTypes;
  ED->LazySemanticInfo.RawTypeAndFlags.setInt(flags);
}

void EnumRawValuesRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  // This request computes the raw type, and so participates in cycles involving
  // it. For now, the raw type provides a rich enough circularity diagnostic
  // that we can silence ourselves.
}

void EnumRawValuesRequest::noteCycleStep(DiagnosticEngine &diags) const {
  
}

//----------------------------------------------------------------------------//
// IsStaticRequest computation.
//----------------------------------------------------------------------------//

Optional<bool> IsStaticRequest::getCachedResult() const {
  auto *FD = std::get<0>(getStorage());
  return FD->getCachedIsStatic();
}

void IsStaticRequest::cacheResult(bool result) const {
  auto *FD = std::get<0>(getStorage());
  FD->setStatic(result);
}

//----------------------------------------------------------------------------//
// NeedsNewVTableEntryRequest computation.
//----------------------------------------------------------------------------//

Optional<bool> NeedsNewVTableEntryRequest::getCachedResult() const {
  auto *decl = std::get<0>(getStorage());
  if (decl->LazySemanticInfo.NeedsNewVTableEntryComputed)
    return decl->LazySemanticInfo.NeedsNewVTableEntry;
  return None;
}

void NeedsNewVTableEntryRequest::cacheResult(bool value) const {
  auto *decl = std::get<0>(getStorage());
  decl->LazySemanticInfo.NeedsNewVTableEntryComputed = true;
  decl->LazySemanticInfo.NeedsNewVTableEntry = value;
}

//----------------------------------------------------------------------------//
// ParamSpecifierRequest computation.
//----------------------------------------------------------------------------//

Optional<ParamSpecifier> ParamSpecifierRequest::getCachedResult() const {
  auto *decl = std::get<0>(getStorage());
  return decl->getCachedSpecifier();
}

void ParamSpecifierRequest::cacheResult(ParamSpecifier specifier) const {
  auto *decl = std::get<0>(getStorage());
  decl->setSpecifier(specifier);
}

//----------------------------------------------------------------------------//
// ResultTypeRequest computation.
//----------------------------------------------------------------------------//

TypeLoc &ResultTypeRequest::getResultTypeLoc() const {
  auto *decl = std::get<0>(getStorage());
  if (auto *funcDecl = dyn_cast<FuncDecl>(decl))
    return funcDecl->getBodyResultTypeLoc();
  auto *subscriptDecl = cast<SubscriptDecl>(decl);
  return subscriptDecl->getElementTypeLoc();
}

Optional<Type> ResultTypeRequest::getCachedResult() const {
  if (auto type = getResultTypeLoc().getType())
    return type;

  return None;
}

void ResultTypeRequest::cacheResult(Type type) const {
  getResultTypeLoc().setType(type);
}

//----------------------------------------------------------------------------//
// PatternBindingEntryRequest computation.
//----------------------------------------------------------------------------//

Optional<const PatternBindingEntry *>
PatternBindingEntryRequest::getCachedResult() const {
  auto *PBD = std::get<0>(getStorage());
  auto idx = std::get<1>(getStorage());
  if (!PBD->getPatternList()[idx].isFullyValidated()) {
    return None;
  }
  return &PBD->getPatternList()[idx];
}

void PatternBindingEntryRequest::cacheResult(
    const PatternBindingEntry *value) const {
  auto *PBD = std::get<0>(getStorage());
  auto idx = std::get<1>(getStorage());
  PBD->getMutablePatternList()[idx].setFullyValidated();
}

//----------------------------------------------------------------------------//
// NamingPatternRequest computation.
//----------------------------------------------------------------------------//

Optional<NamedPattern *> NamingPatternRequest::getCachedResult() const {
  auto *VD = std::get<0>(getStorage());
  if (auto *Pat = VD->NamingPattern) {
    return Pat;
  }
  return None;
}

void NamingPatternRequest::cacheResult(NamedPattern *value) const {
  auto *VD = std::get<0>(getStorage());
  VD->NamingPattern = value;
}

//----------------------------------------------------------------------------//
// InterfaceTypeRequest computation.
//----------------------------------------------------------------------------//

Optional<Type> InterfaceTypeRequest::getCachedResult() const {
  auto *decl = std::get<0>(getStorage());
  if (auto Ty = decl->TypeAndAccess.getPointer()) {
    return Ty;
  }
  return None;
}

void InterfaceTypeRequest::cacheResult(Type type) const {
  auto *decl = std::get<0>(getStorage());
  if (type) {
    assert(!type->hasTypeVariable() && "Type variable in interface type");
    assert(!type->is<InOutType>() && "Interface type must be materializable");
    assert(!type->hasArchetype() && "Archetype in interface type");
  }
  decl->TypeAndAccess.setPointer(type);
}

//----------------------------------------------------------------------------//
// ValidatePrecedenceGroupRequest computation.
//----------------------------------------------------------------------------//

SourceLoc ValidatePrecedenceGroupRequest::getNearestLoc() const {
  auto &desc = std::get<0>(getStorage());
  return desc.getLoc();
}

void ValidatePrecedenceGroupRequest::diagnoseCycle(
    DiagnosticEngine &diags) const {
  auto &desc = std::get<0>(getStorage());
  if (auto pathDir = desc.pathDirection) {
    diags.diagnose(desc.nameLoc, diag::precedence_group_cycle, (bool)*pathDir);
  } else {
    diags.diagnose(desc.nameLoc, diag::circular_reference);
  }
}

void ValidatePrecedenceGroupRequest::noteCycleStep(
    DiagnosticEngine &diag) const {
  auto &desc = std::get<0>(getStorage());
  diag.diagnose(desc.nameLoc,
                 diag::circular_reference_through_precedence_group, desc.ident);
}

SourceLoc PrecedenceGroupDescriptor::getLoc() const {
  return nameLoc;
}

void swift::simple_display(llvm::raw_ostream &out,
                           const PrecedenceGroupDescriptor &desc) {
  out << "precedence group " << desc.ident << " at ";
  desc.nameLoc.print(out, desc.dc->getASTContext().SourceMgr);
}

//----------------------------------------------------------------------------//
// InheritsSuperclassInitializersRequest computation.
//----------------------------------------------------------------------------//

Optional<bool> InheritsSuperclassInitializersRequest::getCachedResult() const {
  auto *decl = std::get<0>(getStorage());
  return decl->getCachedInheritsSuperclassInitializers();
}

void InheritsSuperclassInitializersRequest::cacheResult(bool value) const {
  auto *decl = std::get<0>(getStorage());
  decl->setInheritsSuperclassInitializers(value);
}

//----------------------------------------------------------------------------//
// ResolveImplicitMemberRequest computation.
//----------------------------------------------------------------------------//

void swift::simple_display(llvm::raw_ostream &out,
                           ImplicitMemberAction action) {
  switch (action) {
  case ImplicitMemberAction::ResolveImplicitInit:
    out << "resolve implicit initializer";
    break;
  case ImplicitMemberAction::ResolveCodingKeys:
    out << "resolve CodingKeys";
    break;
  case ImplicitMemberAction::ResolveEncodable:
    out << "resolve Encodable.encode(to:)";
    break;
  case ImplicitMemberAction::ResolveDecodable:
    out << "resolve Decodable.init(from:)";
    break;
  }
}

//----------------------------------------------------------------------------//
// TypeWitnessRequest computation.
//----------------------------------------------------------------------------//

Optional<TypeWitnessAndDecl> TypeWitnessRequest::getCachedResult() const {
  auto *conformance = std::get<0>(getStorage());
  auto *requirement = std::get<1>(getStorage());
  if (conformance->TypeWitnesses.count(requirement) == 0) {
    return None;
  }
  return conformance->TypeWitnesses[requirement];
}

void TypeWitnessRequest::cacheResult(TypeWitnessAndDecl typeWitAndDecl) const {
  // FIXME: Refactor this to be the thing that warms the cache.
}

//----------------------------------------------------------------------------//
// WitnessRequest computation.
//----------------------------------------------------------------------------//

Optional<Witness> ValueWitnessRequest::getCachedResult() const {
  auto *conformance = std::get<0>(getStorage());
  auto *requirement = std::get<1>(getStorage());
  if (conformance->Mapping.count(requirement) == 0) {
    return None;
  }
  return conformance->Mapping[requirement];
}

void ValueWitnessRequest::cacheResult(Witness type) const {
  // FIXME: Refactor this to be the thing that warms the cache.
}

//----------------------------------------------------------------------------//
// PreCheckFunctionBuilderRequest computation.
//----------------------------------------------------------------------------//

void swift::simple_display(llvm::raw_ostream &out,
                           FunctionBuilderBodyPreCheck value) {
  switch (value) {
  case FunctionBuilderBodyPreCheck::Okay:
    out << "okay";
    break;
  case FunctionBuilderBodyPreCheck::HasReturnStmt:
    out << "has return statement";
    break;
  case FunctionBuilderBodyPreCheck::Error:
    out << "error";
    break;
  }
}

//----------------------------------------------------------------------------//
// HasCircularInheritanceRequest computation.
//----------------------------------------------------------------------------//

void HasCircularInheritanceRequest::diagnoseCycle(
    DiagnosticEngine &diags) const {
  auto *decl = std::get<0>(getStorage());
  diags.diagnose(decl, diag::circular_class_inheritance, decl->getName());
}

void HasCircularInheritanceRequest::noteCycleStep(
    DiagnosticEngine &diags) const {
  auto *decl = std::get<0>(getStorage());
  diags.diagnose(decl, diag::kind_declname_declared_here,
                 decl->getDescriptiveKind(), decl->getName());
}

//----------------------------------------------------------------------------//
// HasCircularInheritedProtocolsRequest computation.
//----------------------------------------------------------------------------//

void HasCircularInheritedProtocolsRequest::diagnoseCycle(
    DiagnosticEngine &diags) const {
  auto *decl = std::get<0>(getStorage());
  diags.diagnose(decl, diag::circular_protocol_def, decl->getName());
}

void HasCircularInheritedProtocolsRequest::noteCycleStep(
    DiagnosticEngine &diags) const {
  auto *decl = std::get<0>(getStorage());
  diags.diagnose(decl, diag::kind_declname_declared_here,
                 decl->getDescriptiveKind(), decl->getName());
}

//----------------------------------------------------------------------------//
// HasCircularRawValueRequest computation.
//----------------------------------------------------------------------------//

void HasCircularRawValueRequest::diagnoseCycle(DiagnosticEngine &diags) const {
  auto *decl = std::get<0>(getStorage());
  diags.diagnose(decl, diag::circular_enum_inheritance, decl->getName());
}

void HasCircularRawValueRequest::noteCycleStep(DiagnosticEngine &diags) const {
  auto *decl = std::get<0>(getStorage());
  diags.diagnose(decl, diag::kind_declname_declared_here,
                 decl->getDescriptiveKind(), decl->getName());
}

//----------------------------------------------------------------------------//
// DefaultArgumentInitContextRequest computation.
//----------------------------------------------------------------------------//

Optional<Initializer *>
DefaultArgumentInitContextRequest::getCachedResult() const {
  auto *param = std::get<0>(getStorage());
  return param->getCachedDefaultArgumentInitContext();
}

void DefaultArgumentInitContextRequest::cacheResult(Initializer *init) const {
  auto *param = std::get<0>(getStorage());
  param->setDefaultArgumentInitContext(init);
}

//----------------------------------------------------------------------------//
// DefaultArgumentExprRequest computation.
//----------------------------------------------------------------------------//

Optional<Expr *> DefaultArgumentExprRequest::getCachedResult() const {
  auto *param = std::get<0>(getStorage());
  auto *defaultInfo = param->DefaultValueAndFlags.getPointer();
  if (!defaultInfo)
    return None;

  if (!defaultInfo->InitContextAndIsTypeChecked.getInt())
    return None;

  return defaultInfo->DefaultArg.get<Expr *>();
}

void DefaultArgumentExprRequest::cacheResult(Expr *expr) const {
  auto *param = std::get<0>(getStorage());
  param->setDefaultExpr(expr, /*isTypeChecked*/ true);
}

//----------------------------------------------------------------------------//
// CallerSideDefaultArgExprRequest computation.
//----------------------------------------------------------------------------//

Optional<Expr *> CallerSideDefaultArgExprRequest::getCachedResult() const {
  auto *defaultExpr = std::get<0>(getStorage());
  auto storage = defaultExpr->ContextOrCallerSideExpr;
  assert(!storage.isNull());

  if (auto *expr = storage.dyn_cast<Expr *>())
    return expr;

  return None;
}

void CallerSideDefaultArgExprRequest::cacheResult(Expr *expr) const {
  auto *defaultExpr = std::get<0>(getStorage());
  defaultExpr->ContextOrCallerSideExpr = expr;
}

//----------------------------------------------------------------------------//
// DifferentiableAttributeTypeCheckRequest computation.
//----------------------------------------------------------------------------//

Optional<IndexSubset *>
DifferentiableAttributeTypeCheckRequest::getCachedResult() const {
  auto *attr = std::get<0>(getStorage());
  if (attr->hasBeenTypeChecked())
    return attr->ParameterIndicesAndBit.getPointer();
  return None;
}

void DifferentiableAttributeTypeCheckRequest::cacheResult(
    IndexSubset *parameterIndices) const {
  auto *attr = std::get<0>(getStorage());
  attr->ParameterIndicesAndBit.setPointerAndInt(parameterIndices, true);
}

//----------------------------------------------------------------------------//
// CheckRedeclarationRequest computation.
//----------------------------------------------------------------------------//

Optional<evaluator::SideEffect>
CheckRedeclarationRequest::getCachedResult() const {
  if (!std::get<0>(getStorage())->alreadyCheckedRedeclaration())
    return None;
  return std::make_tuple<>();
}

void CheckRedeclarationRequest::cacheResult(evaluator::SideEffect) const {
  std::get<0>(getStorage())->setCheckedRedeclaration();
}

evaluator::DependencySource CheckRedeclarationRequest::readDependencySource(
    const evaluator::DependencyCollector &eval) const {
  auto *current = std::get<0>(getStorage());
  auto *currentDC = current->getDeclContext();
  return {
    currentDC->getParentSourceFile(),
    evaluator::getScopeForAccessLevel(current->getFormalAccess())
  };
}

void CheckRedeclarationRequest::writeDependencySink(
    evaluator::DependencyCollector &tracker, evaluator::SideEffect) const {
  auto *current = std::get<0>(getStorage());
  if (!current->hasName())
    return;

  DeclContext *currentDC = current->getDeclContext();
  SourceFile *currentFile = currentDC->getParentSourceFile();
  if (!currentFile || currentDC->isLocalContext())
    return;

  if (currentDC->isTypeContext()) {
    if (auto nominal = currentDC->getSelfNominalTypeDecl()) {
      tracker.addUsedMember(nominal, current->getBaseName());
    }
  } else {
    tracker.addTopLevelName(current->getBaseName());
  }
}

//----------------------------------------------------------------------------//
// LookupAllConformancesInContextRequest computation.
//----------------------------------------------------------------------------//

evaluator::DependencySource
LookupAllConformancesInContextRequest::readDependencySource(
    const evaluator::DependencyCollector &collector) const {
  auto *dc = std::get<0>(getStorage());
  AccessLevel defaultAccess;
  if (auto ext = dyn_cast<ExtensionDecl>(dc)) {
    const NominalTypeDecl *nominal = ext->getExtendedNominal();
    if (!nominal) {
      return {collector.getActiveDependencySourceOrNull(),
              evaluator::DependencyScope::Cascading};
    }
    defaultAccess = nominal->getFormalAccess();
  } else {
    defaultAccess = cast<NominalTypeDecl>(dc)->getFormalAccess();
  }
  return {collector.getActiveDependencySourceOrNull(),
          evaluator::getScopeForAccessLevel(defaultAccess)};
}

void LookupAllConformancesInContextRequest::writeDependencySink(
    evaluator::DependencyCollector &tracker,
    ProtocolConformanceLookupResult conformances) const {
  for (auto conformance : conformances) {
    tracker.addPotentialMember(conformance->getProtocol());
  }
}

//----------------------------------------------------------------------------//
// ResolveTypeEraserTypeRequest computation.
//----------------------------------------------------------------------------//

Optional<Type> ResolveTypeEraserTypeRequest::getCachedResult() const {
  auto ty = std::get<1>(getStorage())->TypeEraserLoc.getType();
  if (ty.isNull()) {
    return None;
  }
  return ty;
}

void ResolveTypeEraserTypeRequest::cacheResult(Type value) const {
  assert(value && "Resolved type erasure type to null type!");
  std::get<1>(getStorage())->TypeEraserLoc.setType(value);
}

//----------------------------------------------------------------------------//
// TypeCheckSourceFileRequest computation.
//----------------------------------------------------------------------------//

evaluator::DependencySource TypeCheckSourceFileRequest::readDependencySource(
    const evaluator::DependencyCollector &e) const {
  return {std::get<0>(getStorage()), evaluator::DependencyScope::Cascading};
}

Optional<evaluator::SideEffect>
TypeCheckSourceFileRequest::getCachedResult() const {
  auto *SF = std::get<0>(getStorage());
  if (SF->ASTStage == SourceFile::TypeChecked)
    return std::make_tuple<>();

  return None;
}

void TypeCheckSourceFileRequest::cacheResult(evaluator::SideEffect) const {
  auto *SF = std::get<0>(getStorage());

  // Verify that we've checked types correctly.
  SF->ASTStage = SourceFile::TypeChecked;

  {
    auto &Ctx = SF->getASTContext();
    FrontendStatsTracer tracer(Ctx.Stats, "AST verification");
    // Verify the SourceFile.
    swift::verify(*SF);

    // Verify imported modules.
    //
    // Skip per-file verification in whole-module mode. Verifying imports
    // between files could cause the importer to cache declarations without
    // adding them to the ASTContext. This happens when the importer registers a
    // declaration without a valid TypeChecker instance, as is the case during
    // verification. A subsequent file may require that declaration to be fully
    // imported (e.g. to synthesized a function body), but since it has already
    // been cached, it will never be added to the ASTContext. The solution is to
    // skip verification and avoid caching it.
#ifndef NDEBUG
    if (!Ctx.TypeCheckerOpts.DelayWholeModuleChecking &&
        SF->Kind != SourceFileKind::REPL &&
        SF->Kind != SourceFileKind::SIL &&
        !Ctx.LangOpts.DebuggerSupport) {
      Ctx.verifyAllLoadedModules();
    }
#endif
  }
}

//----------------------------------------------------------------------------//
// TypeCheckFunctionBodyUntilRequest computation.
//----------------------------------------------------------------------------//

evaluator::DependencySource
TypeCheckFunctionBodyUntilRequest::readDependencySource(
    const evaluator::DependencyCollector &e) const {
  // We're going under a function body scope, unconditionally flip the scope
  // to private.
  return {
    std::get<0>(getStorage())->getParentSourceFile(),
    evaluator::DependencyScope::Private
  };
}

//----------------------------------------------------------------------------//
// ModuleImplicitImportsRequest computation.
//----------------------------------------------------------------------------//

void swift::simple_display(llvm::raw_ostream &out,
                           const ImplicitImport &import) {
  out << "implicit import of ";
  simple_display(out, import.Module);
}
