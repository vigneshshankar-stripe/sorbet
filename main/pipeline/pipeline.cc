// has to go first, as it violates poisons
#include "core/proto/proto.h"
#include <sstream>

#include "ProgressIndicator.h"
#include "absl/strings/escaping.h" // BytesToHexString
#include "absl/strings/match.h"
#include "ast/desugar/Desugar.h"
#include "ast/substitute/substitute.h"
#include "ast/treemap/treemap.h"
#include "cfg/CFG.h"
#include "cfg/builder/builder.h"
#include "cfg/proto/proto.h"
#include "common/FileOps.h"
#include "common/Timer.h"
#include "common/concurrency/ConcurrentQueue.h"
#include "common/crypto_hashing/crypto_hashing.h"
#include "core/GlobalSubstitution.h"
#include "core/Unfreeze.h"
#include "core/errors/parser.h"
#include "core/serialize/serialize.h"
#include "definition_validator/validator.h"
#include "dsl/dsl.h"
#include "flattener/flatten.h"
#include "infer/infer.h"
#include "local_vars/local_vars.h"
#include "namer/configatron/configatron.h"
#include "namer/namer.h"
#include "parser/parser.h"
#include "pipeline.h"
#include "plugin/Plugins.h"
#include "plugin/SubprocessTextPlugin.h"
#include "resolver/resolver.h"

using namespace std;

namespace sorbet::realmain::pipeline {

class CFGCollectorAndTyper {
    const options::Options &opts;

public:
    CFGCollectorAndTyper(const options::Options &opts) : opts(opts){};

