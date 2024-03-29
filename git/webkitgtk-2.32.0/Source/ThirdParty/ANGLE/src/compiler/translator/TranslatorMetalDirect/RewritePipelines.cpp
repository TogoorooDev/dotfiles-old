//
// Copyright 2020 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "compiler/translator/TranslatorMetalDirect.h"
#include "compiler/translator/TranslatorMetalDirect/AstHelpers.h"
#include "compiler/translator/TranslatorMetalDirect/DiscoverDependentFunctions.h"
#include "compiler/translator/TranslatorMetalDirect/IdGen.h"
#include "compiler/translator/TranslatorMetalDirect/MapSymbols.h"
#include "compiler/translator/TranslatorMetalDirect/Pipeline.h"
#include "compiler/translator/TranslatorMetalDirect/RewritePipelines.h"
#include "compiler/translator/tree_ops/PruneNoOps.h"
#include "compiler/translator/tree_util/FindMain.h"
#include "compiler/translator/tree_util/IntermRebuild.h"
#include "compiler/translator/tree_util/IntermTraverse.h"

using namespace sh;

////////////////////////////////////////////////////////////////////////////////

namespace
{

using VariableSet  = std::unordered_set<const TVariable *>;
using VariableList = std::vector<const TVariable *>;

////////////////////////////////////////////////////////////////////////////////

struct PipelineStructInfo
{
    VariableSet pipelineVariables;
    PipelineScoped<TStructure> pipelineStruct;
    const TFunction *funcOriginalToModified = nullptr;
    const TFunction *funcModifiedToOriginal = nullptr;

    bool isEmpty() const
    {
        if (pipelineStruct.isTotallyEmpty())
        {
            ASSERT(pipelineVariables.empty());
            return true;
        }
        else
        {
            ASSERT(pipelineStruct.isTotallyFull());
            ASSERT(!pipelineVariables.empty());
            return false;
        }
    }
};

class GeneratePipelineStruct : private TIntermRebuild
{
  private:
    const Pipeline &mPipeline;
    SymbolEnv &mSymbolEnv;
    Invariants &mInvariants;
    VariableList mPipelineVariableList;
    IdGen &mIdGen;
    PipelineStructInfo mInfo;

  public:
    static bool Exec(PipelineStructInfo &out,
                     TCompiler &compiler,
                     TIntermBlock &root,
                     IdGen &idGen,
                     const Pipeline &pipeline,
                     SymbolEnv &symbolEnv,
                     Invariants &invariants)
    {
        GeneratePipelineStruct self(compiler, idGen, pipeline, symbolEnv, invariants);
        if (!self.exec(root))
        {
            return false;
        }
        out = self.mInfo;
        return true;
    }

  private:
    GeneratePipelineStruct(TCompiler &compiler,
                           IdGen &idGen,
                           const Pipeline &pipeline,
                           SymbolEnv &symbolEnv,
                           Invariants &invariants)
        : TIntermRebuild(compiler, true, true),
          mPipeline(pipeline),
          mSymbolEnv(symbolEnv),
          mInvariants(invariants),
          mIdGen(idGen)
    {}

