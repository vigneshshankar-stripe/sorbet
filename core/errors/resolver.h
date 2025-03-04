#ifndef SORBET_CORE_ERRORS_RESOLVER_H
#define SORBET_CORE_ERRORS_RESOLVER_H
#include "core/Error.h"

namespace sorbet::core::errors::Resolver {
constexpr ErrorClass DynamicConstant{5001, StrictLevel::True};
constexpr ErrorClass StubConstant{5002, StrictLevel::False};
constexpr ErrorClass InvalidMethodSignature{5003, StrictLevel::False};
constexpr ErrorClass InvalidTypeDeclaration{5004, StrictLevel::False};
constexpr ErrorClass InvalidDeclareVariables{5005, StrictLevel::True};
constexpr ErrorClass DuplicateVariableDeclaration{5006, StrictLevel::True};
// constexpr ErrorClass UndeclaredVariable{5007, StrictLevel::Strict};
constexpr ErrorClass DynamicSuperclass{5008, StrictLevel::True};
// constexpr ErrorClass InvalidAttr{5009, StrictLevel::True};
constexpr ErrorClass InvalidCast{5010, StrictLevel::False};
constexpr ErrorClass CircularDependency{5011, StrictLevel::False};
constexpr ErrorClass RedefinitionOfParents{5012, StrictLevel::False};
constexpr ErrorClass ConstantAssertType{5013, StrictLevel::False};
constexpr ErrorClass ParentTypeNotDeclared{5014, StrictLevel::False};
constexpr ErrorClass ParentVarianceMismatch{5015, StrictLevel::False};
constexpr ErrorClass VariantTypeMemberInClass{5016, StrictLevel::False};
constexpr ErrorClass TypeMembersInWrongOrder{5017, StrictLevel::False};
constexpr ErrorClass NotATypeVariable{5018, StrictLevel::False};
constexpr ErrorClass AbstractMethodWithBody{5019, StrictLevel::False};
constexpr ErrorClass InvalidMixinDeclaration{5020, StrictLevel::False};
constexpr ErrorClass AbstractMethodOutsideAbstract{5021, StrictLevel::False};
constexpr ErrorClass ConcreteMethodInInterface{5022, StrictLevel::False};
constexpr ErrorClass BadAbstractMethod{5023, StrictLevel::False};
constexpr ErrorClass RecursiveTypeAlias{5024, StrictLevel::False};
constexpr ErrorClass TypeAliasInGenericClass{5025, StrictLevel::False};
constexpr ErrorClass BadStdlibGeneric{5026, StrictLevel::False};

// This is for type signatures that we permit at False but ban in True code
constexpr ErrorClass InvalidTypeDeclarationTyped{5027, StrictLevel::True};
constexpr ErrorClass ConstantMissingTypeAnnotation{5028, StrictLevel::Strict};
constexpr ErrorClass RecursiveClassAlias{5030, StrictLevel::False};
constexpr ErrorClass ConstantInTypeAlias{5031, StrictLevel::False};
constexpr ErrorClass IncludesNonModule{5032, StrictLevel::False};
constexpr ErrorClass OverridesFinal{5033, StrictLevel::False};
constexpr ErrorClass ReassignsTypeAlias{5034, StrictLevel::False};
constexpr ErrorClass BadMethodOverride{5035, StrictLevel::False};
constexpr ErrorClass EnumerableParentTypeNotDeclared{5036, StrictLevel::Strict};
constexpr ErrorClass BadAliasMethod{5037, StrictLevel::True};
constexpr ErrorClass SigInFileWithoutSigil{5038, StrictLevel::False};
constexpr ErrorClass RevealTypeInUntypedFile{5039, StrictLevel::False};

constexpr ErrorClass OverloadNotAllowed{5040, StrictLevel::False};
constexpr ErrorClass SubclassingNotAllowed{5041, StrictLevel::False};
constexpr ErrorClass NonPublicAbstract{5042, StrictLevel::True};
constexpr ErrorClass InvalidTypeAlias{5043, StrictLevel::False};
constexpr ErrorClass InvalidVariance{5044, StrictLevel::True};
constexpr ErrorClass GenericClassWithoutTypeArgs{5045, StrictLevel::False};
constexpr ErrorClass GenericClassWithoutTypeArgsStdlib{5046, StrictLevel::Strict};
constexpr ErrorClass FinalAncestor{5047, StrictLevel::False};
constexpr ErrorClass FinalModuleNonFinalMethod{5048, StrictLevel::False};
constexpr ErrorClass BadParameterOrdering{5049, StrictLevel::False};
} // namespace sorbet::core::errors::Resolver

#endif