    unique_ptr<ast::MethodDef> preTransformMethodDef(core::Context ctx, unique_ptr<ast::MethodDef> m) {
        if (m->loc.file().data(ctx).strictLevel < core::StrictLevel::True || m->symbol.data(ctx)->isOverloaded()) {
            return m;
        }
        auto &print = opts.print;
        auto cfg = cfg::CFGBuilder::buildFor(ctx.withOwner(m->symbol), *m);

        if (opts.stopAfterPhase == options::Phase::CFG) {
            return m;
        }
        cfg = infer::Inference::run(ctx.withOwner(cfg->symbol), move(cfg));
        if (print.CFG.enabled) {
            print.CFG.fmt("{}\n\n", cfg->toString(ctx));
        }
        if ((print.CFGJson.enabled || print.CFGProto.enabled) && cfg->shouldExport(ctx.state)) {
            auto proto = cfg::Proto::toProto(ctx.state, *cfg);
            if (print.CFGJson.enabled) {
                string buf = core::Proto::toJSON(proto);
                print.CFGJson.print(buf);
            } else {
                // The proto wire format allows simply concatenating repeated message fields
                string buf = cfg::Proto::toMulti(proto).SerializeAsString();
                print.CFGProto.print(buf);
            }
        }
        return m;
    }
};

string fileKey(core::GlobalState &gs, core::FileRef file) {
    auto path = file.data(gs).path();
    string key(path.begin(), path.end());
    key += "//";
    auto hashBytes = sorbet::crypto_hashing::hash64(file.data(gs).source());
    key += absl::BytesToHexString(string_view{(char *)hashBytes.data(), size(hashBytes)});
    return key;
}

unique_ptr<ast::Expression> fetchTreeFromCache(core::GlobalState &gs, core::FileRef file,
                                               const unique_ptr<KeyValueStore> &kvstore) {
    if (kvstore && file.id() < gs.filesUsed()) {
        string fileHashKey = fileKey(gs, file);
        auto maybeCached = kvstore->read(fileHashKey);
        if (maybeCached) {
            prodCounterInc("types.input.files.kvstore.hit");
            auto cachedTree = core::serialize::Serializer::loadExpression(gs, maybeCached, file.id());
            file.data(gs).cachedParseTree = true;
            ENFORCE(cachedTree->loc.file() == file);
            return cachedTree;
        } else {
            prodCounterInc("types.input.files.kvstore.miss");
        }
    }
    return nullptr;
}

void cacheTrees(core::GlobalState &gs, unique_ptr<KeyValueStore> &kvstore, vector<ast::ParsedFile> &trees) {
    if (!kvstore) {
        return;
    }
    for (auto &tree : trees) {
        if (tree.file.data(gs).cachedParseTree) {
            continue;
        }
        string fileHashKey = fileKey(gs, tree.file);
        kvstore->write(fileHashKey, core::serialize::Serializer::storeExpression(gs, tree.tree));
    }
}

unique_ptr<parser::Node> runParser(core::GlobalState &gs, core::FileRef file, const options::Printers &print) {
    Timer timeit(gs.tracer(), "runParser", {{"file", (string)file.data(gs).path()}});
    unique_ptr<parser::Node> nodes;
    {
        core::UnfreezeNameTable nameTableAccess(gs); // enters strings from source code as names
        nodes = parser::Parser::run(gs, file);
    }
    if (print.ParseTree.enabled) {
        print.ParseTree.fmt("{}\n", nodes->toStringWithTabs(gs, 0));
    }
    if (print.ParseTreeJson.enabled) {
        print.ParseTreeJson.fmt("{}\n", nodes->toJSON(gs, 0));
    }
    if (print.ParseTreeWhitequark.enabled) {
        print.ParseTreeWhitequark.fmt("{}\n", nodes->toWhitequark(gs, 0));
    }
    return nodes;
}

unique_ptr<ast::Expression> runDesugar(core::GlobalState &gs, core::FileRef file, unique_ptr<parser::Node> parseTree,
                                       const options::Printers &print) {
    Timer timeit(gs.tracer(), "runDesugar", {{"file", (string)file.data(gs).path()}});
    unique_ptr<ast::Expression> ast;
    core::MutableContext ctx(gs, core::Symbols::root());
    {
        core::ErrorRegion errs(gs, file);
        core::UnfreezeNameTable nameTableAccess(gs); // creates temporaries during desugaring
        ast = ast::desugar::node2Tree(ctx, move(parseTree));
    }
    if (print.Desugared.enabled) {
        print.Desugared.fmt("{}\n", ast->toStringWithTabs(gs, 0));
    }
    if (print.DesugaredRaw.enabled) {
        print.DesugaredRaw.fmt("{}\n", ast->showRaw(gs));
    }
    return ast;
}

unique_ptr<ast::Expression> runDSL(core::GlobalState &gs, core::FileRef file, unique_ptr<ast::Expression> ast) {
    core::MutableContext ctx(gs, core::Symbols::root());
    Timer timeit(gs.tracer(), "runDSL", {{"file", (string)file.data(gs).path()}});
    core::UnfreezeNameTable nameTableAccess(gs); // creates temporaries during desugaring
    core::ErrorRegion errs(gs, file);
    return dsl::DSL::run(ctx, move(ast));
}

ast::ParsedFile runLocalVars(core::GlobalState &gs, ast::ParsedFile tree) {
    Timer timeit(gs.tracer(), "runLocalVars", {{"file", (string)tree.file.data(gs).path()}});
    core::MutableContext ctx(gs, core::Symbols::root());
    return sorbet::local_vars::LocalVars::run(ctx, move(tree));
}

ast::ParsedFile emptyParsedFile(core::FileRef file) {
    return {make_unique<ast::EmptyTree>(), file};
}

ast::ParsedFile indexOne(const options::Options &opts, core::GlobalState &lgs, core::FileRef file,
                         unique_ptr<KeyValueStore> &kvstore) {
    auto &print = opts.print;
    ast::ParsedFile dslsInlined{nullptr, file};
    ENFORCE(file.data(lgs).strictLevel == decideStrictLevel(lgs, file, opts));

    Timer timeit(lgs.tracer(), "indexOne");
    try {
        unique_ptr<ast::Expression> tree = fetchTreeFromCache(lgs, file, kvstore);

        if (!tree) {
            // tree isn't cached. Need to start from parser
            if (file.data(lgs).strictLevel == core::StrictLevel::Ignore) {
                return emptyParsedFile(file);
            }
            auto parseTree = runParser(lgs, file, print);
            if (opts.stopAfterPhase == options::Phase::PARSER) {
                return emptyParsedFile(file);
            }
            tree = runDesugar(lgs, file, move(parseTree), print);
            if (opts.stopAfterPhase == options::Phase::DESUGARER) {
                return emptyParsedFile(file);
            }
            if (!opts.skipDSLPasses) {
                tree = runDSL(lgs, file, move(tree));
            }
            tree = runLocalVars(lgs, ast::ParsedFile{move(tree), file}).tree;
            if (opts.stopAfterPhase == options::Phase::LOCAL_VARS) {
                return emptyParsedFile(file);
            }
        }
        if (print.DSLTree.enabled) {
            print.DSLTree.fmt("{}\n", tree->toStringWithTabs(lgs, 0));
        }
        if (print.DSLTreeRaw.enabled) {
            print.DSLTreeRaw.fmt("{}\n", tree->showRaw(lgs));
        }
        if (opts.stopAfterPhase == options::Phase::DSL) {
            return emptyParsedFile(file);
        }

        dslsInlined.tree = move(tree);
        return dslsInlined;
    } catch (SorbetException &) {
        Exception::failInFuzzer();
        if (auto e = lgs.beginError(sorbet::core::Loc::none(file), core::errors::Internal::InternalError)) {
            e.setHeader("Exception parsing file: `{}` (backtrace is above)", file.data(lgs).path());
        }
        return emptyParsedFile(file);
    }
}

pair<ast::ParsedFile, vector<shared_ptr<core::File>>> emptyPluginFile(core::FileRef file) {
    return {emptyParsedFile(file), vector<shared_ptr<core::File>>()};
}

pair<ast::ParsedFile, vector<shared_ptr<core::File>>> indexOneWithPlugins(const options::Options &opts,
                                                                          core::GlobalState &gs, core::FileRef file,
                                                                          unique_ptr<KeyValueStore> &kvstore) {
    auto &print = opts.print;
    ast::ParsedFile dslsInlined{nullptr, file};
    vector<shared_ptr<core::File>> resultPluginFiles;

    Timer timeit(gs.tracer(), "indexOneWithPlugins", {{"file", (string)file.data(gs).path()}});
    try {
        unique_ptr<ast::Expression> tree = fetchTreeFromCache(gs, file, kvstore);

        if (!tree) {
            // tree isn't cached. Need to start from parser
            if (file.data(gs).strictLevel == core::StrictLevel::Ignore) {
                return emptyPluginFile(file);
            }
            auto parseTree = runParser(gs, file, print);
            if (opts.stopAfterPhase == options::Phase::PARSER) {
                return emptyPluginFile(file);
            }
            tree = runDesugar(gs, file, move(parseTree), print);
            if (opts.stopAfterPhase == options::Phase::DESUGARER) {
                return emptyPluginFile(file);
            }
            {
                Timer timeit(gs.tracer(), "plugins_text");
                core::MutableContext ctx(gs, core::Symbols::root());
                core::ErrorRegion errs(gs, file);
                auto [pluginTree, pluginFiles] = plugin::SubprocessTextPlugin::run(ctx, move(tree));
                tree = move(pluginTree);
                resultPluginFiles = move(pluginFiles);
            }

            if (!opts.skipDSLPasses) {
                tree = runDSL(gs, file, move(tree));
            }
            if (print.DSLTree.enabled) {
                print.DSLTree.fmt("{}\n", tree->toStringWithTabs(gs, 0));
            }
            if (print.DSLTreeRaw.enabled) {
                print.DSLTreeRaw.fmt("{}\n", tree->showRaw(gs));
            }

            tree = runLocalVars(gs, ast::ParsedFile{move(tree), file}).tree;
            if (opts.stopAfterPhase == options::Phase::LOCAL_VARS) {
                return emptyPluginFile(file);
            }
        }
        if (print.IndexTree.enabled) {
            print.IndexTree.fmt("{}\n", tree->toStringWithTabs(gs, 0));
        }
        if (print.IndexTreeRaw.enabled) {
            print.IndexTreeRaw.fmt("{}\n", tree->showRaw(gs));
        }
        if (opts.stopAfterPhase == options::Phase::DSL) {
            return emptyPluginFile(file);
        }

        dslsInlined.tree = move(tree);
        return {move(dslsInlined), resultPluginFiles};
    } catch (SorbetException &) {
        Exception::failInFuzzer();
        if (auto e = gs.beginError(sorbet::core::Loc::none(file), core::errors::Internal::InternalError)) {
            e.setHeader("Exception parsing file: `{}` (backtrace is above)", file.data(gs).path());
        }
        return emptyPluginFile(file);
    }
}

vector<ast::ParsedFile> incrementalResolve(core::GlobalState &gs, vector<ast::ParsedFile> what,
                                           const options::Options &opts) {
    try {
        int i = 0;
        Timer timeit(gs.tracer(), "incremental_naming");
        for (auto &tree : what) {
            auto file = tree.file;
            try {
                unique_ptr<ast::Expression> ast;
                core::MutableContext ctx(gs, core::Symbols::root());
                gs.tracer().trace("Naming: {}", file.data(gs).path());
                core::ErrorRegion errs(gs, file);
                core::UnfreezeSymbolTable symbolTable(gs);
                core::UnfreezeNameTable nameTable(gs);
                tree = sorbet::namer::Namer::run(ctx, move(tree));
                i++;
            } catch (SorbetException &) {
                if (auto e = gs.beginError(sorbet::core::Loc::none(file), core::errors::Internal::InternalError)) {
                    e.setHeader("Exception naming file: `{}` (backtrace is above)", file.data(gs).path());
                }
            }
        }

        core::MutableContext ctx(gs, core::Symbols::root());
        {
            Timer timeit(gs.tracer(), "incremental_resolve");
            gs.tracer().trace("Resolving (incremental pass)...");
            core::ErrorRegion errs(gs, sorbet::core::FileRef());
            core::UnfreezeSymbolTable symbolTable(gs);
            core::UnfreezeNameTable nameTable(gs);

            what = sorbet::resolver::Resolver::runTreePasses(ctx, move(what));
        }
    } catch (SorbetException &) {
        if (auto e = gs.beginError(sorbet::core::Loc::none(), sorbet::core::errors::Internal::InternalError)) {
            e.setHeader("Exception resolving (backtrace is above)");
        }
    }

    return what;
}

vector<core::FileRef> reserveFiles(unique_ptr<core::GlobalState> &gs, const vector<string> &files) {
    Timer timeit(gs->tracer(), "reserveFiles");
    vector<core::FileRef> ret;
    core::UnfreezeFileTable unfreezeFiles(*gs);
    for (auto f : files) {
        auto fileRef = gs->findFileByPath(f);
        if (!fileRef.exists()) {
            fileRef = gs->reserveFileRef(f);
        }
        ret.emplace_back(move(fileRef));
    }
    return ret;
}

core::StrictLevel decideStrictLevel(const core::GlobalState &gs, const core::FileRef file,
                                    const options::Options &opts) {
    auto &fileData = file.data(gs);

    core::StrictLevel level;
    string filePath = string(fileData.path());
    // make sure all relative file paths start with ./
    if (!absl::StartsWith(filePath, "/") && !absl::StartsWith(filePath, "./")) {
        filePath.insert(0, "./");
    }
    auto fnd = opts.strictnessOverrides.find(filePath);
    if (fnd != opts.strictnessOverrides.end()) {
        if (fnd->second == fileData.originalSigil) {
            core::ErrorRegion errs(gs, file);
            if (auto e = gs.beginError(sorbet::core::Loc::none(file), core::errors::Parser::ParserError)) {
                e.setHeader("Useless override of strictness level");
            }
        }
        level = fnd->second;
    } else {
        if (fileData.originalSigil == core::StrictLevel::None) {
            level = core::StrictLevel::False;
        } else {
            level = fileData.originalSigil;
        }
    }

    core::StrictLevel minStrict = opts.forceMinStrict;
    core::StrictLevel maxStrict = opts.forceMaxStrict;
    if (level <= core::StrictLevel::Max && level > core::StrictLevel::Ignore) {
        level = max(min(level, maxStrict), minStrict);
    }

    if (gs.runningUnderAutogen) {
        // Autogen stops before infer but needs to see all definitions
        level = core::StrictLevel::False;
    }

    return level;
}

void incrementStrictLevelCounter(core::StrictLevel level) {
    switch (level) {
        case core::StrictLevel::None:
            Exception::raise("Should never happen");
            break;
        case core::StrictLevel::Ignore:
            prodCounterInc("types.input.files.sigil.ignore");
            break;
        case core::StrictLevel::Internal:
            Exception::raise("Should never happen");
            break;
        case core::StrictLevel::False:
            prodCounterInc("types.input.files.sigil.false");
            break;
        case core::StrictLevel::True:
            prodCounterInc("types.input.files.sigil.true");
            break;
        case core::StrictLevel::Strict:
            prodCounterInc("types.input.files.sigil.strict");
            break;
        case core::StrictLevel::Strong:
            prodCounterInc("types.input.files.sigil.strong");
            break;
        case core::StrictLevel::Max:
            Exception::raise("Should never happen");
            break;
        case core::StrictLevel::Autogenerated:
            prodCounterInc("types.input.files.sigil.autogenerated");
            break;
        case core::StrictLevel::Stdlib:
            prodCounterInc("types.input.files.sigil.stdlib");
            break;
    }
}

void readFileWithStrictnessOverrides(unique_ptr<core::GlobalState> &gs, core::FileRef file,
                                     const options::Options &opts) {
    if (file.dataAllowingUnsafe(*gs).sourceType != core::File::NotYetRead) {
        return;
    }
    auto fileName = file.dataAllowingUnsafe(*gs).path();
    Timer timeit(gs->tracer(), "readFileWithStrictnessOverrides", {{"file", (string)fileName}});
    string src;
    bool fileFound = true;
    try {
        src = opts.fs->readFile(fileName);
    } catch (FileNotFoundException e) {
        // continue with an empty source, because the
        // assertion below requires every input file to map
        // to one output tree
        fileFound = false;
    }
    prodCounterAdd("types.input.bytes", src.size());
    prodCounterInc("types.input.files");

    {
        core::UnfreezeFileTable unfreezeFiles(*gs);
        auto entered = gs->enterNewFileAt(
            make_shared<core::File>(string(fileName.begin(), fileName.end()), move(src), core::File::Normal), file);
        ENFORCE(entered == file);
    }
    if (enable_counters) {
        counterAdd("types.input.lines", file.data(*gs).lineCount());
    }

    auto &fileData = file.data(*gs);
    if (!fileFound) {
        if (auto e = gs->beginError(sorbet::core::Loc::none(file), core::errors::Internal::FileNotFound)) {
            e.setHeader("File Not Found");
        }
    }

    if (!opts.storeState.empty()) {
        fileData.sourceType = core::File::PayloadGeneration;
    }

    auto level = decideStrictLevel(*gs, file, opts);
    fileData.strictLevel = level;
    incrementStrictLevelCounter(level);
}

struct IndexResult {
    unique_ptr<core::GlobalState> gs;
    vector<ast::ParsedFile> trees;
    vector<shared_ptr<core::File>> pluginGeneratedFiles;
};

struct IndexThreadResultPack {
    CounterState counters;
    IndexResult res;
};

IndexResult mergeIndexResults(const shared_ptr<core::GlobalState> cgs, const options::Options &opts,
                              shared_ptr<BlockingBoundedQueue<IndexThreadResultPack>> input,
                              unique_ptr<KeyValueStore> &kvstore) {
    ProgressIndicator progress(opts.showProgress, "Indexing", input->bound);
    Timer timeit(cgs->tracer(), "mergeIndexResults");
    IndexThreadResultPack threadResult;
    IndexResult ret;
    for (auto result = input->wait_pop_timed(threadResult, WorkerPool::BLOCK_INTERVAL(), cgs->tracer()); !result.done();
         result = input->wait_pop_timed(threadResult, WorkerPool::BLOCK_INTERVAL(), cgs->tracer())) {
        if (result.gotItem()) {
            counterConsume(move(threadResult.counters));
            if (ret.gs == nullptr) {
                ret.gs = move(threadResult.res.gs);
                ENFORCE(ret.trees.empty());
                ret.trees = move(threadResult.res.trees);
                ret.pluginGeneratedFiles = move(threadResult.res.pluginGeneratedFiles);
                cacheTrees(*ret.gs, kvstore, ret.trees);
            } else {
                core::GlobalSubstitution substitution(*threadResult.res.gs, *ret.gs, cgs.get());
                core::MutableContext ctx(*ret.gs, core::Symbols::root());
                {
                    Timer timeit(cgs->tracer(), "substituteTrees");
                    for (auto &tree : threadResult.res.trees) {
                        auto file = tree.file;
                        core::ErrorRegion errs(*ret.gs, file);
                        if (!file.data(*ret.gs).cachedParseTree) {
                            tree.tree = ast::Substitute::run(ctx, substitution, move(tree.tree));
                        }
                    }
                }
                cacheTrees(*ret.gs, kvstore, threadResult.res.trees);
                ret.trees.insert(ret.trees.end(), make_move_iterator(threadResult.res.trees.begin()),
                                 make_move_iterator(threadResult.res.trees.end()));

                ret.pluginGeneratedFiles.insert(ret.pluginGeneratedFiles.end(),
                                                make_move_iterator(threadResult.res.pluginGeneratedFiles.begin()),
                                                make_move_iterator(threadResult.res.pluginGeneratedFiles.end()));
            }
            progress.reportProgress(input->doneEstimate());
            ret.gs->errorQueue->flushErrors();
        }
    }
    return ret;
}

IndexResult indexSuppliedFiles(const shared_ptr<core::GlobalState> &baseGs, vector<core::FileRef> &files,
                               const options::Options &opts, WorkerPool &workers, unique_ptr<KeyValueStore> &kvstore) {
    Timer timeit(baseGs->tracer(), "indexSuppliedFiles");
    auto resultq = make_shared<BlockingBoundedQueue<IndexThreadResultPack>>(files.size());
    auto fileq = make_shared<ConcurrentBoundedQueue<core::FileRef>>(files.size());
    for (auto &file : files) {
        fileq->push(move(file), 1);
    }

    workers.multiplexJob("indexSuppliedFiles", [baseGs, &opts, fileq, resultq, &kvstore]() {
        Timer timeit(baseGs->tracer(), "indexSuppliedFilesWorker");
        unique_ptr<core::GlobalState> localGs = baseGs->deepCopy();
        IndexThreadResultPack threadResult;

        {
            core::FileRef job;
            for (auto result = fileq->try_pop(job); !result.done(); result = fileq->try_pop(job)) {
                if (result.gotItem()) {
                    core::FileRef file = job;
                    readFileWithStrictnessOverrides(localGs, file, opts);
                    auto [parsedFile, pluginFiles] = indexOneWithPlugins(opts, *localGs, file, kvstore);
                    threadResult.res.pluginGeneratedFiles.insert(threadResult.res.pluginGeneratedFiles.end(),
                                                                 make_move_iterator(pluginFiles.begin()),
                                                                 make_move_iterator(pluginFiles.end()));
                    threadResult.res.trees.emplace_back(move(parsedFile));
                }
            }
        }

        if (!threadResult.res.trees.empty()) {
            threadResult.counters = getAndClearThreadCounters();
            threadResult.res.gs = move(localGs);
            auto computedTreesCount = threadResult.res.trees.size();
            resultq->push(move(threadResult), computedTreesCount);
        }
    });

    return mergeIndexResults(baseGs, opts, resultq, kvstore);
}

IndexResult indexPluginFiles(IndexResult firstPass, const options::Options &opts, WorkerPool &workers,
                             unique_ptr<KeyValueStore> &kvstore) {
    if (firstPass.pluginGeneratedFiles.empty()) {
        return firstPass;
    }
    Timer timeit(firstPass.gs->tracer(), "indexPluginFiles");
    auto resultq = make_shared<BlockingBoundedQueue<IndexThreadResultPack>>(firstPass.pluginGeneratedFiles.size());
    auto pluginFileq = make_shared<ConcurrentBoundedQueue<core::FileRef>>(firstPass.pluginGeneratedFiles.size());
    {
        core::UnfreezeFileTable unfreezeFiles(*firstPass.gs);
        for (const auto &file : firstPass.pluginGeneratedFiles) {
            auto generatedFile = firstPass.gs->enterFile(file);
            pluginFileq->push(move(generatedFile), 1);
        }
    }
    const shared_ptr<core::GlobalState> protoGs = move(firstPass.gs);
    workers.multiplexJob("indexPluginFiles", [protoGs, &opts, pluginFileq, resultq, &kvstore]() {
        Timer timeit(protoGs->tracer(), "indexPluginFilesWorker");
        auto localGs = protoGs->deepCopy();
        IndexThreadResultPack threadResult;
        core::FileRef job;

        for (auto result = pluginFileq->try_pop(job); !result.done(); result = pluginFileq->try_pop(job)) {
            if (result.gotItem()) {
                core::FileRef file = job;
                file.data(*localGs).strictLevel = decideStrictLevel(*localGs, file, opts);
                threadResult.res.trees.emplace_back(indexOne(opts, *localGs, file, kvstore));
            }
        }

        if (!threadResult.res.trees.empty()) {
            threadResult.counters = getAndClearThreadCounters();
            threadResult.res.gs = move(localGs);
            auto sizeIncrement = threadResult.res.trees.size();
            resultq->push(move(threadResult), sizeIncrement);
        }
    });
    auto indexedPluginFiles = mergeIndexResults(protoGs, opts, resultq, kvstore);
    IndexResult suppliedFilesAndPluginFiles;
    if (indexedPluginFiles.trees.empty()) {
        return firstPass;
    }
    suppliedFilesAndPluginFiles.gs = move(indexedPluginFiles.gs);

    {
        Timer timeit(suppliedFilesAndPluginFiles.gs->tracer(), "incremental_resolve");
        core::GlobalSubstitution substitution(*protoGs, *suppliedFilesAndPluginFiles.gs, protoGs.get());
        core::MutableContext ctx(*suppliedFilesAndPluginFiles.gs, core::Symbols::root());
        for (auto &tree : firstPass.trees) {
            auto file = tree.file;
            core::ErrorRegion errs(*suppliedFilesAndPluginFiles.gs, file);
            tree.tree = ast::Substitute::run(ctx, substitution, move(tree.tree));
        }
    }
    suppliedFilesAndPluginFiles.trees = move(firstPass.trees);
    suppliedFilesAndPluginFiles.trees.insert(suppliedFilesAndPluginFiles.trees.end(),
                                             make_move_iterator(indexedPluginFiles.trees.begin()),
                                             make_move_iterator(indexedPluginFiles.trees.end()));
    return suppliedFilesAndPluginFiles;
}

vector<ast::ParsedFile> index(unique_ptr<core::GlobalState> &gs, vector<core::FileRef> files,
                              const options::Options &opts, WorkerPool &workers, unique_ptr<KeyValueStore> &kvstore) {
    Timer timeit(gs->tracer(), "index");
    vector<ast::ParsedFile> ret;
    vector<ast::ParsedFile> empty;

    if (opts.stopAfterPhase == options::Phase::INIT) {
        return empty;
    }

    gs->sanityCheck();

    if (files.size() < 3) {
        // Run singlethreaded if only using 2 files
        size_t pluginFileCount = 0;
        for (auto file : files) {
            readFileWithStrictnessOverrides(gs, file, opts);
            auto [parsedFile, pluginFiles] = indexOneWithPlugins(opts, *gs, file, kvstore);
            ret.emplace_back(move(parsedFile));
            pluginFileCount += pluginFiles.size();
            for (auto &pluginFile : pluginFiles) {
                core::FileRef pluginFileRef;
                {
                    core::UnfreezeFileTable fileTableAccess(*gs);
                    pluginFileRef = gs->enterFile(pluginFile);
                    pluginFileRef.data(*gs).strictLevel = decideStrictLevel(*gs, pluginFileRef, opts);
                }
                ret.emplace_back(indexOne(opts, *gs, pluginFileRef, kvstore));
            }
            cacheTrees(*gs, kvstore, ret);
        }
        ENFORCE(files.size() + pluginFileCount == ret.size());
    } else {
        auto firstPass = indexSuppliedFiles(move(gs), files, opts, workers, kvstore);
        auto pluginPass = indexPluginFiles(move(firstPass), opts, workers, kvstore);
        gs = move(pluginPass.gs);
        ret = move(pluginPass.trees);
    }

    fast_sort(ret, [](ast::ParsedFile const &a, ast::ParsedFile const &b) { return a.file < b.file; });
    return ret;
}

ast::ParsedFile typecheckOne(core::Context ctx, ast::ParsedFile resolved, const options::Options &opts) {
    ast::ParsedFile result{make_unique<ast::EmptyTree>(), resolved.file};
    core::FileRef f = resolved.file;

    resolved = definition_validator::runOne(ctx, std::move(resolved));

    resolved = flatten::runOne(ctx, move(resolved));

    if (opts.print.FlattenedTree.enabled) {
        opts.print.FlattenedTree.fmt("{}\n", resolved.tree->toString(ctx));
    }
    if (opts.print.FlattenedTreeRaw.enabled) {
        opts.print.FlattenedTreeRaw.fmt("{}\n", resolved.tree->showRaw(ctx));
    }

    if (opts.stopAfterPhase == options::Phase::NAMER || opts.stopAfterPhase == options::Phase::RESOLVER) {
        return result;
    }
    if (f.data(ctx).isRBI()) {
        return result;
    }

    Timer timeit(ctx.state.tracer(), "typecheckOne", {{"file", (string)f.data(ctx).path()}});
    try {
        if (opts.print.CFG.enabled) {
            opts.print.CFG.fmt("digraph \"{}\" {{\n", FileOps::getFileName(f.data(ctx).path()));
        }
        CFGCollectorAndTyper collector(opts);
        {
            core::ErrorRegion errs(ctx, f);
            result.tree = ast::TreeMap::apply(ctx, collector, move(resolved.tree));
        }
        if (opts.print.CFG.enabled) {
            opts.print.CFG.fmt("}}\n\n");
        }
    } catch (SorbetException &) {
        Exception::failInFuzzer();
        if (auto e = ctx.state.beginError(sorbet::core::Loc::none(f), core::errors::Internal::InternalError)) {
            e.setHeader("Exception in cfg+infer: {} (backtrace is above)", f.data(ctx).path());
        }
    }
    return result;
}

struct typecheck_thread_result {
    vector<ast::ParsedFile> trees;
    CounterState counters;
};

vector<ast::ParsedFile> name(core::GlobalState &gs, vector<ast::ParsedFile> what, const options::Options &opts,
                             bool skipConfigatron) {
    Timer timeit(gs.tracer(), "name");
    if (!skipConfigatron) {
        core::UnfreezeNameTable nameTableAccess(gs);     // creates names from config
        core::UnfreezeSymbolTable symbolTableAccess(gs); // creates methods for them
        namer::configatron::fillInFromFileSystem(gs, opts.configatronDirs, opts.configatronFiles);
    }

    {
        ProgressIndicator namingProgress(opts.showProgress, "Naming", what.size());

        shared_ptr<namer::NamerCtx> namerCtx = make_shared<namer::NamerCtx>();
        int i = 0;
        for (auto &tree : what) {
            auto file = tree.file;
            try {
                ast::ParsedFile ast;
                {
                    core::MutableContext ctx(gs, core::Symbols::root());
                    Timer timeit(gs.tracer(), "naming", {{"file", (string)file.data(gs).path()}});
                    core::ErrorRegion errs(gs, file);
                    core::UnfreezeNameTable nameTableAccess(gs);     // creates singletons and class names
                    core::UnfreezeSymbolTable symbolTableAccess(gs); // enters symbols
                    tree = namer::Namer::run(ctx, namerCtx, move(tree));
                }
                gs.errorQueue->flushErrors();
                namingProgress.reportProgress(i);
                i++;
            } catch (SorbetException &) {
                Exception::failInFuzzer();
                if (auto e = gs.beginError(sorbet::core::Loc::none(file), core::errors::Internal::InternalError)) {
                    e.setHeader("Exception naming file: `{}` (backtrace is above)", file.data(gs).path());
                }
            }
        }
    }

    return what;
}
class GatherUnresolvedConstantsWalk {
public:
    vector<string> unresolvedConstants;
    unique_ptr<ast::Expression> postTransformConstantLit(core::MutableContext ctx,
                                                         unique_ptr<ast::ConstantLit> original) {
        auto unresolvedPath = original->fullUnresolvedPath(ctx);
        if (unresolvedPath.has_value()) {
            unresolvedConstants.emplace_back(fmt::format(
                "{}::{}",
                unresolvedPath->first != core::Symbols::root() ? unresolvedPath->first.data(ctx)->show(ctx) : "",
                fmt::map_join(unresolvedPath->second,
                              "::", [&](const auto &el) -> string { return el.data(ctx)->show(ctx); })));
        }
        return original;
    }
};

vector<ast::ParsedFile> printMissingConstants(core::GlobalState &gs, const options::Options &opts,
                                              vector<ast::ParsedFile> what) {
    Timer timeit(gs.tracer(), "printMissingConstants");
    core::MutableContext ctx(gs, core::Symbols::root());
    GatherUnresolvedConstantsWalk walk;
    for (auto &resolved : what) {
        resolved.tree = ast::TreeMap::apply(ctx, walk, move(resolved.tree));
    }
    fast_sort(walk.unresolvedConstants);
    opts.print.MissingConstants.fmt("{}\n", fmt::join(walk.unresolvedConstants, "\n"));
    return what;
}

class DefinitionLinesBlacklistEnforcer {
private:
    const core::FileRef file;
    const int prohibitedLinesStart;
    const int prohibitedLinesEnd;

