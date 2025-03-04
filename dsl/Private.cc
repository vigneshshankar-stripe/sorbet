#include "dsl/Private.h"
#include "ast/Helpers.h"
#include "ast/ast.h"
#include "core/Context.h"
#include "core/Names.h"
#include "core/core.h"
#include "core/errors/dsl.h"
#include "dsl/dsl.h"

using namespace std;

namespace sorbet::dsl {

vector<unique_ptr<ast::Expression>> Private::replaceDSL(core::MutableContext ctx, ast::Send *send) {
    vector<unique_ptr<ast::Expression>> empty;

    if (send->args.size() != 1) {
        return empty;
    }

    auto mdef = ast::cast_tree<ast::MethodDef>(send->args[0].get());
    if (mdef == nullptr) {
        return empty;
    }

    if (send->fun == core::Names::private_() && mdef->isSelf()) {
        if (auto e = ctx.state.beginError(send->loc, core::errors::DSL::PrivateMethodMismatch)) {
            e.setHeader("Use `{}` to define private class methods", "private_class_method");
            auto beginPos = send->loc.beginPos();
            auto replacementLoc = core::Loc{send->loc.file(), beginPos, beginPos + 7};
            e.replaceWith(replacementLoc, "private_class_method");
        }
    } else if (send->fun == core::Names::privateClassMethod() && !mdef->isSelf()) {
        if (auto e = ctx.state.beginError(send->loc, core::errors::DSL::PrivateMethodMismatch)) {
            e.setHeader("Use `{}` to define private instance methods", "private");
            auto beginPos = send->loc.beginPos();
            auto replacementLoc = core::Loc{send->loc.file(), beginPos, beginPos + 20};
            e.replaceWith(replacementLoc, "private");
        }
    }

    return empty;
}

}; // namespace sorbet::dsl
