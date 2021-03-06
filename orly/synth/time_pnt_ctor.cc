/* <orly/synth/time_pnt_ctor.cc>

   Implements <orly/synth/time_pnt_ctor.h>.

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

#include <orly/synth/time_pnt_ctor.h>

#include <base/assert_true.h>
#include <orly/expr/ref.h>
#include <orly/symbol/built_in_function.h>
#include <orly/synth/get_pos_range.h>
#include <tools/nycr/error.h>

using namespace Orly;
using namespace Orly::Synth;

TTimePntCtor::TTimePntCtor(const Package::Syntax::TTimePntCtor *time_pnt_ctor)
    : TimePntCtor(Base::AssertTrue(time_pnt_ctor)) {}

Expr::TExpr::TPtr TTimePntCtor::Build() const {
  return Expr::TRef::New(Symbol::TBuiltInFunction::GetTimePnt()->GetResultDefs()[0], GetPosRange(TimePntCtor));
}
