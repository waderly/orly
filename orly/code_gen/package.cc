/* <orly/code_gen/package.cc>

   Implements <orly/code_gen/package.h>

   Copyright 2010-2014 OrlyAtomics, Inc.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */

#include <orly/code_gen/package.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>

#include <base/make_dir.h>
#include <base/split.h>
#include <orly/code_gen/obj.h>
#include <orly/code_gen/util.h>
#include <orly/expr/addr_walker.h>
#include <orly/expr/walker.h>
#include <orly/expr/where.h>
#include <orly/package/api_version.h>
#include <orly/type/gen_code.h>
#include <orly/type/impl.h>
#include <orly/type/object_collector.h> //TODO: This header should be renamed
#include <orly/type/orlyify.h>
#include <orly/type/util.h>
#include <orly/expr/util.h>

using namespace Base;
using namespace Jhm;
using namespace std;
using namespace std::placeholders;
using namespace Orly;
using namespace Orly::CodeGen;

void ForEachExprInTestBlock(const function<void (const Expr::TExpr::TPtr &expr)> &cb,
      const Symbol::Test::TTestCaseBlock::TPtr &block) {
  assert(&cb);
  assert(cb);
  assert(&block);
  assert(block);

  for(auto &test: block->GetTestCases()) {
    cb(test->GetExpr());
    if(test->GetOptTestCaseBlock()) {
      ForEachExprInTestBlock(cb, test->GetOptTestCaseBlock());
    }
  }
}

void GetAddrsInTestBlock(const Symbol::Test::TTestCaseBlock::TPtr &block,
                         unordered_set<pair<Type::TType, Type::TType>> &addr_set) {
  for(auto &test: block->GetTestCases()) {
    Expr::DatabaseAddrsInExpr(test->GetExpr(), addr_set);
    if(test->GetOptTestCaseBlock()) {
      GetAddrsInTestBlock(test->GetOptTestCaseBlock(), addr_set);
    }
  }
}

TPackage::TPackage(const Symbol::TPackage::TPtr &package) : L0::TPackage(package) {
/* Stages
1) Gather information
   - All objects, needed object comparisons.
   - List of top level functions, their signatures.
   - Effecting blocks, assertion predicates
2) Build function bodies (The tree of inlines)
   - The whole tree of inlines and scopes
      - includes converting sequences into maps when they pass through non-sequence operations
*/
  //Collect all the objects.
  std::function<void (const Expr::TExpr::TPtr &expr)> collect_objects;
  collect_objects = [this, &collect_objects](const Expr::TExpr::TPtr &expr) {
    Expr::ForEachExpr(expr, [this, &collect_objects](const Expr::TExpr::TPtr &expr) {
        Type::CollectObjects(expr->GetType(), Objects);
        const Expr::TWhere *where = expr->TryAs<Expr::TWhere>();
        if(where) {
          for(auto &it: where->GetFunctions()) {
            collect_objects(it->GetExpr());
          }
        }
        return false;
    },true);
  };

  for(auto &func: package->GetFunctions()) {
    collect_objects(func->GetExpr());
  }

  for(auto &test: package->GetTests()) {
    if(test->GetOptWithClause()) {
      for(auto &new_stmt: test->GetOptWithClause()->GetNewStmts()) {
        collect_objects(new_stmt->GetLhs()->GetExpr());
        collect_objects(new_stmt->GetRhs()->GetExpr());
      }
    }
    ForEachExprInTestBlock(collect_objects, test->GetTestCaseBlock());
  }

  /* Get the addrs that are used in the expressions */
  unordered_set<pair<Type::TType, Type::TType>> addr_set;
  for(auto &func: package->GetFunctions()) {
    Expr::DatabaseAddrsInExpr(func->GetExpr(), addr_set);
  }
  for(auto &test: package->GetTests()) {
    /* With clause */
    if(test->GetOptWithClause()) {
      for(auto &new_stmt: test->GetOptWithClause()->GetNewStmts()) {
        Expr::DatabaseAddrsFromNewStmt(new_stmt.get(), addr_set);
      }
    }
    /* Test clause */
    GetAddrsInTestBlock(test->GetTestCaseBlock(), addr_set);
  }
  /* Assign each addr in the addr_set a Uuid and add it to a map */
  for(auto &addr: addr_set) {
    Base::TUuid uuid (TUuid::TimeAndMAC);
    /* Don't include sequence of address */
    if (!addr.first.Is<Type::TSeq>()) {
      AddrMap.insert(std::make_pair(uuid, addr));
      ReverseAddrMap.insert(std::make_pair(addr, uuid));
    }
  }

  //TODO: Collect all the object comparisons.

  assert(package);
  //Build all the function declarations
  for(auto &func: package->GetFunctions()) {
    Exports.insert(TExportFunc::New(this, Symbol->GetNamespace(), func));
  }

  //Build the definitions
  for(auto &func: Exports) {
    //TODO: The shared ptr to the Id Gen here is sort of ugly, but it is needed because Scopes hold pointers to id
    //generators and keep them after we finish building the body (Although they should never be touched again).
    func->Build();
  }

  TId<TIdKind::Test>::TGen TestIdGen;

  for(auto &test: package->GetTests()) {
    assert(&test); // To keep GCC from warning about unused variables.
    Tests.push_back(std::make_unique<TTest>(this, test, TestIdGen));
  }
}

