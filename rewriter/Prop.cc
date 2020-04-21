#include "rewriter/Prop.h"
#include "ast/Helpers.h"
#include "ast/ast.h"
#include "core/Context.h"
#include "core/Names.h"
#include "core/core.h"
#include "core/errors/rewriter.h"
#include "rewriter/Util.h"

using namespace std;

namespace sorbet::rewriter {
namespace {

// these helpers work on a purely syntactic level. for instance, this function determines if an expression is `T`,
// either with no scope or with the root scope (i.e. `::T`). this might not actually refer to the `T` that we define for
// users, but we don't know that information in the Rewriter passes.
bool isT(ast::Expression *expr) {
    auto *t = ast::cast_tree<ast::UnresolvedConstantLit>(expr);
    if (t == nullptr || t->cnst != core::Names::Constants::T()) {
        return false;
    }
    auto scope = t->scope.get();
    if (ast::isa_tree<ast::EmptyTree>(scope)) {
        return true;
    }
    auto root = ast::cast_tree<ast::ConstantLit>(scope);
    return root != nullptr && root->symbol == core::Symbols::root();
}

bool isTNilable(ast::Expression *expr) {
    auto *nilable = ast::cast_tree<ast::Send>(expr);
    return nilable != nullptr && nilable->fun == core::Names::nilable() && isT(nilable->recv.get());
}

bool isTStruct(ast::Expression *expr) {
    auto *struct_ = ast::cast_tree<ast::UnresolvedConstantLit>(expr);
    return struct_ != nullptr && struct_->cnst == core::Names::Constants::Struct() && isT(struct_->scope.get());
}

struct PropInfo {
    core::LocOffsets loc;
    bool isImmutable = false;
    core::NameRef name = core::NameRef::noName();
    core::LocOffsets nameLoc;
    unique_ptr<ast::Expression> type;
    unique_ptr<ast::Expression> default_;
    core::NameRef computedByMethodName = core::NameRef::noName();
    core::LocOffsets computedByMethodNameLoc;
    unique_ptr<ast::Expression> foreign;
    unique_ptr<ast::Expression> ifunset;
};

struct NodesAndPropInfo {
    vector<unique_ptr<ast::Expression>> nodes;
    PropInfo propInfo;
};

optional<PropInfo> parseProp(core::MutableContext ctx, const ast::Send *send) {
    PropInfo ret;
    ret.loc = send->loc;

    // ----- Is this a send we care about? -----
    switch (send->fun._id) {
        case core::Names::prop()._id:
            // Nothing special
            break;
        case core::Names::const_()._id:
            ret.isImmutable = true;
            break;
        case core::Names::tokenProp()._id:
        case core::Names::timestampedTokenProp()._id:
            ret.name = core::Names::token();
            ret.nameLoc = core::LocOffsets{send->loc.beginPos() +
                                               (send->fun._id == core::Names::timestampedTokenProp()._id ? 12 : 0),
                                           send->loc.endPos() - 5}; // get the 'token' part of it
            ret.type = ast::MK::Constant(send->loc, core::Symbols::String());
            break;
        case core::Names::createdProp()._id:
            ret.name = core::Names::created();
            ret.nameLoc =
                core::LocOffsets{send->loc.beginPos(),
                                 send->loc.endPos() - 5}; // 5 is the difference between `created_prop` and `created`
            ret.type = ast::MK::Constant(send->loc, core::Symbols::Float());
            break;
        case core::Names::merchantProp()._id:
            ret.isImmutable = true;
            ret.name = core::Names::merchant();
            ret.nameLoc =
                core::LocOffsets{send->loc.beginPos(),
                                 send->loc.endPos() - 5}; // 5 is the difference between `merchant_prop` and `merchant`
            ret.type = ast::MK::Constant(send->loc, core::Symbols::String());
            break;

        default:
            return std::nullopt;
    }

    if (send->args.size() >= 4) {
        // Too many args, even if all optional args were provided.
        return nullopt;
    }

    // ----- What's the prop's name? -----
    if (!ret.name.exists()) {
        if (send->args.empty()) {
            return nullopt;
        }
        auto *sym = ast::cast_tree<ast::Literal>(send->args[0].get());
        if (!sym || !sym->isSymbol(ctx)) {
            return nullopt;
        }
        ret.name = sym->asSymbol(ctx);
        ENFORCE(!core::Loc(ctx.file, sym->loc).source(ctx).empty() &&
                core::Loc(ctx.file, sym->loc).source(ctx)[0] == ':');
        ret.nameLoc = core::LocOffsets{sym->loc.beginPos() + 1, sym->loc.endPos()};
    }

    // ----- What's the prop's type? -----
    if (ret.type == nullptr) {
        if (send->args.size() == 1) {
            // Type must have been inferred from prop method (like created_prop) or
            // been given in second argument.
            return nullopt;
        } else {
            ret.type = ASTUtil::dupType(send->args[1].get());
            if (ret.type == nullptr) {
                return nullopt;
            }
        }
    }

    ENFORCE(ASTUtil::dupType(ret.type.get()) != nullptr, "No obvious type AST for this prop");

    // ----- Does the prop have any extra options? -----
    unique_ptr<ast::Hash> rules;
    if (!send->args.empty()) {
        if (auto back = ast::cast_tree<ast::Hash>(send->args.back().get())) {
            // Deep copy the rules hash so that we can destruct it at will to parse things,
            // without having to worry about whether we stole things from the tree.
            rules.reset(ast::cast_tree<ast::Hash>(back->deepCopy().release()));
        }
    }
    if (rules == nullptr && send->args.size() >= 3) {
        // No rules, but 3 args including name and type. Also not a T::Props
        return std::nullopt;
    }

    // ----- Parse any extra options -----

    if (rules) {
        if (ASTUtil::hasTruthyHashValue(ctx, *rules, core::Names::immutable())) {
            ret.isImmutable = true;
        }

        if (ASTUtil::hasTruthyHashValue(ctx, *rules, core::Names::factory())) {
            ret.default_ = ast::MK::RaiseUnimplemented(ret.loc);
        } else if (ASTUtil::hasHashValue(ctx, *rules, core::Names::default_())) {
            auto [key, val] = ASTUtil::extractHashValue(ctx, *rules, core::Names::default_());
            ret.default_ = std::move(val);
        }

        // e.g. `const :foo, type, computed_by: :method_name`
        if (ASTUtil::hasTruthyHashValue(ctx, *rules, core::Names::computedBy())) {
            auto [key, val] = ASTUtil::extractHashValue(ctx, *rules, core::Names::computedBy());
            auto lit = ast::cast_tree<ast::Literal>(val.get());
            if (lit != nullptr && lit->isSymbol(ctx)) {
                ret.computedByMethodNameLoc = lit->loc;
                ret.computedByMethodName = lit->asSymbol(ctx);
            } else {
                if (auto e = ctx.beginError(val->loc, core::errors::Rewriter::ComputedBySymbol)) {
                    e.setHeader("Value for `{}` must be a symbol literal", "computed_by");
                }
            }
        }

        auto [fk, foreignTree] = ASTUtil::extractHashValue(ctx, *rules, core::Names::foreign());
        if (foreignTree != nullptr) {
            ret.foreign = move(foreignTree);
            if (auto body = ASTUtil::thunkBody(ctx, ret.foreign.get())) {
                ret.foreign = std::move(body);
            } else {
                if (auto e = ctx.beginError(ret.foreign->loc, core::errors::Rewriter::PropForeignStrict)) {
                    e.setHeader("The argument to `{}` must be a lambda", "foreign:");
                    e.replaceWith("Convert to lambda", core::Loc(ctx.file, ret.foreign->loc), "-> {{{}}}",
                                  core::Loc(ctx.file, ret.foreign->loc).source(ctx));
                }
            }
        }

        auto [ifunsetKey, ifunset] = ASTUtil::extractHashValue(ctx, *rules, core::Names::ifunset());
        if (ifunset != nullptr) {
            ret.ifunset = std::move(ifunset);
        }
    }

    if (ret.default_ == nullptr && isTNilable(ret.type.get())) {
        ret.default_ = ast::MK::Nil(ret.loc);
    }

    return ret;
}

vector<unique_ptr<ast::Expression>> processProp(core::MutableContext ctx, const PropInfo &ret, bool forTStruct) {
    vector<unique_ptr<ast::Expression>> nodes;

    const auto loc = ret.loc;
    const auto isImmutable = ret.isImmutable;
    const auto name = ret.name;
    const auto nameLoc = ret.nameLoc;

    const auto getType = ASTUtil::dupType(ret.type.get());

    const auto computedByMethodName = ret.computedByMethodName;
    const auto computedByMethodNameLoc = ret.computedByMethodNameLoc;

    auto varName = name.addAt(ctx);

    nodes.emplace_back(ast::MK::Sig(loc, ast::MK::Hash0(loc), ASTUtil::dupType(getType.get())));

    if (computedByMethodName.exists()) {
        // Given `const :foo, type, computed_by: <name>`, where <name> is a Symbol pointing to a class method,
        // assert that the method takes 1 argument (of any type), and returns the same type as the prop,
        // via `T.assert_type!(self.class.compute_foo(T.unsafe(nil)), type)` in the getter.
        auto selfSendClass = ast::MK::Send0(computedByMethodNameLoc, ast::MK::Self(loc), core::Names::class_());
        auto unsafeNil = ast::MK::Unsafe(computedByMethodNameLoc, ast::MK::Nil(computedByMethodNameLoc));
        auto sendComputedMethod = ast::MK::Send1(computedByMethodNameLoc, std::move(selfSendClass),
                                                 computedByMethodName, std::move(unsafeNil));
        auto assertTypeMatches = ast::MK::AssertType(computedByMethodNameLoc, std::move(sendComputedMethod),
                                                     ASTUtil::dupType(getType.get()));
        auto insSeq = ast::MK::InsSeq1(loc, std::move(assertTypeMatches), ast::MK::RaiseUnimplemented(loc));
        nodes.emplace_back(ASTUtil::mkGet(ctx, loc, name, std::move(insSeq)));
    } else if (ret.ifunset == nullptr && forTStruct) {
        nodes.emplace_back(ASTUtil::mkGet(ctx, loc, name, ast::MK::Instance(nameLoc, varName)));
    } else {
        nodes.emplace_back(ASTUtil::mkGet(ctx, loc, name, ast::MK::RaiseUnimplemented(loc)));
    }

    core::NameRef setName = name.addEq(ctx);

    // Compute the setter
    if (!isImmutable) {
        auto setType = ASTUtil::dupType(ret.type.get());
        nodes.emplace_back(ast::MK::Sig(
            loc, ast::MK::Hash1(loc, ast::MK::Symbol(nameLoc, core::Names::arg0()), ASTUtil::dupType(setType.get())),
            ASTUtil::dupType(setType.get())));
        nodes.emplace_back(ASTUtil::mkSet(ctx, loc, setName, nameLoc, ast::MK::RaiseUnimplemented(loc)));
    }

    // Compute the `_` foreign accessor
    if (ret.foreign) {
        unique_ptr<ast::Expression> type;
        unique_ptr<ast::Expression> nonNilType;
        if (ASTUtil::dupType(ret.foreign.get()) == nullptr) {
            // If it's not a valid type, just use untyped
            type = ast::MK::Untyped(loc);
            nonNilType = ast::MK::Untyped(loc);
        } else {
            type = ast::MK::Nilable(loc, ASTUtil::dupType(ret.foreign.get()));
            nonNilType = ASTUtil::dupType(ret.foreign.get());
        }

        // sig {params(opts: T.untyped).returns(T.nilable($foreign))}
        nodes.emplace_back(
            ast::MK::Sig1(loc, ast::MK::Symbol(nameLoc, core::Names::opts()), ast::MK::Untyped(loc), std::move(type)));

        // def $fk_method(**opts)
        //  T.unsafe(nil)
        // end

        auto fkMethod = ctx.state.enterNameUTF8(name.data(ctx)->show(ctx) + "_");

        unique_ptr<ast::Expression> arg =
            ast::MK::RestArg(nameLoc, ast::MK::KeywordArg(nameLoc, ast::MK::Local(nameLoc, core::Names::opts())));
        nodes.emplace_back(ast::MK::SyntheticMethod1(loc, core::Loc(ctx.file, loc), fkMethod, std::move(arg),
                                                     ast::MK::RaiseUnimplemented(loc)));

        // sig {params(opts: T.untyped).returns($foreign)}
        nodes.emplace_back(ast::MK::Sig1(loc, ast::MK::Symbol(nameLoc, core::Names::opts()), ast::MK::Untyped(loc),
                                         std::move(nonNilType)));

        // def $fk_method_!(**opts)
        //  T.unsafe(nil)
        // end

        auto fkMethodBang = ctx.state.enterNameUTF8(name.data(ctx)->show(ctx) + "_!");
        unique_ptr<ast::Expression> arg2 =
            ast::MK::RestArg(nameLoc, ast::MK::KeywordArg(nameLoc, ast::MK::Local(nameLoc, core::Names::opts())));
        nodes.emplace_back(ast::MK::SyntheticMethod1(loc, core::Loc(ctx.file, loc), fkMethodBang, std::move(arg2),
                                                     ast::MK::RaiseUnimplemented(loc)));
    }

    // Compute the Mutator
    {
        // Compute a setter
        auto setType = ASTUtil::dupType(ret.type.get());
        ast::ClassDef::RHS_store rhs;
        rhs.emplace_back(ast::MK::Sig(
            loc, ast::MK::Hash1(loc, ast::MK::Symbol(nameLoc, core::Names::arg0()), ASTUtil::dupType(setType.get())),
            ASTUtil::dupType(setType.get())));
        rhs.emplace_back(ASTUtil::mkSet(ctx, loc, setName, nameLoc, ast::MK::RaiseUnimplemented(loc)));

        // Maybe make a getter
        unique_ptr<ast::Expression> mutator;
        if (ASTUtil::isProbablySymbol(ctx, ret.type.get(), core::Symbols::Hash())) {
            mutator = ASTUtil::mkMutator(ctx, loc, core::Names::Constants::HashMutator());
            auto send = ast::cast_tree<ast::Send>(ret.type.get());
            if (send && send->fun == core::Names::squareBrackets() && send->args.size() == 2) {
                mutator = ast::MK::Send2(loc, std::move(mutator), core::Names::squareBrackets(),
                                         ASTUtil::dupType(send->args[0].get()), ASTUtil::dupType(send->args[1].get()));
            } else {
                mutator = ast::MK::Send2(loc, std::move(mutator), core::Names::squareBrackets(), ast::MK::Untyped(loc),
                                         ast::MK::Untyped(loc));
            }
        } else if (ASTUtil::isProbablySymbol(ctx, ret.type.get(), core::Symbols::Array())) {
            mutator = ASTUtil::mkMutator(ctx, loc, core::Names::Constants::ArrayMutator());
            auto send = ast::cast_tree<ast::Send>(ret.type.get());
            if (send && send->fun == core::Names::squareBrackets() && send->args.size() == 1) {
                mutator = ast::MK::Send1(loc, std::move(mutator), core::Names::squareBrackets(),
                                         ASTUtil::dupType(send->args[0].get()));
            } else {
                mutator = ast::MK::Send1(loc, std::move(mutator), core::Names::squareBrackets(), ast::MK::Untyped(loc));
            }
        } else if (ast::isa_tree<ast::UnresolvedConstantLit>(ret.type.get())) {
            // In a perfect world we could know if there was a Mutator we could reference instead, like this:
            // mutator = ast::MK::UnresolvedConstant(loc, ASTUtil::dupType(type.get()),
            // core::Names::Constants::Mutator()); For now we're just going to leave these in method_missing.rbi
        }

        if (mutator.get()) {
            rhs.emplace_back(ast::MK::Sig0(loc, ASTUtil::dupType(mutator.get())));
            rhs.emplace_back(ASTUtil::mkGet(ctx, loc, name, ast::MK::RaiseUnimplemented(loc)));

            ast::ClassDef::ANCESTORS_store ancestors;
            auto name = core::Names::Constants::Mutator();
            nodes.emplace_back(ast::MK::Class(loc, core::Loc(ctx.file, loc),
                                              ast::MK::UnresolvedConstant(loc, ast::MK::EmptyTree(), name),
                                              std::move(ancestors), std::move(rhs)));
        }
    }

    return nodes;
}

vector<unique_ptr<ast::Expression>> mkTStructInitialize(core::MutableContext ctx, core::LocOffsets klassLoc,
                                                        const vector<PropInfo> &props) {
    ast::MethodDef::ARGS_store args;
    ast::Hash::ENTRY_store sigKeys;
    ast::Hash::ENTRY_store sigVals;
    args.reserve(props.size());
    sigKeys.reserve(props.size());
    sigVals.reserve(props.size());

    // add all the required props first.
    for (const auto &prop : props) {
        if (prop.default_ != nullptr) {
            continue;
        }
        auto loc = prop.loc;
        args.emplace_back(ast::MK::KeywordArg(loc, ast::MK::Local(loc, prop.name)));
        sigKeys.emplace_back(ast::MK::Symbol(loc, prop.name));
        sigVals.emplace_back(prop.type->deepCopy());
    }

    // then, add all the optional props.
    for (const auto &prop : props) {
        if (prop.default_ == nullptr) {
            continue;
        }
        auto loc = prop.loc;
        args.emplace_back(ast::MK::OptionalArg(loc, ast::MK::KeywordArg(loc, ast::MK::Local(loc, prop.name)),
                                               prop.default_->deepCopy()));
        sigKeys.emplace_back(ast::MK::Symbol(loc, prop.name));
        sigVals.emplace_back(prop.type->deepCopy());
    }

    // then initialize all the instance variables in the body
    ast::InsSeq::STATS_store stats;
    for (const auto &prop : props) {
        auto varName = prop.name.addAt(ctx);
        stats.emplace_back(ast::MK::Assign(prop.loc, ast::MK::Instance(prop.nameLoc, varName),
                                           ast::MK::Local(prop.nameLoc, prop.name)));
    }
    auto body = ast::MK::InsSeq(klassLoc, std::move(stats), ast::MK::ZSuper(klassLoc));

    vector<unique_ptr<ast::Expression>> result;
    result.emplace_back(ast::MK::SigVoid(klassLoc, ast::MK::Hash(klassLoc, std::move(sigKeys), std::move(sigVals))));
    result.emplace_back(ast::MK::SyntheticMethod(klassLoc, core::Loc(ctx.file, klassLoc), core::Names::initialize(),
                                                 std::move(args), std::move(body)));
    return result;
}

} // namespace

void Prop::run(core::MutableContext ctx, ast::ClassDef *klass) {
    if (ctx.state.runningUnderAutogen) {
        return;
    }
    auto forTStruct = false;
    for (auto &a : klass->ancestors) {
        if (isTStruct(a.get())) {
            forTStruct = true;
            break;
        }
    }
    UnorderedMap<ast::Expression *, vector<unique_ptr<ast::Expression>>> replaceNodes;
    vector<PropInfo> props;
    for (auto &stat : klass->rhs) {
        auto *send = ast::cast_tree<ast::Send>(stat.get());
        if (send == nullptr) {
            continue;
        }
        auto propInfo = parseProp(ctx, send);
        if (!propInfo.has_value()) {
            continue;
        }
        auto nodes = processProp(ctx, propInfo.value(), forTStruct);
        ENFORCE(!nodes.empty(), "if parseProp completed successfully, processProp must complete too");
        replaceNodes[stat.get()] = std::move(nodes);
        props.emplace_back(std::move(propInfo.value()));
    }
    auto oldRHS = std::move(klass->rhs);
    klass->rhs.clear();
    klass->rhs.reserve(oldRHS.size());
    if (forTStruct) {
        // we define our synthesized initialize first so that if the user wrote one themselves, it overrides ours.
        for (auto &stat : mkTStructInitialize(ctx, klass->loc, props)) {
            klass->rhs.emplace_back(std::move(stat));
        }
    }
    // this is cargo-culted from rewriter.cc.
    for (auto &stat : oldRHS) {
        if (replaceNodes.find(stat.get()) == replaceNodes.end()) {
            klass->rhs.emplace_back(std::move(stat));
        } else {
            for (auto &newNode : replaceNodes.at(stat.get())) {
                klass->rhs.emplace_back(std::move(newNode));
            }
        }
    }
}

}; // namespace sorbet::rewriter
