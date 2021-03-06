/* <orly/synth/dict_ctor.cc>

   Implements <orly/synth/dict_ctor.h>.

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

#include <orly/synth/dict_ctor.h>

#include <cassert>

#include <orly/error.h>
#include <orly/expr/dict.h>
#include <orly/synth/cst_utils.h>
#include <orly/synth/get_pos_range.h>
#include <tools/nycr/error.h>

using namespace Orly;
using namespace Orly::Synth;

TDictCtor::TDictCtor(const TExprFactory *expr_factory, const Package::Syntax::TDictCtor *dict_ctor)
    : DictCtor(Base::AssertTrue(dict_ctor)) {
  assert(expr_factory);
  try {
    ForEach<Package::Syntax::TDictMember>(DictCtor->GetDictMemberList(),
        [this, expr_factory](const Package::Syntax::TDictMember *dict_member) -> bool {
          auto result = Members.emplace(std::make_pair(expr_factory->NewExpr(dict_member->GetKey()),
                                                       expr_factory->NewExpr(dict_member->GetValue())));
          assert(result.second);
        return true;
        });
  } catch (...) {
    Cleanup();
    throw;
  }
  if (Members.empty()) {
    throw TCompileError(HERE, GetPosRange(DictCtor), "Dictionary is empty. Use empty constructor instead.");
  }
}

TDictCtor::~TDictCtor() {
  Cleanup();
}

Expr::TExpr::TPtr TDictCtor::Build() const {
  assert(this);
  Expr::TDict::TMemberMap members;
  for (auto member : Members) {
    /* TODO: Detect duplicate constant keys and throw */
    auto result = members.emplace(std::make_pair((member.first)->Build(), (member.second)->Build()));
    assert(result.second);
  }
  return Expr::TDict::New(members, GetPosRange(DictCtor));
}

void TDictCtor::Cleanup() {
  assert(this);
  for (auto member : Members) {
    delete member.first;
    delete member.second;
  }
}

void TDictCtor::ForEachInnerScope(const std::function<void (TScope *)> &cb) {
  assert(this);
  assert(&cb);
  assert(cb);
  for (auto member : Members) {
    (member.first)->ForEachInnerScope(cb);
    (member.second)->ForEachInnerScope(cb);
  }
}

void TDictCtor::ForEachRef(const std::function<void (TAnyRef &)> &cb) {
  assert(this);
  assert(&cb);
  assert(cb);
  for (auto member : Members) {
    (member.first)->ForEachRef(cb);
    (member.second)->ForEachRef(cb);
  }
}