TPackage::~TPackage() {}

void TPackage::Emit(const Jhm::TAbsBase &out_dir) const {;
  assert(this);
  assert(&out_dir);

  /* Emit the following files:

     h // Interface which will be used by packages which import this package.
       Include for every object (include all the ones used by our external interface in our external interface)
       Inside the namespace of the package
        Exported function signatures
        The core package API (GetPackageInfo)
     cc // Core implementation code of external interfaces
       Include for all the package interfaces we need
       Include for all the objects we need
       Include for the flux api, orly Rt environment
       Forward declare internal function signatures (I don't think there are any...)
       Define all the EffectBindingSets and AssertionPredicateMaps we need
       Define all the implementation functions
       Build structs for
       Inside the namespace of the package
        Define all the package export functions
        Define GetPackageInfo()

     link.cc // TLinkInfo class, GetApiVesion function.
       Include header for every module in the link
       Define all the API Functions
       Define all the test functions

       Package structures
    orly.sig
     - Function signature of every exported function
  */

  auto dir = Symbol->GetNamespace().Get();
  dir.pop_back();

  /* EXTRA */ {
    std::ostringstream s;
    s << '/' << out_dir << '/' << Join(dir, '/') << '/';
    MakeDirs(s.str().c_str());
  }

  auto emit_code = [&](const std::function<void (TCppPrinter &, const TRelPath &)> &func, const vector<string> &ext) -> void {
    TRelPath rel_path(dir, TName(Symbol->GetNamespace().Get().back(), ext));
    TCppPrinter out(TAbsPath(out_dir, rel_path).AsStr());
    func(out, rel_path);
  };

  /* EXTRA */ {
    std::ostringstream s;
    auto ns = Symbol->GetNamespace().Get();
    ns.pop_back();
    s << '/' << out_dir << '/' << TNamespace(ns) << '/';
    MakeDirs(s.str().c_str());
  }

  emit_code(bind(&TPackage::WriteHeader, this, _1, _2), {"h"});
  emit_code(bind(&TPackage::WriteCc, this, _1, _2), {"cc"});
  emit_code(bind(&TPackage::WriteLink, this, _1, _2), {"link","cc"});
  emit_code(bind(&TPackage::WriteSignatures, this, _1, _2), {"orly", "sig"});


}

void TPackage::EmitObjectHeaders(const Jhm::TAbsBase &out_dir) const {
  for(auto &it: Objects) {
    GenObjHeader(out_dir, it);
  }
}


void TPackage::WriteStartingComment(TCppPrinter &out, const TRelPath &path) const {
  //TODO: Multi-line comment printer in the TCppPrinter
  out << "/* <" << path << ">" << Eol
      << Eol
      << "   This file was generated by the Orly compiler. */" << Eol
      << Eol;
}

void TPackage::WriteHeader(TCppPrinter &out, const TRelPath &path) const {
  //TODO: Reduce number of needed includes.
  WriteStartingComment(out, path);
  out << "#pragma once" << Eol
      << "#include <orly/package/api.h>" << Eol
      << "#include <orly/package/rt.h>" << Eol
      << "#include <orly/rt.h>" << Eol;

  //TODO: Reduce to only objects needed by the export set.
  All(Objects, bind(GenObjInclude, _1, ref(out)));

  out << Eol;

  TOrlyNamespacePrinter ns_printer(Symbol->GetNamespace(), out);
  out << "extern const Orly::Package::TInfo PackageInfo;" << Eol
      << Eol;

  for (const auto &addr : AddrMap) {
    char uuid[37];
    addr.first.FormatUnderscore(uuid);
    out << "extern Base::TUuid My" << uuid;
    addr.first.Format(uuid);
    out << ';' << Eol;
  }

  for (auto &it: Exports) {
    it->WriteDecl(out);
    out << ';' << Eol;
  }
}