    bool exec(TIntermBlock &root)
    {
        if (!rebuildRoot(root))
        {
            return false;
        }

        if (mInfo.pipelineVariables.empty())
        {
            return true;
        }

        TIntermSequence seq;

        const TStructure &pipelineStruct = [&]() -> const TStructure & {
            if (mPipeline.globalInstanceVar)
            {
                return *mPipeline.globalInstanceVar->getType().getStruct();
            }
            else
            {
                return createInternalPipelineStruct(root, seq);
            }
        }();

        ModifiedStructMachineries modifiedMachineries;
        const bool modified = TryCreateModifiedStruct(
            mSymbolEnv, mIdGen, mPipeline.externalStructModifyConfig(), pipelineStruct,
            mPipeline.getStructTypeName(Pipeline::Variant::Modified), modifiedMachineries);

        if (modified)
        {
            ASSERT(mPipeline.type != Pipeline::Type::Texture);
            ASSERT(!mPipeline.globalInstanceVar);  // This shouldn't happen by construction.

            auto getFunction = [](sh::TIntermFunctionDefinition *funcDecl) {
                return funcDecl ? funcDecl->getFunction() : nullptr;
            };

            const size_t size = modifiedMachineries.size();
            ASSERT(size > 0);
            for (size_t i = 0; i < size; ++i)
            {
                const ModifiedStructMachinery &machinery = modifiedMachineries.at(i);
                ASSERT(machinery.modifiedStruct);

                seq.push_back(new TIntermDeclaration{
                    &CreateStructTypeVariable(mSymbolTable, *machinery.modifiedStruct)});

                if (mPipeline.isPipelineOut())
                {
                    ASSERT(machinery.funcOriginalToModified);
                    ASSERT(!machinery.funcModifiedToOriginal);
                    seq.push_back(machinery.funcOriginalToModified);
                }
                else
                {
                    ASSERT(machinery.funcModifiedToOriginal);
                    ASSERT(!machinery.funcOriginalToModified);
                    seq.push_back(machinery.funcModifiedToOriginal);
                }

                if (i == size - 1)
                {
                    mInfo.funcOriginalToModified = getFunction(machinery.funcOriginalToModified);
                    mInfo.funcModifiedToOriginal = getFunction(machinery.funcModifiedToOriginal);

                    mInfo.pipelineStruct.internal = &pipelineStruct;
                    mInfo.pipelineStruct.external =
                        modified ? machinery.modifiedStruct : &pipelineStruct;
                }
            }
        }
        else
        {
            mInfo.pipelineStruct.internal = &pipelineStruct;
            mInfo.pipelineStruct.external = &pipelineStruct;
        }

        root.insertChildNodes(FindMainIndex(&root), seq);

        return true;
    }

  private:
    PreResult visitFunctionDefinitionPre(TIntermFunctionDefinition &node) override
    {
        return {node, VisitBits::Neither};
    }

    PostResult visitDeclarationPost(TIntermDeclaration &declNode) override
    {
        Declaration decl     = ViewDeclaration(declNode);
        const TVariable &var = decl.symbol.variable();

        if (mPipeline.uses(var))
        {
            ASSERT(mInfo.pipelineVariables.find(&var) == mInfo.pipelineVariables.end());
            mInfo.pipelineVariables.insert(&var);
            mPipelineVariableList.push_back(&var);
            return nullptr;
        }

        return declNode;
    }