    bool isWhiteListed(core::Context ctx, core::SymbolRef sym) {
        return sym.data(ctx)->name == core::Names::staticInit() ||
               sym.data(ctx)->name == core::Names::Constants::Root();
    }

    void checkLoc(core::Context ctx, core::Loc loc) {
        auto detailStart = core::Loc::offset2Pos(file.data(ctx), loc.beginPos());
        auto detailEnd = core::Loc::offset2Pos(file.data(ctx), loc.endPos());
        ENFORCE(!(detailStart.line >= prohibitedLinesStart && detailEnd.line <= prohibitedLinesEnd));
    }

    void checkSym(core::Context ctx, core::SymbolRef sym) {
        if (isWhiteListed(ctx, sym)) {
            return;
        }
        checkLoc(ctx, sym.data(ctx)->loc());
    }

public:
    DefinitionLinesBlacklistEnforcer(core::FileRef file, int prohibitedLinesStart, int prohibitedLinesEnd)
        : file(file), prohibitedLinesStart(prohibitedLinesStart), prohibitedLinesEnd(prohibitedLinesEnd) {
        // Can be equal if file was empty.
        ENFORCE(prohibitedLinesStart <= prohibitedLinesEnd);
        ENFORCE(file.exists());
    };

    unique_ptr<ast::ClassDef> preTransformClassDef(core::Context ctx, unique_ptr<ast::ClassDef> original) {
        checkSym(ctx, original->symbol);
        return original;
    }
    unique_ptr<ast::MethodDef> preTransformMethodDef(core::Context ctx, unique_ptr<ast::MethodDef> original) {
        checkSym(ctx, original->symbol);
        return original;
    }
};

ast::ParsedFile checkNoDefinitionsInsideProhibitedLines(core::GlobalState &gs, ast::ParsedFile what,
                                                        int prohibitedLinesStart, int prohibitedLinesEnd) {
    DefinitionLinesBlacklistEnforcer enforcer(what.file, prohibitedLinesStart, prohibitedLinesEnd);
    what.tree = ast::TreeMap::apply(core::Context(gs, core::Symbols::root()), enforcer, move(what.tree));
    return what;
}

vector<ast::ParsedFile> resolve(unique_ptr<core::GlobalState> &gs, vector<ast::ParsedFile> what,
                                const options::Options &opts, WorkerPool &workers, bool skipConfigatron) {
    try {
        what = name(*gs, move(what), opts, skipConfigatron);

        for (auto &named : what) {
            if (opts.print.NameTree.enabled) {
                opts.print.NameTree.fmt("{}\n", named.tree->toStringWithTabs(*gs, 0));
            }
            if (opts.print.NameTreeRaw.enabled) {
                opts.print.NameTreeRaw.fmt("{}\n", named.tree->showRaw(*gs));
            }
        }

        if (opts.stopAfterPhase == options::Phase::NAMER) {
            return what;
        }

        core::MutableContext ctx(*gs, core::Symbols::root());
        ProgressIndicator namingProgress(opts.showProgress, "Resolving", 1);
        {
            Timer timeit(gs->tracer(), "resolving");
            vector<core::ErrorRegion> errs;
            for (auto &tree : what) {
                auto file = tree.file;
                errs.emplace_back(*gs, file);
            }
            core::UnfreezeNameTable nameTableAccess(*gs);     // Resolver::defineAttr
            core::UnfreezeSymbolTable symbolTableAccess(*gs); // enters stubs
            what = resolver::Resolver::run(ctx, move(what), workers);
        }
        if (opts.stressIncrementalResolver) {
            for (auto &f : what) {
                // Shift contents of file past current file's EOF, re-run incrementalResolve, assert that no locations
                // appear before file's old EOF.
                const int prohibitedLines = f.file.data(*gs).source().size();
                auto newSource = fmt::format("{}\n{}", string(prohibitedLines, '\n'), f.file.data(*gs).source());
                auto newFile = make_shared<core::File>((string)f.file.data(*gs).path(), move(newSource),
                                                       f.file.data(*gs).sourceType);
                gs = core::GlobalState::replaceFile(move(gs), f.file, move(newFile));
                unique_ptr<KeyValueStore> kvstore;
                f.file.data(*gs).strictLevel = decideStrictLevel(*gs, f.file, opts);
                auto reIndexed = indexOne(opts, *gs, f.file, kvstore);
                vector<ast::ParsedFile> toBeReResolved;
                toBeReResolved.emplace_back(move(reIndexed));
                auto reresolved = pipeline::incrementalResolve(*gs, move(toBeReResolved), opts);
                ENFORCE(reresolved.size() == 1);
                f = checkNoDefinitionsInsideProhibitedLines(*gs, move(reresolved[0]), 0, prohibitedLines);
            }
        }
    } catch (SorbetException &) {
        Exception::failInFuzzer();
        if (auto e = gs->beginError(sorbet::core::Loc::none(), core::errors::Internal::InternalError)) {
            e.setHeader("Exception resolving (backtrace is above)");
        }
    }

    gs->errorQueue->flushErrors();
    if (opts.print.ResolveTree.enabled || opts.print.ResolveTreeRaw.enabled) {
        for (auto &resolved : what) {
            if (opts.print.ResolveTree.enabled) {
                opts.print.ResolveTree.fmt("{}\n", resolved.tree->toString(*gs));
            }
            if (opts.print.ResolveTreeRaw.enabled) {
                opts.print.ResolveTreeRaw.fmt("{}\n", resolved.tree->showRaw(*gs));
            }
        }
    }
    if (opts.print.MissingConstants.enabled) {
        what = printMissingConstants(*gs, opts, move(what));
    }

    return what;
}

vector<ast::ParsedFile> typecheck(unique_ptr<core::GlobalState> &gs, vector<ast::ParsedFile> what,
                                  const options::Options &opts, WorkerPool &workers) {
    vector<ast::ParsedFile> typecheck_result;

    {
        Timer timeit(gs->tracer(), "typecheck");

        shared_ptr<ConcurrentBoundedQueue<ast::ParsedFile>> fileq;
        shared_ptr<BlockingBoundedQueue<typecheck_thread_result>> resultq;

        {
            fileq = make_shared<ConcurrentBoundedQueue<ast::ParsedFile>>(what.size());
            resultq = make_shared<BlockingBoundedQueue<typecheck_thread_result>>(what.size());
        }

        core::Context ctx(*gs, core::Symbols::root());

        for (auto &resolved : what) {
            fileq->push(move(resolved), 1);
        }

        {
            ProgressIndicator cfgInferProgress(opts.showProgress, "CFG+Inference", what.size());
            workers.multiplexJob("typecheck", [ctx, &opts, fileq, resultq]() {
                typecheck_thread_result threadResult;
                ast::ParsedFile job;
                int processedByThread = 0;

                {
                    for (auto result = fileq->try_pop(job); !result.done(); result = fileq->try_pop(job)) {
                        if (result.gotItem()) {
                            processedByThread++;
                            core::FileRef file = job.file;
                            try {
                                threadResult.trees.emplace_back(typecheckOne(ctx, move(job), opts));
                            } catch (SorbetException &) {
                                Exception::failInFuzzer();
                                ctx.state.tracer().error("Exception typing file: {} (backtrace is above)",
                                                         file.data(ctx).path());
                            }
                        }
                    }
                }
                if (processedByThread > 0) {
                    threadResult.counters = getAndClearThreadCounters();
                    resultq->push(move(threadResult), processedByThread);
                }
            });

            typecheck_thread_result threadResult;
            {
                for (auto result = resultq->wait_pop_timed(threadResult, WorkerPool::BLOCK_INTERVAL(), gs->tracer());
                     !result.done();
                     result = resultq->wait_pop_timed(threadResult, WorkerPool::BLOCK_INTERVAL(), gs->tracer())) {
                    if (result.gotItem()) {
                        counterConsume(move(threadResult.counters));
                        typecheck_result.insert(typecheck_result.end(), make_move_iterator(threadResult.trees.begin()),
                                                make_move_iterator(threadResult.trees.end()));
                    }
                    cfgInferProgress.reportProgress(fileq->doneEstimate());
                    gs->errorQueue->flushErrors();
                }
            }
        }

        if (opts.print.SymbolTable.enabled) {
            opts.print.SymbolTable.fmt("{}\n", gs->toString());
        }
        if (opts.print.SymbolTableRaw.enabled) {
            opts.print.SymbolTableRaw.fmt("{}\n", gs->showRaw());
        }
        if (opts.print.SymbolTableJson.enabled) {
            auto root = core::Proto::toProto(*gs, core::Symbols::root(), false);
            if (opts.print.SymbolTableJson.outputPath.empty()) {
                core::Proto::toJSON(root, cout);
            } else {
                stringstream buf;
                core::Proto::toJSON(root, buf);
                opts.print.SymbolTableJson.print(buf.str());
            }
        }
        if (opts.print.SymbolTableFullJson.enabled) {
            auto root = core::Proto::toProto(*gs, core::Symbols::root(), true);
            if (opts.print.SymbolTableJson.outputPath.empty()) {
                core::Proto::toJSON(root, cout);
            } else {
                stringstream buf;
                core::Proto::toJSON(root, buf);
                opts.print.SymbolTableJson.print(buf.str());
            }
        }
        if (opts.print.SymbolTableFull.enabled) {
            opts.print.SymbolTableFull.fmt("{}\n", gs->toStringFull());
        }
        if (opts.print.SymbolTableFullRaw.enabled) {
            opts.print.SymbolTableFullRaw.fmt("{}\n", gs->showRawFull());
        }
        if (opts.print.FileTableJson.enabled) {
            auto files = core::Proto::filesToProto(*gs);
            if (opts.print.FileTableJson.outputPath.empty()) {
                core::Proto::toJSON(files, cout);
            } else {
                stringstream buf;
                core::Proto::toJSON(files, buf);
                opts.print.FileTableJson.print(buf.str());
            }
        }
        if (opts.print.PluginGeneratedCode.enabled) {
            plugin::Plugins::dumpPluginGeneratedFiles(*gs, opts.print.PluginGeneratedCode);
        }

        return typecheck_result;
    }
}

class AllNamesCollector {
public:
    core::UsageHash acc;
    unique_ptr<ast::Send> preTransformSend(core::Context ctx, unique_ptr<ast::Send> original) {
        acc.sends.emplace_back(ctx.state, original->fun.data(ctx));
        return original;
    }

