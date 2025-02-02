/*
Copyright 2020 Intel Corp.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifndef BACKENDS_DPDK_PROGRAM_H_
#define BACKENDS_DPDK_PROGRAM_H_

#include "dpdkArch.h"
#include "dpdkProgramStructure.h"
#include "frontends/common/constantFolding.h"
#include "frontends/common/resolveReferences/referenceMap.h"
#include "frontends/p4/coreLibrary.h"
#include "frontends/p4/enumInstance.h"
#include "frontends/p4/evaluator/evaluator.h"
#include "frontends/p4/methodInstance.h"
#include "frontends/p4/simplify.h"
#include "frontends/p4/typeMap.h"
#include "frontends/p4/unusedDeclarations.h"
#include "ir/ir.h"
#include "lib/gmputil.h"
#include "lib/json.h"
namespace DPDK {

/* Maximum size in bits for fields in header and metadata structures */
#define DPDK_MAX_HEADER_METADATA_FIELD_SIZE 64

class ConvertToDpdkProgram : public Transform {
    P4::TypeMap *typemap;
    P4::ReferenceMap *refmap;
    DpdkProgramStructure *structure;
    const IR::DpdkAsmProgram *dpdk_program;

  public:
    ConvertToDpdkProgram(P4::ReferenceMap *refmap, P4::TypeMap *typemap,
                         DpdkProgramStructure *structure)
        : typemap(typemap), refmap(refmap), structure(structure) { }

    const IR::DpdkAsmProgram *create(IR::P4Program *prog);
    const IR::DpdkAsmStatement *createListStatement(
        cstring name,
        std::initializer_list<IR::IndexedVector<IR::DpdkAsmStatement>>
            statements);
    const IR::Node *preorder(IR::P4Program *p) override;
    const IR::DpdkAsmProgram *getDpdkProgram() { return dpdk_program; }
    IR::IndexedVector<IR::DpdkStructType> UpdateHeaderMetadata(
                      IR::P4Program *prog, IR::Type_Struct *metadata);
};

class ConvertToDpdkParser : public Inspector {
    IR::IndexedVector<IR::DpdkAsmStatement> instructions;
    P4::ReferenceMap *refmap;
    P4::TypeMap *typemap;
    DpdkProgramStructure *structure;
    IR::Type_Struct *metadataStruct;

  public:
    ConvertToDpdkParser(
        P4::ReferenceMap *refmap, P4::TypeMap *typemap,
        DpdkProgramStructure* structure,
        IR::Type_Struct *metadataStruct)
        : refmap(refmap), typemap(typemap), structure(structure),
          metadataStruct(metadataStruct) {}
    IR::IndexedVector<IR::DpdkAsmStatement> getInstructions() {
        return instructions;
    }

    bool preorder(const IR::P4Parser *a) override;
    bool preorder(const IR::ParserState *s) override;
    void add_instr(const IR::DpdkAsmStatement *s) { instructions.push_back(s); }
    cstring append_parser_name(const IR::P4Parser* p, cstring);
    IR::Declaration_Variable *addNewTmpVarToMetadata (cstring name, const IR::Type* type);
    void handleTupleExpression(const IR::ListExpression *cl, const IR::ListExpression *input,
                               int inputSize, cstring trueLabel, cstring falseLabel);
    void getCondVars(const IR::Expression *sv, const IR::Expression *ce,
                     IR::Expression **leftExpr, IR::Expression **rightExpr);
};


class ConvertToDpdkControl : public Inspector {
    P4::TypeMap *typemap;
    P4::ReferenceMap *refmap;
    DpdkProgramStructure *structure;
    IR::IndexedVector<IR::DpdkAsmStatement> instructions;
    IR::IndexedVector<IR::DpdkTable> tables;
    IR::IndexedVector<IR::DpdkSelector> selectors;
    IR::IndexedVector<IR::DpdkAction> actions;
    std::set<cstring> unique_actions;
    bool deparser;

  public:
    ConvertToDpdkControl(
        P4::ReferenceMap *refmap, P4::TypeMap *typemap,
        DpdkProgramStructure *structure,
        bool deparser = false)
        : typemap(typemap), refmap(refmap), structure(structure), deparser(deparser) {}

    IR::IndexedVector<IR::DpdkTable> &getTables() { return tables; }
    IR::IndexedVector<IR::DpdkSelector> &getSelectors() { return selectors; }
    IR::IndexedVector<IR::DpdkAction> &getActions() { return actions; }
    IR::IndexedVector<IR::DpdkAsmStatement> &getInstructions() {
        return instructions;
    }

    bool preorder(const IR::P4Action *a) override;
    bool preorder(const IR::P4Table *a) override;
    bool preorder(const IR::P4Control *) override;
    bool checkTableValid(const IR::P4Table *a);

    void add_inst(const IR::DpdkAsmStatement *s) { instructions.push_back(s); }
    void add_table(const IR::DpdkTable *t) { tables.push_back(t); }
    void add_table(const IR::DpdkSelector *s) { selectors.push_back(s); }
    void add_action(const IR::DpdkAction *a) { actions.push_back(a); }

    boost::optional<cstring> getIdFromProperty(const IR::P4Table*, cstring);
    boost::optional<int> getNumberFromProperty(const IR::P4Table*, cstring);
};

class CollectActionUses : public Inspector {
    ordered_set<cstring>& actions;

 public:
    CollectActionUses(ordered_set<cstring>& a) : actions(a) { }
    bool preorder(const IR::ActionListElement* ale) {
        if (auto mce = ale->expression->to<IR::MethodCallExpression>()) {
            if (auto path = mce->method->to<IR::PathExpression>()) {
                actions.insert(path->path->name.toString());
            }
        }
        return false;
    }
};

class ElimUnusedActions : public Transform {
    const ordered_set<cstring>& used_actions;
    std::set<cstring> kept_actions;

 public:
    ElimUnusedActions(const ordered_set<cstring>& a) : used_actions(a) {}
    const IR::Node* postorder(IR::DpdkAction* a) override {
        if (kept_actions.count(a->name.toString()) != 0)
            return nullptr;
        if (used_actions.find(a->name.toString()) != used_actions.end()) {
            kept_actions.insert(a->name.toString());
            return a; }
        return nullptr;
    }
};

// frontend generates multiple copies of the same action in the localizeActions pass,
// they are not useful for dpdk datapath. Removed the duplicated actions in
// this pass.
class EliminateUnusedAction : public PassManager {
    ordered_set<cstring> actions;

 public:
    EliminateUnusedAction() {
        addPasses( {
            new CollectActionUses(actions),
            new ElimUnusedActions(actions)
        });
    }
};

} // namespace DPDK
#endif