    const TStructure &createInternalPipelineStruct(TIntermBlock &root, TIntermSequence &outDeclSeq)
    {
        auto &fields = *new TFieldList();

        switch (mPipeline.type)
        {
            case Pipeline::Type::Texture:
            {
                for (const TVariable *var : mPipelineVariableList)
                {
                    ASSERT(!mInvariants.contains(*var));
                    const TType &varType         = var->getType();
                    const TBasicType samplerType = varType.getBasicType();

                    const TStructure &textureEnv = mSymbolEnv.getTextureEnv(samplerType);
                    auto *textureEnvType         = new TType(&textureEnv, false);
                    if (varType.isArray())
                    {
                        textureEnvType->makeArrays(varType.getArraySizes());
                    }

                    fields.push_back(
                        new TField(textureEnvType, var->name(), kNoSourceLoc, var->symbolType()));
                }
            }
            break;

            default:
            {
                for (const TVariable *var : mPipelineVariableList)
                {
                    auto &type  = CloneType(var->getType());
                    auto *field = new TField(&type, var->name(), kNoSourceLoc, var->symbolType());
                    fields.push_back(field);

                    if (mInvariants.contains(*var))
                    {
                        mInvariants.insert(*field);
                    }
                }
            }
            break;
        }

        Name pipelineStructName = mPipeline.getStructTypeName(Pipeline::Variant::Original);
        auto &s = *new TStructure(&mSymbolTable, pipelineStructName.rawName(), &fields,
                                  pipelineStructName.symbolType());

        outDeclSeq.push_back(new TIntermDeclaration{&CreateStructTypeVariable(mSymbolTable, s)});

        return s;
    }
};

////////////////////////////////////////////////////////////////////////////////

PipelineScoped<TVariable> CreatePipelineMainLocalVar(TSymbolTable &symbolTable,
                                                     const Pipeline &pipeline,
                                                     PipelineScoped<TStructure> pipelineStruct)
{
    ASSERT(pipelineStruct.isTotallyFull());

    PipelineScoped<TVariable> pipelineMainLocalVar;

    auto populateExternalMainLocalVar = [&]() {
        ASSERT(!pipelineMainLocalVar.external);
        pipelineMainLocalVar.external = &CreateInstanceVariable(
            symbolTable, *pipelineStruct.external,
            pipeline.getStructInstanceName(pipelineStruct.isUniform()
                                               ? Pipeline::Variant::Original
                                               : Pipeline::Variant::Modified));
    };

    auto populateDistinctInternalMainLocalVar = [&]() {
        ASSERT(!pipelineMainLocalVar.internal);
        pipelineMainLocalVar.internal =
            &CreateInstanceVariable(symbolTable, *pipelineStruct.internal,
                                    pipeline.getStructInstanceName(Pipeline::Variant::Original));
    };

    if (pipeline.type == Pipeline::Type::InstanceId)
    {
        populateDistinctInternalMainLocalVar();
    }
    else if (pipeline.alwaysRequiresLocalVariableDeclarationInMain())
    {
        populateExternalMainLocalVar();

        if (pipelineStruct.isUniform())
        {
            pipelineMainLocalVar.internal = pipelineMainLocalVar.external;
        }
        else
        {
            populateDistinctInternalMainLocalVar();
        }
    }
    else if (!pipelineStruct.isUniform())
    {
        populateDistinctInternalMainLocalVar();
    }

    return pipelineMainLocalVar;
}

class PipelineFunctionEnv
{
  private:
    TCompiler &mCompiler;
    SymbolEnv &mSymbolEnv;
    TSymbolTable &mSymbolTable;
    IdGen &mIdGen;
    const Pipeline &mPipeline;
    const std::unordered_set<const TFunction *> &mPipelineFunctions;
    const PipelineScoped<TStructure> mPipelineStruct;
    PipelineScoped<TVariable> &mPipelineMainLocalVar;

    std::unordered_map<const TFunction *, const TFunction *> mFuncMap;

  public:
    PipelineFunctionEnv(TCompiler &compiler,
                        SymbolEnv &symbolEnv,
                        IdGen &idGen,
                        const Pipeline &pipeline,
                        const std::unordered_set<const TFunction *> &pipelineFunctions,
                        PipelineScoped<TStructure> pipelineStruct,
                        PipelineScoped<TVariable> &pipelineMainLocalVar)
        : mCompiler(compiler),
          mSymbolEnv(symbolEnv),
          mSymbolTable(symbolEnv.symbolTable()),
          mIdGen(idGen),
          mPipeline(pipeline),
          mPipelineFunctions(pipelineFunctions),
          mPipelineStruct(pipelineStruct),
          mPipelineMainLocalVar(pipelineMainLocalVar)
    {}

    bool isOriginalPipelineFunction(const TFunction &func) const
    {
        return mPipelineFunctions.find(&func) != mPipelineFunctions.end();
    }

    bool isUpdatedPipelineFunction(const TFunction &func) const
    {
        auto it = mFuncMap.find(&func);
        if (it == mFuncMap.end())
        {
            return false;
        }
        return &func == it->second;
    }