    unique_ptr<ast::MethodDef> postTransformMethodDef(core::Context ctx, unique_ptr<ast::MethodDef> original) {
        acc.constants.emplace_back(ctx.state, original->name.data(ctx.state));
        return original;
    }

    void handleUnresolvedConstantLit(core::Context ctx, ast::UnresolvedConstantLit *expr) {
        while (expr) {
            acc.constants.emplace_back(ctx.state, expr->cnst.data(ctx));
            // Handle references to 'Foo' in 'Foo::Bar'.
            expr = ast::cast_tree<ast::UnresolvedConstantLit>(expr->scope.get());
        }
    }

    unique_ptr<ast::ClassDef> postTransformClassDef(core::Context ctx, unique_ptr<ast::ClassDef> original) {
        acc.constants.emplace_back(ctx.state, original->symbol.data(ctx)->name.data(ctx));
        original->name->showRaw(ctx.state);

        handleUnresolvedConstantLit(ctx, ast::cast_tree<ast::UnresolvedConstantLit>(original->name.get()));

        // Grab names of superclasses. (N.B. `include` and `extend` are captured as ConstantLits.)
        for (auto &ancst : original->ancestors) {
            handleUnresolvedConstantLit(ctx, ast::cast_tree<ast::UnresolvedConstantLit>(ancst.get()));
        }

        return original;
    }

