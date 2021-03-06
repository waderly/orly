/* <orly/synth/unknown_ctor.cc>

   Implements <orly/synth/unknown_ctor.h>.

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

#include <orly/synth/unknown_ctor.h>

#include <cassert>

#include <base/assert_true.h>
#include <orly/expr/unknown.h>
#include <orly/synth/get_pos_range.h>
#include <orly/synth/new_type.h>
#include <tools/nycr/error.h>

using namespace Orly;
using namespace Orly::Synth;

TUnknownCtor::TUnknownCtor(const Package::Syntax::TUnknownCtor *unknown_ctor)
    : UnknownCtor(Base::AssertTrue(unknown_ctor)),
      Type(NewType(UnknownCtor->GetType())) {}

TUnknownCtor::~TUnknownCtor() {
  assert(this);
  delete Type;
}

Expr::TExpr::TPtr TUnknownCtor::Build() const {
  assert(this);
  return Expr::TUnknown::New(Type->GetSymbolicType(), GetPosRange(UnknownCtor));
}

void TUnknownCtor::ForEachRef(const std::function<void (TAnyRef &)> &cb) {
  assert(this);
  assert(&cb);
  assert(cb);
  Type->ForEachRef(cb);
}