    const TFunction &getUpdatedFunction(const TFunction &func)
    {
        ASSERT(isOriginalPipelineFunction(func) || isUpdatedPipelineFunction(func));

        const TFunction *newFunc;

        auto it = mFuncMap.find(&func);
        if (it == mFuncMap.end())
        {
            const bool isMain = func.isMain();

            if (isMain && mPipeline.isPipelineOut())
            {
                ASSERT(func.getReturnType().getBasicType() == TBasicType::EbtVoid);
                newFunc = &CloneFunctionAndChangeReturnType(mSymbolTable, nullptr, func,
                                                            *mPipelineStruct.external);
            }
            else if (isMain && (mPipeline.type == Pipeline::Type::InvocationVertexGlobals ||
                                mPipeline.type == Pipeline::Type::InvocationFragmentGlobals))
            {
                std::vector<const TVariable *> variables;
                for (const TField *field : mPipelineStruct.external->fields())
                {
                    variables.push_back(new TVariable(&mSymbolTable, field->name(), field->type(),
                                                      field->symbolType()));
                }
                newFunc = &CloneFunctionAndAppendParams(mSymbolTable, nullptr, func, variables);
            }
            else if (isMain && mPipeline.type == Pipeline::Type::Texture)
            {
                std::vector<const TVariable *> variables;
                TranslatorMetalReflection *reflection =
                    ((sh::TranslatorMetalDirect *)&mCompiler)->getTranslatorMetalReflection();
                for (const TField *field : mPipelineStruct.external->fields())
                {
                    const TStructure *textureEnv = field->type()->getStruct();
                    ASSERT(textureEnv && textureEnv->fields().size() == 2);
                    for (const TField *subfield : textureEnv->fields())
                    {
                        const Name name = mIdGen.createNewName({field->name(), subfield->name()});
                        TType &type     = *new TType(*subfield->type());
                        ASSERT(!type.isArray());
                        type.makeArrays(field->type()->getArraySizes());
                        auto *var =
                            new TVariable(&mSymbolTable, name.rawName(), &type, name.symbolType());
                        variables.push_back(var);
                        reflection->addOriginalName(var->uniqueId().get(), field->name().data());
                    }
                }
                newFunc = &CloneFunctionAndAppendParams(mSymbolTable, nullptr, func, variables);
            }
            else if (isMain && mPipeline.type == Pipeline::Type::InstanceId)
            {
                Name name = mPipeline.getStructInstanceName(Pipeline::Variant::Modified);
                auto *var = new TVariable(&mSymbolTable, name.rawName(),
                                          new TType(TBasicType::EbtUInt), name.symbolType());
                newFunc   = &CloneFunctionAndPrependParam(mSymbolTable, nullptr, func, *var);
                mSymbolEnv.markAsReference(*var, mPipeline.externalAddressSpace());
                mPipelineMainLocalVar.external = var;
            }
            else if (isMain && mPipeline.alwaysRequiresLocalVariableDeclarationInMain())
            {
                ASSERT(mPipelineMainLocalVar.isTotallyFull());
                newFunc = &func;
            }
            else
            {
                const TVariable *var;
                AddressSpace addressSpace;

                if (isMain && !mPipelineMainLocalVar.isUniform())
                {
                    var = &CreateInstanceVariable(
                        mSymbolTable, *mPipelineStruct.external,
                        mPipeline.getStructInstanceName(Pipeline::Variant::Modified));
                    addressSpace = mPipeline.externalAddressSpace();
                }
                else
                {
                    var = &CreateInstanceVariable(
                        mSymbolTable, *mPipelineStruct.internal,
                        mPipeline.getStructInstanceName(Pipeline::Variant::Original));
                    addressSpace = mPipelineMainLocalVar.isUniform()
                                       ? mPipeline.externalAddressSpace()
                                       : AddressSpace::Thread;
                }

                bool markAsReference = true;
                if (isMain)
                {
                    switch (mPipeline.type)
                    {
                        case Pipeline::Type::VertexIn:
                        case Pipeline::Type::FragmentIn:
                            markAsReference = false;
                            break;

                        default:
                            break;
                    }
                }

                if (markAsReference)
                {
                    mSymbolEnv.markAsReference(*var, addressSpace);
                }

                newFunc = &CloneFunctionAndPrependParam(mSymbolTable, nullptr, func, *var);
            }

            mFuncMap[&func]   = newFunc;
            mFuncMap[newFunc] = newFunc;
        }
        else
        {
            newFunc = it->second;
        }

        return *newFunc;
    }

    TIntermFunctionPrototype *createUpdatedFunctionPrototype(
        TIntermFunctionPrototype &funcProtoNode)
    {
        const TFunction &func = *funcProtoNode.getFunction();
        if (!isOriginalPipelineFunction(func) && !isUpdatedPipelineFunction(func))
        {
            return nullptr;
        }
        const TFunction &newFunc = getUpdatedFunction(func);
        return new TIntermFunctionPrototype(&newFunc);
    }
};

class UpdatePipelineFunctions : private TIntermRebuild
{
  private:
    const Pipeline &mPipeline;
    const PipelineScoped<TStructure> mPipelineStruct;
    PipelineScoped<TVariable> &mPipelineMainLocalVar;
    SymbolEnv &mSymbolEnv;
    PipelineFunctionEnv mEnv;
    const TFunction *mFuncOriginalToModified;
    const TFunction *mFuncModifiedToOriginal;

