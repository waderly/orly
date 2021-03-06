/* <orly/compiler.cc>

   Implements <orly/compiler.h>

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

#include <orly/compiler.h>

#include <cassert>
#include <fstream>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <unistd.h>
#include <unordered_map>

#include <base/os_error.h>
#include <base/piece.h>
#include <base/split.h>
#include <base/source_root.h>
#include <base/subprocess.h>
#include <base/thrower.h>
#include <orly/code_gen/package.h>
#include <orly/orly.package.cst.h>
#include <orly/synth/package.h>
#include <strm/fd.h>
#include <strm/bin/in.h>
#include <tools/nycr/error.h>

using namespace std;
using namespace Orly;
using namespace Orly::Compiler;
using namespace Jhm;

/* Compiles a orlyscript package. On construction it builds to the symbolic layer. It can be asked to emit the
   necessary C++ */
class TPackageBuilder {
  NO_COPY(TPackageBuilder);
  public:

  //TODO: Version report?
  TPackageBuilder(const TRelPath &rel_path) : RelPath(rel_path) {}
  TPackageBuilder(TPackageBuilder &&that)
      : RelPath(move(that.RelPath)), Cst(move(that.Cst)), Synth(move(that.Synth)) {}

  void BuildSymbols(const TAbsBase &src_root) {
    assert(this);
    Cst = Package::Syntax::TPackage::ParseFile(TAbsPath(src_root, RelPath).AsStr().c_str());
    if (!HasErrors()) {
      assert(Cst);
      Synth = make_unique<Synth::TPackage>(RelPath.ToNamespaceIncludingName(), &*Cst, false);
    }
  }

  bool HasErrors() {
    assert(this);
    return Tools::Nycr::TError::GetFirstError() != 0;
  }

  void PrintErrors(ostream &out) const {
    assert(this);
    Tools::Nycr::TError::PrintSortedErrors(out);
    Tools::Nycr::TError::DeleteEach();
  }

  //TODO: be able to use temp filenames again?
  /* Generates the '.h' interfaces, as well as the '.cc' implementations. */
  void GenerateIntermediateCode(const TAbsBase &out_root) {
    assert(this);
    //Generate the CodeGen representation
    CodeGen::TPackage cg(Synth->GetSymbol());
    //Spit out the cc file for the package
    cg.Emit(out_root);

    //Spit out the language object files as necessary (Everything in the header to simplify linking.)
    //TODO: Don't re-emit already emitted object headers.
    cg.EmitObjectHeaders(Jhm::TAbsBase(Base::GetSrcRoot() + "/orly/rt/objects"));
  }

  Jhm::TNamespace GetNamespace() const {
    assert(this);
    assert(Synth);
    return Synth->GetNamespace();
  }

  unsigned int GetVersion() const {
    assert(this);
    assert(Synth);

    return Synth->GetVersion();
  }

  void TypeCheck() const {
    assert(this);
    assert(Synth);
    Synth->GetSymbol()->TypeCheck();
  }

  private:
  TRelPath RelPath;

  unique_ptr<Package::Syntax::TPackage> Cst;
  //note: symbolic is owned by synth, and we don't have a good reason to pull it out at the moment.
  unique_ptr<Synth::TPackage> Synth;

};  // TPackageBuilder

/* Nabbed by Compile() to prevent multiple threads from trying to compile. */
static mutex Compiling;