void TPackage::WriteCc(TCppPrinter &out, const TRelPath &rel_path) const {

  WriteStartingComment(out, rel_path);
  WriteInclude(out);

  //Include for all the package interfaces we need
  WriteImportIncludes(out);

  //TODO: Reduce to no objects used by exported or imported functions.
  //Include for all the objects we need
  All(Objects, bind(GenObjInclude, _1, ref(out)));

  //Include for the flux api, orly Rt environment
  out << Eol
      << "#include <orly/var/mutation.h>" << Eol
      << Eol
      << "using namespace Orly;" << Eol
      << "using namespace Orly::Rt;" << Eol
      << Eol;

  for(auto &it: NeededObjectComparisons) {
    GenObjComparison(it.first, it.second, out);
  }

  //Define all the unit test functions and their meta information
  for(auto &it: Tests) {
    it->Write(out);
    out << Eol;
  }

  //Define function wrappers for exported functions from generic to specific args.
  for(auto &it: Exports) {
    if(it->GetReturnType().Is<Type::TSeq>()) {
      std::cout << "WARNING: Top level function [" << it->GetName() << "] returning sequence cannot currently be called via the Spa server. Cast your sequence to a list in your orlyscript if you want to call the function" << std::endl;
      continue;
    }
    out << "Atom::TCore RF_" << it->GetName() << "(Package::TContext &ctx, const Package::TArgMap &args) {" << Eol;

    /* indent */ {
      TIndent indent(out);
      out << "assert(&ctx);" << Eol
          << "assert(&args);" << Eol
      //NOTE: we assume that the arguments have been checked for correctness before here. Here, we simply check that
      //at least some checking has been done, then grab the parameters needed for the function and run.
      //Check that the argument
          << "assert(args.size() == " << it->GetArgs().size() << ");" << Eol
          << Eol
          << "void *state_alloc = alloca(Sabot::State::GetMaxStateSize());" << Eol
          << "return Atom::TCore(ctx.GetArena(), Native::State::New("
          << TOrlyNamespace(Symbol->GetNamespace()) << "::F" << it->GetName() << "(ctx";
      if(!it->GetArgs().empty()) {
        out << ", ";
      }
      out
        << Join(it->GetArgs(),
                ", ",
                [](TCppPrinter &out, TFunction::TArgs::const_reference arg) {
                  out << "Sabot::AsNative<" << arg.second->GetType()
                      << ">(*Sabot::State::TAny::TWrapper(args.at(\"" << arg.first
                      << "\").GetState(alloca(Sabot::State::GetMaxStateSize()))))";
                })
        << "), state_alloc));" << Eol;
    }
    out << '}' << Eol;
  }
  out << Eol;

  // Define the package info stucts
  for(auto &it: Exports) {
    if(it->GetReturnType().Is<Type::TSeq>()) {
      continue;
    }
    out << "static const Package::TFuncInfo IF_" << it->GetName() << "{" << Eol;
    /* indent*/ {
      TIndent indent(out);
      out
        << "Package::TParamMap{"
        << Join(it->GetArgs(),
                ", ",
                [](TCppPrinter &out, TFunction::TArgs::const_reference arg) {
                  out << "{\"" << arg.first << "\", ";
                  Type::GenCode(out.GetOstream(), arg.second->GetType());
                  out << '}';
                })
        << "}," << Eol
        << "/* ret */ "; //NOTE: GetOstream at the start of a line will cause indent to not be printed.
      Type::GenCode(out.GetOstream(), it->GetReturnType());
      out << ',' << Eol
          << "RF_" << it->GetName() << Eol;
    }
    out << "};" << Eol;
  }

  //Inside the namespace of the package
  //TODO: Prefix each of the namespaces with a fixed string 'NS_' so we are guaranteed non-conflicting.
  TOrlyNamespacePrinter ns_printer(Symbol->GetNamespace(), out);

  for (const auto &addr : AddrMap) {
    char uuid[37];
    addr.first.FormatUnderscore(uuid);
    out << "Base::TUuid My" << uuid;
    addr.first.Format(uuid);
    out << "(\"" << uuid << "\");" << Eol;
  }

  out << "const Orly::Package::TInfo PackageInfo {" << Eol;
  /* indent */ {
    TIndent indent(out);
    out << '"' << Symbol->GetNamespace() << "\"," << Eol
        << Symbol->GetVersion() << ',' << Eol
        << "/* exports */ std::unordered_map<std::string, const Package::TFuncInfo*>{" << Eol;
    /* indent */ {
      TIndent indent(out);
      //TODO: really want a Base::Join on a filtered list of exports...
      bool first = true;
      for(auto &func: Exports) {
        if(func->GetReturnType().Is<Type::TSeq>()) {
          continue;
        }
        if(first) {
          first = false;
        } else {
          out << ',';
        }
        out << "{\"" << func->GetName() << "\", &IF_" << func->GetName() << "}" << Eol;
      }
    }
    out << "}," << Eol
        << "/* tests */ std::vector<const Package::TTest*>{" << Eol
        << Join(Tests,
                ',',
                [](TCppPrinter &out, const std::unique_ptr<TTest> &test) {
                  out << "&TI_" << test->GetId() << Eol;
                })
        << "}," << Eol
        << "/* index ids */ std::unordered_set<Base::TUuid *>{" << Eol;
    /* indent */ {
      TIndent indent(out);
      out << Join(AddrMap,
                  ',',
                  [](TCppPrinter &out, const TAddrMap::value_type &addr_pair) {
                    char uuid[37];
                    addr_pair.first.FormatUnderscore(uuid);
                    out << "&My" << uuid << Eol;
                  });
    }
    out << '}' << Eol;

  }
  out << "};" << Eol
      << Eol;

  //  Define all the package export functions
  for(auto &it: Exports) {
    it->WriteDef(out);
    out << Eol
        << Eol;
  }
}