  public:
    static bool ThreadPipeline(TCompiler &compiler,
                               TIntermBlock &root,
                               const Pipeline &pipeline,
                               const std::unordered_set<const TFunction *> &pipelineFunctions,
                               PipelineScoped<TStructure> pipelineStruct,
                               PipelineScoped<TVariable> &pipelineMainLocalVar,
                               IdGen &idGen,
                               SymbolEnv &symbolEnv,
                               const TFunction *funcOriginalToModified,
                               const TFunction *funcModifiedToOriginal)
    {
        UpdatePipelineFunctions self(compiler, pipeline, pipelineFunctions, pipelineStruct,
                                     pipelineMainLocalVar, idGen, symbolEnv, funcOriginalToModified,
                                     funcModifiedToOriginal);
        if (!self.rebuildRoot(root))
        {
            return false;
        }
        return true;
    }

  private:
    UpdatePipelineFunctions(TCompiler &compiler,
                            const Pipeline &pipeline,
                            const std::unordered_set<const TFunction *> &pipelineFunctions,
                            PipelineScoped<TStructure> pipelineStruct,
                            PipelineScoped<TVariable> &pipelineMainLocalVar,
                            IdGen &idGen,
                            SymbolEnv &symbolEnv,
                            const TFunction *funcOriginalToModified,
                            const TFunction *funcModifiedToOriginal)
        : TIntermRebuild(compiler, false, true),
          mPipeline(pipeline),
          mPipelineStruct(pipelineStruct),
          mPipelineMainLocalVar(pipelineMainLocalVar),
          mSymbolEnv(symbolEnv),
          mEnv(compiler,
               symbolEnv,
               idGen,
               pipeline,
               pipelineFunctions,
               pipelineStruct,
               mPipelineMainLocalVar),
          mFuncOriginalToModified(funcOriginalToModified),
          mFuncModifiedToOriginal(funcModifiedToOriginal)
    {
        ASSERT(mPipelineStruct.isTotallyFull());
    }

    const TVariable &getInternalPipelineVariable(const TFunction &pipelineFunc)
    {
        if (pipelineFunc.isMain() && (mPipeline.alwaysRequiresLocalVariableDeclarationInMain() ||
                                      !mPipelineMainLocalVar.isUniform()))
        {
            ASSERT(mPipelineMainLocalVar.internal);
            return *mPipelineMainLocalVar.internal;
        }
        else
        {
            ASSERT(pipelineFunc.getParamCount() > 0);
            return *pipelineFunc.getParam(0);
        }
    }

    const TVariable &getExternalPipelineVariable(const TFunction &mainFunc)
    {
        ASSERT(mainFunc.isMain());
        if (mPipelineMainLocalVar.external)
        {
            return *mPipelineMainLocalVar.external;
        }
        else
        {
            ASSERT(mainFunc.getParamCount() > 0);
            return *mainFunc.getParam(0);
        }
    }

    PostResult visitAggregatePost(TIntermAggregate &callNode) override
    {
        if (callNode.isConstructor())
        {
            return callNode;
        }
        else
        {
            const TFunction &oldCalledFunc = *callNode.getFunction();
            if (!mEnv.isOriginalPipelineFunction(oldCalledFunc))
            {
                return callNode;
            }
            const TFunction &newCalledFunc = mEnv.getUpdatedFunction(oldCalledFunc);

            const TFunction *oldOwnerFunc = getParentFunction();
            ASSERT(oldOwnerFunc);
            const TFunction &newOwnerFunc = mEnv.getUpdatedFunction(*oldOwnerFunc);

            return *TIntermAggregate::CreateFunctionCall(
                newCalledFunc, &CloneSequenceAndPrepend(
                                   *callNode.getSequence(),
                                   *new TIntermSymbol(&getInternalPipelineVariable(newOwnerFunc))));
        }
    }