    unique_ptr<ast::UnresolvedConstantLit>
    postTransformUnresolvedConstantLit(core::Context ctx, unique_ptr<ast::UnresolvedConstantLit> original) {
        handleUnresolvedConstantLit(ctx, original.get());
        return original;
    }

    unique_ptr<ast::UnresolvedIdent> postTransformUnresolvedIdent(core::Context ctx,
                                                                  unique_ptr<ast::UnresolvedIdent> id) {
        if (id->kind != ast::UnresolvedIdent::Local) {
            acc.constants.emplace_back(ctx.state, id->name.data(ctx));
        }
        return id;
    }
};

core::UsageHash getAllNames(const core::GlobalState &gs, unique_ptr<ast::Expression> &tree) {
    AllNamesCollector collector;
    tree = ast::TreeMap::apply(core::Context(gs, core::Symbols::root()), collector, move(tree));
    core::NameHash::sortAndDedupe(collector.acc.sends);
    core::NameHash::sortAndDedupe(collector.acc.constants);
    return move(collector.acc);
};

core::FileHash computeFileHash(shared_ptr<core::File> forWhat, spdlog::logger &logger) {
    Timer timeit(logger, "computeFileHash");
    const static options::Options emptyOpts{};
    unique_ptr<core::GlobalState> lgs = make_unique<core::GlobalState>((make_shared<core::ErrorQueue>(logger, logger)));
    lgs->initEmpty();
    lgs->errorQueue->ignoreFlushes = true;
    lgs->silenceErrors = true;
    core::FileRef fref;
    {
        core::UnfreezeFileTable fileTableAccess(*lgs);
        fref = lgs->enterFile(forWhat);
        fref.data(*lgs).strictLevel = pipeline::decideStrictLevel(*lgs, fref, emptyOpts);
    }
    vector<ast::ParsedFile> single;
    unique_ptr<KeyValueStore> kvstore;

    single.emplace_back(pipeline::indexOne(emptyOpts, *lgs, fref, kvstore));
    auto errs = lgs->errorQueue->drainAllErrors();
    for (auto &e : errs) {
        if (e->what == core::errors::Parser::ParserError) {
            core::GlobalStateHash invalid;
            invalid.hierarchyHash = core::GlobalStateHash::HASH_STATE_INVALID;
            return {move(invalid), {}};
        }
    }
    auto allNames = getAllNames(*lgs, single[0].tree);
    auto workers = WorkerPool::create(0, lgs->tracer());
    pipeline::resolve(lgs, move(single), emptyOpts, *workers, true);

    return {move(*lgs->hash()), move(allNames)};
}

} // namespace sorbet::realmain::pipeline
