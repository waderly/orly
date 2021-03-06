/* <orly/code_gen/test.cc>

   Implements <orly/code_gen/test.h>

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

#include <orly/code_gen/test.h>

#include <base/not_implemented.h>
#include <base/split.h>
#include <orly/code_gen/builder.h>
#include <orly/code_gen/context.h>
#include <orly/code_gen/inner_func.h>
#include <orly/expr/util.h>
#include <orly/expr/walker.h>
#include <orly/expr/where.h>
#include <orly/type/bool.h>
#include <tools/nycr/pos_range.h>

using namespace std;
using namespace Orly;
using namespace Orly::CodeGen;

template <>
void TCppPrinter::Write(const Tools::Nycr::TPos &pos) {
  assert(this);

  *this << "Tools::Nycr::TPos(" << pos.GetLineNumber() << ", " << pos.GetColumnNumber() << ')';
}

template <>
void TCppPrinter::Write(const TPosRange &pos_range) {
  *this << "Tools::Nycr::TPosRange(" << pos_range.GetStart() << ", " << pos_range.GetLimit() << ')';
}

TTestCase::TTestCase(const L0::TPackage *package,
                     const Symbol::Test::TTestCase::TPtr &symbol,
                     TId<TIdKind::Test>::TGen &id_gen)
    : TFunction(package, TIdScope::New()), TTopFunc(package), Id(id_gen.New()), Pos(symbol->GetPosRange()) {

  PostCtor({}, symbol->GetExpr(), false);

  if(symbol->HasName()) {
    Name = symbol->GetName();
  }

  if(symbol->GetOptTestCaseBlock()) {
    OptChildren = make_unique<TTestBlock>(package, symbol->GetOptTestCaseBlock(), id_gen);
  }

  Build();
}

void TTestCase::WriteCcName(TCppPrinter &out) const {
  assert(this);

  out << Id;
}

const TId<TIdKind::Test> &TTestCase::GetId() const {
  assert(this);

  return Id;
}

string TTestCase::GetName() const {
  NOT_IMPLEMENTED();
}

Type::TType TTestCase::GetReturnType() const {
  return Type::TBool::Get();
}

Type::TType TTestCase::GetType() const {
  return Type::TFunc::Get(Type::TObj::Get({}), GetReturnType());
}

void TTestCase::Write(TCppPrinter &out) const {
  assert(this);
  assert(&out);

  WriteDef(out);
  out << Eol << Eol;
  WriteWrapperFunction(out);

  //Seperate the function definition and info structure.
  out << Eol << Eol;

  if (OptChildren) {
    OptChildren->Write(out);
  }

  out << "static const Package::TFuncInfo TF_" << Id << " {" << Eol;
  /* indent */ {
    TIndent indent(out);
    out << "Package::TParamMap{}," << Eol
        << "/* ret */ Orly::Type::TBool::Get()," << Eol
        << "RT_" << Id << Eol;
  }
  out << "};" << Eol << Eol;

  out << "static const Package::TTestCase TI_" << Id << " {" << Eol;
  /* indent */ {
    TIndent indent(out);
    out << '"' << Name << "\"," << Pos << ',' << Eol
        << "&TF_" << Id << ", ";
    if(OptChildren) {
      OptChildren->WriteMeta(out);
    } else {
      out << "Package::TTestBlock{}";
    }
    out << Eol;
  }
  out << "};" << Eol
      << Eol;

}

void TTestCase::WriteName(TCppPrinter &out) const {
  assert(this);
  assert(&out);

  out << Id;
}

void TTestCase::WriteWrapperFunction(TCppPrinter &out) const {
  out << "Atom::TCore RT_" << Id
      << "(Package::TContext &ctx, const Package::TArgMap &args) {" << Eol;
  /* indent */ {
    TIndent indent(out);
    out << "assert(&ctx);" << Eol
        << "assert(&args);" << Eol
        << "assert(args.empty());" << Eol
        << Eol
        << "void *state_alloc = alloca(Sabot::State::GetMaxStateSize());" << Eol
        << "return Atom::TCore(ctx.GetArena(), Native::State::New(" << Id
        << "(ctx), state_alloc));" << Eol;
  }
  out << '}' << Eol;
}

TTestBlock::TTestBlock(const L0::TPackage *package,
                       const Symbol::Test::TTestCaseBlock::TPtr &symbol,
                       TId<TIdKind::Test>::TGen &id_gen) {
  try {
    for(auto &it: symbol->GetTestCases()) {
      auto tc_ptr = make_unique<TTestCase>(package, it, id_gen);
      Children.push_back(tc_ptr.get());
      tc_ptr.release();
    }
  } catch(...) {
    for(auto &it: Children) {
      delete it;
    }
    throw;
  }
}

TTestBlock::~TTestBlock() {
  for(auto &it: Children) {
    delete it;
  }
}

void TTestBlock::Write(TCppPrinter &out) const {
  assert(this);
  assert(&out);

  for(auto &test: Children) {
    test->Write(out);
  }
}

void TTestBlock::WriteMeta(TCppPrinter &out) const {
  assert(this);
  assert(&out);

  out
    << "Package::TTestBlock{"
    << Base::Join(Children,
                  ", ",
                  [](TCppPrinter &out, const TTestCase *test) {
                    out << "&TI_" << test->GetId();
                  })
    << '}';
}