    PostResult visitFunctionPrototypePost(TIntermFunctionPrototype &funcProtoNode) override
    {
        TIntermFunctionPrototype *newFuncProtoNode =
            mEnv.createUpdatedFunctionPrototype(funcProtoNode);
        if (newFuncProtoNode == nullptr)
        {
            return funcProtoNode;
        }
        return *newFuncProtoNode;
    }

    PostResult visitFunctionDefinitionPost(TIntermFunctionDefinition &funcDefNode) override
    {
        if (funcDefNode.getFunction()->isMain())
        {
            return visitMain(funcDefNode);
        }
        else
        {
            return visitNonMain(funcDefNode);
        }
    }

    TIntermNode &visitNonMain(TIntermFunctionDefinition &funcDefNode)
    {
        TIntermFunctionPrototype &funcProtoNode = *funcDefNode.getFunctionPrototype();
        ASSERT(!funcProtoNode.getFunction()->isMain());

        TIntermFunctionPrototype *newFuncProtoNode =
            mEnv.createUpdatedFunctionPrototype(funcProtoNode);
        if (newFuncProtoNode == nullptr)
        {
            return funcDefNode;
        }

        const TFunction &func = *newFuncProtoNode->getFunction();
        ASSERT(!func.isMain());

        TIntermBlock *body = funcDefNode.getBody();

        return *new TIntermFunctionDefinition(newFuncProtoNode, body);
    }