//Note: This should probably be promted to a compile management class.
//TODO: Reintroduce machine mode, not saving cc. Also reintroduce syntax check only and semantic check only compilation.
/* Returns the versioned package name of the final build target. */
Package::TVersionedName Orly::Compiler::Compile(
      const TAbsPath &core_file,
      const TAbsBase &out_tree,
      bool /*found_root*/, //Only if we found the root can we look at sub scopes.
      bool debug_cc,
      bool machine_mode,
      ostream &out_strm) {

  lock_guard<mutex> lock_compiling(Compiling);

  const TAbsBase &src_tree = core_file.GetAbsBase();
  const TRelPath &core_rel = core_file.GetRelPath();


  typedef unordered_map<TRelPath, unique_ptr<TPackageBuilder>> TPackageMap;
  TPackageMap packages;
  packages.insert(make_pair(core_rel, make_unique<TPackageBuilder>(core_rel)));

  bool failed = false;

  /* extra */ {
    queue<TRelPath> todo;
    todo.push(core_rel);

    do {
      auto cur = todo.front();
      todo.pop();

      if (cur.GetName().GetExtensions().size() != 1 || cur.GetName().GetExtensions()[0] != "orly") {
        out_strm << "Invalid package name. Package names may not contain a '.'. This means, for example, a source file can only be 'a.orly', not 'a.foo.orly'" << endl;
        failed = true;
        break;
      }

      auto &builder = packages[cur];
      if (machine_mode) {
        out_strm << "MM_NOTICE: Synth + Symbols" << endl;
      }
      builder->BuildSymbols(src_tree);
      if (builder->HasErrors()) {
        out_strm << "Errors in: " << cur << endl;
        builder->PrintErrors(out_strm);
        failed = true;
        break;
      }
      if (machine_mode) {
        // TODO: UNCOMMENT
        // out_strm << "MM_NOTICE: TypeCheck" << endl;
      }
      builder->TypeCheck();
      if (builder->HasErrors()) {
        out_strm << "Errors in: " << cur << endl;
        builder->PrintErrors(out_strm);
        failed = true;
        break;
      }
      //TODO: Imports
      /* TODO
      for (const auto &dep: builder->GetSymbols().GetImportPackages()) {
        if (packages.count(dep)) {
          break;
        }
        if (!found_root) {
          throw TCompileFailure(HERE, "Unless you're in a Orly package tree (Has a __orly__ file in the tld), you can not use scopes")
        }
        packages.insert(make_pair(dep, unique_ptr<TPackageBuilder>(new TPackageBuilder(dep))));
        todo.push(dep);
      }
      */
      if (machine_mode) {
        out_strm << "MM_NOTICE: Code Gen" << endl;
      }
      //TODO: We only need to generate the C++ if we don't already have up to date C++. Note that doing this means we need
      //      to always gen to a temp file, then move it into place to be atomic in case someone uses Ctrl-C.
      builder->GenerateIntermediateCode(out_tree);
      if (packages[cur]->HasErrors()) {
        //TODO: It would be nice not to have this duplication.
        out_strm << "Errors in: " << cur << endl;
        builder->PrintErrors(out_strm);
        failed = true;
        break;
      }
    } while (!todo.empty());
  }

  if(failed) {
    throw TCompileFailure(HERE, "Compiling Orly language");
  }

  /* extra */ {
    if(machine_mode) {
      out_strm << "MM_NOTICE: Compiling C++" << endl;
    }
    stringstream args;
    /* extra */ {
      //Take all the packages needed directly or indirectly by the compilation and link  them together in one swoop.
      std::ostringstream version_str_builder;
      version_str_builder << packages[core_rel]->GetVersion();

      TAbsPath out_path(out_tree, core_rel.SwapLastExtension({version_str_builder.str(), "so"}));
      //TODO: Check these compile flags.
      args << "g++ -std=c++1y -x c++ -I" << Base::GetSrcRoot() << " -fPIC -shared -o"<< out_path << " -iquote " << out_tree << ' '
           << TAbsPath(out_tree, core_rel.SwapLastExtension(Jhm::TStrList{"link","cc"}))
           << Base::Join(packages,
                         ' ',
                         [&out_tree](ostream &strm, const TPackageMap::value_type &that) {
                           strm << ' ' << TAbsPath(out_tree, that.first.SwapLastExtension("cc"));
                         });
      if (debug_cc) {
        args << " -g -Wno-unused-variable -Wno-type-limits -Werror -Wno-parentheses -Wall -Wextra -Wno-unused-parameter";
      } else {
        //TODO: Better optimization flags.
        args << " -O2 -DNDEBUG";
      }
    }

    Base::TPump pump;
    auto subproc = Base::TSubprocess::New(pump, args.str().c_str());
    auto status = subproc->Wait();
    if (status || failed) {
      if (debug_cc) {
        Base::EchoOutput(subproc->TakeStdOutFromChild());
        Base::EchoOutput(subproc->TakeStdErrFromChild());
      }

      //NOTE: use '-d' to get the error messages.
      out_strm << "Error while compiling an Intermediate Representation. See a Orly team member with your Orly code for support" << endl;
      throw TCompileFailure(HERE, "Compiling C++ and linking");
    }
  }

  /* TODO
  if(!save_cc) {
    unlink(filenames.GetCcPath().c_str());
  }
  */

  return Package::TVersionedName{packages[core_rel]->GetNamespace(), packages[core_rel]->GetVersion()};
}