void TWith::Write(TCppPrinter &out) const {

  //TODO: This is copied from TFunction
  //Write out inner functions (forward decl followed by definitions).
  for(auto &func: ChildFuncs) {
    func->WriteDecl(out);
    out << ';' << Eol;
  }

  if(!ChildFuncs.empty()) {
    out << Eol;
  }

  for(auto &func: ChildFuncs) {
    func->WriteDef(out);
    out << ';' << Eol;
  }

  if(!ChildFuncs.empty()) {
    out << Eol;
  }

  CodeScope->WriteStart(out);
  for(auto &it: News) {
    //NOTE: The duplicate code here to print out new effects is sort of ugly, but way easier than not doing it du
    const Base::TUuid &index_id = Package->GetIndexIdFor(it.first->GetReturnType(), it.second->GetReturnType());
    char uuid[37];
    index_id.FormatUnderscore(uuid);
    out << "ctx.AddEffect(" << it.first << ", " << TOrlyNamespace(Package->GetNamespace()) << "::My" << uuid
        << ", Var::TNew::New(Var::ToVar(*Sabot::State::TAny::TWrapper(Native::State::New(" << it.second
        << ", alloca(Sabot::State::GetMaxStateSize()))))));" << Eol;
  }
  out << "return true;" << Eol;
}

TWith::TWith(const L0::TPackage *package, const Symbol::Test::TWithClause::TPtr &symbol) :  CodeScope(new TCodeScope(TIdScope::New())), Package(package) {
  TScopeCtx ctx(CodeScope.get());

  //Gather child functions
  auto gather_child_functions = [this, package](const Expr::TExpr::TPtr &expr) {
    //TODO: This is copied from function.cc TFunction ctor.
    Expr::ForEachExpr(expr, [this, package](const Expr::TExpr::TPtr &expr) {
      const Expr::TWhere *where = expr->TryAs<Expr::TWhere>();
      if(where) {
        for(auto &func: where->GetFunctions()) {
          ChildFuncs.insert(TInnerFunc::New(package, func, CodeScope->GetIdScope()));
        }
      }
      return false;
    });
  };

  for(auto &it: symbol->GetNewStmts()) {
    gather_child_functions(it->GetLhs()->GetExpr());
    gather_child_functions(it->GetRhs()->GetExpr());
  }

  //TODO: This is copied from TFunction
  for(auto &it: ChildFuncs) {
    it->Build();
  }

  for(auto &it: symbol->GetNewStmts()) {
    News.push_back({BuildInline(package, it->GetLhs()->GetExpr(), false), BuildInline(package, it->GetRhs()->GetExpr(), false)});
  }
}

TTest::TTest(const L0::TPackage *package, const Symbol::Test::TTest::TPtr &symbol, TId<TIdKind::Test>::TGen &id_gen)
    : Id(id_gen.New()), Tests(package, symbol->GetTestCaseBlock(), id_gen) {

  if(symbol->GetOptWithClause()) {
    //TODO: http://www.open-std.org/jtc1/sc22/wg21/docs/lwg-active.html#2070
    OptWith = std::unique_ptr<TWith>(new TWith(package, symbol->GetOptWithClause()));
  }
}

const TId<TIdKind::Test> &TTest::GetId() const {
  assert(this);

  return Id;
}

void TTest::Write(TCppPrinter &out) const {
  assert(this);
  assert(&out);

  Tests.Write(out);

  if(OptWith) {
    //NOTE: We print out the function manually because it has soo little in common with TFunction's work.
    out << "bool TW_" << Id << "(Package::TContext &ctx) {" << Eol;
    /* indent */ {
      TIndent indent(out);
      OptWith->Write(out);
    }
    out << '}' << Eol
        << Eol;
    WriteWrapperFunction(out);
    out << "static const Package::TFuncInfo IW_" << Id << " {" << Eol;
    /* indent */ {
      TIndent indent(out);
      out << "Package::TParamMap{}," << Eol
          << "/* ret */ Orly::Type::TBool::Get()," << Eol
          << "RW_" << Id << Eol;
    }
    out << "};" << Eol
        << Eol;
  }

  out << "static const Package::TTest TI_" << Id << " {" << Eol;
  /* indent */ {
    TIndent indent(out);
    if(OptWith) {
      out << "&IW_" << Id;
    } else {
      out << "nullptr";
    }
    out << ',' << Eol;
    Tests.WriteMeta(out);
    out << Eol;
  }
  out << "};" << Eol << Eol;
}

void TTest::WriteWrapperFunction(TCppPrinter &out) const {
  out << "Atom::TCore RW_" << Id
      << "(Package::TContext &ctx, const Package::TArgMap &args) {" << Eol;
  /* indent */ {
    TIndent indent(out);
    out << "assert(&ctx);" << Eol
        << "assert(&args);" << Eol
        << "assert(args.empty());" << Eol
        << Eol
        << "void *state_alloc = alloca(Sabot::State::GetMaxStateSize());" << Eol
        << "return Atom::TCore(ctx.GetArena(), Native::State::New(TW_" << Id
        << "(ctx), state_alloc));" << Eol;
  }
  out << '}' << Eol << Eol;
}