    TIntermNode &visitMain(TIntermFunctionDefinition &funcDefNode)
    {
        TIntermFunctionPrototype &funcProtoNode = *funcDefNode.getFunctionPrototype();
        ASSERT(funcProtoNode.getFunction()->isMain());

        TIntermFunctionPrototype *newFuncProtoNode =
            mEnv.createUpdatedFunctionPrototype(funcProtoNode);
        if (newFuncProtoNode == nullptr)
        {
            return funcDefNode;
        }

        const TFunction &func = *newFuncProtoNode->getFunction();
        ASSERT(func.isMain());

        auto callModifiedToOriginal = [&](TIntermBlock &body) {
            ASSERT(mPipelineMainLocalVar.internal);
            if (!mPipeline.isPipelineOut())
            {
                ASSERT(mFuncModifiedToOriginal);
                auto *m = new TIntermSymbol(&getExternalPipelineVariable(func));
                auto *o = new TIntermSymbol(mPipelineMainLocalVar.internal);
                body.appendStatement(TIntermAggregate::CreateFunctionCall(
                    *mFuncModifiedToOriginal, new TIntermSequence{m, o}));
            }
        };

        auto callOriginalToModified = [&](TIntermBlock &body) {
            ASSERT(mPipelineMainLocalVar.internal);
            if (mPipeline.isPipelineOut())
            {
                ASSERT(mFuncOriginalToModified);
                auto *o = new TIntermSymbol(mPipelineMainLocalVar.internal);
                auto *m = new TIntermSymbol(&getExternalPipelineVariable(func));
                body.appendStatement(TIntermAggregate::CreateFunctionCall(
                    *mFuncOriginalToModified, new TIntermSequence{o, m}));
            }
        };

        TIntermBlock *body = funcDefNode.getBody();

        if (mPipeline.alwaysRequiresLocalVariableDeclarationInMain())
        {
            ASSERT(mPipelineMainLocalVar.isTotallyFull());

            auto *newBody = new TIntermBlock();
            newBody->appendStatement(new TIntermDeclaration{mPipelineMainLocalVar.internal});

            if (mPipeline.type == Pipeline::Type::InvocationVertexGlobals ||
                mPipeline.type == Pipeline::Type::InvocationFragmentGlobals)
            {
                // Populate struct instance with references to global pipeline variables.
                for (const TField *field : mPipelineStruct.external->fields())
                {
                    auto *var        = new TVariable(&mSymbolTable, field->name(), field->type(),
                                              field->symbolType());
                    auto *symbol     = new TIntermSymbol(var);
                    auto &accessNode = AccessField(*mPipelineMainLocalVar.internal, var->name());
                    auto *assignNode = new TIntermBinary(TOperator::EOpAssign, &accessNode, symbol);
                    newBody->appendStatement(assignNode);
                }
            }
            else if (mPipeline.type == Pipeline::Type::Texture)
            {
                const TFieldList &fields = mPipelineStruct.external->fields();

                ASSERT(func.getParamCount() >= 2 * fields.size());
                size_t paramIndex = func.getParamCount() - 2 * fields.size();

                for (const TField *field : fields)
                {
                    const TVariable &textureParam = *func.getParam(paramIndex++);
                    const TVariable &samplerParam = *func.getParam(paramIndex++);

                    auto go = [&](TIntermTyped &env, const int *index) {
                        TIntermTyped &textureField = AccessField(
                            AccessIndex(*env.deepCopy(), index), ImmutableString("texture"));
                        TIntermTyped &samplerField = AccessField(
                            AccessIndex(*env.deepCopy(), index), ImmutableString("sampler"));

                        auto mkAssign = [&](TIntermTyped &field, const TVariable &param) {
                            return new TIntermBinary(TOperator::EOpAssign, &field,
                                                     &mSymbolEnv.callFunctionOverload(
                                                         Name("addressof"), field.getType(),
                                                         *new TIntermSequence{&AccessIndex(
                                                             *new TIntermSymbol(&param), index)}));
                        };

                        newBody->appendStatement(mkAssign(textureField, textureParam));
                        newBody->appendStatement(mkAssign(samplerField, samplerParam));
                    };

                    TIntermTyped &env = AccessField(*mPipelineMainLocalVar.internal, field->name());
                    const TType &envType = env.getType();

                    if (envType.isArray())
                    {
                        ASSERT(!envType.isArrayOfArrays());
                        const auto n = static_cast<int>(envType.getArraySizeProduct());
                        for (int i = 0; i < n; ++i)
                        {
                            go(env, &i);
                        }
                    }
                    else
                    {
                        go(env, nullptr);
                    }
                }
            }
            else if (mPipeline.type == Pipeline::Type::InstanceId)
            {
                newBody->appendStatement(new TIntermBinary(
                    TOperator::EOpAssign,
                    &AccessFieldByIndex(*new TIntermSymbol(&getInternalPipelineVariable(func)), 0),
                    &AsType(mSymbolEnv, *new TType(TBasicType::EbtInt),
                            *new TIntermSymbol(&getExternalPipelineVariable(func)))));
            }
            else if (!mPipelineMainLocalVar.isUniform())
            {
                newBody->appendStatement(new TIntermDeclaration{mPipelineMainLocalVar.external});
                callModifiedToOriginal(*newBody);
            }

            newBody->appendStatement(body);

            if (!mPipelineMainLocalVar.isUniform())
            {
                callOriginalToModified(*newBody);
            }

            if (mPipeline.isPipelineOut())
            {
                newBody->appendStatement(new TIntermBranch(
                    TOperator::EOpReturn, new TIntermSymbol(mPipelineMainLocalVar.external)));
            }

            body = newBody;
        }
        else if (!mPipelineMainLocalVar.isUniform())
        {
            ASSERT(!mPipelineMainLocalVar.external);
            ASSERT(mPipelineMainLocalVar.internal);

            auto *newBody = new TIntermBlock();
            newBody->appendStatement(new TIntermDeclaration{mPipelineMainLocalVar.internal});
            callModifiedToOriginal(*newBody);
            newBody->appendStatement(body);
            callOriginalToModified(*newBody);
            body = newBody;
        }

        return *new TIntermFunctionDefinition(newFuncProtoNode, body);
    }
};

////////////////////////////////////////////////////////////////////////////////

bool UpdatePipelineSymbols(Pipeline::Type pipelineType,
                           TCompiler &compiler,
                           TIntermBlock &root,
                           SymbolEnv &symbolEnv,
                           const VariableSet &pipelineVariables,
                           PipelineScoped<TVariable> pipelineMainLocalVar)
{
    auto map = [&](const TFunction *owner, TIntermSymbol &symbol) -> TIntermNode & {
        const TVariable &var = symbol.variable();
        if (pipelineVariables.find(&var) == pipelineVariables.end())
        {
            return symbol;
        }
        ASSERT(owner);
        const TVariable *structInstanceVar;
        if (owner->isMain())
        {
            ASSERT(pipelineMainLocalVar.internal);
            structInstanceVar = pipelineMainLocalVar.internal;
        }
        else
        {
            ASSERT(owner->getParamCount() > 0);
            structInstanceVar = owner->getParam(0);
        }
        ASSERT(structInstanceVar);
        return AccessField(*structInstanceVar, var.name());
    };
    return MapSymbols(compiler, root, map);
}

////////////////////////////////////////////////////////////////////////////////

bool RewritePipeline(TCompiler &compiler,
                     TIntermBlock &root,
                     IdGen &idGen,
                     const Pipeline &pipeline,
                     SymbolEnv &symbolEnv,
                     Invariants &invariants,
                     PipelineScoped<TStructure> &outStruct)
{
    ASSERT(outStruct.isTotallyEmpty());

    TSymbolTable &symbolTable = compiler.getSymbolTable();

    PipelineStructInfo psi;
    if (!GeneratePipelineStruct::Exec(psi, compiler, root, idGen, pipeline, symbolEnv, invariants))
    {
        return false;
    }

    if (psi.isEmpty())
    {
        return true;
    }

    const auto pipelineFunctions = DiscoverDependentFunctions(root, [&](const TVariable &var) {
        return psi.pipelineVariables.find(&var) != psi.pipelineVariables.end();
    });

    auto pipelineMainLocalVar =
        CreatePipelineMainLocalVar(symbolTable, pipeline, psi.pipelineStruct);

    if (!UpdatePipelineFunctions::ThreadPipeline(
            compiler, root, pipeline, pipelineFunctions, psi.pipelineStruct, pipelineMainLocalVar,
            idGen, symbolEnv, psi.funcOriginalToModified, psi.funcModifiedToOriginal))
    {
        return false;
    }

    if (!pipeline.globalInstanceVar)
    {
        if (!UpdatePipelineSymbols(pipeline.type, compiler, root, symbolEnv, psi.pipelineVariables,
                                   pipelineMainLocalVar))
        {
            return false;
        }
    }

    if (!PruneNoOps(&compiler, &root, &compiler.getSymbolTable()))
    {
        return false;
    }

    outStruct = psi.pipelineStruct;
    return true;
}

}  // anonymous namespace

