/* <tools/nycr/nycr.cc>

   A compiler-compiler-compiler!

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

#include <cassert>
#include <exception>
#include <fstream>
#include <iostream>
#include <unistd.h>

#include <base/cmd.h>
#include <base/split.h>
#include <base/thrower.h>
#include <tools/nycr/error.h>
#include <tools/nycr/build.h>
#include <tools/nycr/cst_redirect.h>
#include <tools/nycr/symbol/bootstrap.h>
#include <tools/nycr/symbol/language.h>
#include <tools/nycr/symbol/write_bison.h>
#include <tools/nycr/symbol/write_cst.h>
#include <tools/nycr/symbol/write_dump.h>
#include <tools/nycr/symbol/write_flex.h>
#include <tools/nycr/symbol/write_nycr.h>
#include <tools/nycr/symbol/write_xml.h>

using namespace std;
using namespace Tools::Nycr;

class TNycr : public Base::TCmd {
  NO_COPY(TNycr);
  public:

  TNycr(int argc, char *argv[])
      : Bootstrap(false), LanguageReport(false) {
    Parse(argc, argv, TMeta());
  }

  int Run() {
    assert(this);
    int result;
    try {
      //TODO: This should go in the arg verification logic...
      if (!LanguageReport) {
        if (!Atom.size()) {
          THROW << "atom (-a) not specified";
        }

        if (!Branch.size()) {
          THROW << "branch (-p) not specified";
        }
        if (!Root.size()) {
          THROW << "root directory (-r) not specified";
        }
      }

      // build langauge object(s) either by parsing or bootstrapping
      if (!Bootstrap) {
        if(!Compiland.size()) {
          THROW << "no compiland";
        }
        auto nycr = Syntax::TNycr::ParseFile(Compiland.c_str());
        if (!TError::GetFirstError()) {
          Build(nycr.get());
        }
      } else {
        if (Compiland.size()) {
          THROW << "bootstrap option cannot be used with compilands";
        }
        Symbol::Bootstrap();
      }

      // if we have no errors, write output; otherwise, show the errors
      if (!TError::GetFirstError()) {
        const Symbol::TLanguage::TLanguages &languages = Symbol::TLanguage::GetLanguages();
        if (LanguageReport && !LanguageReportFile.empty()) {
          ofstream out(LanguageReportFile);
          if (!out.is_open()) {
            THROW << "Unable to open language report output file " << LanguageReportFile;
          }

          // NOTE: We're hand-converting the list into json, because that's wayyyy easier for now
          out
            << '['
            << Base::Join(languages,
                          ", ",
                          [](ostream &strm, const Symbol::TLanguage *language) {
                            strm << '"' << Symbol::TLower(language->GetName()) << '"';
                          })
            << ']';
        }
        while (!languages.empty()) {
          Symbol::TLanguage *language = *languages.begin();
          if (LanguageReport) {
            if (LanguageReportFile.empty()) {
              cout << Symbol::TLower(language->GetName()) << endl;
            }
          } else {
            //TODO: Should probably change this to constant ref in the std::string.
            WriteCst(Root.c_str(), Branch.c_str(), Atom.c_str(), language);
            WriteDump(Root.c_str(), Branch.c_str(), Atom.c_str(), language);
            WriteBison(Root.c_str(), Branch.c_str(), Atom.c_str(), language);
            WriteFlex(Root.c_str(), Branch.c_str(), Atom.c_str(), language);
            WriteXml(Root.c_str(), Branch.c_str(), Atom.c_str(), language);
            WriteNycr(Root.c_str(), Branch.c_str(), Atom.c_str(), language);
          }
          delete language;
        }
        result = EXIT_SUCCESS;
      } else {
        TError::PrintSortedErrors(cerr);
        result = EXIT_FAILURE;
      }
    } catch (const exception &ex) {
      cerr << "exception: " << ex.what() << endl;
      result = EXIT_FAILURE;
    }
    return result;
  }

  private:

  class TMeta : public Base::TCmd::TMeta {
    public:
    TMeta() : Base::TCmd::TMeta("Nycr compiler compiler") {
      Param(&TNycr::Atom, "atom", Optional, "atom\0a\0", "The JHM atom to use for output");
      Param(&TNycr::Bootstrap, "bootstrap", Optional, "bootstra\0b\0", "Run in bootstrap mode");
      Param(&TNycr::LanguageReport, "language_report", Optional, "language-report\0l\0", "Show language report");
      Param(&TNycr::LanguageReportFile,
            "language_report_file",
            Optional,
            "language-report-file\0",
            "Write language report to the given file instead of stdout. '-l' must be given.");
      Param(&TNycr::Branch, "branch", Optional, "branch\0p\0", "The JHM branch name to use for output");
      Param(&TNycr::Root, "root", Optional, "root\0r\0", "The JHM root to use");
      Param(&TNycr::Compiland, "compiland", Required, "The source file to use");
    };


    private:
    virtual void WriteAfterDesc(std::ostream &strm) const {
      assert(this);
      assert(&strm);
      strm << "Build: Unknown" << endl //TODO: Use Version from SCM.
           << endl
           << "Copyright OrlyAtomics, Inc." << endl
           << "Licensed under the Apache License, Version 2.0" << endl;
    }
  };

  virtual TCmd::TMeta *NewMeta() const {
    assert(this);
    return new TMeta();
  }

  bool Bootstrap, LanguageReport;
  std::string LanguageReportFile;
  std::string Atom, Branch, Compiland, Root;
};


int main(int argc, char *argv[]) {
  // process command line
  return TNycr(argc, argv).Run();
}
