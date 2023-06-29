#include "clad/Differentiator/VectorForwardModeVisitor.h"

#include "ConstantFolder.h"
#include "clad/Differentiator/CladUtils.h"

#include "llvm/Support/SaveAndRestore.h"

using namespace clang;

namespace clad {
VectorForwardModeVisitor::VectorForwardModeVisitor(DerivativeBuilder& builder)
    : BaseForwardModeVisitor(builder) {}

VectorForwardModeVisitor::~VectorForwardModeVisitor() {}

DerivativeAndOverload
VectorForwardModeVisitor::DeriveVectorMode(const FunctionDecl* FD,
                                           const DiffRequest& request) {
  m_Function = FD;
  m_Mode = DiffMode::vector_forward_mode;

  // Generated function name for the derivative.
  IdentifierInfo* II =
      &m_Context.Idents.get(request.BaseFunctionName + "_d_all_args");
  DeclarationNameInfo name(II, noLoc);

  // Generate the function type for the derivative.
  llvm::SmallVector<clang::QualType, 8> paramTypes;
  paramTypes.reserve(m_Function->getNumParams() * 2);
  for (auto PVD : m_Function->parameters()) {
    paramTypes.push_back(PVD->getType());
  }
  for (auto PVD : m_Function->parameters()) {
    QualType ValueType = utils::GetValueType(PVD->getType());
    ValueType.removeLocalConst();
    // Generate pointer type for the derivative.
    QualType dParamType = m_Context.getPointerType(ValueType);
    paramTypes.push_back(dParamType);
  }

  QualType vectorDiffFunctionType = m_Context.getFunctionType(
      m_Context.VoidTy,
      llvm::ArrayRef<QualType>(paramTypes.data(), paramTypes.size()),
      // Cast to function pointer.
      dyn_cast<FunctionProtoType>(m_Function->getType())->getExtProtoInfo());

  // Create the function declaration for the derivative.
  DeclContext* DC = const_cast<DeclContext*>(m_Function->getDeclContext());
  m_Sema.CurContext = DC;
  DeclWithContext result =
      m_Builder.cloneFunction(m_Function, *this, DC, m_Sema, m_Context, noLoc,
                              name, vectorDiffFunctionType);
  FunctionDecl* vectorDiffFD = result.first;
  m_Derivative = vectorDiffFD;

  // Function declaration scope
  llvm::SaveAndRestore<DeclContext*> SaveContext(m_Sema.CurContext);
  llvm::SaveAndRestore<Scope*> SaveScope(m_CurScope);
  beginScope(Scope::FunctionPrototypeScope | Scope::FunctionDeclarationScope |
             Scope::DeclScope);
  m_Sema.PushFunctionScope();
  m_Sema.PushDeclContext(getCurrentScope(), m_Derivative);

  // Set the parameters for the derivative.
  DiffParams args{};
  std::copy(FD->param_begin(), FD->param_end(), args.begin());
  auto params = BuildVectorModeParams(args);
  vectorDiffFD->setParams(llvm::makeArrayRef(params.data(), params.size()));
  vectorDiffFD->setBody(nullptr);

  // Create the body of the derivative.
  beginScope(Scope::FnScope | Scope::DeclScope);
  m_DerivativeFnScope = getCurrentScope();
  beginBlock();
  for (size_t i = 0; i < m_Function->getNumParams(); ++i) {
    auto param = params[i];
    QualType dParamType = clad::utils::GetValueType(param->getType());

    // initialize a one hot vector for the parameter with 1 at the index.
    Expr* dVectorParam = getOneHotInitExpr(i, m_Function->getNumParams());
    // For each function arg, create a variable _d_vector_arg to store the
    // vector of derivatives for that arg.
    // for ex: double f(double x, double y);
    // -> clad::array<double> _d_vector_x = {1, 0};
    // -> clad::array<double> _d_vector_y = {0, 1};
    auto dVectorParamDecl =
        BuildVarDecl(GetCladArrayOfType(dParamType),
                     "_d_vector_" + param->getNameAsString(), dVectorParam);
    addToCurrentBlock(BuildDeclStmt(dVectorParamDecl));
    dVectorParam = BuildDeclRef(dVectorParamDecl);
    // Memorize the derivative vector for the parameter.
    m_Variables[param] = dVectorParam;
  }

  // Traverse the function body and generate the derivative.
  Stmt* BodyDiff = Visit(FD->getBody()).getStmt();
  if (auto CS = dyn_cast<CompoundStmt>(BodyDiff))
    for (Stmt* S : CS->body())
      addToCurrentBlock(S);
  else
    addToCurrentBlock(BodyDiff);

  Stmt* vectorDiffBody = endBlock();
  m_Derivative->setBody(vectorDiffBody);
  endScope(); // Function body scope
  m_Sema.PopFunctionScopeInfo();
  m_Sema.PopDeclContext();
  endScope(); // Function decl scope

  return DerivativeAndOverload{vectorDiffFD, nullptr};
}

clang::Expr* VectorForwardModeVisitor::getOneHotInitExpr(size_t index,
                                                         size_t size) {
  // define a vector of size `size` with all elements set to 0,
  // except for the element at `index` which is set to 1.
  auto zero =
      ConstantFolder::synthesizeLiteral(m_Context.DoubleTy, m_Context, 0);
  auto one =
      ConstantFolder::synthesizeLiteral(m_Context.DoubleTy, m_Context, 1);
  llvm::SmallVector<clang::Expr*, 8> oneHotInitList(size, zero);
  oneHotInitList[index] = one;
  return m_Sema.ActOnInitList(noLoc, oneHotInitList, noLoc).get();
}

llvm::SmallVector<clang::ParmVarDecl*, 8>
VectorForwardModeVisitor::BuildVectorModeParams(DiffParams& diffParams) {
  llvm::SmallVector<clang::ParmVarDecl*, 8> params, paramDerivatives;
  params.reserve(m_Function->getNumParams() + diffParams.size());
  auto derivativeFnType = cast<FunctionProtoType>(m_Derivative->getType());
  std::size_t dParamTypesIdx = m_Function->getNumParams();

  for (auto PVD : m_Function->parameters()) {
    auto newPVD = utils::BuildParmVarDecl(
        m_Sema, m_Derivative, PVD->getIdentifier(), PVD->getType(),
        PVD->getStorageClass(), /*DefArg=*/nullptr, PVD->getTypeSourceInfo());
    params.push_back(newPVD);

    if (newPVD->getIdentifier())
      m_Sema.PushOnScopeChains(newPVD, getCurrentScope(),
                               /*AddToContext=*/false);

    QualType dType = derivativeFnType->getParamType(dParamTypesIdx);
    IdentifierInfo* dII =
        CreateUniqueIdentifier("_d_" + PVD->getNameAsString());
    auto dPVD = utils::BuildParmVarDecl(m_Sema, m_Derivative, dII, dType,
                                        PVD->getStorageClass());
    paramDerivatives.push_back(dPVD);
    ++dParamTypesIdx;

    if (dPVD->getIdentifier())
      m_Sema.PushOnScopeChains(dPVD, getCurrentScope(),
                               /*AddToContext=*/false);

    m_ParamVariables[newPVD] = BuildOp(UO_Deref, BuildDeclRef(dPVD), noLoc);
  }
  // insert the derivative parameters at the end of the parameter list.
  params.insert(params.end(), paramDerivatives.begin(), paramDerivatives.end());
  return params;
}

StmtDiff VectorForwardModeVisitor::VisitReturnStmt(const ReturnStmt* RS) {
  StmtDiff retValDiff = Visit(RS->getRetValue());
  Expr* derivedRetValE = retValDiff.getExpr_dx();
  // If we are in vector mode, we need to wrap the return value in a
  // vector.
  auto dVectorParamDecl =
      BuildVarDecl(GetCladArrayOfType(
                       clad::utils::GetValueType(RS->getRetValue()->getType())),
                   "_d_vector_return", derivedRetValE);
  // Create an array of statements to hold the return statement and the
  // assignments to the derivatives of the parameters.
  Stmts returnStmts;
  returnStmts.push_back(BuildDeclStmt(dVectorParamDecl));
  // Assign values from return vector to the derivatives of the
  // parameters.
  auto dVectorRef = BuildDeclRef(dVectorParamDecl);
  for (size_t i = 0; i < m_Function->getNumParams(); ++i) {
    // Get the derivative of the ith parameter.
    auto dParam = m_ParamVariables[m_Derivative->getParamDecl(i)];
    // Create an array subscript expression to access the ith element
    auto indexExpr =
        ConstantFolder::synthesizeLiteral(m_Context.IntTy, m_Context, i);
    auto dParamValue =
        m_Sema
            .ActOnArraySubscriptExpr(getCurrentScope(), dVectorRef,
                                     dVectorRef->getExprLoc(), indexExpr, noLoc)
            .get();
    // Create an assignment expression to assign the ith element of the
    // return vector to the derivative of the ith parameter.
    auto dParamAssign = BuildOp(BO_Assign, dParam, dParamValue);
    // Add the assignment statement to the array of statements.
    returnStmts.push_back(dParamAssign);
  }
  // Add an empty return statement to the array of statements.
  returnStmts.push_back(
      m_Sema.ActOnReturnStmt(noLoc, nullptr, getCurrentScope()).get());

  // Create a return statement from the compound statement.
  Stmt* returnStmt = MakeCompoundStmt(returnStmts);
  return StmtDiff(returnStmt);
}

VarDeclDiff VectorForwardModeVisitor::DifferentiateVarDecl(const VarDecl* VD) {
  StmtDiff initDiff = VD->getInit() ? Visit(VD->getInit()) : StmtDiff{};
  // Here we are assuming that derived type and the original type are same.
  // This may not necessarily be true in the future.
  VarDecl* VDClone =
      BuildVarDecl(VD->getType(), VD->getNameAsString(), initDiff.getExpr(),
                   VD->isDirectInit(), nullptr, VD->getInitStyle());
  VarDecl* VDDerived =
      BuildVarDecl(GetCladArrayOfType(utils::GetValueType(VD->getType())),
                   "_d_vector_" + VD->getNameAsString(), initDiff.getExpr_dx(),
                   VD->isDirectInit(), nullptr, VD->getInitStyle());
  m_Variables.emplace(VDClone, BuildDeclRef(VDDerived));

  return VarDeclDiff(VDClone, VDDerived);
}

} // namespace clad