bool sh::RewritePipelines(TCompiler &compiler,
                          TIntermBlock &root,
                          IdGen &idGen,
                          const TVariable &angleUniformsGlobalInstanceVar,
                          SymbolEnv &symbolEnv,
                          Invariants &invariants,
                          PipelineStructs &outStructs)
{
    struct Info
    {
        Pipeline::Type pipelineType;
        PipelineScoped<TStructure> &outStruct;
        const TVariable *globalInstanceVar;
    };

    Info infos[] = {
        {Pipeline::Type::InstanceId, outStructs.instanceId, nullptr},
        {Pipeline::Type::Texture, outStructs.texture, nullptr},
        {Pipeline::Type::NonConstantGlobals, outStructs.nonConstantGlobals, nullptr},
        {Pipeline::Type::AngleUniforms, outStructs.angleUniforms, &angleUniformsGlobalInstanceVar},
        {Pipeline::Type::UserUniforms, outStructs.userUniforms, nullptr},
        {Pipeline::Type::VertexIn, outStructs.vertexIn, nullptr},
        {Pipeline::Type::VertexOut, outStructs.vertexOut, nullptr},
        {Pipeline::Type::FragmentIn, outStructs.fragmentIn, nullptr},
        {Pipeline::Type::FragmentOut, outStructs.fragmentOut, nullptr},
        {Pipeline::Type::InvocationVertexGlobals, outStructs.invocationVertexGlobals, nullptr},
        {Pipeline::Type::InvocationFragmentGlobals, outStructs.invocationFragmentGlobals, nullptr},
    };

    for (Info &info : infos)
    {
        Pipeline pipeline{info.pipelineType, info.globalInstanceVar};
        if (!RewritePipeline(compiler, root, idGen, pipeline, symbolEnv, invariants,
                             info.outStruct))
        {
            return false;
        }
    }

    return true;
}