void TPackage::WriteLink(TCppPrinter &out, const TRelPath &path) const {
  WriteStartingComment(out, path);
  out << "#include <unordered_map>" << Eol
      << "#include <utility>" << Eol
      << Eol
      << "#include <base/uuid.h>" << Eol
      << "#include <orly/rt.h>" << Eol
      << "#include <orly/shared_enum.h>" << Eol
      << "#include <orly/type/impl.h>" << Eol
      << Eol;
  // Include header for every module in the link
  WriteInclude(out);
  WriteImportIncludes(out);

  // Define all the link test object, TLinkInfo struct.
  out << Eol
      << "static Orly::Package::TLinkInfo LinkInfo {" << Eol;
  /* instance body*/ {
    TIndent indent(out);
    out << '"' << Symbol->GetNamespace() << "\"," << Eol
        << Symbol->GetVersion() << ',' << Eol
        << '&' << TOrlyNamespace(Symbol->GetNamespace()) << "::PackageInfo," << Eol
        << "std::unordered_map<std::vector<std::string>, const Orly::Package::TInfo *>{" << Eol;
    /* included packages */ {
      TIndent package_indent(out);
      out << Join(NeededPackages,
                  ", ",
                  [](TCppPrinter &out, const Package::TName &name) {
                    out
                      << "{{\""
                      << Join(name.Get(), "\", \"")
                      << "\"}, &" << TOrlyNamespace(name.Get()) << "::PackageInfo}";
                  });
    }
    out
      << "}," << Eol
      << "std::unordered_map<Base::TUuid, std::pair<Orly::Type::TType, Orly::Type::TType>>{" << Eol
      << Join(AddrMap,
              ", \n",
              [](TCppPrinter &out, const TAddrMap::value_type &pair) {
                class t_addr_printer final
                  : public Type::TType::TVisitor {
                  public:

                  t_addr_printer(TCppPrinter &out)
                    : Out(out) {}

                  virtual void operator()(const Type::TAddr     *that) const {
                    bool is_sequence = false;
                    for (auto member : that->GetElems()) {
                      is_sequence |= member.second.Is<Type::TSeq>();
                    }

                    if (is_sequence) {
                      Out << "Orly::Type::TSeq::Get(";
                    }

                    Out
                      << "Orly::Type::TAddr::Get(std::vector<std::pair<Orly::TAddrDir, Orly::Type::TType>> {\n"
                      << Join(that->GetElems(),
                              ", \n",
                              [this](TCppPrinter &out, const std::pair<TAddrDir, Type::TType> &elem) {
                                out << "          std::make_pair<Orly::TAddrDir, Orly::Type::TType>(" <<
                                    ((elem.first == Orly::TAddrDir::Asc) ?
                                      "Orly::TAddrDir::Asc, " :
                                      "Orly::TAddrDir::Desc, ");
                                elem.second.Accept(*this);
                                out << ")";
                              })
                      << "})";

                    if (is_sequence) {
                      /* Close off the extra open paren introduced to make a sequence*/
                      Out << ")";
                    }
                  };
                  virtual void operator()(const Type::TAny      *) const { Out << "Orly::Type::TAny::Get()"; };
                  virtual void operator()(const Type::TBool     *) const { Out << "Orly::Type::TBool::Get()"; };
                  virtual void operator()(const Type::TDict     *that) const {
                    Out << "Orly::Type::TDict::Get(";
                    that->GetKey().Accept(*this);
                    Out << ", ";
                    that->GetVal().Accept(*this);
                    Out << ")";
                  };
                  virtual void operator()(const Type::TErr      *) const {};
                  virtual void operator()(const Type::TFunc     *) const {};
                  virtual void operator()(const Type::TId       *) const { Out << "Orly::Type::TId::Get()"; };
                  virtual void operator()(const Type::TInt      *) const { Out << "Orly::Type::TInt::Get()"; };
                  virtual void operator()(const Type::TList     *that) const {
                    Out << "Orly::Type::TList::Get(";
                    that->GetElem().Accept(*this);
                    Out << ")";
                  };
                  virtual void operator()(const Type::TMutable  *that) const { that->GetSrcAtAddr().Accept(*this); };
                  virtual void operator()(const Type::TObj      *that) const {
                    Out
                      << "Orly::Type::TObj::Get(std::map<std::string, Orly::Type::TType> {"
                      << Join(that->GetElems(),
                              ", ",
                              [this](TCppPrinter &out, const std::pair<string, Type::TType> &elem) {
                                out << "{"
                                    << "\"" << elem.first << "\""
                                    << ",";
                                elem.second.Accept(*this);
                                out << "}";
                              })
                      << "})";
                  };
                  virtual void operator()(const Type::TOpt      *that) const {
                    Out << "Orly::Type::TOpt::Get(";
                    that->GetElem().Accept(*this);
                    Out << ")";
                  };
                  virtual void operator()(const Type::TReal     *) const { Out << "Orly::Type::TReal::Get()"; };
                  virtual void operator()(const Type::TSeq      *that) const {
                    Out << "Orly::Type::TSeq::Get(";
                    that->GetElem().Accept(*this);
                    Out << ")";
                  };
                  virtual void operator()(const Type::TSet      *that) const {
                    Out << "Orly::Type::TSet::Get(";
                    that->GetElem().Accept(*this);
                    Out << ")";
                  };
                  virtual void operator()(const Type::TStr      *) const { Out << "Orly::Type::TStr::Get()"; };
                  virtual void operator()(const Type::TTimeDiff *) const { Out << "Orly::Type::TTimeDiff::Get()"; };
                  virtual void operator()(const Type::TTimePnt  *) const { Out << "Orly::Type::TTimePnt::Get()"; };

                  private:
                  TCppPrinter &Out;
                };
                /* Keys */
                char uuid[37];
                pair.first.Format(uuid);
                out << "    { Base::TUuid(\"" << uuid << "\"), \n";
                /* Value*/
                out << "      std::make_pair<Orly::Type::TType, Orly::Type::TType>(" << Eol;
                out << "        ";
                pair.second.first.Accept(t_addr_printer(out));
                out << "," << Eol;
                out << "        ";
                pair.second.second.Accept(t_addr_printer(out));
                out << ") }";
              })
      << "}" << Eol;
  }
  out << "};" << Eol
      << Eol
      << "extern \"C\" Orly::Package::TLinkInfo *GetLinkInfo() {" << Eol
      << "  return &LinkInfo;" << Eol
      << '}' << Eol
      << Eol
      << "extern \"C\" int32_t GetApiVersion() {" << Eol
      << "  return " << ORLY_API_VERSION << ";" << Eol
      << '}' << Eol;
  out << Eol;
}

void TPackage::WriteSignatures(TCppPrinter &out, const TRelPath &) const {
  for(auto &it: Exports) {
    out << it->GetName() << " is ";
    Type::Orlyify(out.GetOstream(), it->GetType());
    out << ";" << Eol;
  }
}

void WritePackageInclude(const Jhm::TNamespace &ns, TCppPrinter &out) {
  out << "#include \"" << ns << ".h\"" << Eol;
}

void TPackage::WriteInclude(TCppPrinter &out) const {
  WritePackageInclude(Symbol->GetNamespace(), out);
}

void TPackage::WriteImportIncludes(TCppPrinter &out) const {
  if(!NeededPackages.empty()) {
    out << Eol;
  }
  for(auto &it: NeededPackages) {
    WritePackageInclude(it, out);
  }
}
