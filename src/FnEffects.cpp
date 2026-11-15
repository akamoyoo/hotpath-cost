// fn-effects: an LLVM pass plugin (new pass manager) that computes, for every function of a
// whole-program module, the effect classes it can reach (the policy defines the classes), and for each
// selected entry function the full list of sites with call chains, hot/cold tags and assumptions.
//
// Usage: opt -load-pass-plugin=FnEffects.so -passes=fn-effects -disable-output whole.bc
//            -fx-policy=policies/hotpath.json -fx-out=report.json [-fx-effects=effects.json]
//            [-fx-entry=PATTERN ...] [-fx-summary=lib.json ...]
//
// Nothing about particular hazard classes is in this file: classes, the symbols and instruction kinds that
// define them, the known-harmless symbols, the ABI facts (terminate handlers, registration-only functions)
// and the entry selectors all come from the policy file. The engine provides generic matchers, the call
// graph (direct, virtual via type metadata, function pointers by type, constant tables, callbacks to
// external functions), the hot/cold rule, debug locations, recursion and the effect propagation.

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "PointsTo.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/TypeMetadataUtils.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/ProfDataUtils.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/GlobPattern.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <functional>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace llvm;
namespace json = llvm::json;

static cl::opt<std::string> OptPolicy("fx-policy", cl::desc("fn-effects: policy JSON file"), cl::init(""));
static cl::list<std::string> OptEntry("fx-entry", cl::desc("fn-effects: entry pattern (mangled, demangled, glob)"));
static cl::opt<std::string> OptEntryKind("fx-entry-kind", cl::desc("fn-effects: entry kind for -fx-entry"), cl::init(""));
static cl::list<std::string> OptSummary("fx-summary", cl::desc("fn-effects: library summary JSON file"));
static cl::opt<std::string> OptOut("fx-out", cl::desc("fn-effects: report JSON output"), cl::init("-"));
static cl::opt<std::string> OptEffects("fx-effects", cl::desc("fn-effects: effect table JSON output"), cl::init(""));
static cl::opt<std::string> OptTarget("fx-target", cl::desc("fn-effects: target name for the report"), cl::init(""));
static cl::list<std::string> OptPathMap("fx-path-map", cl::desc("fn-effects: FROM=TO rewrite of source paths"));
static cl::opt<std::string> OptMissing("fx-missing", cl::desc("fn-effects: JSON {symbol: tu} of functions defined in translation units without bitcode"), cl::init(""));
static cl::opt<bool> OptAllFindings("fx-all-findings", cl::desc("fn-effects: list findings of informational classes too (default: counts only)"), cl::init(false));
static cl::opt<std::string> OptPrecision("fx-precision", cl::desc("fn-effects: type | rta | pta (default: the policy's, else pta)"), cl::init(""));
static cl::opt<bool> OptFull("fx-full", cl::desc("fn-effects: findings per function with every site, and per-function lint"), cl::init(false));
static cl::opt<double> OptPtaBudget("fx-pta-budget", cl::desc("fn-effects: seconds after which the points-to analysis is abandoned (0: none)"), cl::init(0));
static cl::opt<int> OptIncomplete("fx-incomplete", cl::desc("fn-effects: number of translation units that did not compile (RTA and the closed-hierarchy rule are off when > 0)"), cl::init(0));
static cl::opt<bool> OptStrict("fx-strict", cl::desc("fn-effects: every none that rests on library data (summaries, known patterns, pointer models, closed-world rules) becomes unknown"), cl::init(false));
static cl::opt<std::string> OptPtaScope("fx-pta-scope", cl::desc("fn-effects: module (one points-to solve over the module, default) | component (per strongly connected component of the direct-call graph, calls between components cut) | auto (module, and component if the module solve is over its budget)"), cl::init("module"));
static cl::opt<bool> OptEmulatedTLS("fx-emulated-tls", cl::desc("fn-effects: the program is compiled with -femulated-tls (not recorded in the IR): every thread-local access is a call of __emutls_get_address"), cl::init(false));
static cl::opt<bool> OptNoModelCheck("fx-no-model-check", cl::desc("fn-effects: do not re-solve the points-to analysis without pointer models (every points-to resolution then trusts them)"), cl::init(false));
static cl::opt<bool> OptNoSelfCheck("fx-no-self-check", cl::desc("fn-effects: skip the table/BFS agreement check"), cl::init(false));

namespace {

// ------------------------------------------------------------------------------------------------ utils

std::string demangleName(StringRef N) {
  static StringMap<std::string> Cache;
  auto It = Cache.find(N);
  if (It != Cache.end()) return It->second;
  std::string D = llvm::demangle(N.str());
  Cache[N] = D;
  return D;
}

std::string stripParams(const std::string &D) {
  // "ns::C::f(int) const" -> "ns::C::f"; templates in the name are kept as they are.
  int depth = 0;
  for (size_t i = 0; i < D.size(); i++) {
    char c = D[i];
    if (c == '<') depth++;
    else if (c == '>') depth--;
    else if (c == '(' && depth == 0 && i > 0) return D.substr(0, i);
  }
  return D;
}

std::string mapPath(std::string P) {
  for (auto &M : OptPathMap) {
    size_t Eq = M.find('=');
    if (Eq == std::string::npos) continue;
    std::string From = M.substr(0, Eq), To = M.substr(Eq + 1);
    if (!From.empty() && StringRef(P).starts_with(From)) {
      std::string Rest = P.substr(From.size());
      if (!Rest.empty() && Rest[0] == '/' && (To.empty() || To.back() == '/')) Rest = Rest.substr(1);
      P = To + Rest;
      if (StringRef(P).starts_with("./")) P = P.substr(2);
      break;
    }
  }
  return P;
}

std::string fullPath(const DIScope *S, StringRef File) {
  if (File.empty()) return "";
  if (sys::path::is_absolute(File) || !S) return mapPath(File.str());
  StringRef Dir = S->getDirectory();
  if (Dir.empty()) return mapPath(File.str());
  SmallString<256> P(Dir);
  sys::path::append(P, File);
  return mapPath(std::string(P.str()));
}

std::string typeStr(Type *T) {
  std::string S;
  raw_string_ostream OS(S);
  T->print(OS);
  return S;
}

// --------------------------------------------------------------------------------------------- patterns

// A pattern string is one of: "exact", "glob*" (contains * ? or [), "re:REGEX" (POSIX ERE, anchored by the
// author), each optionally prefixed with "dem:" to match against the demangled name instead of the symbol.
struct Pattern {
  bool Dem = false;
  enum { Exact, Glob, Re } Kind = Exact;
  std::string Text, Sub, Temp;
  std::optional<GlobPattern> G;
  std::shared_ptr<std::string> GText;  // GlobPattern keeps a StringRef into its source text
  std::shared_ptr<Regex> R;
  // For an anchored regex: the literal prefixes one of which the name must start with (empty: no filter).
  // LLVM's Regex is slow on long alternations, and most names fail this test.
  std::vector<std::string> Prefixes;
  bool matches(StringRef Mangled) const {
    std::string DemS;
    StringRef S = Mangled;
    if (Dem) { DemS = demangleName(Mangled); S = DemS; }
    switch (Kind) {
    case Exact: return S == Text;
    case Glob: return G->match(S);
    case Re:
      if (!Prefixes.empty()) {
        bool Any = false;
        for (auto &Px : Prefixes) if (S.starts_with(Px)) { Any = true; break; }
        if (!Any) {
          if (getenv("FX_CHECK_PREFILTER") && R->match(S)) {
            errs() << "fn-effects: prefilter bug: '" << Text << "' matches '" << S << "'\n";
            abort();
          }
          return false;
        }
      }
      return R->match(S);
    }
    return false;
  }
};

struct PatList {
  std::vector<Pattern> V;
  const Pattern *match(StringRef S) const {
    for (auto &P : V)
      if (P.matches(S)) return &P;
    return nullptr;
  }
  bool empty() const { return V.empty(); }
};

// Literal prefixes of an anchored regex "^lit..." or "^(alt1|alt2|...)...": each alternative's leading
// literal characters. Any doubt (an alternative that starts with a metacharacter) disables the filter.
std::vector<std::string> literalPrefixes(StringRef Re) {
  auto IsMeta = [](char C) { return strchr("[](){}.*+?|^$\\", C) != nullptr; };
  std::vector<std::string> Out;
  if (!Re.consume_front("^")) return Out;
  if (!Re.starts_with("(")) {
    std::string L;
    for (char C : Re) { if (IsMeta(C)) break; L += C; }
    // a quantifier right after the literal applies to its last character
    if (!L.empty() && L.size() < Re.size() && strchr("*?{", Re[L.size()])) L.pop_back();
    if (!L.empty()) Out.push_back(L);
    return Out;
  }
  int Depth = 0;
  size_t I = 1;
  std::string Cur;
  bool InLit = true;
  for (; I < Re.size(); I++) {
    char C = Re[I];
    if (C == '\\') { InLit = false; I++; continue; }
    if (C == '(') { Depth++; InLit = false; continue; }
    if (C == ')') {
      if (Depth == 0) break;
      Depth--;
      continue;
    }
    if (C == '|' && Depth == 0) {
      if (Cur.empty()) return {};
      Out.push_back(Cur);
      Cur.clear();
      InLit = true;
      continue;
    }
    if (InLit && !IsMeta(C)) {
      // a quantifier on this character would make it optional
      if (I + 1 < Re.size() && strchr("*?{", Re[I + 1])) { InLit = false; continue; }
      Cur += C;
    } else {
      InLit = false;
    }
  }
  if (I >= Re.size() || Cur.empty()) return {};
  if (I + 1 < Re.size() && strchr("*?{", Re[I + 1])) return {};  // the whole group is optional
  Out.push_back(Cur);
  return Out;
}

bool parsePattern(const json::Value &JV, Pattern &P, std::string &Err) {
  std::string Text;
  if (auto S = JV.getAsString()) Text = S->str();
  else if (auto *O = JV.getAsObject()) {
    if (auto S = O->getString("pattern")) Text = S->str();
    if (auto S = O->getString("subclass")) P.Sub = S->str();
    if (auto S = O->getString("temp")) P.Temp = S->str();
  } else { Err = "pattern must be a string or object"; return false; }
  StringRef T(Text);
  if (T.consume_front("dem:")) P.Dem = true;
  if (T.consume_front("re:")) {
    P.Kind = Pattern::Re;
    P.R = std::make_shared<Regex>(T);
    std::string E;
    if (!P.R->isValid(E)) { Err = "bad regex '" + T.str() + "': " + E; return false; }
    P.Prefixes = literalPrefixes(T);
  } else if (T.find_first_of("*?[") != StringRef::npos) {
    P.Kind = Pattern::Glob;
    P.GText = std::make_shared<std::string>(T.str());
    auto G = GlobPattern::create(*P.GText);
    if (!G) { Err = toString(G.takeError()); return false; }
    P.G.emplace(std::move(*G));
  } else {
    P.Kind = Pattern::Exact;
  }
  P.Text = T.str();
  return true;
}

bool parsePatList(const json::Value *JV, PatList &L, std::string &Err) {
  if (!JV) return true;
  auto *A = JV->getAsArray();
  if (!A) { Err = "expected an array of patterns"; return false; }
  for (auto &E : *A) {
    Pattern P;
    if (!parsePattern(E, P, Err)) return false;
    L.V.push_back(std::move(P));
  }
  return true;
}

// ----------------------------------------------------------------------------------------------- policy

struct ClassDef {
  std::string Name, Description;
  PatList Symbols, Intrinsics, Exclude;
  // Primitives: library functions whose every call is an operation of the class (malloc, operator new,
  // pthread_mutex_lock, a system-call wrapper). Only they can make a function `always` of the class, and
  // what a library summary says happens inside them is attributed to them (a consequence), not counted as
  // an operation of another class on its own account (README, "Per-function signatures").
  PatList Primitives;
  // Symbols whose sites in OTHER classes are consequences of this class (throw: __cxa_allocate_exception).
  PatList Attributed;
  // Rule (f): primitives LLVM may remove after the analysis (an allocation whose result does not escape,
  // a release of a null or fresh pointer). An `always` that rests only on these is `always (elidable)`.
  PatList Elidable;
  // Symbols of this class that are on the function's own account although they are primitives of another
  // class (free: realloc, which releases the old block itself).
  PatList OwnAccount;
  bool HasExcept = false;          // class = every external call NOT matching ExceptList
  PatList ExceptList;
  std::vector<std::string> Insts;  // instruction kinds
  std::vector<std::pair<std::shared_ptr<Regex>, std::string>> Asm;  // inline asm regex -> subclass
  std::vector<std::string> SummaryClasses;
  bool Informational = false;
  bool Engine = false;  // generated by the engine (recursion, indirect-unknown, external-unknown)
  bool Hidden = false;  // "unknown:<c>": not a class of its own, makes <c> unknown
};

struct EntryKind {
  std::string Name;
  std::vector<std::string> Select;
  std::vector<std::string> Forbid;
  std::string Start = "all";  // "all" | "terminate-invokes"
};

struct Policy {
  std::string Name, Description;
  std::vector<ClassDef> Classes;
  PatList Known;               // external symbols known to reach none of the classes
  PatList Opaque;              // functions treated as leaves even when they have a body (custom allocators)
  std::vector<std::string> KnownIfCallAttrs;  // external callee with this call-site/declaration attribute is known
  std::vector<std::string> PruneCallAttrs;    // call sites with this attribute are not followed at all
  PatList Terminate;           // ABI: functions that terminate the program after an escaped exception
  PatList RegistrationOnly;    // ABI: external functions whose function-pointer arguments are not called now
  PatList ColdTargets;         // targets (any via) whose sites are tagged cold
  PatList OpenTypes;           // type ids whose class hierarchy is open (derived classes may live in libraries)
  PatList Libm, LibmExact;     // lint
  // Compiler facts: calls that exist only after code generation, or that an intrinsic lowers to.
  std::vector<std::pair<Pattern, std::vector<std::string>>> IntrinsicLibcalls;  // "$1" = first regex group
  std::vector<int64_t> IntrinsicInlineMax;  // per entry: constant length (argument 2) up to which no call is made
  PatList IntrinsicsNoCall;
  struct AttrCall { std::string Attr; std::vector<std::string> Calls; std::string Temp; bool NeedsAlloca = false; std::vector<std::string> SkipValues; };
  std::set<std::string> KnownFnAttrs, KnownModuleFlags, AsmHarmless;
  std::vector<AttrCall> AttrLibcalls;
  std::string FremLibcall = "fmod";
  int AtomicInlineMaxBytes = 8;
  PatList ExitFunctions, ExitRegistrations, DynamicLoading, DlsymLike;
  std::vector<std::pair<std::string, int>> ExecMapping;  // function, index of the prot argument
  std::shared_ptr<Regex> AsmUnknown;
  std::string FnptrMatch = "exact";
  int Precision = 2;                    // 0 type rule, 1 + rapid type analysis, 2 + points-to
  std::string ExtCallee = "unknown";    // a callee that may come from library code: unknown | assume-module
  std::vector<std::pair<Pattern, fneffects::PtrModel>> PointerModels;
  PatList CalledFromOutside;
  bool Lint = true;
  double ColdProb = 0.002;
  std::vector<EntryKind> Kinds;
  std::vector<std::string> GlobalAssumptions;
  std::vector<std::string> Files;

  int classIndex(StringRef N) const {
    for (size_t i = 0; i < Classes.size(); i++)
      if (Classes[i].Name == N) return (int)i;
    return -1;
  }
};

bool loadPolicyFile(const std::string &Path, Policy &P, std::string &Err, int Depth = 0) {
  if (Depth > 8) { Err = "policy include depth exceeded"; return false; }
  auto Buf = MemoryBuffer::getFile(Path);
  if (!Buf) { Err = "cannot read policy " + Path; return false; }
  auto J = json::parse((*Buf)->getBuffer());
  if (!J) { Err = Path + ": " + toString(J.takeError()); return false; }
  auto *O = J->getAsObject();
  if (!O) { Err = Path + ": top level must be an object"; return false; }
  P.Files.push_back(sys::path::filename(Path).str());
  if (auto *Inc = O->getArray("include")) {
    for (auto &I : *Inc) {
      auto S = I.getAsString();
      if (!S) continue;
      SmallString<256> IP(sys::path::parent_path(Path));
      sys::path::append(IP, *S);
      if (!loadPolicyFile(std::string(IP.str()), P, Err, Depth + 1)) return false;
    }
  }
  if (auto S = O->getString("name")) P.Name = S->str();
  if (auto S = O->getString("description")) P.Description = S->str();
  auto Ctx = [&](const std::string &What) { Err = Path + ": " + What + ": " + Err; return false; };
  if (auto *C = O->getObject("classes")) {
    // JSON objects are unordered in llvm::json; keep a stable order by name.
    std::vector<std::string> Names;
    for (auto &KV : *C) Names.push_back(KV.first.str());
    std::sort(Names.begin(), Names.end());
    for (auto &N : Names) {
      auto *CO = C->getObject(N);
      if (!CO) { Err = "class must be an object"; return Ctx(N); }
      ClassDef D;
      D.Name = N;
      if (auto S = CO->getString("description")) D.Description = S->str();
      if (!parsePatList(CO->get("symbols"), D.Symbols, Err)) return Ctx(N);
      if (!parsePatList(CO->get("intrinsics"), D.Intrinsics, Err)) return Ctx(N);
      if (!parsePatList(CO->get("exclude"), D.Exclude, Err)) return Ctx(N);
      if (!parsePatList(CO->get("primitives"), D.Primitives, Err)) return Ctx(N);
      if (!parsePatList(CO->get("attributed"), D.Attributed, Err)) return Ctx(N);
      if (!parsePatList(CO->get("elidable"), D.Elidable, Err)) return Ctx(N);
      if (!parsePatList(CO->get("own_account"), D.OwnAccount, Err)) return Ctx(N);
      if (CO->get("external_except")) {
        D.HasExcept = true;
        if (!parsePatList(CO->get("external_except"), D.ExceptList, Err)) return Ctx(N);
      }
      if (auto *A = CO->getArray("instructions"))
        for (auto &E : *A)
          if (auto S = E.getAsString()) D.Insts.push_back(S->str());
      if (auto *A = CO->getArray("asm"))
        for (auto &E : *A) {
          std::string Re, Sub;
          if (auto S = E.getAsString()) Re = S->str();
          else if (auto *EO = E.getAsObject()) {
            if (auto S = EO->getString("pattern")) Re = S->str();
            if (auto S = EO->getString("subclass")) Sub = S->str();
          }
          auto R = std::make_shared<Regex>(Re);
          std::string E2;
          if (!R->isValid(E2)) { Err = "bad asm regex: " + E2; return Ctx(N); }
          D.Asm.push_back({R, Sub});
        }
      if (auto *A = CO->getArray("summary_classes")) {
        for (auto &E : *A)
          if (auto S = E.getAsString()) D.SummaryClasses.push_back(S->str());
      } else {
        D.SummaryClasses.push_back(N);
      }
      if (auto B = CO->getBoolean("informational")) D.Informational = *B;
      int Idx = P.classIndex(N);
      bool Extend = CO->getBoolean("extend").value_or(false);
      if (Idx >= 0 && Extend) {
        // A user policy adds patterns to a class of an included policy instead of replacing it.
        auto &Old = P.Classes[Idx];
        for (auto &X : D.Symbols.V) Old.Symbols.V.push_back(X);
        for (auto &X : D.Intrinsics.V) Old.Intrinsics.V.push_back(X);
        for (auto &X : D.Exclude.V) Old.Exclude.V.push_back(X);
        for (auto &X : D.Primitives.V) Old.Primitives.V.push_back(X);
        for (auto &X : D.Attributed.V) Old.Attributed.V.push_back(X);
        for (auto &X : D.Elidable.V) Old.Elidable.V.push_back(X);
        for (auto &X : D.OwnAccount.V) Old.OwnAccount.V.push_back(X);
        for (auto &X : D.ExceptList.V) Old.ExceptList.V.push_back(X);
        for (auto &X : D.Insts) Old.Insts.push_back(X);
        for (auto &X : D.Asm) Old.Asm.push_back(X);
      } else if (Idx >= 0) P.Classes[Idx] = std::move(D);
      else P.Classes.push_back(std::move(D));
    }
  }
  if (!parsePatList(O->get("known"), P.Known, Err)) return Ctx("known");
  if (!parsePatList(O->get("opaque"), P.Opaque, Err)) return Ctx("opaque");
  if (auto *A = O->getArray("known_if_call_attrs"))
    for (auto &E : *A) if (auto S = E.getAsString()) P.KnownIfCallAttrs.push_back(S->str());
  if (auto *A = O->getArray("prune_call_attrs"))
    for (auto &E : *A) if (auto S = E.getAsString()) P.PruneCallAttrs.push_back(S->str());
  if (auto *A = O->getObject("abi")) {
    if (!parsePatList(A->get("terminate"), P.Terminate, Err)) return Ctx("abi.terminate");
    if (!parsePatList(A->get("registration_only"), P.RegistrationOnly, Err)) return Ctx("abi.registration_only");
    if (!parsePatList(A->get("cold_targets"), P.ColdTargets, Err)) return Ctx("abi.cold_targets");
    if (!parsePatList(A->get("open_types"), P.OpenTypes, Err)) return Ctx("abi.open_types");
    if (auto *IL = A->getArray("intrinsic_libcalls"))
      for (auto &E : *IL) {
        auto *EO = E.getAsObject();
        if (!EO) continue;
        Pattern Pt;
        if (!parsePattern(*EO->get("pattern"), Pt, Err)) return Ctx("abi.intrinsic_libcalls");
        std::vector<std::string> Calls;
        if (auto *CA = EO->getArray("calls"))
          for (auto &X : *CA) if (auto XS = X.getAsString()) Calls.push_back(XS->str());
        P.IntrinsicLibcalls.push_back({Pt, Calls});
        P.IntrinsicInlineMax.push_back(EO->getInteger("inline_max_bytes").value_or(-1));
      }
    if (!parsePatList(A->get("intrinsics_no_call"), P.IntrinsicsNoCall, Err)) return Ctx("abi.intrinsics_no_call");
    if (auto *AL = A->getArray("attr_libcalls"))
      for (auto &E : *AL) {
        auto *EO = E.getAsObject();
        if (!EO) continue;
        Policy::AttrCall AC;
        AC.Attr = EO->getString("attr").value_or("").str();
        AC.Temp = EO->getString("temp").value_or("").str();
        AC.NeedsAlloca = EO->getBoolean("needs_alloca").value_or(false);
        if (auto *CA = EO->getArray("calls"))
          for (auto &X : *CA) if (auto XS = X.getAsString()) AC.Calls.push_back(XS->str());
        if (auto *CA = EO->getArray("skip_values"))
          for (auto &X : *CA) if (auto XS = X.getAsString()) AC.SkipValues.push_back(XS->str());
        P.AttrLibcalls.push_back(AC);
      }
    for (auto [Key, Set] : {std::pair<const char *, std::set<std::string> *>{"known_fn_attrs", &P.KnownFnAttrs},
                            {"known_module_flags", &P.KnownModuleFlags}, {"asm_harmless", &P.AsmHarmless}})
      if (auto *KA = A->getArray(Key))
        for (auto &X : *KA) if (auto XS = X.getAsString()) Set->insert(XS->str());
    if (auto S2 = A->getString("frem_libcall")) P.FremLibcall = S2->str();
    if (auto N2 = A->getInteger("atomic_inline_max_bytes")) P.AtomicInlineMaxBytes = (int)*N2;
    if (!parsePatList(A->get("exit_functions"), P.ExitFunctions, Err)) return Ctx("abi.exit_functions");
    if (!parsePatList(A->get("exit_registrations"), P.ExitRegistrations, Err)) return Ctx("abi.exit_registrations");
    if (!parsePatList(A->get("dynamic_loading"), P.DynamicLoading, Err)) return Ctx("abi.dynamic_loading");
    if (!parsePatList(A->get("dlsym_like"), P.DlsymLike, Err)) return Ctx("abi.dlsym_like");
    if (auto *EM = A->getArray("exec_mapping"))
      for (auto &E : *EM)
        if (auto *EO = E.getAsObject())
          P.ExecMapping.push_back({EO->getString("function").value_or("").str(), (int)EO->getInteger("prot_arg").value_or(2)});
    if (auto S2 = A->getString("asm_unknown")) {
      P.AsmUnknown = std::make_shared<Regex>(*S2);
      std::string E2;
      if (!P.AsmUnknown->isValid(E2)) { Err = E2; return Ctx("abi.asm_unknown"); }
    }
  }
  if (auto *L = O->getObject("lint")) {
    if (auto B = L->getBoolean("enabled")) P.Lint = *B;
    if (!parsePatList(L->get("libm"), P.Libm, Err)) return Ctx("lint.libm");
    if (!parsePatList(L->get("libm_correctly_rounded"), P.LibmExact, Err)) return Ctx("lint.libm_correctly_rounded");
  }
  if (auto *Rz = O->getObject("resolution"))
    if (auto S2 = Rz->getString("fnptr_match")) P.FnptrMatch = S2->str();
  if (auto *Rz = O->getObject("resolution")) {
    if (auto S2 = Rz->getString("precision")) P.Precision = *S2 == "type" ? 0 : *S2 == "rta" ? 1 : 2;
    if (auto S2 = Rz->getString("ext_callee")) P.ExtCallee = S2->str();
  }
  if (auto *A = O->getObject("abi")) {
    if (!parsePatList(A->get("called_from_outside"), P.CalledFromOutside, Err)) return Ctx("abi.called_from_outside");
    if (auto *PM = A->getArray("pointer_models"))
      for (auto &E : *PM) {
        auto *EO = E.getAsObject();
        if (!EO || !EO->get("pattern")) continue;
        Pattern Pt;
        if (!parsePattern(*EO->get("pattern"), Pt, Err)) return Ctx("abi.pointer_models");
        fneffects::PtrModel M;
        StringRef K = EO->getString("model").value_or("escape");
        using PK = fneffects::PtrModel;
        M.K = K == "none" ? PK::None : K == "fresh" ? PK::Fresh : K == "realloc" ? PK::Realloc
            : (K == "ret_arg0" || K == "ret_arg1") ? PK::RetArg0
            : K == "copy" ? PK::Copy : K == "data" ? PK::Data : K == "ret_ext" ? PK::RetExt : K == "throw" ? PK::Throw
            : K == "catch" ? PK::Catch : PK::Escape;
        if (K == "ret_arg1") M.RetArg = 1;
        if (M.K == PK::Escape && K != "escape") { Err = "abi.pointer_models: unknown model " + K.str(); return false; }
        if (auto Nt = EO->getString("note")) M.Note = Nt->str();
        // an argument: its index, "varargs", or {arg, reach: true} (the objects reachable from it, two
        // levels: an iovec's buffers), encoded as -1000 - index
        std::function<int(const json::Value &)> argIdx = [&](const json::Value &V) -> int {
          if (auto N = V.getAsInteger()) return (int)*N;
          if (auto S = V.getAsString()) return *S == "varargs" ? -1 : *S == "varargs_reach" ? -2 : -100;
          if (auto *O = V.getAsObject()) {
            int I = O->get("arg") ? argIdx(*O->get("arg")) : -100;
            if (O->getBoolean("reach").value_or(false) && I >= 0) return -1000 - I;
            return I;
          }
          return -100;
        };
        if (auto *WA = EO->getArray("writes_bytes"))
          for (auto &Y : *WA) {
            if (auto *YO = Y.getAsObject()) {
              int I = argIdx(Y);
              M.WritesBytes.push_back({I, (uint64_t)YO->getInteger("size").value_or(0)});
            } else {
              M.WritesBytes.push_back({argIdx(Y), 0});
            }
            if (M.WritesBytes.back().first == -100) { Err = "abi.pointer_models: bad writes_bytes entry"; return false; }
          }
        if (auto *XA = EO->getArray("exposes")) for (auto &Y : *XA) M.Exposes.push_back(argIdx(Y));
        if (auto *XA = EO->getArray("sends")) for (auto &Y : *XA) M.Sends.push_back(argIdx(Y));
        if (auto *XA = EO->getArray("permutes")) for (auto &Y : *XA) M.Permutes.push_back(argIdx(Y));
        if (auto Bd = EO->getBoolean("fresh_unknown")) M.FreshUnknown = *Bd;
        if (auto *IB = EO->getObject("inline_buffer")) {
          M.InlineOffset = (uint64_t)IB->getInteger("offset").value_or(0);
          M.InlineSize = (uint64_t)IB->getInteger("size").value_or(0);
        }
        if (auto N = EO->getInteger("va_list")) M.VaList = (int)*N;
        if (auto N = EO->getInteger("format")) M.Format = (int)*N;
        if (auto N = EO->getInteger("fill_value")) M.FillValue = (int)*N;
        if (M.K != PK::Escape && M.Note.empty()) { Err = "abi.pointer_models: every model weaker than escape needs a note (its justification)"; return false; }
        if (auto Sz = EO->getInteger("size")) M.Size = (uint64_t)*Sz;
        if (auto Bd = EO->getBoolean("bodies")) M.Bodies = *Bd;
        if (auto Bd = EO->getBoolean("closure")) M.Closure = *Bd;
        if (auto Bd = EO->getBoolean("self")) M.Self = *Bd;
        if (auto Bd = EO->getBoolean("write_reached")) M.WriteReached = *Bd;
        if (auto *RA = EO->getArray("reads")) for (auto &Y : *RA) if (auto N = Y.getAsInteger()) M.Reads.push_back((int)*N);
        if (auto *WA = EO->getArray("writes")) for (auto &Y : *WA) if (auto N = Y.getAsInteger()) M.Writes.push_back((int)*N);
        if (auto *CA = EO->getArray("calls"))
          for (auto &X : *CA)
            if (auto *XO = X.getAsObject()) {
              std::vector<int> Args;
              if (auto *AA = XO->getArray("args"))
                for (auto &Y : *AA) if (auto N = Y.getAsInteger()) Args.push_back((int)*N);
              M.Calls.push_back({(int)XO->getInteger("fn").value_or(0), Args});
            }
        P.PointerModels.push_back({Pt, M});
      }
  }
  if (auto *T = O->getObject("temp"))
    if (auto D = T->getNumber("cold_branch_probability")) P.ColdProb = *D;
  if (auto *A = O->getArray("assumptions"))
    for (auto &E : *A) if (auto S = E.getAsString()) P.GlobalAssumptions.push_back(S->str());
  if (auto *K = O->getArray("entry_kinds")) {
    for (auto &E : *K) {
      auto *KO = E.getAsObject();
      if (!KO) continue;
      EntryKind EK;
      if (auto S = KO->getString("name")) EK.Name = S->str();
      if (auto S = KO->getString("start")) EK.Start = S->str();
      if (auto *A = KO->getArray("select"))
        for (auto &X : *A) if (auto S = X.getAsString()) EK.Select.push_back(S->str());
      if (auto *A = KO->getArray("forbid"))
        for (auto &X : *A) if (auto S = X.getAsString()) EK.Forbid.push_back(S->str());
      bool Replaced = false;
      for (auto &Old : P.Kinds)
        if (Old.Name == EK.Name) { Old = EK; Replaced = true; }
      if (!Replaced) P.Kinds.push_back(EK);
    }
  }
  return true;
}

void addEngineClasses(Policy &P) {
  const char *Eng[][2] = {
      {"recursion", "a cycle in the call graph"},
      {"indirect-unknown", "an indirect call whose targets could not be (fully) resolved"},
      {"external-unknown", "a call to a function with no body, no policy match and no library summary"}};
  for (auto &E : Eng) {
    int I = P.classIndex(E[0]);
    if (I < 0) {
      ClassDef D;
      D.Name = E[0];
      D.Description = E[1];
      D.Engine = true;
      P.Classes.push_back(D);
    } else {
      P.Classes[I].Engine = true;
    }
  }
}

// ------------------------------------------------------------------------------------------- summaries

struct SummarySym {
  std::string Library;
  std::map<std::string, std::vector<std::string>> Classes;
  std::map<std::string, std::string> Verdict;  // class -> none | reachable | unknown (summaries from bin/)
  int64_t UnresolvedIndirect = 0;
  bool HasVdso = false;                  // the binary analyser says the vDSO answers this function
  std::vector<std::string> VdsoFallback; // the system calls it makes when the vDSO cannot answer
};

struct Summaries {
  StringMap<SummarySym> Syms;
  std::vector<std::string> Libraries;
  bool load(const std::string &Path, std::string &Err) {
    auto Buf = MemoryBuffer::getFile(Path);
    if (!Buf) { Err = "cannot read summary " + Path; return false; }
    auto J = json::parse((*Buf)->getBuffer());
    if (!J) { Err = Path + ": " + toString(J.takeError()); return false; }
    auto *O = J->getAsObject();
    if (!O) { Err = Path + ": not an object"; return false; }
    std::string Lib = O->getString("library").value_or(sys::path::filename(Path)).str();
    Libraries.push_back(Lib);
    auto *S = O->getObject("symbols");
    if (!S) return true;
    for (auto &KV : *S) {
      if (Syms.count(KV.first)) continue;  // the first summary that has a symbol wins
      auto *SO = KV.second.getAsObject();
      if (!SO) continue;
      SummarySym SS;
      SS.Library = Lib;
      if (auto *C = SO->getObject("classes"))
        for (auto &CV : *C) {
          std::vector<std::string> L;
          if (auto *A = CV.second.getAsArray())
            for (auto &X : *A) if (auto XS = X.getAsString()) L.push_back(XS->str());
          SS.Classes[CV.first.str()] = L;
        }
      if (auto N = SO->getInteger("unresolved_indirect")) SS.UnresolvedIndirect = *N;
      if (auto *V = SO->getObject("verdict"))
        for (auto &VV : *V)
          if (auto VS = VV.second.getAsString()) SS.Verdict[VV.first.str()] = VS->str();
      if (auto *VD = SO->getObject("vdso")) {
        SS.HasVdso = true;
        if (auto *FA = VD->getArray("fallback_syscalls"))
          for (auto &X : *FA) if (auto XS = X.getAsString()) SS.VdsoFallback.push_back(XS->str());
      }
      Syms[KV.first] = SS;
    }
    return true;
  }
};

// ------------------------------------------------------------------------------------- call resolution

enum class Via { Direct, Virtual, Fnptr, Callback };
const char *viaStr(Via V) {
  switch (V) {
  case Via::Direct: return "direct";
  case Via::Virtual: return "virtual";
  case Via::Fnptr: return "fnptr";
  case Via::Callback: return "callback";
  }
  return "?";
}

struct Tgt {
  Function *F;
  Via V;
};

struct CallRes {
  std::vector<Tgt> Targets;
  bool Indirect = false;
  bool Unresolved = false;  // no target found
  bool Open = false;        // targets found, but more may exist outside the module
  bool Asm = false;
  std::string Rule;         // direct | vtable | const-table | fnptr-type | asm
  std::string Detail;       // type id, global name or function type
  std::string Visibility;   // for vtable: public | hidden | checked-load
  unsigned RTAPruned = 0;   // targets removed because no code of the module installs their vtable
  bool ClosedWorld = false; // resolved by the closed-hierarchy rule
  bool UsedPTA = false;     // the points-to analysis (and so the pointer models) decided the targets
  // Rule (a): the vtable rule is complete only for a hierarchy that cannot be extended outside the module.
  // That is established for an internal type and for a class with hidden LTO visibility (the build's own
  // statement); for every other class a verdict that uses the vtable rule is closed-world.
  bool VtClosed = false;    // resolved by the vtable rule on a hierarchy not established as closed
  bool VtResolved = false;  // the vtable rule gave a target set (not open, not unresolved)
  std::vector<Tgt> BaseVt;  // the vtable rule's targets, when points-to replaced them by a subset
  // Rule (c): the callee symbol can be something else at link or load time (a weak or exported alias).
  std::string LinkVar;
  bool closedWorld() const {
    return ClosedWorld || RTAPruned > 0 || (VtClosed && (StringRef(Rule).starts_with("vtable") || StringRef(Rule).starts_with("points-to+ext:vtable")));
  }
  const std::vector<Tgt> &vtTargets() const { return BaseVt.empty() ? Targets : BaseVt; }
};

struct VCallInfo {
  Metadata *TypeId;
  uint64_t Offset;
  std::string Kind;
};

Function *asFunction(Value *V) {
  V = V->stripPointerCasts();
  if (auto *GA = dyn_cast<GlobalAlias>(V)) V = GA->getAliaseeObject();
  return dyn_cast_or_null<Function>(V);
}

void collectFunctions(Constant *C, std::vector<Function *> &Out, DenseSet<Constant *> &Seen) {
  if (!Seen.insert(C).second) return;
  if (auto *F = dyn_cast<Function>(C)) { Out.push_back(F); return; }
  if (auto *GA = dyn_cast<GlobalAlias>(C)) {
    if (auto *F = dyn_cast_or_null<Function>(GA->getAliaseeObject())) Out.push_back(F);
    return;
  }
  if (isa<GlobalValue>(C)) return;
  for (auto &Op : C->operands())
    if (auto *OC = dyn_cast<Constant>(Op)) collectFunctions(OC, Out, Seen);
}

std::string typeIdStr(Metadata *MD) {
  if (auto *S = dyn_cast<MDString>(MD)) return S->getString().str();
  return "<internal type>";
}

class Resolver {
public:
  Module &M;
  const Policy &P;
  DenseMap<const CallBase *, VCallInfo> VCalls;
  DenseMap<Metadata *, std::vector<std::pair<GlobalVariable *, uint64_t>>> VTables;
  DenseMap<FunctionType *, std::vector<Function *>> AddrTaken;
  DenseMap<const CallBase *, CallRes> Res;
  unsigned NumAddrTaken = 0, NumVirtual = 0, NumFnptr = 0, NumTable = 0, NumUnresolved = 0, NumOpen = 0;
  std::vector<Function *> ExitHandlers;           // run by exit(): atexit/__cxa_atexit registrations, global dtors
  std::vector<const CallBase *> DynLoadSites;     // dlopen, dlsym, ...
  std::vector<const CallBase *> ExecMapSites;     // mmap/mprotect that may request PROT_EXEC
  bool HasDlsym = false;
  bool MissingVTables = false;  // a translation unit that did not compile defines a vtable
  unsigned RTADropped = 0;
  unsigned VTablesNoMeta = 0;   // vtables defined in the module without !type metadata (rule (a))
  std::vector<std::string> VTablesNoMetaNames;
  std::map<std::string, unsigned> RefineStats;

  // Points-to refinement of every indirect call (README, "Points-to analysis").
  void refine(fneffects::PointsTo &PT, bool Component = false) {
    for (auto &[CB, CR] : Res) {
      if (!CR.Indirect || CR.Asm) continue;
      auto SR = PT.site(*CB);
      if (Component && SR.Ext && !SR.Unk) {
        // component mode: EXT also stands for every other component of the module, so the pointer may be
        // any function the call's own rule allows; that rule keeps the call
        // A function pointer that may come from outside the component may also come from a library: the
        // module solve would report the call open. Here the type rule keeps it, as a closed-world claim
        // ("the pointer was made in the module"), never as a plain one. (The vtable rule has its own test.)
        if (!StringRef(CR.Rule).starts_with("vtable") && !StringRef(CR.Rule).starts_with("const-table")) CR.ClosedWorld = true;
        CR.Rule += "+fallback(outside the component)";
        RefineStats["fallback: the pointer may come from outside the component"]++;
        continue;
      }
      std::vector<Function *> Typed;
      for (Function *F : SR.Funcs)
        if (P.FnptrMatch != "exact" || F->getFunctionType() == CB->getFunctionType()) Typed.push_back(F);
      std::string Base = CR.Rule;
      if (SR.Unk) { CR.Rule = Base + "+fallback(unknown provenance)"; RefineStats["fallback: unknown provenance"]++; continue; }
      if (Typed.empty() && !SR.Ext) { CR.Rule = Base + "+fallback(no pointer reaches)"; RefineStats["fallback: no pointer reaches"]++; continue; }
      bool BaseResolved = !CR.Unresolved && (Base == "vtable" || Base == "const-table");
      DenseSet<Function *> BaseSet;
      for (auto &T : CR.Targets) BaseSet.insert(T.F);
      Via V = Base == "vtable" ? Via::Virtual : Via::Fnptr;
      std::vector<Tgt> NT;
      for (Function *F : Typed)
        if (!BaseResolved || BaseSet.count(F)) NT.push_back({F, V});
      if (BaseResolved && NT.empty() && !Typed.empty()) {
        CR.Rule = Base + "+fallback(points-to disagrees)";
        RefineStats["fallback: points-to disagrees with the vtable rule"]++;
        continue;
      }
      bool WasOpen = CR.Open;
      if (SR.Ext && Base == "vtable" && OptIncomplete > 0) {
        // A translation unit did not compile: the object may come from it, of any class whose vtable
        // the module has (RTA is off), or of one only that unit defines.
        DenseSet<Function *> Have;
        for (auto &T : CR.Targets) Have.insert(T.F);
        for (auto &T : NT) if (Have.insert(T.F).second) CR.Targets.push_back(T);
        CR.Rule = "vtable+ext(incomplete module)";
        if (MissingVTables) CR.Open = true;
        RefineStats["vtable rule, library object, incomplete module"]++;
        continue;
      }
      if (SR.Ext && Base == "vtable" && !WasOpen && !CR.Unresolved) {
        // The object may come through library memory, but its class hierarchy is closed (not a std
        // type, its vtable is in the module): its dynamic type is one whose vtable the module installs,
        // so the vtable rule (with RTA) bounds the targets. The call is resolved, not open.
        CR.Rule = "points-to+ext:vtable";
        CR.Detail = Base + ": " + CR.Detail;
        // rests on: no library constructs an object of a class derived outside the module (rule (a):
        // closed-world unless the hierarchy is established as closed)
        RefineStats["points-to, library object of a closed hierarchy (vtable rule)"]++;
        continue;
      }
      if (CR.VtResolved) CR.BaseVt = CR.Targets;
      CR.Targets = NT;
      CR.Detail = Base + ": " + CR.Detail;
      CR.Rule = SR.Ext ? "points-to+ext" : "points-to";
      CR.UsedPTA = true;
      CR.Unresolved = false;
      CR.Open = false;
      if (SR.Ext && (P.ExtCallee == "unknown" || (Base == "vtable" && WasOpen) || HasDlsym)) CR.Open = true;
      if (NT.empty() && !CR.Open) CR.Unresolved = true;
      RefineStats[SR.Ext ? (CR.Open ? "points-to, may be library code (open)" : "points-to, may be library code (assumed module)") : "points-to"]++;
    }
    NumUnresolved = NumOpen = 0;
    for (auto &[CB, CR] : Res) {
      if (!CR.Indirect) continue;
      if (CR.Unresolved) NumUnresolved++;
      else if (CR.Open) NumOpen++;
    }
  }

  Resolver(Module &M, const Policy &P) : M(M), P(P) {}

  void build() {
    // Virtual call sites from type tests and checked loads.
    std::map<Function *, std::unique_ptr<DominatorTree>> DTs;
    auto getDT = [&](Function *F) -> DominatorTree & {
      auto &D = DTs[F];
      if (!D) D = std::make_unique<DominatorTree>(*F);
      return *D;
    };
    for (Function &F : M) {
      if (!F.isIntrinsic()) continue;
      Intrinsic::ID ID = F.getIntrinsicID();
      bool TT = ID == Intrinsic::type_test || ID == Intrinsic::public_type_test;
      bool CL = ID == Intrinsic::type_checked_load || ID == Intrinsic::type_checked_load_relative;
      if (!TT && !CL) continue;
      for (User *U : F.users()) {
        auto *CI = dyn_cast<CallInst>(U);
        if (!CI || CI->getCalledFunction() != &F) continue;
        Function *Caller = CI->getFunction();
        SmallVector<DevirtCallSite, 4> DC;
        Metadata *TypeId = nullptr;
        if (TT) {
          SmallVector<CallInst *, 2> Assumes;
          findDevirtualizableCallsForTypeTest(DC, Assumes, CI, getDT(Caller));
          TypeId = cast<MetadataAsValue>(CI->getArgOperand(1))->getMetadata();
        } else {
          SmallVector<Instruction *, 2> Loaded, Preds;
          bool HasNonCall = false;
          findDevirtualizableCallsForTypeCheckedLoad(DC, Loaded, Preds, HasNonCall, CI, getDT(Caller));
          TypeId = cast<MetadataAsValue>(CI->getArgOperand(2))->getMetadata();
        }
        std::string Kind = ID == Intrinsic::public_type_test ? "public" : ID == Intrinsic::type_test ? "hidden" : "checked-load";
        for (auto &D : DC) VCalls[&D.CB] = {TypeId, D.Offset, Kind};
      }
    }
    for (GlobalVariable &GV : M.globals()) {
      SmallVector<MDNode *, 4> Types;
      GV.getMetadata(LLVMContext::MD_type, Types);
      for (MDNode *T : Types) {
        if (T->getNumOperands() < 2) continue;
        auto *Off = mdconst::dyn_extract<ConstantInt>(T->getOperand(0));
        if (!Off) continue;
        VTables[T->getOperand(1).get()].push_back({&GV, Off->getZExtValue()});
      }
    }
    // Rule (a): a vtable without !type metadata (bitcode not built with -flto -fwhole-program-vtables,
    // or produced by another front end) is invisible to the vtable rule.
    for (GlobalVariable &GV : M.globals()) {
      StringRef N = GV.getName();
      if (!(N.starts_with("_ZTV") || N.starts_with("_ZTC")) || GV.isDeclaration() || !GV.hasInitializer()) continue;
      if (N.starts_with("_ZTVN10__cxxabiv1")) continue;
      if (!GV.hasMetadata(LLVMContext::MD_type)) { VTablesNoMeta++; if (VTablesNoMetaNames.size() < 20) VTablesNoMetaNames.push_back(N.str()); }
    }
    for (Function &F : M) {
      if (F.isIntrinsic()) continue;
      if (F.hasAddressTaken(nullptr, /*IgnoreCallbackUses=*/false, /*IgnoreAssumeLikeCalls=*/true,
                            /*IgnoreLLVMUsed=*/true)) {
        AddrTaken[F.getFunctionType()].push_back(&F);
        NumAddrTaken++;
      }
    }
    // Exit handlers and dynamic loading, before resolution (exit() gets edges to the handlers).
    DenseSet<Function *> EH;
    if (GlobalVariable *GD = M.getNamedGlobal("llvm.global_dtors"))
      if (GD->hasInitializer()) {
        std::vector<Function *> Fs;
        DenseSet<Constant *> Seen;
        collectFunctions(GD->getInitializer(), Fs, Seen);
        for (Function *F : Fs) EH.insert(F);
      }
    for (Function &F : M)
      for (Instruction &I : instructions(F))
        if (auto *CB = dyn_cast<CallBase>(&I)) {
          Function *C = asFunction(CB->getCalledOperand());
          if (!C) continue;
          StringRef N = C->getName();
          if (P.ExitRegistrations.match(N))
            for (Value *A : CB->args()) if (Function *AF = asFunction(A)) EH.insert(AF);
          if (P.DynamicLoading.match(N)) DynLoadSites.push_back(CB);
          if (P.DlsymLike.match(N)) HasDlsym = true;
          for (auto &[EF, Arg] : P.ExecMapping)
            if (N == EF && (int)CB->arg_size() > Arg) {
              auto *CI = dyn_cast<ConstantInt>(CB->getArgOperand(Arg));
              if (!CI || (CI->getZExtValue() & 4 /*PROT_EXEC*/)) ExecMapSites.push_back(CB);
            }
        }
    ExitHandlers.assign(EH.begin(), EH.end());
    std::sort(ExitHandlers.begin(), ExitHandlers.end(), [](Function *A, Function *B) { return A->getName() < B->getName(); });
    for (Function &F : M)
      for (Instruction &I : instructions(F))
        if (auto *CB = dyn_cast<CallBase>(&I)) Res[CB] = resolve(*CB);
  }

  bool isOpenType(Metadata *TypeId) {
    auto *S = dyn_cast<MDString>(TypeId);
    if (!S) return false;  // internal (anonymous namespace) types cannot be extended elsewhere
    StringRef N = S->getString();
    if (P.OpenTypes.match(N)) return true;
    if (N.starts_with("_ZTS")) {
      // The class's own vtable is declared but not defined here: it lives in a library, and so may derived classes.
      if (GlobalVariable *V = M.getNamedGlobal(("_ZTV" + N.drop_front(4)).str()))
        if (V->isDeclaration()) return true;
    }
    return false;
  }

  CallRes resolve(CallBase &CB) {
    CallRes R;
    Value *Callee = CB.getCalledOperand();
    if (CB.isInlineAsm()) { R.Asm = true; R.Rule = "asm"; return R; }
    if (Function *F = asFunction(Callee)) {
      R.Rule = "direct";
      R.Targets.push_back({F, Via::Direct});
      // Rule (c): a call through an alias that the linker or the loader may bind to another definition
      if (auto *GA = dyn_cast<GlobalAlias>(Callee->stripPointerCasts())) {
        auto L = GA->getLinkage();
        bool PICnotPIE = M.getPICLevel() != PICLevel::NotPIC && M.getPIELevel() == PIELevel::Default;
        if (L == GlobalValue::WeakAnyLinkage || L == GlobalValue::LinkOnceAnyLinkage || L == GlobalValue::ExternalWeakLinkage ||
            L == GlobalValue::CommonLinkage || (PICnotPIE && !GA->isDSOLocal() && GA->hasDefaultVisibility() && !GA->hasLocalLinkage()))
          R.LinkVar = GA->getName().str();
      }
      if (F->isDeclaration() && !F->isIntrinsic() && !P.RegistrationOnly.match(F->getName())) {
        // An external function may call any function passed to it as an argument.
        for (Value *A : CB.args())
          if (Function *AF = asFunction(A))
            if (!AF->isIntrinsic()) R.Targets.push_back({AF, Via::Callback});
      }
      if (P.ExitFunctions.match(F->getName()))
        for (Function *H : ExitHandlers) R.Targets.push_back({H, Via::Callback});
      return R;
    }
    R.Indirect = true;
    auto VI = VCalls.find(&CB);
    if (VI != VCalls.end()) {
      R.Rule = "vtable";
      R.Detail = typeIdStr(VI->second.TypeId);
      R.Visibility = VI->second.Kind;
      DenseSet<Function *> Seen;
      auto It = VTables.find(VI->second.TypeId);
      if (It != VTables.end())
        for (auto &[GV, AddrOff] : It->second) {
          auto [Fn, C] = getFunctionAtVTableOffset(GV, AddrOff + VI->second.Offset, M);
          if (!Fn) continue;
          // Rapid type analysis: an object has this dynamic type only if some code or initializer stores
          // the vtable's address (a constructor, a VTT, a static object); a vtable nothing refers to was
          // never installed in an object of the module.
          // (off when a translation unit did not compile: it may install any vtable)
          if (P.Precision >= 1 && OptIncomplete == 0 && GV->use_empty()) { RTADropped++; R.RTAPruned++; continue; }
          if (Seen.insert(Fn).second) R.Targets.push_back({Fn, Via::Virtual});
        }
      NumVirtual++;
      if (R.Targets.empty()) { R.Unresolved = true; NumUnresolved++; }
      else if (isOpenType(VI->second.TypeId) || VTablesNoMeta > 0) { R.Open = true; NumOpen++; }  // rule (a)
      else {
        R.VtResolved = true;
        bool Established = !isa<MDString>(VI->second.TypeId) || VI->second.Kind != "public";
        R.VtClosed = !Established;
      }
      return R;
    }
    FunctionType *FT = CB.getFunctionType();
    // A pointer loaded from a constant table: the targets are the functions in the table's initializer.
    if (auto *LI = dyn_cast<LoadInst>(Callee->stripPointerCasts())) {
      const Value *Obj = getUnderlyingObject(LI->getPointerOperand(), 12);
      if (auto *GV = dyn_cast<GlobalVariable>(const_cast<Value *>(Obj)))
        if (GV->isConstant() && GV->hasDefinitiveInitializer() && !GV->isThreadLocal()) {
          std::vector<Function *> Fs;
          DenseSet<Constant *> Seen;
          collectFunctions(GV->getInitializer(), Fs, Seen);
          std::vector<Function *> Typed;
          for (Function *F : Fs) if (F->getFunctionType() == FT) Typed.push_back(F);
          if (Typed.empty()) Typed = Fs;  // mismatched types: keep every function in the table
          if (!Typed.empty()) {
            R.Rule = "const-table";
            R.Detail = GV->getName().str();
            DenseSet<Function *> U;
            for (Function *F : Typed) if (U.insert(F).second) R.Targets.push_back({F, Via::Fnptr});
            NumTable++;
            return R;
          }
        }
    }
    R.Rule = "fnptr-type";
    R.Detail = typeStr(FT);
    // Rule (c): an ifunc is bound by its resolver at load time, to whatever the resolver returns
    if (isa<GlobalIFunc>(Callee->stripPointerCasts())) { R.ClosedWorld = true; R.Detail = "ifunc " + Callee->stripPointerCasts()->getName().str() + ": " + R.Detail; }
    if (P.FnptrMatch == "exact") {
      auto It = AddrTaken.find(FT);
      if (It != AddrTaken.end())
        for (Function *F : It->second) R.Targets.push_back({F, Via::Fnptr});
    } else {
      R.Rule = "fnptr-" + P.FnptrMatch;
      for (auto &[T2, Fs] : AddrTaken) {
        bool Ok = P.FnptrMatch == "any" ||
                  (T2->getNumParams() == FT->getNumParams() && T2->isVarArg() == FT->isVarArg());
        if (Ok) for (Function *F : Fs) R.Targets.push_back({F, Via::Fnptr});
      }
    }
    NumFnptr++;
    if (R.Targets.empty()) { R.Unresolved = true; NumUnresolved++; }
    else if (HasDlsym) { R.Open = true; NumOpen++; }  // a pointer from dlsym can reach code we do not see
    return R;
  }
};

// ------------------------------------------------------------------------------------------ hot / cold

class ColdInfo {
public:
  const Policy &P;
  struct FC {
    DenseSet<const BasicBlock *> Cold;
    DenseSet<const BasicBlock *> FirstUse;  // subset of Cold: reached only through a first-use guard edge
    bool EntryCold = false;
  };

  // A first-use guard: a branch on the guard variable of a function-local static (_ZGV...) or of
  // thread-local initialisation (__tls_guard, a thread-local _ZGV...). Returns the successor taken
  // while the variable is not initialised (guard byte 0), or -1.
  static const GlobalVariable *guardVar(const Value *V) {
    V = V->stripPointerCasts();
    if (auto *II = dyn_cast<IntrinsicInst>(V))
      if (II->getIntrinsicID() == Intrinsic::threadlocal_address) V = II->getArgOperand(0)->stripPointerCasts();
    auto *GV = dyn_cast<GlobalVariable>(V);
    if (!GV) return nullptr;
    StringRef N = GV->getName();
    return (N.starts_with("_ZGV") || N.starts_with("__tls_guard")) ? GV : nullptr;
  }
  static int guardSucc(const Instruction *T) {
    auto *BI = dyn_cast<BranchInst>(T);
    if (!BI || !BI->isConditional()) return -1;
    // evaluate the condition with the guard byte 0
    const Value *C = BI->getCondition();
    bool Neg = false;
    while (true) {
      if (auto *X = dyn_cast<BinaryOperator>(C))
        if (X->getOpcode() == Instruction::Xor)
          if (auto *One = dyn_cast<ConstantInt>(X->getOperand(1)); One && One->isOne()) { Neg = !Neg; C = X->getOperand(0); continue; }
      if (auto *Tr = dyn_cast<TruncInst>(C)) { C = Tr->getOperand(0); continue; }
      break;
    }
    bool TrueWhenZero;
    if (auto *IC = dyn_cast<ICmpInst>(C)) {
      auto *Z = dyn_cast<ConstantInt>(IC->getOperand(1));
      if (!Z || !Z->isZero()) return -1;
      if (IC->getPredicate() == ICmpInst::ICMP_EQ) TrueWhenZero = true;
      else if (IC->getPredicate() == ICmpInst::ICMP_NE) TrueWhenZero = false;
      else return -1;
      C = IC->getOperand(0);
      if (auto *A = dyn_cast<BinaryOperator>(C))  // guard & 1
        if (A->getOpcode() == Instruction::And) C = A->getOperand(0);
    } else {
      TrueWhenZero = false;  // br i1 (load guard): true when initialised
    }
    auto *L = dyn_cast<LoadInst>(C);
    if (!L || !guardVar(L->getPointerOperand())) return -1;
    if (Neg) TrueWhenZero = !TrueWhenZero;
    return TrueWhenZero ? 0 : 1;
  }
  std::map<const Function *, FC> Memo;
  std::set<const Function *> InProgress;

  explicit ColdInfo(const Policy &P) : P(P) {}

  bool edgeUnlikely(const Instruction *T, unsigned SuccIdx) {
    SmallVector<uint32_t, 4> W;
    if (extractBranchWeights(*T, W) && W.size() == T->getNumSuccessors()) {
      uint64_t Sum = 0;
      for (auto X : W) Sum += X;
      if (Sum == 0) return false;
      return (double)W[SuccIdx] / (double)Sum < P.ColdProb;
    }
    // Unlowered llvm.expect (e.g. at -O0): br (icmp ne/eq (llvm.expect(x, c)), 0) or br (llvm.expect.i1).
    if (auto *BI = dyn_cast<BranchInst>(T)) {
      if (!BI->isConditional()) return false;
      Value *C = BI->getCondition();
      bool Negate = false;
      Value *E = C;
      if (auto *IC = dyn_cast<ICmpInst>(C)) {
        auto *Z = dyn_cast<ConstantInt>(IC->getOperand(1));
        if (!Z || !Z->isZero()) return false;
        if (IC->getPredicate() == ICmpInst::ICMP_EQ) Negate = true;
        else if (IC->getPredicate() != ICmpInst::ICMP_NE) return false;
        E = IC->getOperand(0);
      }
      auto *II = dyn_cast<IntrinsicInst>(E);
      if (!II || II->getIntrinsicID() != Intrinsic::expect) return false;
      auto *Exp = dyn_cast<ConstantInt>(II->getArgOperand(1));
      if (!Exp) return false;
      bool ExpectTrue = !Exp->isZero();
      if (Negate) ExpectTrue = !ExpectTrue;
      // successor 0 is taken when the condition is true
      return ExpectTrue ? SuccIdx == 1 : SuccIdx == 0;
    }
    return false;
  }

  bool callIsColdSeed(const CallBase *CB) {
    if (CB->hasFnAttr(Attribute::Cold)) return true;
    Function *Fn = asFunction(CB->getCalledOperand());
    if (Fn && P.ColdTargets.match(Fn->getName())) return true;
    if (!CB->doesNotReturn()) return false;
    if (!Fn || Fn->isDeclaration()) return true;  // external or indirect noreturn: abort, throw, exit
    // A defined noreturn function is cold only if its own entry is cold (it throws or aborts), not if it
    // is an endless loop such as an event loop.
    return get(*Fn).EntryCold;
  }

  const FC &get(const Function &F) {
    auto It = Memo.find(&F);
    if (It != Memo.end()) return It->second;
    FC R;
    if (F.isDeclaration()) { return Memo[&F] = R; }
    if (!InProgress.insert(&F).second) { return R.EntryCold = false, Memo[&F]; }
    if (F.hasFnAttribute(Attribute::Cold)) {
      for (auto &B : F) R.Cold.insert(&B);
      R.EntryCold = true;
      InProgress.erase(&F);
      return Memo[&F] = R;
    }
    DenseSet<const BasicBlock *> Seed;
    for (auto &B : F) {
      const Instruction *T = B.getTerminator();
      bool C = B.isEHPad() || isa<UnreachableInst>(T) || isa<ResumeInst>(T);
      if (!C)
        for (auto &I : B)
          if (auto *CB = dyn_cast<CallBase>(&I))
            if (callIsColdSeed(CB)) { C = true; break; }
      if (C) Seed.insert(&B);
    }
    // Backward closure: a block all of whose successors are cold is cold.
    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (auto &B : F) {
        if (Seed.count(&B)) continue;
        if (succ_empty(&B)) continue;
        bool All = true;
        for (auto *S : successors(&B)) if (!Seed.count(S)) { All = false; break; }
        if (All) { Seed.insert(&B); Changed = true; }
      }
    }
    // Forward: hot blocks are those reachable from the entry without entering a cold block or taking an
    // unlikely edge. Everything else is cold.
    DenseSet<const BasicBlock *> Hot;
    std::deque<const BasicBlock *> Q;
    const BasicBlock *E = &F.getEntryBlock();
    if (!Seed.count(E)) { Hot.insert(E); Q.push_back(E); }
    while (!Q.empty()) {
      const BasicBlock *B = Q.front();
      Q.pop_front();
      const Instruction *T = B->getTerminator();
      for (unsigned i = 0, n = T->getNumSuccessors(); i < n; i++) {
        const BasicBlock *S = T->getSuccessor(i);
        if (Seed.count(S) || Hot.count(S)) continue;
        if (edgeUnlikely(T, i)) continue;
        Hot.insert(S);
        Q.push_back(S);
      }
    }
    for (auto &B : F) if (!Hot.count(&B)) R.Cold.insert(&B);
    R.EntryCold = !Hot.count(E);
    // First use: blocks reached from a likely block through a guard's not-initialised edge (and then
    // likely edges), that are not likely themselves.
    std::deque<const BasicBlock *> Q2;
    for (const BasicBlock *B : Hot) {
      int G = guardSucc(B->getTerminator());
      if (G < 0) continue;
      const BasicBlock *S = B->getTerminator()->getSuccessor(G);
      if (!Hot.count(S) && !Seed.count(S) && R.FirstUse.insert(S).second) Q2.push_back(S);
    }
    while (!Q2.empty()) {
      const BasicBlock *B = Q2.front();
      Q2.pop_front();
      const Instruction *T = B->getTerminator();
      for (unsigned i = 0, n = T->getNumSuccessors(); i < n; i++) {
        const BasicBlock *S = T->getSuccessor(i);
        if (Seed.count(S) || Hot.count(S) || R.FirstUse.count(S)) continue;
        if (edgeUnlikely(T, i) && (int)i != guardSucc(T)) continue;
        R.FirstUse.insert(S);
        Q2.push_back(S);
      }
    }
    InProgress.erase(&F);
    return Memo[&F] = R;
  }

  bool isCold(const Instruction *I) { return get(*I->getFunction()).Cold.count(I->getParent()); }
};

// ---------------------------------------------------------------------------------------------- sites

// The tag of a site (README, "Tags"): it describes the compiler's view of the path, never a verdict.
const char *tempName(char T) {
  return T == 'h' ? "likely" : T == 'c' ? "unlikely" : T == 'f' ? "first-use" : "unknown";
}

json::Object siteJSON(const Instruction *I, const Function *Fallback = nullptr) {
  json::Object S;
  const Function *F = I ? I->getFunction() : Fallback;
  S["func"] = F ? F->getName().str() : "";
  const DILocation *L = I ? I->getDebugLoc().get() : nullptr;
  if (L) {
    S["file"] = fullPath(L->getScope(), L->getFilename());
    S["line"] = (int64_t)L->getLine();
    S["col"] = (int64_t)L->getColumn();
    if (auto *SP = L->getScope()->getSubprogram()) {
      StringRef LN = SP->getLinkageName();
      std::string Scope = LN.empty() ? SP->getName().str() : LN.str();
      if (Scope != S["func"].getAsString()->str()) S["scope"] = Scope;
    }
    json::Array IA;
    for (const DILocation *In = L->getInlinedAt(); In; In = In->getInlinedAt()) {
      json::Object X;
      X["file"] = fullPath(In->getScope(), In->getFilename());
      X["line"] = (int64_t)In->getLine();
      X["col"] = (int64_t)In->getColumn();
      if (auto *SP = In->getScope()->getSubprogram()) {
        StringRef LN = SP->getLinkageName();
        X["scope"] = LN.empty() ? SP->getName().str() : LN.str();
      }
      IA.push_back(std::move(X));
    }
    if (!IA.empty()) S["inlined_at"] = std::move(IA);
  } else if (F && F->getSubprogram()) {
    auto *SP = F->getSubprogram();
    S["file"] = fullPath(SP, SP->getFilename());
    S["line"] = (int64_t)SP->getLine();
    S["approx"] = true;
  } else {
    S["file"] = "";
    S["line"] = 0;
  }
  return S;
}

// One local site of a class in a function. Temp (a tag; it never changes a verdict): 'h' likely (no
// branch hint against it), 'c' unlikely (the compiler's branch hints or a cold call say so), 'f'
// first-use (the initialisation of a function-local static or thread-local), 'u' unknown (inside a
// library whose internal paths we do not see).
struct LSite {
  const Instruction *I = nullptr;
  int Cls = -1;
  std::string Sub, Sym, Via, Lib, Rule;
  std::vector<std::string> Inner;
  char Temp = 'h';
  int64_t Targets = -1;
};

struct LEdge {
  Function *T;
  const CallBase *CB;
  Via V;
  bool Hot;
  bool FirstUse = false;  // from a block that runs on the first use of a static or thread-local only
};

struct Assump {
  json::Object O;
  const Function *F;
};

struct FInfo {
  std::vector<LSite> Sites;
  std::vector<LEdge> Edges;
  std::vector<Assump> Assumptions;
  json::Object Lint;
  bool HasLint = false;
  bool HasTerminatePad = false;
  DenseSet<const CallBase *> TerminateInvokes;
  std::set<std::string> Relied;  // external symbols whose policy entry or summary a verdict rests on
  std::set<std::string> LinkVars; // aliases called here that the linker or loader may bind elsewhere (rule (c))
  std::vector<int> IndirectIds;  // indirect call sites in this function (index into Analysis::Indirect)
};

// -------------------------------------------------------------------------------------------- analysis

class Analysis {
public:
  Module &M;
  Policy &P;
  Summaries &S;
  Resolver R;
  ColdInfo CI;
  std::map<const Function *, FInfo> Info;
  std::vector<Function *> Defined;
  DenseMap<const Function *, unsigned> SCCId;
  DenseMap<unsigned, unsigned> SCCSize;
  int IRec, IInd, IExt;
  std::vector<int> UnknownOf;  // class -> its hidden unknown class, or -1

  const char *DiagDrop = getenv("FX_DIAG_DROP");
  StringMap<std::string> Missing;  // symbol -> translation unit that could not be compiled to bitcode
  std::unique_ptr<fneffects::PointsTo> PT;
  bool PtaComponent = false, PtaModuleOverBudget = false;
  unsigned PtaComponents = 0;
  bool ModelCheckDone = false;
  int64_t ModelFree = 0;  // points-to resolutions that the solution without pointer models also gives
  std::vector<std::pair<std::string, int64_t>> Timings;
  struct IndRec { std::string Rule; int64_t Targets; json::Object O; bool ClosedWorld = false, UsedPTA = false; };
  std::vector<IndRec> Indirect;  // every indirect call site of the module, listed once in the report
  Analysis(Module &M, Policy &P, Summaries &S) : M(M), P(P), S(S), R(M, P), CI(P) {}

  bool callHasAttr(const CallBase *CB, StringRef A) {
    Attribute::AttrKind K = Attribute::getAttrKindFromName(A);
    if (K != Attribute::None) return CB->hasFnAttr(K);
    return CB->hasFnAttr(A);
  }
  bool pruned(const CallBase *CB) {
    for (auto &A : P.PruneCallAttrs) if (callHasAttr(CB, A)) return true;
    return false;
  }

  // Classes matched by name (used for every callee, with or without body).
  void matchSymbol(Function *F, std::vector<std::pair<int, std::string>> &Out, const Pattern **Hit) {
    StringRef N = F->getName();
    for (size_t i = 0; i < P.Classes.size(); i++) {
      auto &C = P.Classes[i];
      const Pattern *Pt = F->isIntrinsic() ? C.Intrinsics.match(N) : C.Symbols.match(N);
      if (Pt && !C.Exclude.match(N)) { Out.push_back({(int)i, Pt->Sub}); if (Hit) *Hit = Pt; }
    }
  }

  // Classify a call to symbol N at instruction I. N is a real callee (defined, declared or intrinsic) or
  // a call the compiler will emit later (an intrinsic's library call, a stack-protector check, ...).
  void externalCall(FInfo &FI, const Instruction *I, StringRef N, const std::string &ViaS, char Temp, bool IsDecl,
                    bool IsIntrinsic, const CallBase *CB, const Function *T, int Depth = 0) {
    auto Emit = [&](int C, const std::string &Sub, char Tp, const std::string &VS, const std::string &Lib,
                    std::vector<std::string> Inner) {
      LSite L;
      L.I = I;
      L.Cls = C;
      L.Sub = Sub;
      L.Sym = N.str();
      L.Via = VS;
      L.Lib = Lib;
      L.Inner = std::move(Inner);
      L.Temp = Tp;
      FI.Sites.push_back(std::move(L));
    };
    bool Matched = false;
    const NameInfo &NI = nameInfo(N, IsIntrinsic);
    // Rule (b): a class or primitive named in the policy is a library function. A function with that
    // name defined in the module (the program's own send, a replaced or placement operator new, an
    // allocator built from source) is an ordinary function: its body decides. `opaque` is the user's
    // statement that a defined function is to be taken by its name.
    bool ByName = IsDecl || IsIntrinsic || (T && isOpaque(T));
    if (ByName)
      for (auto &[Ci, Sub] : NI.Classes) { Emit(Ci, Sub, Temp, ViaS, "", {}); Matched = true; }
    if (IsIntrinsic) {
      // An intrinsic may become a library call in code generation; classify those calls too.
      bool Lowers = false;
      for (size_t li = 0; li < P.IntrinsicLibcalls.size(); li++) {
        auto &[Pt, Calls] = P.IntrinsicLibcalls[li];
        if (!Pt.matches(N)) continue;
        Lowers = true;
        // a short copy of constant length is expanded into moves by the code generator, never a call
        if (int64_t Max = P.IntrinsicInlineMax[li]; Max >= 0 && CB && CB->arg_size() > 2)
          if (auto *Len = dyn_cast<ConstantInt>(CB->getArgOperand(2)))
            if (Len->getValue().ule((uint64_t)Max)) continue;
        SmallVector<StringRef, 3> Groups;
        if (Pt.Kind == Pattern::Re) Pt.R->match(N, &Groups);
        for (auto &C : Calls) {
          std::string Name = C;
          size_t D = Name.find("$1");
          if (D != std::string::npos) Name.replace(D, 2, Groups.size() > 1 ? Groups[1].str() : std::string());
          if (Depth < 2) externalCall(FI, I, Name, "lowered:" + N.str(), Temp, true, false, nullptr, nullptr, Depth + 1);
        }
      }
      if (!Lowers && !Matched && !P.IntrinsicsNoCall.match(N)) Emit(IExt, "intrinsic", Temp, ViaS, "", {});
      return;
    }
    if (!IsDecl) return;  // defined (or opaque) function: its body or the policy decides
    for (int Ci : NI.ExceptHits) Emit(Ci, "not-listed", Temp, ViaS, "", {});
    if (NI.AnyExcept) Matched = true;
    bool Known = Matched || NI.Known;
    for (auto &A : P.KnownIfCallAttrs) {
      Attribute::AttrKind K = Attribute::getAttrKindFromName(A);
      bool Has = false;
      if (K != Attribute::None) Has = (CB && CB->hasFnAttr(K)) || (T && T->hasFnAttribute(K));
      else Has = T && T->hasFnAttribute(A);
      if (Has) Known = true;
    }
    // A summary adds what the library really does, also to symbols the policy already classifies
    // (malloc also locks and may call mmap).
    auto SI = S.Syms.find(N);
    if (SI != S.Syms.end()) {
      auto &SS = SI->second;
      std::string VS = "summary:" + SS.Library;
      char Tp = Temp == 'c' ? 'c' : 'u';
      for (auto &[SC, Inner] : SS.Classes) {
        // reached inside the library only on the way to process termination: cold
        // or only through functions that stop or unwind (abort, failed checks, throws): cold, as such a
        // path is in the module's own code
        bool OnlyTerminate = !Inner.empty();
        for (auto &X : Inner) {
          if (X == "[terminate]" || X == "[unwinding]" || X == "ud2" || X == "int3" || X == "hlt") continue;  // traps
          bool Terminal = false;
          for (auto &[Ci, Sub] : nameInfo(X, false).Classes)
            if (P.Classes[Ci].Name == "abort" || P.Classes[Ci].Name == "throw") Terminal = true;
          if (!Terminal) { OnlyTerminate = false; break; }
        }
        for (size_t i = 0; i < P.Classes.size(); i++)
          for (auto &PC : P.Classes[i].SummaryClasses)
            if (PC == SC) Emit((int)i, OnlyTerminate ? "on the way to termination" : "", OnlyTerminate ? 'c' : Tp, VS, SS.Library, Inner);
      }
      if (!SS.Verdict.empty()) {
        // per-class unknown: blocks that class only
        for (auto &[SC, V] : SS.Verdict) {
          if (V != "unknown") continue;
          for (size_t i = 0; i < P.Classes.size(); i++)
            for (auto &PC : P.Classes[i].SummaryClasses)
              if (PC == SC && UnknownOf[i] >= 0) Emit(UnknownOf[i], "summary: " + SC + " unknown in " + SS.Library, Tp, VS, SS.Library, {});
        }
      } else if (SS.UnresolvedIndirect > 0) {
        Emit(IExt, "summary-unresolved-indirect", Tp, VS, SS.Library, {});
      }
      // vDSO functions: the binary analyser does not count the fallback system call; the summary names
      // it (vdso.fallback_syscalls), and it is made whenever the vDSO cannot answer (CPU-time clocks,
      // a clock source without vDSO mode). Without that field nothing is claimed: syscall unknown.
      bool InVdsoClass = false;
      for (auto &[Ci, Sub] : NI.Classes) if (P.Classes[Ci].Name == "vdso") InVdsoClass = true;
      if (SS.HasVdso || InVdsoClass)
        for (size_t i = 0; i < P.Classes.size(); i++) {
          bool Sys = false;
          for (auto &PC : P.Classes[i].SummaryClasses) if (PC == "syscall") Sys = true;
          if (!Sys) continue;
          if (!SS.VdsoFallback.empty()) {
            std::string L;
            for (auto &X : SS.VdsoFallback) L += (L.empty() ? "" : ", ") + X;
            Emit((int)i, "vdso fallback: " + L, Temp == 'c' ? 'c' : 'u', VS, SS.Library, SS.VdsoFallback);
          } else if (UnknownOf[i] >= 0) {
            Emit(UnknownOf[i], "vdso: the summary does not name the fallback system call", Temp == 'c' ? 'c' : 'u', VS, SS.Library, {});
          }
        }
      json::Object A;
      A["kind"] = "summary";
      A["library"] = SS.Library;
      A["symbol"] = N.str();
      A["unresolved_indirect"] = SS.UnresolvedIndirect;
      FI.Assumptions.push_back({std::move(A), I->getFunction()});
      FI.Relied.insert("summary:" + N.str());
      return;
    }
    if (Known) {
      FI.Relied.insert((NI.Known ? "known:" : "policy:") + N.str());
      // a vDSO function without a summary: whether it enters the kernel is not known
      for (auto &[Ci, Sub] : NI.Classes)
        if (P.Classes[Ci].Name == "vdso")
          for (size_t i = 0; i < P.Classes.size(); i++)
            for (auto &PC : P.Classes[i].SummaryClasses)
              if (PC == "syscall" && UnknownOf[i] >= 0) Emit(UnknownOf[i], "vdso: no summary names the fallback system call", Temp, ViaS, "", {});
      return;
    }
    auto MI = Missing.find(N);
    if (MI != Missing.end()) { Emit(IExt, "tu-not-compiled:" + MI->second, Temp, ViaS, "", {}); return; }
    Emit(IExt, "", Temp, ViaS, "", {});
  }

  // Name-only part of the classification, cached: the policy's patterns are matched once per symbol.
  struct NameInfo {
    std::vector<std::pair<int, std::string>> Classes;
    std::vector<int> ExceptHits;
    bool AnyExcept = false, Known = false, ColdTarget = false;
  };
  StringMap<NameInfo> NameCache[2];
  const NameInfo &nameInfo(StringRef N, bool IsIntrinsic) {
    auto &C = NameCache[IsIntrinsic ? 1 : 0];
    auto It = C.find(N);
    if (It != C.end()) return It->second;
    NameInfo NI;
    for (size_t i = 0; i < P.Classes.size(); i++) {
      auto &Cl = P.Classes[i];
      const Pattern *Pt = IsIntrinsic ? Cl.Intrinsics.match(N) : Cl.Symbols.match(N);
      if (Pt && !Cl.Exclude.match(N)) NI.Classes.push_back({(int)i, Pt->Sub});
      if (Cl.HasExcept && !IsIntrinsic) {
        NI.AnyExcept = true;
        if (!Cl.ExceptList.match(N)) NI.ExceptHits.push_back((int)i);
      }
    }
    NI.Known = P.Known.match(N) != nullptr;
    NI.ColdTarget = P.ColdTargets.match(N) != nullptr;
    return C[N] = NI;
  }

  void addCallSites(FInfo &FI, const CallBase *CB, Function *T, Via V, bool BlockCold) {
    bool ColdTarget = nameInfo(T->getName(), T->isIntrinsic()).ColdTarget;
    char Temp = (BlockCold || ColdTarget) ? 'c' : 'h';
    externalCall(FI, CB, T->getName(), viaStr(V), Temp, T->isDeclaration(), T->isIntrinsic(), CB, T);
  }

  void instSite(FInfo &FI, const Instruction *I, const std::string &Kind, const std::string &Sym, bool Cold) {
    for (size_t i = 0; i < P.Classes.size(); i++)
      for (auto &K : P.Classes[i].Insts)
        if (K == Kind) {
          LSite L;
          L.I = I;
          L.Cls = (int)i;
          L.Sub = Kind;
          L.Sym = Sym;
          L.Via = "instruction";
          L.Temp = Cold ? 'c' : 'h';
          FI.Sites.push_back(std::move(L));
        }
  }

  std::string tlsModel(const GlobalVariable *GV, bool &Dynamic) {
    // Mirrors TargetMachine::getTLSModel: the model follows from the relocation model and dso_local, and
    // an explicit model in the IR is used when it is more specific.
    bool PIC = M.getPICLevel() != PICLevel::NotPIC;
    bool PIE = M.getPIELevel() != PIELevel::Default;
    bool Local = GV->isDSOLocal();
    int Computed;  // 0 GD, 1 LD, 2 IE, 3 LE
    if (PIC && !PIE) Computed = Local ? 1 : 0;
    else Computed = Local ? 3 : 2;
    int IR = 0;
    switch (GV->getThreadLocalMode()) {
    case GlobalValue::LocalDynamicTLSModel: IR = 1; break;
    case GlobalValue::InitialExecTLSModel: IR = 2; break;
    case GlobalValue::LocalExecTLSModel: IR = 3; break;
    default: IR = 0;
    }
    int Eff = std::max(Computed, IR);
    Dynamic = Eff <= 1;
    const char *Names[] = {"general-dynamic", "local-dynamic", "initial-exec", "local-exec"};
    return Names[Eff];
  }

  DenseMap<const Function *, bool> OpaqueCache;
  bool isOpaque(const Function *F) {
    auto It = OpaqueCache.find(F);
    if (It != OpaqueCache.end()) return It->second;
    return OpaqueCache[F] = P.Opaque.match(F->getName()) != nullptr;
  }
  DenseMap<const BasicBlock *, bool> TermPadCache;
  bool unwindReachesTerminate(const BasicBlock *Pad) {
    auto It = TermPadCache.find(Pad);
    if (It != TermPadCache.end()) return It->second;
    return TermPadCache[Pad] = unwindReachesTerminateUncached(Pad);
  }
  bool unwindReachesTerminateUncached(const BasicBlock *Pad) {
    std::deque<const BasicBlock *> Q{Pad};
    DenseSet<const BasicBlock *> Seen{Pad};
    while (!Q.empty()) {
      auto *B = Q.front();
      Q.pop_front();
      for (auto &I : *B)
        if (auto *CB = dyn_cast<CallBase>(&I))
          if (Function *F = asFunction(CB->getCalledOperand()))
            if (P.Terminate.match(F->getName())) return true;
      for (auto *Sx : successors(B))
        if (Seen.insert(Sx).second) Q.push_back(Sx);
    }
    return false;
  }

  json::Object siteRef(const Instruction *I) { return siteJSON(I); }

  StringMap<int> LibmCache;
  void lintInst(FInfo &FI, const Instruction &I, std::map<std::string, int64_t> &FMF, int64_t &FMulAdd,
                int64_t &FMA, std::set<std::string> &Libm, std::set<std::string> &LibmExact) {
    if (auto *FPO = dyn_cast<FPMathOperator>(&I)) {
      FastMathFlags F = FPO->getFastMathFlags();
      if (F.allowReassoc()) FMF["reassoc"]++;
      if (F.noNaNs()) FMF["nnan"]++;
      if (F.noInfs()) FMF["ninf"]++;
      if (F.noSignedZeros()) FMF["nsz"]++;
      if (F.allowReciprocal()) FMF["arcp"]++;
      if (F.allowContract()) FMF["contract"]++;
      if (F.approxFunc()) FMF["afn"]++;
    }
    auto *CB = dyn_cast<CallBase>(&I);
    if (!CB) return;
    Function *Fn = asFunction(CB->getCalledOperand());
    if (!Fn) return;
    if (Fn->isIntrinsic()) {
      Intrinsic::ID ID = Fn->getIntrinsicID();
      if (ID == Intrinsic::fmuladd) FMulAdd++;
      else if (ID == Intrinsic::fma) FMA++;
    }
    StringRef N = Fn->getName();
    auto It = LibmCache.find(N);
    if (It == LibmCache.end()) It = LibmCache.insert({N, P.LibmExact.match(N) ? 2 : P.Libm.match(N) ? 1 : 0}).first;
    if (It->second == 2) LibmExact.insert(N.str());
    else if (It->second == 1) Libm.insert(N.str());
  }

  void analyseFunction(Function &F) {
    FInfo &FI = Info[&F];
    std::map<std::string, int64_t> FMF;
    int64_t FMulAdd = 0, FMA = 0;
    std::set<std::string> Libm, LibmExact;
    for (BasicBlock &B : F) {
      bool Cold = CI.get(F).Cold.count(&B);
      const Instruction *Prev = nullptr;
      for (Instruction &I : B) {
        if (P.Lint) lintInst(FI, I, FMF, FMulAdd, FMA, Libm, LibmExact);
        if (auto *A = dyn_cast<AtomicRMWInst>(&I)) {
          instSite(FI, &I, "atomicrmw", ("atomicrmw " + AtomicRMWInst::getOperationName(A->getOperation()) + " " +
                                        toIRString(A->getOrdering())).str(), Cold);
        } else if (auto *X = dyn_cast<AtomicCmpXchgInst>(&I)) {
          instSite(FI, &I, "cmpxchg", std::string("cmpxchg ") + toIRString(X->getSuccessOrdering()), Cold);
        } else if (auto *Fe = dyn_cast<FenceInst>(&I)) {
          instSite(FI, &I, "fence", std::string("fence ") + toIRString(Fe->getOrdering()), Cold);
        } else if (auto *L = dyn_cast<LoadInst>(&I)) {
          if (L->isAtomic()) instSite(FI, &I, "load-atomic", std::string("load atomic ") + toIRString(L->getOrdering()), Cold);
        } else if (auto *St = dyn_cast<StoreInst>(&I)) {
          if (St->isAtomic()) instSite(FI, &I, "store-atomic", std::string("store atomic ") + toIRString(St->getOrdering()), Cold);
        } else if (isa<LandingPadInst>(&I)) {
          instSite(FI, &I, "landingpad", "landingpad", Cold);
        } else if (isa<ResumeInst>(&I)) {
          instSite(FI, &I, "resume", "resume", Cold);
        } else if (isa<UnreachableInst>(&I)) {
          bool AfterNoReturn = false;
          if (auto *PC = dyn_cast_or_null<CallBase>(Prev)) AfterNoReturn = PC->doesNotReturn();
          bool FromInvoke = false;
          for (auto *Pred : predecessors(&B))
            if (auto *II = dyn_cast<InvokeInst>(Pred->getTerminator()))
              if (II->getNormalDest() == &B && II->doesNotReturn()) FromInvoke = true;
          if (!AfterNoReturn && !FromInvoke) instSite(FI, &I, "unreachable-bare", "unreachable", Cold);
        } else if (auto *AI = dyn_cast<AllocaInst>(&I)) {
          if (!isa<ConstantInt>(AI->getArraySize()))
            instSite(FI, &I, "alloca-dynamic", "alloca " + typeStr(AI->getAllocatedType()) + " x <variable>", Cold);
        }
        // Thread-local storage: through llvm.threadlocal.address or a direct operand.
        if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
          if (II->getIntrinsicID() == Intrinsic::threadlocal_address)
            if (auto *GV = dyn_cast<GlobalVariable>(II->getArgOperand(0)->stripPointerCasts())) tlsAccess(FI, &I, GV, Cold);
        } else {
          for (auto &Op : I.operands())
            if (auto *GV = dyn_cast<GlobalVariable>(Op->stripPointerCasts()))
              if (GV->isThreadLocal()) tlsAccess(FI, &I, GV, Cold);
        }
        if (auto *CB = dyn_cast<CallBase>(&I)) {
          auto C0 = std::chrono::steady_clock::now();
          analyseCall(F, FI, CB, Cold);
          auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - C0).count();
          if (us > 20000 && getenv("FX_PROFILE")) {
            auto &CR = R.Res[CB];
            errs() << "  call " << us << " us rule " << CR.Rule << " targets " << CR.Targets.size() << " dbg " << (CB->getDebugLoc() ? 1 : 0) << "\n";
          }
        }
        loweredCalls(F, FI, I, Cold);
        Prev = &I;
      }
    }
    attributeCalls(F, FI);
    // first-use tags (a tag only): sites and calls in blocks that run on a static's first use
    const auto &FU = CI.get(F).FirstUse;
    if (!FU.empty()) {
      for (auto &S : FI.Sites)
        if (S.I && S.Temp == 'c' && FU.count(S.I->getParent())) S.Temp = 'f';
      for (auto &E : FI.Edges)
        if (!E.Hot && FU.count(E.CB->getParent())) E.FirstUse = true;
    }
    if (P.Lint && (!FMF.empty() || FMulAdd || FMA || !Libm.empty() || !LibmExact.empty() || fpAttrs(F).size())) {
      json::Object L;
      json::Object FM;
      for (auto &[K, V] : FMF) FM[K] = V;
      if (!FMF.empty()) L["fast_math"] = std::move(FM);
      if (FMulAdd) L["fmuladd"] = FMulAdd;
      if (FMA) L["fma"] = FMA;
      json::Array LA, LE;
      for (auto &N : Libm) LA.push_back(N);
      for (auto &N : LibmExact) LE.push_back(N);
      if (!LA.empty()) L["libm"] = std::move(LA);
      if (!LE.empty()) L["libm_correctly_rounded"] = std::move(LE);
      auto Attrs = fpAttrs(F);
      if (!Attrs.empty()) {
        json::Object AO;
        for (auto &[K, V] : Attrs) AO[K] = V;
        L["fp_attrs"] = std::move(AO);
      }
      if (auto *SP = F.getSubprogram()) L["tu"] = mapPath(SP->getUnit()->getFilename().str());
      FI.Lint = std::move(L);
      FI.HasLint = true;
    }
  }

  // Calls that code generation inserts for an instruction: frem -> fmod, atomics wider than the target's
  // lock-free width -> libatomic.
  void loweredCalls(Function &F, FInfo &FI, Instruction &I, bool Cold) {
    char Tp = Cold ? 'c' : 'h';
    const DataLayout &DL = M.getDataLayout();
    if (I.getOpcode() == Instruction::FRem) {
      Type *T = I.getType()->getScalarType();
      std::string N = P.FremLibcall + (T->isFloatTy() ? "f" : T->isDoubleTy() ? "" : "l");
      externalCall(FI, &I, N, "lowered:frem", Tp, true, false, nullptr, nullptr);
      return;
    }
    // Integer division wider than 64 bits and IEEE quad arithmetic are compiler-runtime calls on x86-64
    // (LLVM's RuntimeLibcalls: __divti3, __addtf3, ...).
    if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      Type *T = BO->getType()->getScalarType();
      const char *N = nullptr;
      if (T->isIntegerTy() && T->getIntegerBitWidth() > 64) {
        switch (BO->getOpcode()) {
        case Instruction::SDiv: N = "__divti3"; break;
        case Instruction::UDiv: N = "__udivti3"; break;
        case Instruction::SRem: N = "__modti3"; break;
        case Instruction::URem: N = "__umodti3"; break;
        default: break;
        }
      } else if (T->isFP128Ty()) {
        switch (BO->getOpcode()) {
        case Instruction::FAdd: N = "__addtf3"; break;
        case Instruction::FSub: N = "__subtf3"; break;
        case Instruction::FMul: N = "__multf3"; break;
        case Instruction::FDiv: N = "__divtf3"; break;
        default: break;
        }
      }
      if (N) externalCall(FI, &I, N, "lowered:runtime-arithmetic", Tp, true, false, nullptr, nullptr);
      return;
    }
    Type *AT = nullptr;
    std::string Op;
    Align Al;
    if (auto *A = dyn_cast<AtomicRMWInst>(&I)) {
      AT = A->getValOperand()->getType();
      Al = A->getAlign();
      Op = A->getOperation() == AtomicRMWInst::Xchg ? "exchange" : ("fetch_" + AtomicRMWInst::getOperationName(A->getOperation())).str();
    } else if (auto *X = dyn_cast<AtomicCmpXchgInst>(&I)) {
      AT = X->getNewValOperand()->getType();
      Al = X->getAlign();
      Op = "compare_exchange";
    } else if (auto *L = dyn_cast<LoadInst>(&I)) {
      if (L->isAtomic()) { AT = L->getType(); Op = "load"; Al = L->getAlign(); }
    } else if (auto *St = dyn_cast<StoreInst>(&I)) {
      if (St->isAtomic()) { AT = St->getValueOperand()->getType(); Op = "store"; Al = St->getAlign(); }
    }
    if (!AT) return;
    uint64_t Bytes = DL.getTypeStoreSize(AT);
    int Max = P.AtomicInlineMaxBytes;
    Attribute TF = F.getFnAttribute("target-features");
    if (TF.isValid() && TF.getValueAsString().contains("+cx16") && Max < 16) Max = 16;
    // AtomicExpand: an atomic whose alignment is below its size is a call to the generic (unsized)
    // libatomic function, which takes a lock; a wide or odd-sized one calls the sized function.
    if (Al.value() < Bytes) {
      std::string G = (Op == "load" || Op == "store" || Op == "exchange") ? Op : "compare_exchange";
      externalCall(FI, &I, "__atomic_" + G, "lowered:atomic (misaligned: align " + std::to_string(Al.value()) + " < " +
                   std::to_string(Bytes) + " bytes)", Tp, true, false, nullptr, nullptr);
      return;
    }
    if (Bytes > (uint64_t)Max || (Bytes & (Bytes - 1)))
      externalCall(FI, &I, "__atomic_" + Op + "_" + std::to_string(Bytes), "lowered:atomic", Tp, true, false, nullptr, nullptr);
  }

  // Rule (d): a call that code generation adds. It is a call of a library function: the classes its
  // summary gives, and without a summary nothing is known about it (external-unknown: verdict unknown).
  void codegenCall(FInfo &FI, const Instruction *I, const std::string &N, const std::string &Why, char Tp) {
    externalCall(FI, I, N, "codegen:" + Why, Tp, true, false, nullptr, nullptr);
    if (S.Syms.find(N) != S.Syms.end()) return;
    for (auto &St : FI.Sites)
      if (St.I == I && St.Cls == IExt && St.Sym == N) return;  // already unknown
    LSite L;
    L.I = I;
    L.Cls = IExt;
    L.Sub = "codegen:" + Why + " (no summary)";
    L.Sym = N;
    L.Via = "codegen:" + Why;
    L.Temp = Tp;
    FI.Sites.push_back(std::move(L));
  }
  void tlsAccess(FInfo &FI, const Instruction *I, const GlobalVariable *GV, bool Cold) {
    bool Dyn;
    std::string Mo = tlsModel(GV, Dyn);
    bool Emu = OptEmulatedTLS || Triple(M.getTargetTriple()).hasDefaultEmulatedTLS();
    if (Emu) {
      // -femulated-tls: every access calls __emutls_get_address (libgcc, compiler-rt), which allocates
      // and locks on a thread's first access
      instSite(FI, I, "tls-dynamic", "tls " + GV->getName().str() + " (emulated)", Cold);
      codegenCall(FI, I, "__emutls_get_address", "emulated-tls", Cold ? 'c' : 'h');
      return;
    }
    instSite(FI, I, Dyn ? "tls-dynamic" : "tls-static", "tls " + GV->getName().str() + " (" + Mo + ")", Cold);
    // general- and local-dynamic models (and TLS descriptors): a call of __tls_get_addr (the loader),
    // which allocates the thread's block of a dlopen'ed module on first access, under the loader's lock
    if (Dyn) codegenCall(FI, I, "__tls_get_addr", "tls-dynamic", Cold ? 'c' : 'h');
  }

  // Calls that code generation inserts because of a function attribute (stack protector, instrumentation).
  void attributeCalls(Function &F, FInfo &FI) {
    const Instruction *First = &*F.getEntryBlock().getFirstNonPHIOrDbgOrAlloca();
    bool EntryCold = CI.get(F).EntryCold;
    for (auto &AC : P.AttrLibcalls) {
      Attribute::AttrKind K = Attribute::getAttrKindFromName(AC.Attr);
      std::string Value;
      bool Has = false;
      if (K != Attribute::None) Has = F.hasFnAttribute(K);
      else if (F.hasFnAttribute(AC.Attr)) { Has = true; Value = F.getFnAttribute(AC.Attr).getValueAsString().str(); }
      if (!Has) continue;
      if (std::find(AC.SkipValues.begin(), AC.SkipValues.end(), Value) != AC.SkipValues.end()) continue;
      if (AC.NeedsAlloca) {
        // the stack protector instruments only functions with stack objects (LLVM's
        // requiresStackProtector looks at allocas only): a function with none gets no check
        bool AnyAlloca = false;
        for (auto &I : instructions(F)) if (isa<AllocaInst>(I)) { AnyAlloca = true; break; }
        if (!AnyAlloca) continue;
      }
      char Tp = (AC.Temp == "cold" || EntryCold) ? 'c' : 'h';
      for (auto &C : AC.Calls) {
        std::string N = C == "$value" ? Value : C;
        if (N.empty()) continue;
        codegenCall(FI, First, N, AC.Attr, Tp);
      }
    }
    // target features that turn returns and indirect branches into calls of external thunks
    Attribute TF = F.getFnAttribute("target-features");
    if (TF.isValid() && TF.getValueAsString().contains("+retpoline-external-thunk"))
      codegenCall(FI, First, "__x86_indirect_thunk", "retpoline-external-thunk", EntryCold ? 'c' : 'h');
    // Rule (d): an attribute the tool does not know is reported, not ignored
    for (const Attribute &A : F.getAttributes().getFnAttrs())
      if (A.isStringAttribute()) {
        std::string K = A.getKindAsString().str();
        bool Handled = P.KnownFnAttrs.count(K) > 0;
        for (auto &AC : P.AttrLibcalls) if (AC.Attr == K) Handled = true;
        if (!Handled) UnknownFnAttrs[K]++;
      }
  }
  std::map<std::string, int64_t> UnknownFnAttrs;

  std::vector<std::pair<std::string, std::string>> fpAttrs(Function &F) {
    std::vector<std::pair<std::string, std::string>> R;
    for (const char *A : {"unsafe-fp-math", "no-nans-fp-math", "no-infs-fp-math", "no-signed-zeros-fp-math",
                          "approx-func-fp-math"}) {
      Attribute At = F.getFnAttribute(A);
      if (At.isValid() && At.getValueAsString() == "true") R.push_back({A, "true"});
    }
    for (const char *A : {"denormal-fp-math", "denormal-fp-math-f32"}) {
      Attribute At = F.getFnAttribute(A);
      if (At.isValid() && At.getValueAsString() != "ieee,ieee" && At.getValueAsString() != "ieee")
        R.push_back({A, At.getValueAsString().str()});
    }
    return R;
  }

  struct AsmBlock { const Function *F; std::string Text; bool Harmless; const CallBase *CB; };
  std::vector<AsmBlock> AsmBlocks;
  static std::string asmNorm(StringRef Str) {
    std::string N;
    for (char C : Str) {
      if (C == '\t' || C == '\n' || C == '\r') C = ' ';
      if (C == ' ' && (N.empty() || N.back() == ' ')) continue;
      N.push_back(C);
    }
    while (!N.empty() && (N.back() == ' ' || N.back() == ';')) N.pop_back();
    return N;
  }
  bool asmHarmless(StringRef Str) { return P.AsmHarmless.count(asmNorm(Str)) > 0; }
  void analyseCall(Function &F, FInfo &FI, CallBase *CB, bool Cold) {
    auto &CR = R.Res[CB];
    if (auto *II = dyn_cast<InvokeInst>(CB))
      if (unwindReachesTerminate(II->getUnwindDest())) {
        FI.HasTerminatePad = true;
        FI.TerminateInvokes.insert(CB);
      }
    if (CR.Asm) {
      auto *IA = cast<InlineAsm>(CB->getCalledOperand());
      std::string Str = IA->getAsmString().str();
      bool Any = false;
      for (size_t i = 0; i < P.Classes.size(); i++)
        for (auto &[Re, Sub] : P.Classes[i].Asm)
          if (Re->match(Str)) {
            LSite L;
            L.I = CB;
            L.Cls = (int)i;
            L.Sub = Sub.empty() ? "asm" : Sub;
            L.Sym = "asm \"" + Str.substr(0, 60) + "\"";
            L.Via = "instruction";
            L.Temp = Cold ? 'c' : 'h';
            FI.Sites.push_back(std::move(L));
            Any = true;
          }
      // Rule (e): the text of a block is not interpreted. A block is harmless only if it is, exactly, one
      // of the strings of abi.asm_harmless; any other block may enter the kernel or transfer control
      // (SYSCALL, .byte 0x0f, 0x05, int $128, a pushed return address and a jump): an unknown callee.
      // The class patterns above only add what they recognise (a `may`); they never clear anything.
      bool Harmless = asmHarmless(Str);
      AsmBlocks.push_back({&F, Str.substr(0, 120), Harmless, CB});
      (void)Any;
      if (!Harmless) {
        LSite L;
        L.I = CB;
        L.Cls = IExt;
        L.Sub = "inline-asm";
        L.Sym = "asm \"" + Str.substr(0, 60) + "\"";
        L.Via = "instruction";
        L.Temp = Cold ? 'c' : 'h';
        FI.Sites.push_back(std::move(L));
      } else {
        json::Object A;
        A["kind"] = "inline-asm";
        A["asm"] = Str.substr(0, 80);
        A["site"] = siteJSON(CB);
        FI.Assumptions.push_back({std::move(A), &F});
      }
      return;
    }
    for (auto *D : R.DynLoadSites)
      if (D == CB) {
        json::Object A;
        A["kind"] = "dynamic-loading";
        A["symbol"] = asFunction(CB->getCalledOperand())->getName().str();
        A["site"] = siteJSON(CB);
        FI.Assumptions.push_back({std::move(A), &F});
      }
    for (auto *D : R.ExecMapSites)
      if (D == CB) {
        json::Object A;
        A["kind"] = "exec-mapping";
        A["symbol"] = asFunction(CB->getCalledOperand())->getName().str();
        A["site"] = siteJSON(CB);
        FI.Assumptions.push_back({std::move(A), &F});
      }
    if (pruned(CB)) return;
    bool IsVirtual = CR.Rule == "vtable";
    if (CR.Indirect) {
      json::Object A;
      A["kind"] = "indirect-call";
      A["id"] = (int64_t)Indirect.size();
      A["rule"] = CR.Rule;
      A["detail"] = CR.Detail;
      if (!CR.Visibility.empty()) A["visibility"] = CR.Visibility;
      A["targets"] = (int64_t)CR.Targets.size();
      if (CR.Open) A["open"] = true;
      if (CR.Unresolved) A["unresolved"] = true;
      A["site"] = siteJSON(CB);
      FI.IndirectIds.push_back((int)Indirect.size());
      if (CR.Unresolved || CR.Open) FI.Assumptions.push_back({json::Object(A), &F});
      if (CR.closedWorld()) A["closed_world"] = true;
      Indirect.push_back({CR.Rule, (int64_t)CR.Targets.size(), std::move(A), CR.closedWorld(), CR.UsedPTA});
      if (CR.Unresolved || CR.Open) {
        LSite L;
        L.I = CB;
        L.Cls = IInd;
        L.Sub = CR.Unresolved ? "unresolved" : "open-type";
        L.Sym = "<" + CR.Rule + ": " + CR.Detail + ">";
        L.Via = IsVirtual ? "virtual" : "fnptr";
        L.Rule = CR.Rule;
        L.Targets = (int64_t)CR.Targets.size();
        L.Temp = Cold ? 'c' : 'h';
        FI.Sites.push_back(std::move(L));
      }
    }
    for (auto &T : CR.Targets) {
      bool TCold = Cold || nameInfo(T.F->getName(), T.F->isIntrinsic()).ColdTarget;
      addCallSites(FI, CB, T.F, T.V, TCold);
      if (DiagDrop && ((T.V == Via::Fnptr && strstr(DiagDrop, "fnptr")) || (T.V == Via::Virtual && strstr(DiagDrop, "virtual")) ||
                       (T.V == Via::Callback && strstr(DiagDrop, "callback"))))
        continue;  // diagnosis only (FX_DIAG_DROP): never set in normal use, the result is then unsound
      if (!T.F->isDeclaration() && !isOpaque(T.F)) FI.Edges.push_back({T.F, CB, T.V, !TCold});
    }
  }

  void computeSCC() {
    // Iterative Tarjan over defined functions.
    DenseMap<const Function *, int> Index, Low;
    DenseSet<const Function *> OnStack;
    std::vector<const Function *> Stack;
    int Counter = 0;
    unsigned NextId = 0;
    for (Function *Root : Defined) {
      if (Index.count(Root)) continue;
      std::vector<std::pair<const Function *, size_t>> Work{{Root, 0}};
      Index[Root] = Low[Root] = Counter++;
      Stack.push_back(Root);
      OnStack.insert(Root);
      while (!Work.empty()) {
        auto &[F, EI] = Work.back();
        auto &Edges = Info[F].Edges;
        if (EI < Edges.size()) {
          const Function *T = Edges[EI++].T;
          if (!Index.count(T)) {
            Index[T] = Low[T] = Counter++;
            Stack.push_back(T);
            OnStack.insert(T);
            Work.push_back({T, 0});
          } else if (OnStack.count(T)) {
            Low[F] = std::min(Low[F], Index[T]);
          }
        } else {
          const Function *Done = F;
          Work.pop_back();
          if (!Work.empty()) Low[Work.back().first] = std::min(Low[Work.back().first], Low[Done]);
          if (Low[Done] == Index[Done]) {
            unsigned Id = NextId++, N = 0;
            while (true) {
              const Function *X = Stack.back();
              Stack.pop_back();
              OnStack.erase(X);
              SCCId[X] = Id;
              N++;
              if (X == Done) break;
            }
            SCCSize[Id] = N;
          }
        }
      }
    }
    // A recursion site: a call edge inside one SCC that is a real cycle.
    for (Function *F : Defined) {
      auto &FI = Info[F];
      DenseSet<const CallBase *> Done;
      for (auto &E : FI.Edges) {
        bool Cyc = SCCId[F] == SCCId[E.T] && (SCCSize[SCCId[F]] > 1 || E.T == F);
        if (!Cyc || !Done.insert(E.CB).second) continue;
        LSite L;
        L.I = E.CB;
        L.Cls = IRec;
        L.Sym = E.T->getName().str();
        L.Via = viaStr(E.V);
        L.Sub = SCCSize[SCCId[F]] > 1 ? "mutual" : "self";
        L.Temp = E.Hot ? 'h' : 'c';
        FI.Sites.push_back(std::move(L));
      }
    }
  }

  void run() {
    addEngineClasses(P);
    // For every class, a hidden class "unknown:<c>" whose sites make only <c> unknown (from summaries).
    size_t NReal = P.Classes.size();
    UnknownOf.assign(NReal, -1);
    for (size_t i = 0; i < NReal; i++) {
      if (P.Classes[i].Engine && P.Classes[i].SummaryClasses.empty()) continue;
      ClassDef D;
      D.Name = "unknown:" + P.Classes[i].Name;
      D.Engine = true;
      D.Hidden = true;
      UnknownOf[i] = (int)P.Classes.size();
      P.Classes.push_back(D);
    }
    UnknownOf.resize(P.Classes.size(), -1);
    IRec = P.classIndex("recursion");
    IInd = P.classIndex("indirect-unknown");
    IExt = P.classIndex("external-unknown");
    auto T0 = std::chrono::steady_clock::now();
    auto lap = [&](const char *What) {
      auto T1 = std::chrono::steady_clock::now();
      Timings.push_back({What, std::chrono::duration_cast<std::chrono::milliseconds>(T1 - T0).count()});
      T0 = T1;
    };
    R.build();
    lap("resolve");
    if (P.Precision >= 2) {
      StringMap<fneffects::PtrModel> MCache;
      auto ModelOf = [&](const Function &F) -> fneffects::PtrModel {
        auto It = MCache.find(F.getName());
        if (It != MCache.end()) return It->second;
        fneffects::PtrModel M;
        for (auto &[Pt, PM] : P.PointerModels)
          if (Pt.matches(F.getName())) { M = PM; break; }
        return MCache[F.getName()] = M;
      };
      bool PICnotPIE = M.getPICLevel() != PICLevel::NotPIC && M.getPIELevel() == PIELevel::Default;
      auto FromOutside = [&](const Function &F) {
        if (F.getName() == "main" || P.CalledFromOutside.match(F.getName())) return true;
        // a shared object: every exported function can be called by other modules
        return PICnotPIE && !F.hasLocalLinkage() && F.hasDefaultVisibility();
      };
      // Component mode (README, "Points-to per component"): components are the strongly connected
      // components of the direct-call graph. Any partition is sound; this one keeps recursion together.
      DenseMap<const Function *, unsigned> CompOf;
      auto ComponentOf = [&](const Function &F) -> unsigned {
        if (CompOf.empty()) {
          CallGraph CG(M);
          unsigned Id = 1;
          for (scc_iterator<CallGraph *> I = scc_begin(&CG); !I.isAtEnd(); ++I, ++Id)
            for (CallGraphNode *N : *I)
              if (Function *Fn = N->getFunction()) CompOf[Fn] = Id;
          PtaComponents = Id - 1;
        }
        auto It = CompOf.find(&F);
        return It == CompOf.end() ? 0 : It->second;
      };
      auto FallbackOf = [&](const CallBase &CB) {
        std::vector<Function *> Fs;
        for (auto &T : R.Res[&CB].Targets) Fs.push_back(T.F);
        return Fs;
      };
      PtaComponent = OptPtaScope == "component";
      PT = std::make_unique<fneffects::PointsTo>(M, ModelOf, FromOutside);
      if (PtaComponent) PT->setComponents(ComponentOf);
      PT->solve(FallbackOf, OptPtaBudget);
      lap("points-to");
      if (PT->stats().OverBudget && OptPtaScope == "auto") {
        errs() << "fn-effects: points-to analysis over its time budget; solving per component\n";
        PtaModuleOverBudget = true;
        PtaComponent = true;
        PT = std::make_unique<fneffects::PointsTo>(M, ModelOf, FromOutside);
        PT->setComponents(ComponentOf);
        PT->solve(FallbackOf, OptPtaBudget);
        lap("points-to per component");
      }
      if (const char *Dbg = getenv("FX_PTA_DEBUG"))
        for (auto &[CB, CR] : R.Res)
          if (CR.Indirect && CB->getFunction()->getName().contains(Dbg))
          {
            errs() << "pta " << CB->getFunction()->getName() << ": callee {" << PT->describe(CB->getCalledOperand()) << "}\n";
            const Value *V = CB->getCalledOperand();
            for (int D = 0; D < 4; D++) {
              auto *L = dyn_cast<LoadInst>(V);
              if (!L) break;
              V = L->getPointerOperand();
              std::string W;
              raw_string_ostream OS(W);
              V->printAsOperand(OS, false);
              errs() << "   level " << D << " pointer " << W << " = {" << PT->describe(V).substr(0, 600) << "}\n";
              if (auto *G = dyn_cast<GetElementPtrInst>(V)) V = G->getPointerOperand();
            }
          }
      if (PT->stats().OverBudget) {
        // Abandoned: every indirect call keeps the rapid-type-analysis rule. Sound, and said in the report.
        errs() << "fn-effects: points-to analysis over its time budget; indirect calls resolved by the rta rule\n";
      } else {
        R.refine(*PT, PtaComponent);
        // Which resolutions rest on the pointer models? Solve again with every library function at the
        // default (escape) and no body replaced: a call whose targets that solution also bounds rests on
        // the module's IR alone (README, "What none means").
        if (!OptNoModelCheck) {
          auto Plain = [&](const Function &) { return fneffects::PtrModel(); };
          fneffects::PointsTo PS(M, Plain, FromOutside);
          if (PtaComponent) PS.setComponents(ComponentOf);
          PS.solve([&](const CallBase &CB) {
            std::vector<Function *> Fs;
            for (auto &T : R.Res[&CB].Targets) Fs.push_back(T.F);
            return Fs;
          }, OptPtaBudget);
          if (!PS.stats().OverBudget) {
            ModelCheckDone = true;
            for (auto &[CB, CR] : R.Res) {
              if (!CR.UsedPTA) continue;
              auto SS = PS.site(*CB);
              if (SS.Unk || (SS.Funcs.empty() && !SS.Ext)) continue;
              if (SS.Ext && !StringRef(CR.Rule).ends_with("+ext")) continue;
              DenseSet<Function *> Have;
              for (auto &T : CR.Targets) Have.insert(T.F);
              bool Sub = true;
              for (Function *F : SS.Funcs)
                if ((P.FnptrMatch != "exact" || F->getFunctionType() == CB->getFunctionType()) && !Have.count(F)) { Sub = false; break; }
              if (Sub) { CR.UsedPTA = false; ModelFree++; }
            }
          }
          lap("points-to without models");
        }
      }
      lap("refine");
    }
    for (Function &F : M)
      if (!F.isDeclaration()) Defined.push_back(&F);
    for (Function *F : Defined) CI.get(*F);
    lap("hot-cold");
    std::vector<std::pair<int64_t, const Function *>> Slow;
    for (Function *F : Defined) {
      auto A0 = std::chrono::steady_clock::now();
      analyseFunction(*F);
      Slow.push_back({std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - A0).count(), F});
    }
    if (getenv("FX_PROFILE")) {
      std::sort(Slow.rbegin(), Slow.rend());
      size_t NS = 0, NE = 0;
      for (auto *F : Defined) { NS += Info[F].Sites.size(); NE += Info[F].Edges.size(); }
      errs() << "sites " << NS << " edges " << NE << "\n";
      for (size_t i = 0; i < 8 && i < Slow.size(); i++)
        errs() << Slow[i].first << " us " << Slow[i].second->getName().substr(0, 80) << " blocks " << Slow[i].second->size()
               << " sites " << Info[Slow[i].second].Sites.size() << " edges " << Info[Slow[i].second].Edges.size() << "\n";
    }
    lap("sites");
    computeSCC();
    lap("scc");
    computeTable();
    lap("effect-table");
    computeSignatures();
    checkSignatures();
    lap("signatures");
  }

  // ------------------------------------------------------------------------------------ effect table
  // For each function and class: how the class is first reached (a local site, or an edge to a callee
  // that has it). Three tables: any path; paths that are hot at every step; hot-or-unknown.
  struct Witness {
    int Site = -1;  // index into Sites, or
    int Edge = -1;  // index into Edges
  };
  using Table = std::map<const Function *, std::vector<Witness>>;
  Table TAny, THot, TMaybe;

  void propagate(Table &T, int Mode /*0 any, 1 hot, 2 hot-or-unknown*/) {
    size_t NC = P.Classes.size();
    DenseMap<const Function *, std::vector<std::pair<const Function *, int>>> Callers;
    for (Function *F : Defined) {
      T[F] = std::vector<Witness>(NC);
      auto &E = Info[F].Edges;
      for (size_t i = 0; i < E.size(); i++)
        if (Mode == 0 || E[i].Hot) Callers[E[i].T].push_back({F, (int)i});
    }
    std::deque<std::pair<const Function *, int>> Q;
    for (Function *F : Defined) {
      auto &Sites = Info[F].Sites;
      for (size_t i = 0; i < Sites.size(); i++) {
        auto &Sx = Sites[i];
        bool Ok = Mode == 0 || Sx.Temp == 'h' || (Mode == 2 && Sx.Temp == 'u');
        if (!Ok) continue;
        auto &W = T[F][Sx.Cls];
        if (W.Site < 0 && W.Edge < 0) { W.Site = (int)i; Q.push_back({F, Sx.Cls}); }
      }
    }
    while (!Q.empty()) {
      auto [F, C] = Q.front();
      Q.pop_front();
      auto It = Callers.find(F);
      if (It == Callers.end()) continue;
      for (auto &[G, EIdx] : It->second) {
        auto &W = T[G][C];
        if (W.Site < 0 && W.Edge < 0) { W.Edge = EIdx; Q.push_back({G, C}); }
      }
    }
  }

  void computeTable() {
    propagate(TAny, 0);
    propagate(THot, 1);
    propagate(TMaybe, 2);
  }

  // ------------------------------------------------------------------------- per-function signatures
  // README, "Per-function signatures". For every defined function and class: always | may | never |
  // unknown, the evidence it rests on (the weakest link), on whose account a `may` is (the function's own,
  // a library function's inside, or only a consequence of another class's primitive), and the callees
  // that bring it. Computed bottom-up over the strongly connected components of the call graph.
  enum Ev : int8_t { EvOwn = 0, EvDirect, EvPta, EvVtable, EvType, EvPolicy, EvSummary, EvModel, EvClosed, EvUnknown, EvNone };
  static const char *evName(int E) {
    static const char *N[] = {"own", "direct", "indirect-pta", "indirect-vtable", "indirect-type", "library-policy",
                              "library-summary", "model", "closed-world", "unknown", "-"};
    return N[E < 0 || E > EvNone ? EvNone : E];
  }
  enum Acct : int8_t { AcOwn = 0, AcLibrary, AcConsequence, AcNone };
  static const char *acctName(int A) {
    static const char *N[] = {"own", "library", "consequence", "-"};
    return N[A < 0 || A > AcNone ? AcNone : A];
  }
  struct Sig {
    std::vector<char> May, Always;
    std::vector<char> Firm;  // always, with a justification that uses no elidable primitive (rule (f))
    std::vector<int8_t> MayEv, AlwaysEv, MayAcct;
    std::vector<uint16_t> AlwaysVia;  // every evidence kind the justification of `always` uses (bit mask)
    std::vector<uint32_t> ConsOf;     // classes whose primitives bring this class as a consequence (bit mask)
    int8_t NeverEv = EvOwn;           // weakest link over everything the function's closure rests on
    bool Returns = false;             // has a path to a normal return
  };
  std::map<const Function *, Sig> Sigs;
  // evidence, the class this call is a primitive of when it is the site's own class (-1), the class
  // whose primitive the site is a consequence of (-1), the account
  struct SiteInfo { int8_t Ev; int Prim; int Cons; int8_t Acct; };

  SiteInfo siteInfo(const LSite &S) {
    SiteInfo R{EvPolicy, -1, -1, AcOwn};
    const ClassDef &C = P.Classes[S.Cls];
    if (C.Hidden || S.Cls == IExt || S.Cls == IInd) { R.Ev = EvUnknown; return R; }
    bool Summ = !S.Lib.empty() || StringRef(S.Via).starts_with("summary:");
    if (S.Via == "instruction" || S.Cls == IRec) { R.Ev = EvOwn; return R; }
    if (Summ) R.Ev = EvSummary;
    bool OwnPrim = C.Primitives.match(S.Sym) != nullptr;
    if (!Summ && C.OwnAccount.match(S.Sym)) return R;  // named by the policy as this class's own doing
    // A site at a symbol another class claims (its `attributed` list, or one of its primitives that is not
    // a primitive of this class) is a consequence of that class: malloc's lock, a throw's allocation and
    // unwinder lock, a mutex's futex system call.
    for (size_t k = 0; k < P.Classes.size() && R.Cons < 0; k++)
      if ((int)k != S.Cls && P.Classes[k].Attributed.match(S.Sym)) R.Cons = (int)k;
    for (size_t k = 0; k < P.Classes.size() && R.Cons < 0 && !OwnPrim; k++)
      if ((int)k != S.Cls && P.Classes[k].Primitives.match(S.Sym)) R.Cons = (int)k;
    if (R.Cons >= 0) R.Acct = AcConsequence;
    else if (Summ && !OwnPrim) R.Acct = AcLibrary;  // inside a library function, by its summary: the cause is not known
    // a call of a primitive of this very class, made directly by this function
    if (OwnPrim && R.Ev == EvPolicy && S.Via == "direct") R.Prim = S.Cls;
    return R;
  }
  int8_t ruleEv(const CallRes &CR) {
    StringRef Ru = CR.Rule;
    if (CR.closedWorld()) return EvClosed;  // RTA, the closed-hierarchy rule, an unestablished vtable hierarchy, an ifunc
    if (CR.UsedPTA) return EvModel;
    // a fallback keeps the call's own rule: the vtable and constant-table rules do not depend on points-to
    if (Ru.starts_with("vtable") && !Ru.contains("+ext")) return EvVtable;
    if (Ru.starts_with("points-to+ext:vtable")) return EvVtable;
    if (Ru.starts_with("const-table")) return EvPta;
    if (Ru.contains("fallback") || Ru.contains("+ext") || Ru.starts_with("fnptr")) return EvType;
    if (Ru.starts_with("points-to")) return EvPta;
    return EvType;
  }
  int8_t edgeEv(const LEdge &E) {
    auto It = R.Res.find(E.CB);
    if (E.V == Via::Direct) return It != R.Res.end() && !It->second.LinkVar.empty() ? EvClosed : EvDirect;
    if (E.V == Via::Callback) return EvPolicy;  // the library may call what it is given (abi rule)
    if (It == R.Res.end()) return EvType;
    return ruleEv(It->second);
  }
  // A definition the linker or the loader may replace (a weak definition, an exported function of a
  // -fPIC shared object): what is said about it holds for callers only if it is not replaced (assumption
  // 2; evidence `closed-world`, the entry report's `interposable`).
  bool interposable(const Function *F) {
    auto L = F->getLinkage();
    if (L == GlobalValue::WeakAnyLinkage || L == GlobalValue::LinkOnceAnyLinkage || L == GlobalValue::ExternalWeakLinkage ||
        L == GlobalValue::CommonLinkage)
      return true;
    bool PICnotPIE = M.getPICLevel() != PICLevel::NotPIC && M.getPIELevel() == PIELevel::Default;
    return PICnotPIE && !F->isDSOLocal() && F->hasDefaultVisibility() && !F->hasLocalLinkage();
  }
  // A body that is only a copy of a library's definition (available_externally): the code that runs is
  // the library's, compiled separately (it may have dropped an allocation this copy still shows).
  bool externalCopy(const Function *F) { return F->getLinkage() == GlobalValue::AvailableExternallyLinkage; }
  // Blocks on every path from the entry to a normal return (a `ret`): the post-dominators of the entry in
  // the CFG restricted to blocks that can reach a `ret`. Unwinding out of the function, `resume`,
  // `unreachable` and calls that never return are not returns; a landing pad that leads to a `ret` (a
  // caught exception) is part of the graph like any other block.
  struct MustInfo { DenseSet<const BasicBlock *> CanRet, Must; bool Returns = false, NoClaim = false; };
  std::map<const Function *, MustInfo> MustMemo;
  const MustInfo &mustBlocks(const Function &F) {
    auto It = MustMemo.find(&F);
    if (It != MustMemo.end()) return It->second;
    MustInfo MI;
    std::deque<const BasicBlock *> Q;
    for (auto &B : F)
      if (isa<ReturnInst>(B.getTerminator())) { MI.CanRet.insert(&B); Q.push_back(&B); }
    while (!Q.empty()) {
      const BasicBlock *B = Q.front();
      Q.pop_front();
      for (auto *Pd : predecessors(B))
        if (MI.CanRet.insert(Pd).second) Q.push_back(Pd);
    }
    // Control flow the CFG does not show, or a body that is not the code that runs: nothing is claimed.
    // setjmp (returns_twice); inline assembly that can transfer control; a coroutine before it is split
    // (a suspend returns to the caller); a naked function; a copy of a library's definition.
    if (F.isPresplitCoroutine() || F.hasFnAttribute(Attribute::Naked) || externalCopy(&F)) MI.NoClaim = true;
    for (auto &B : F)
      for (auto &I : B)
        if (auto *CB = dyn_cast<CallBase>(&I)) {
          if (CB->hasFnAttr(Attribute::ReturnsTwice)) MI.NoClaim = true;
          if (CB->isInlineAsm() && !asmHarmless(cast<InlineAsm>(CB->getCalledOperand())->getAsmString()))
            MI.NoClaim = true;
          if (isa<CallBrInst>(CB)) MI.NoClaim = true;
        }
    const BasicBlock *Entry = &F.getEntryBlock();
    MI.Returns = MI.CanRet.count(Entry) > 0;
    if (MI.Returns && !MI.NoClaim) {
      // Must(B) = {B} + intersection over successors S in CanRet of Must(S); greatest fixed point
      std::vector<const BasicBlock *> Bs;
      DenseMap<const BasicBlock *, unsigned> Idx;
      for (auto &B : F) if (MI.CanRet.count(&B)) { Idx[&B] = Bs.size(); Bs.push_back(&B); }
      size_t N = Bs.size();
      std::vector<BitVector> Must(N, BitVector(N, true));
      bool Changed = true;
      while (Changed) {
        Changed = false;
        for (size_t k = N; k-- > 0;) {
          const BasicBlock *B = Bs[k];
          BitVector NewM(N, false);
          bool First = true;
          if (!isa<ReturnInst>(B->getTerminator()))
            for (auto *S : successors(B)) {
              auto J = Idx.find(S);
              if (J == Idx.end()) continue;
              if (First) { NewM = Must[J->second]; First = false; }
              else NewM &= Must[J->second];
            }
          NewM.set(k);
          if (NewM != Must[k]) { Must[k] = NewM; Changed = true; }
        }
      }
      for (unsigned b : Must[Idx[Entry]].set_bits()) MI.Must.insert(Bs[b]);
    }
    return MustMemo[&F] = std::move(MI);
  }
  // A call at CB counts as completed on every returning path only if no returning path leaves it through
  // its unwind edge (an invoke whose landing pad can reach a `ret` may be skipped by a caught exception).
  bool completesOnReturn(const CallBase *CB, const MustInfo &MI) {
    if (!MI.Must.count(CB->getParent())) return false;
    if (auto *II = dyn_cast<InvokeInst>(CB)) return !MI.CanRet.count(II->getUnwindDest());
    return true;
  }
  // The targets of a call whose resolution may carry `always`: the direct callee, or every target of an
  // indirect call resolved by a rule that is complete without library data: points-to without pointer
  // models and without library pointers, a constant table, or the vtable rule on a closed hierarchy with
  // nothing pruned. Not: type matching (it is the fallback), an open or unresolved call, a call that may
  // go into library code, a closed-world or model-dependent resolution, an incomplete module. Every
  // target must have a body that is the code that runs; an interposable target makes the evidence
  // `closed-world`. Empty: the call carries nothing.
  std::vector<Function *> alwaysTargets(const CallBase *CB, int8_t &Ev) {
    std::vector<Function *> T;
    auto It = R.Res.find(CB);
    if (It == R.Res.end() || pruned(CB)) return T;
    const CallRes &CR = It->second;
    if (CR.Asm) return T;
    if (!CR.Indirect) {
      for (auto &X : CR.Targets) if (X.V == Via::Direct) T.push_back(X.F);
      Ev = CR.LinkVar.empty() ? EvDirect : EvClosed;  // rule (c): a weak or exported alias
    } else {
      if (OptIncomplete > 0) return T;
      int8_t RE = ruleEv(CR);
      if (RE == EvPta && !CR.Unresolved && !CR.Open) {
        Ev = EvPta;
        for (auto &X : CR.Targets) if (X.V != Via::Callback) T.push_back(X.F);
      } else if (CR.VtResolved) {
        // the vtable rule's own target set (also when points-to narrowed it with a pointer model, or
        // fell back): complete if the hierarchy is closed; closed-world unless that is established
        Ev = (CR.VtClosed || CR.RTAPruned > 0) ? EvClosed : EvVtable;
        for (auto &X : CR.vtTargets()) if (X.V != Via::Callback) T.push_back(X.F);
      } else {
        return T;
      }
    }
    for (Function *F : T) {
      if (F->isDeclaration() || isOpaque(F) || externalCopy(F)) return {};
      if (interposable(F)) Ev = std::max<int8_t>(Ev, EvClosed);
    }
    return T;
  }
  // Is the site an operation of its class that is executed whenever its instruction is?
  bool siteExecutes(const LSite &St, const SiteInfo &In) {
    if (!St.I || St.Cls == IRec) return false;
    if (In.Prim == St.Cls) return true;  // a direct call of a primitive of the class
    if (In.Ev != EvOwn) return false;
    // an instruction of the class; not a phi (its operand is evaluated on one edge only) and not an
    // assembly block (the matched instruction may sit behind a jump inside the block)
    if (isa<PHINode>(St.I)) return false;
    if (auto *CB = dyn_cast<CallBase>(St.I)) if (CB->isInlineAsm()) return false;
    return true;
  }

  std::map<const Function *, std::vector<SiteInfo>> SInfo;
  void computeSignatures() {
    size_t NC = P.Classes.size();
    std::map<unsigned, std::vector<Function *>> ByScc;  // Tarjan numbers components callees first
    for (Function *F : Defined) ByScc[SCCId[F]].push_back(F);
    for (Function *F : Defined) {
      Sig &S = Sigs[F];
      S.May.assign(NC, 0); S.Always.assign(NC, 0); S.Firm.assign(NC, 0);
      S.MayEv.assign(NC, EvNone); S.AlwaysEv.assign(NC, EvNone); S.MayAcct.assign(NC, AcNone);
      S.ConsOf.assign(NC, 0); S.AlwaysVia.assign(NC, 0);
      S.Returns = mustBlocks(*F).Returns;
    }
    // local part: own sites, external calls the answer relies on, and the call sites' rules
    std::map<const Function *, int8_t> LocalNever;
    for (Function *F : Defined) {
      auto &FI = Info[F];
      auto &V = SInfo[F];
      for (auto &St : FI.Sites) V.push_back(siteInfo(St));
      int8_t L = EvOwn;
      for (auto &E : FI.Edges) L = std::max(L, edgeEv(E));
      for (auto &N : FI.Relied) L = std::max<int8_t>(L, StringRef(N).starts_with("summary:") ? EvSummary : EvPolicy);
      for (auto &Ex : FI.Sites) if (Ex.Cls == IExt || Ex.Cls == IInd) L = EvUnknown;
      // an indirect call rests on its rule even when no target has a body in the module
      for (auto &I : instructions(*F))
        if (auto *CB = dyn_cast<CallBase>(&I)) {
          auto It = R.Res.find(CB);
          if (It != R.Res.end() && It->second.Indirect && !It->second.Asm && !pruned(CB)) L = std::max(L, ruleEv(It->second));
          if (It != R.Res.end() && !It->second.LinkVar.empty() && !pruned(CB)) {
            L = std::max<int8_t>(L, EvClosed);  // rule (c)
            FI.LinkVars.insert(It->second.LinkVar);
          }
        }
      if (interposable(F)) L = std::max<int8_t>(L, EvClosed);  // the entry report's none*
      LocalNever[F] = L;
    }
    for (auto &[Id, Fs] : ByScc) {
      bool Changed = true;
      for (int Round = 0; Changed && Round < 100000; Round++) {
        Changed = false;
        for (Function *F : Fs) {
          Sig &S = Sigs[F];
          auto &FI = Info[F];
          const MustInfo &MI = mustBlocks(*F);
          auto lower = [&](int8_t &X, int8_t V) { if (V < X) { X = V; Changed = true; } };
          auto set1 = [&](char &X) { if (!X) { X = 1; Changed = true; } };
          auto always = [&](size_t c, int8_t Ev, uint16_t Via, bool Firm) {
            if (!S.Always[c] || Ev < S.AlwaysEv[c]) { S.Always[c] = 1; S.AlwaysEv[c] = Ev; S.AlwaysVia[c] = Via; Changed = true; }
            if (Firm && !S.Firm[c]) { S.Firm[c] = 1; Changed = true; }
          };
          int8_t NE = LocalNever[F];
          for (size_t i = 0; i < FI.Sites.size(); i++) {
            const LSite &St = FI.Sites[i];
            const SiteInfo &In = SInfo[F][i];
            int c = St.Cls;
            set1(S.May[c]);
            lower(S.MayEv[c], In.Ev);
            lower(S.MayAcct[c], In.Acct);
            if (In.Cons >= 0 && !(S.ConsOf[c] & (1u << In.Cons))) { S.ConsOf[c] |= 1u << In.Cons; Changed = true; }
            // always: a primitive call or an own instruction in a block on every returning path
            if (MI.Returns && !MI.NoClaim && siteExecutes(St, In) && MI.Must.count(St.I->getParent()))
              always(c, In.Ev, (uint16_t)(1u << In.Ev), P.Classes[c].Elidable.match(St.Sym) == nullptr);
          }
          for (auto &E : FI.Edges) {
            Sig &T = Sigs[E.T];
            int8_t EE = edgeEv(E);
            NE = std::max(NE, T.NeverEv);
            for (size_t c = 0; c < NC; c++) {
              if (!T.May[c]) continue;
              set1(S.May[c]);
              lower(S.MayEv[c], std::max(EE, T.MayEv[c]));
              lower(S.MayAcct[c], T.MayAcct[c]);
              if ((S.ConsOf[c] | T.ConsOf[c]) != S.ConsOf[c]) { S.ConsOf[c] |= T.ConsOf[c]; Changed = true; }
            }
          }
          if (NE > S.NeverEv) { S.NeverEv = NE; Changed = true; }
          // always through calls: every target `always`, the call completed on every returning path
          if (MI.Returns && !MI.NoClaim)
            for (auto &I : instructions(*F)) {  // in function order: the result does not depend on set order
                auto *CB = dyn_cast<CallBase>(&I);
                if (!CB || !completesOnReturn(CB, MI)) continue;
                int8_t RE = EvDirect;
                auto Ts = alwaysTargets(CB, RE);
                if (Ts.empty()) continue;
                for (size_t c = 0; c < NC; c++) {
                  bool All = true;
                  int8_t Ev = RE;
                  uint16_t Via = (uint16_t)(1u << RE);
                  bool Firm = true;
                  for (Function *T : Ts) {
                    Sig &TS = Sigs[T];
                    if (!TS.Always[c]) { All = false; break; }
                    Ev = std::max(Ev, TS.AlwaysEv[c]);
                    Via |= TS.AlwaysVia[c];
                    if (!TS.Firm[c]) Firm = false;
                  }
                  if (All) always(c, Ev, Via, Firm);
                }
              }
        }
      }
    }
  }
  int64_t SigMismatch = 0;
  void checkSignatures() {
    // the bottom-up signatures must reach exactly what the effect table reaches; always implies may
    for (Function *F : Defined)
      for (size_t c = 0; c < P.Classes.size(); c++) {
        bool T = has(TAny, F, (int)c);
        if ((bool)Sigs[F].May[c] != T || (Sigs[F].Always[c] && !Sigs[F].May[c])) SigMismatch++;
      }
    if (SigMismatch) errs() << "fn-effects: internal check failed: signatures disagree with the effect table (" << SigMismatch << ")\n";
  }
  // the verdict of class c for F (README): always | may | never | unknown
  const char *sigVerdict(const Function *F, int c) {
    Sig &S = Sigs[F];
    if (S.Always[c]) return "always";
    if (S.May[c]) return "may";
    bool Blocked = S.May[IExt] || S.May[IInd] || (UnknownOf[c] >= 0 && S.May[UnknownOf[c]]);
    return Blocked ? "unknown" : "never";
  }
  json::Object sigJSON(const Function *F) {
    json::Object O;
    Sig &S = Sigs[F];
    auto &FI = Info[F];
    const MustInfo &MI = mustBlocks(*F);
    for (size_t c = 0; c < P.Classes.size(); c++) {
      if (P.Classes[c].Hidden) continue;
      json::Object X;
      const char *V = sigVerdict(F, (int)c);
      X["verdict"] = V;
      StringRef VS = V;
      int8_t Ev = VS == "always" ? S.AlwaysEv[c] : VS == "may" ? S.MayEv[c] : VS == "never" ? S.NeverEv : (int8_t)EvUnknown;
      X["evidence"] = evName(Ev);
      if (S.May[c]) {
        X["account"] = acctName(S.MayAcct[c]);
        if (S.ConsOf[c]) {
          json::Array CA;
          for (size_t k = 0; k < P.Classes.size(); k++) if (S.ConsOf[c] & (1u << k)) CA.push_back(P.Classes[k].Name);
          X["consequence_of"] = std::move(CA);
        }
        int64_t Own = 0;
        for (auto &St : FI.Sites) if (St.Cls == (int)c) Own++;
        X["own_sites"] = Own;
        std::map<int, std::set<std::string>> By;
        for (auto &E : FI.Edges)
          if (Sigs[E.T].May[c]) By[edgeEv(E)].insert(E.T->getName().str());
        json::Object CO;
        for (auto &[K, Ns] : By) {
          json::Array A;
          for (auto &N : Ns) A.push_back(N);
          CO[evName(K)] = std::move(A);
        }
        if (!By.empty()) X["callees"] = std::move(CO);
      }
      if (S.Always[c]) {
        // what makes it `always`: the own sites on every returning path, and the calls on every
        // returning path all of whose targets are `always`
        json::Object AO;
        int64_t Own = 0;
        for (size_t i = 0; i < FI.Sites.size(); i++)
          if (FI.Sites[i].Cls == (int)c && siteExecutes(FI.Sites[i], SInfo[F][i]) && MI.Must.count(FI.Sites[i].I->getParent())) Own++;
        AO["own_sites"] = Own;
        std::set<std::string> Cs;
        for (auto &I : instructions(*F)) {
          auto *CB = dyn_cast<CallBase>(&I);
          if (!CB || !completesOnReturn(CB, MI)) continue;
          int8_t RE = EvDirect;
          auto Ts = alwaysTargets(CB, RE);
          bool All = !Ts.empty();
          for (Function *T : Ts) if (!Sigs[T].Always[c]) All = false;
          if (All) for (Function *T : Ts) Cs.insert(T->getName().str());
        }
        json::Array CA, VA;
        for (auto &N : Cs) CA.push_back(N);
        AO["callees"] = std::move(CA);
        for (int k = 0; k < EvNone; k++) if (S.AlwaysVia[c] & (1u << k)) VA.push_back(evName(k));
        AO["rests_on"] = std::move(VA);
        if (!S.Firm[c]) { AO["elidable"] = true; X["qualifier"] = "elidable"; }
        X["always"] = std::move(AO);
      }
      O[P.Classes[c].Name] = std::move(X);
    }
    return O;
  }


  bool has(Table &T, const Function *F, int C) {
    auto &W = T[F][C];
    return W.Site >= 0 || W.Edge >= 0;
  }

  json::Object witnessJSON(const Function *F, int C, Table &T) {
    json::Array Chain;
    json::Array Vias;
    const Function *Cur = F;
    for (int Guard = 0; Guard < 100000; Guard++) {
      Chain.push_back(Cur->getName().str());
      auto &W = T[Cur][C];
      if (W.Site >= 0) {
        auto &Sx = Info[Cur].Sites[W.Site];
        json::Object O;
        O["symbol"] = Sx.Sym;
        if (!Sx.Sub.empty()) O["subclass"] = Sx.Sub;
        O["via"] = Sx.Via;
        O["site"] = siteJSON(Sx.I);
        O["chain"] = std::move(Chain);
        O["edges"] = std::move(Vias);
        return O;
      }
      auto &E = Info[Cur].Edges[W.Edge];
      Vias.push_back(viaStr(E.V));
      Cur = E.T;
    }
    return json::Object{{"error", "witness loop"}};
  }

  json::Value effectsJSON() {
    json::Object Fns;
    for (Function *F : Defined) {
      json::Object E;
      E["demangled"] = demangleName(F->getName());
      json::Object Cls;
      bool Blocked = has(TAny, F, IExt) || has(TAny, F, IInd);
      for (size_t c = 0; c < P.Classes.size(); c++) {
        if (!has(TAny, F, (int)c)) continue;
        if (P.Classes[c].Hidden) {
          Cls[P.Classes[c].Name] = json::Object{{"blocked", true}};
          continue;
        }
        json::Object W = witnessJSON(F, (int)c, has(THot, F, (int)c) ? THot : TAny);
        W["temp"] = has(THot, F, (int)c) ? "hot" : has(TMaybe, F, (int)c) ? "unknown" : "cold";
        Cls[P.Classes[c].Name] = std::move(W);
      }
      E["effects"] = std::move(Cls);
      E["blocked"] = Blocked;
      // What this function does itself (sites in its own body, inlined code included) and whom it calls.
      // A per-function view is built from these two lists: own effects and the callees that bring the rest.
      {
        json::Array Own, Calls;
        auto It = Info.find(F);
        if (It != Info.end()) {
          for (const LSite &St : It->second.Sites) {
            if (St.Cls < 0 || St.Cls >= (int)P.Classes.size()) continue;
            json::Object O;
            O["class"] = P.Classes[St.Cls].Name;
            O["symbol"] = St.Sym;
            O["via"] = St.Via;
            O["temp"] = std::string(1, St.Temp);
            SiteInfo In = siteInfo(St);
            O["evidence"] = evName(In.Ev);
            O["account"] = acctName(In.Acct);
            if (In.Cons >= 0) O["consequence_of"] = P.Classes[In.Cons].Name;
            if (In.Prim >= 0) O["primitive"] = true;
            if (St.I && siteExecutes(St, In) && mustBlocks(*F).Must.count(St.I->getParent())) O["on_every_return"] = true;
            if (St.I) if (const DebugLoc &DL = St.I->getDebugLoc()) {
              const DILocation *L = DL.get();
              while (L->getInlinedAt()) L = L->getInlinedAt();
              O["line"] = (int64_t)L->getLine();
              O["file"] = L->getFilename().str();
            }
            Own.push_back(std::move(O));
          }
          for (const LEdge &Ed : It->second.Edges) {
            if (!Ed.T) continue;
            json::Object O;
            O["callee"] = Ed.T->getName().str();
            O["hot"] = Ed.Hot;
            O["first_use"] = Ed.FirstUse;
            O["kind"] = (int64_t)Ed.V;
            Calls.push_back(std::move(O));
          }
        }
        E["own"] = std::move(Own);
        E["calls"] = std::move(Calls);
      }
      E["signature"] = sigJSON(F);
      E["returns"] = Sigs[F].Returns;
      Fns[F->getName()] = std::move(E);
    }
    json::Object Top;
    Top["tool"] = "fn-effects";
    Top["kind"] = "effects";
    Top["policy"] = P.Name;
    Top["target"] = OptTarget.empty() ? M.getModuleIdentifier() : OptTarget.getValue();
    json::Array CN;
    for (auto &C : P.Classes) CN.push_back(C.Name);
    Top["classes"] = std::move(CN);
    Top["functions"] = std::move(Fns);
    return Top;
  }

  // --------------------------------------------------------------------------------------- entries
  std::map<const Function *, std::vector<std::string>> annotations() {
    std::map<const Function *, std::vector<std::string>> R;
    GlobalVariable *GA = M.getNamedGlobal("llvm.global.annotations");
    if (!GA || !GA->hasInitializer()) return R;
    auto *A = dyn_cast<ConstantArray>(GA->getInitializer());
    if (!A) return R;
    for (auto &Op : A->operands()) {
      auto *CS = dyn_cast<ConstantStruct>(Op);
      if (!CS || CS->getNumOperands() < 2) continue;
      Function *F = asFunction(CS->getOperand(0));
      auto *SV = dyn_cast<GlobalVariable>(CS->getOperand(1)->stripPointerCasts());
      if (!F || !SV || !SV->hasInitializer()) continue;
      if (auto *CDA = dyn_cast<ConstantDataArray>(SV->getInitializer()))
        if (CDA->isString()) R[F].push_back(CDA->getAsCString().str());
    }
    return R;
  }

  std::set<const Function *> registeredWith(const std::vector<std::string> &Regs) {
    std::set<const Function *> R;
    for (Function *F : Defined) {
      bool CallsReg = false;
      std::vector<Function *> Passed, Stored;
      for (Instruction &I : instructions(*F)) {
        if (auto *CB = dyn_cast<CallBase>(&I)) {
          Function *C = asFunction(CB->getCalledOperand());
          bool IsReg = false;
          if (C) for (auto &N : Regs) if (C->getName() == N) IsReg = true;
          if (IsReg) {
            CallsReg = true;
            for (Value *A : CB->args()) if (Function *AF = asFunction(A)) Passed.push_back(AF);
          }
        } else if (auto *St = dyn_cast<StoreInst>(&I)) {
          if (Function *AF = asFunction(St->getValueOperand())) Stored.push_back(AF);
        }
      }
      for (auto *X : Passed) R.insert(X);
      if (CallsReg) for (auto *X : Stored) R.insert(X);  // e.g. sa.sa_handler = h; sigaction(..., &sa, ...)
    }
    return R;
  }

  bool nameMatches(const Function *F, StringRef Pat) {
    std::string Mangled = F->getName().str();
    std::string D = demangleName(Mangled);
    std::string DP = stripParams(D);
    if (Pat == Mangled || Pat == D || Pat == DP) return true;
    if (Pat.find_first_of("*?[") != StringRef::npos) {
      auto G = GlobPattern::create(Pat);
      if (!G) return false;
      return G->match(Mangled) || G->match(D) || G->match(DP);
    }
    return false;
  }

  std::vector<Function *> select(const std::vector<std::string> &Sel, const std::vector<std::string> &Extra) {
    auto Ann = annotations();
    std::vector<Function *> Out;
    std::set<Function *> Seen;
    std::map<std::string, std::set<const Function *>> RegCache;
    auto atom = [&](Function *F, StringRef A) -> bool {
      if (A == "defined") return true;
      if (A.consume_front("name:")) return nameMatches(F, A);
      if (A.consume_front("annotate:")) {
        auto It = Ann.find(F);
        if (It == Ann.end()) return false;
        for (auto &S : It->second) if (S == A) return true;
        return false;
      }
      if (A.consume_front("attr:")) {
        Attribute::AttrKind K = Attribute::getAttrKindFromName(A);
        return K != Attribute::None ? F->hasFnAttribute(K) : F->hasFnAttribute(A);
      }
      if (A == "has-terminate-pad") return Info[F].HasTerminatePad;
      if (A.consume_front("registered-with:")) {
        auto &C = RegCache[A.str()];
        if (C.empty()) {
          SmallVector<StringRef, 4> Parts;
          A.split(Parts, '|');
          std::vector<std::string> Regs;
          for (auto P2 : Parts) Regs.push_back(P2.str());
          C = registeredWith(Regs);
          C.insert(nullptr);  // mark computed
        }
        return C.count(F);
      }
      if (A.consume_front("not:")) return false;
      return false;
    };
    auto expr = [&](Function *F, StringRef E) -> bool {
      SmallVector<StringRef, 4> Parts;
      E.split(Parts, '&');
      for (auto Pt : Parts) {
        bool Neg = Pt.consume_front("!");
        bool V = atom(F, Pt);
        if (V == Neg) return false;
      }
      return true;
    };
    for (Function *F : Defined) {
      bool Sel1 = false;
      for (auto &E : Sel) if (expr(F, E)) { Sel1 = true; break; }
      if (!Sel1) for (auto &E : Extra) if (nameMatches(F, E)) { Sel1 = true; break; }
      if (Sel1 && Seen.insert(F).second) Out.push_back(F);
    }
    return Out;
  }

  json::Object entryReport(Function *Entry, const EntryKind &K, bool &Agree) {
    bool OnlyTerm = K.Start == "terminate-invokes";
    auto &EI = Info[Entry];
    auto allowSite = [&](const Function *F, const LSite &Sx) {
      if (F != Entry || !OnlyTerm) return true;
      auto *CB = dyn_cast_or_null<CallBase>(Sx.I);
      return CB && EI.TerminateInvokes.count(CB) && Sx.Cls != IRec;
    };
    auto allowEdge = [&](const Function *F, const LEdge &E) {
      if (F != Entry || !OnlyTerm) return true;
      return EI.TerminateInvokes.count(E.CB) > 0;
    };
    // Hot BFS, then full BFS; parents give one chain per function, preferring hot chains.
    struct Par { const Function *From; const CallBase *CB; Via V; };
    DenseMap<const Function *, Par> HotPar, AnyPar;
    std::vector<const Function *> Order;
    DenseSet<const Function *> HotSet, AnySet, FUSet;
    DenseMap<const Function *, Par> FUPar;
    // Mode 1: likely edges only; 2: likely and first-use edges; 0: every edge
    auto bfs = [&](int Mode, DenseSet<const Function *> &Set, DenseMap<const Function *, Par> &Parent) {
      bool HotOnly = Mode != 0;
      std::deque<const Function *> Q{Entry};
      Set.insert(Entry);
      while (!Q.empty()) {
        const Function *F = Q.front();
        Q.pop_front();
        if (!HotOnly) Order.push_back(F);
        for (auto &E : Info[F].Edges) {
          if (Mode == 1 && !E.Hot) continue;
          if (Mode == 2 && !E.Hot && !E.FirstUse) continue;
          if (!allowEdge(F, E)) continue;
          if (Set.insert(E.T).second) { Parent[E.T] = {F, E.CB, E.V}; Q.push_back(E.T); }
        }
      }
    };
    bfs(1, HotSet, HotPar);
    bfs(2, FUSet, FUPar);
    bfs(0, AnySet, AnyPar);
    auto chainTo = [&](const Function *F, json::Array &Chain, json::Array &Edges) {
      std::vector<const Function *> Fs;
      std::vector<std::string> Vs;
      auto &PM = HotSet.count(F) ? HotPar : FUSet.count(F) ? FUPar : AnyPar;
      const Function *Cur = F;
      while (Cur != Entry) {
        auto It = PM.find(Cur);
        if (It == PM.end()) break;
        Fs.push_back(Cur);
        Vs.push_back(viaStr(It->second.V));
        Cur = It->second.From;
      }
      Fs.push_back(Entry);
      for (auto It = Fs.rbegin(); It != Fs.rend(); ++It) Chain.push_back((*It)->getName().str());
      for (auto It = Vs.rbegin(); It != Vs.rend(); ++It) Edges.push_back(*It);
    };

    size_t NC = P.Classes.size();
    std::vector<int64_t> Count(NC, 0), HotCount(NC, 0), ColdCount(NC, 0), UnkCount(NC, 0), FUCount(NC, 0);
    json::Array Findings, Assumptions, Lint;
    std::set<int> AssumIds;
    std::set<std::string> ExtSeen;
    // Findings are grouped by (class, subclass, symbol, function): one chain per group, all sites listed.
    struct Group {
      const LSite *First;
      const Function *F;
      char Temp;
      std::vector<const Instruction *> Others;
    };
    std::vector<Group> Groups;
    std::map<std::tuple<int, std::string, std::string, const Function *>, size_t> GIdx;
    auto rank = [](char T) { return T == 'h' ? 3 : T == 'f' ? 2 : T == 'u' ? 1 : 0; };
    for (const Function *F : Order) {
      auto &FI = Info[F];
      for (auto &Sx : FI.Sites) {
        if (!allowSite(F, Sx)) continue;
        char Tp = HotSet.count(F) ? Sx.Temp : (FUSet.count(F) && Sx.Temp != 'c') ? 'f' : 'c';
        Count[Sx.Cls]++;
        (Tp == 'h' ? HotCount : Tp == 'c' ? ColdCount : Tp == 'f' ? FUCount : UnkCount)[Sx.Cls]++;
        if (Sx.Cls == IExt && Sx.Sub.empty() && ExtSeen.insert(Sx.Sym).second) {
          json::Object A;
          A["kind"] = "external-unknown";
          A["symbol"] = Sx.Sym;
          AssumIds.insert(intern("ext:" + Sx.Sym, std::move(A)));
        }
        if (P.Classes[Sx.Cls].Informational && !OptAllFindings) continue;
        // compact: one finding per (class, subclass, symbol); full: per function as well
        auto Key = std::make_tuple(Sx.Cls, Sx.Sub, Sx.Sym, OptFull ? F : nullptr);
        auto It = GIdx.find(Key);
        if (It == GIdx.end()) {
          GIdx[Key] = Groups.size();
          Groups.push_back({&Sx, F, Tp, {}});
        } else {
          Group &G = Groups[It->second];
          if (rank(Tp) > rank(G.Temp)) { G.Others.push_back(G.First->I); G.First = &Sx; G.Temp = Tp; }
          else if (G.First->I != Sx.I) G.Others.push_back(Sx.I);
        }
      }
    }
    for (auto &G : Groups) {
      const LSite &Sx = *G.First;
      json::Object O;
      O["class"] = P.Classes[Sx.Cls].Name;
      if (!Sx.Sub.empty()) O["subclass"] = Sx.Sub;
      O["symbol"] = Sx.Sym;
      json::Array Chain, Edges;
      chainTo(G.F, Chain, Edges);
      if (!OptFull) {
        // compact: function names once, in the top-level "functions" table
        json::Array Ids;
        for (auto &X : Chain) Ids.push_back((int64_t)funcId(X.getAsString()->str()));
        O["chain"] = std::move(Ids);
      } else {
        O["chain"] = std::move(Chain);
      }
      O["edges"] = std::move(Edges);
      O["site"] = siteJSON(Sx.I);
      if (!OptFull) compactSite(*O.getObject("site"));
      if (!G.Others.empty()) {
        json::Array OS;
        std::set<std::tuple<std::string, unsigned, unsigned>> Seen;
        for (auto *I : G.Others) {
          const DILocation *L = I ? I->getDebugLoc().get() : nullptr;
          std::string Fl = L ? fullPath(L->getScope(), L->getFilename()) : "";
          unsigned Ln = L ? L->getLine() : 0, Cl = L ? L->getColumn() : 0;
          if (!Seen.insert({Fl, Ln, Cl}).second) continue;
          json::Object X;
          X["file"] = Fl;
          X["line"] = (int64_t)Ln;
          X["col"] = (int64_t)Cl;
          OS.push_back(std::move(X));
        }
        O["sites"] = (int64_t)(1 + G.Others.size());
        if (!OS.empty() && OptFull) O["other_sites"] = std::move(OS);
      }
      O["temp"] = tempName(G.Temp);
      {
        SiteInfo In = siteInfo(Sx);
        if (In.Cons >= 0) O["consequence_of"] = P.Classes[In.Cons].Name;  // e.g. malloc's lock: reported under alloc
      }
      O["via"] = Sx.Via;
      if (!Sx.Inner.empty()) {
        json::Array In;
        for (size_t k = 0; k < Sx.Inner.size() && (OptFull || k < 4); k++) In.push_back(Sx.Inner[k]);
        O["inner"] = std::move(In);
        if (!OptFull && Sx.Inner.size() > 4) O["inner_total"] = (int64_t)Sx.Inner.size();
      }
      if (Sx.Targets >= 0) O["targets"] = Sx.Targets;
      Findings.push_back(std::move(O));
    }
    int64_t LintFM = 0, LintFMA = 0;
    std::set<std::string> LintLibm;
    for (const Function *F : Order) {
      auto &FI = Info[F];
      for (auto &A : FI.Assumptions) {
        auto K = A.O.getString("kind");
        if (K && *K == "indirect-call") continue;  // listed in indirect_calls
        std::string Key;
        raw_string_ostream KS(Key);
        KS << json::Value(json::Object(A.O));
        AssumIds.insert(intern(KS.str(), json::Object(A.O)));
      }
      if (FI.HasLint) {
        if (OptFull) {
          json::Object L(FI.Lint);
          L["func"] = F->getName().str();
          Lint.push_back(std::move(L));
        }
        if (FI.Lint.getObject("fast_math")) LintFM++;
        if (auto N = FI.Lint.getInteger("fmuladd")) LintFMA += *N;
        if (auto *LA = FI.Lint.getArray("libm"))
          for (auto &X : *LA) if (auto XS = X.getAsString()) LintLibm.insert(XS->str());
      }
    }
    std::map<std::string, int64_t> AssumByKind;
    for (int Id : AssumIds) {
      Assumptions.push_back((int64_t)Id);
      if (auto K = AssumTab[Id].getString("kind")) AssumByKind[K->str()]++;
    }
    json::Array IIds;
    std::map<std::string, std::pair<int64_t, int64_t>> ByRule;
    for (const Function *F : Order)
      for (int Id : Info[F].IndirectIds) {
        IIds.push_back((int64_t)Id);
        auto &BR = ByRule[Indirect[Id].Rule];
        BR.first++;
        BR.second += Indirect[Id].Targets;
      }
    std::set<std::string> Relied, Interp;
    bool PICnotPIE = M.getPICLevel() != PICLevel::NotPIC && M.getPIELevel() == PIELevel::Default;
    bool AnyPTA = false;
    int64_t ClosedWorldSites = 0;
    for (const Function *F : Order)
      for (int Id : Info[F].IndirectIds) {
        if (Indirect[Id].UsedPTA) AnyPTA = true;
        if (Indirect[Id].ClosedWorld) ClosedWorldSites++;
      }
    for (const Function *F : Order) {
      for (auto &N : Info[F].Relied) Relied.insert(N);
      auto L = F->getLinkage();
      if (L == GlobalValue::WeakAnyLinkage || L == GlobalValue::LinkOnceAnyLinkage || L == GlobalValue::ExternalWeakLinkage ||
          L == GlobalValue::CommonLinkage)
        Interp.insert(F->getName().str() + " (weak definition: a strong one elsewhere replaces it)");
      else if (PICnotPIE && !F->isDSOLocal() && F->hasDefaultVisibility() && !F->hasLocalLinkage())
        Interp.insert(F->getName().str() + " (exported from a shared object: interposable)");
    }
    // Trust marking (README, "What none means"): a none that rests on anything but the module's own IR
    // and rules that need no library data is none*, with the list of what it trusts.
    std::set<std::string> Trusted(Relied.begin(), Relied.end());
    if (AnyPTA && PT)
      for (auto &N : PT->modelsUsed()) Trusted.insert("pointer-model:" + N);
    if (ClosedWorldSites) Trusted.insert("closed-world:" + std::to_string(ClosedWorldSites) + " indirect calls (RTA or closed hierarchy)");
    for (const Function *F : Order)
      for (auto &N : Info[F].LinkVars) Interp.insert(N + " (an alias the linker or loader may bind to another definition)");
    for (auto &N : Interp) Trusted.insert("interposable:" + N);
    bool Blocked = Count[IExt] > 0 || Count[IInd] > 0;
    json::Object Verdict, Counts;
    for (size_t c = 0; c < NC; c++) {
      if (P.Classes[c].Hidden) continue;
      bool ClassBlocked = UnknownOf[c] >= 0 && Count[UnknownOf[c]] > 0;
      const char *V = Count[c] ? "reachable" : (Blocked || ClassBlocked) ? "unknown" : "none";
      if ((int)c == IExt || (int)c == IInd) V = Count[c] ? "reachable" : "none";
      if (StringRef(V) == "none" && !Trusted.empty()) V = OptStrict ? "unknown" : "none*";
      Verdict[P.Classes[c].Name] = V;
      if (Count[c]) {
        json::Object CO;
        CO["sites"] = Count[c];
        CO["likely"] = HotCount[c];
        CO["unlikely"] = ColdCount[c];
        CO["first_use"] = FUCount[c];
        CO["unknown"] = UnkCount[c];
        Counts[P.Classes[c].Name] = std::move(CO);
      }
    }
    // Self-check: the effect table must give the same reachable classes (only for start = all).
    if (!OnlyTerm && !OptNoSelfCheck) {
      for (size_t c = 0; c < NC; c++) {
        bool T = has(TAny, Entry, (int)c), B = Count[c] > 0;
        if (T != B) Agree = false;
        bool TH = has(THot, Entry, (int)c), BH = HotCount[c] > 0;
        if (TH != BH) Agree = false;
      }
    }
    json::Array Viol;
    for (auto &Fb : K.Forbid) {
      auto V = Verdict.getString(Fb);
      if (V && *V != "none") {
        json::Object X;
        X["class"] = Fb;
        X["verdict"] = V->str();
        Viol.push_back(std::move(X));
      }
    }
    json::Object E;
    E["entry"] = Entry->getName().str();
    E["demangled"] = demangleName(Entry->getName());
    E["kind"] = K.Name;
    if (OnlyTerm) {
      json::Array SS;
      for (auto *CB : EI.TerminateInvokes) SS.push_back(siteJSON(CB));
      E["start_sites"] = std::move(SS);
    }
    E["verdict"] = std::move(Verdict);
    E["counts"] = std::move(Counts);
    E["violations"] = std::move(Viol);
    E["functions_reached"] = (int64_t)Order.size();
    json::Array RA, IA, TA;
    for (auto &N : Relied) RA.push_back(StringRef(N).split(':').second.str());
    for (auto &N : Interp) IA.push_back(N);
    for (auto &N : Trusted) TA.push_back(N);
    E["relies_on_externals"] = std::move(RA);
    E["trusted"] = std::move(TA);  // what a none* rests on: summary:, known:, policy:, pointer-model:, closed-world:, interposable:
    json::Object IC, BRO;
    for (auto &[Rl, NT] : ByRule) BRO[Rl] = json::Object{{"sites", NT.first}, {"targets", NT.second}};
    IC["sites"] = (int64_t)IIds.size();
    IC["by_rule"] = std::move(BRO);
    IC["ids"] = std::move(IIds);
    E["indirect_calls"] = std::move(IC);
    E["interposable_definitions"] = std::move(IA);
    E["findings"] = std::move(Findings);
    E["assumptions"] = std::move(Assumptions);
    json::Object ABK;
    for (auto &[K, N] : AssumByKind) ABK[K] = N;
    E["assumption_counts"] = std::move(ABK);
    if (P.Lint) {
      if (OptFull) E["lint"] = std::move(Lint);
      json::Array LL;
      for (auto &N : LintLibm) LL.push_back(N);
      E["lint_summary"] = json::Object{{"functions_with_fast_math", LintFM}, {"fmuladd", LintFMA}, {"libm", std::move(LL)}};
    }
    return E;
  }

  std::vector<std::string> FuncTab;
  std::map<std::string, int> FuncIdx;
  int funcId(const std::string &N) {
    auto It = FuncIdx.find(N);
    if (It != FuncIdx.end()) return It->second;
    FuncTab.push_back(N);
    return FuncIdx[N] = (int)FuncTab.size() - 1;
  }
  // compact sites: function and scope names become indices into the "functions" table
  void compactSite(json::Object &S) {
    for (const char *K : {"func", "scope"})
      if (auto V = S.getString(K)) S[K] = (int64_t)funcId(V->str());
    if (auto *IA = S.getArray("inlined_at"))
      for (auto &X : *IA)
        if (auto *XO = X.getAsObject())
          if (auto V = XO->getString("scope")) (*XO)["scope"] = (int64_t)funcId(V->str());
  }
  std::vector<json::Object> AssumTab;
  std::map<std::string, int> AssumKey;
  int intern(const std::string &Key, json::Object O) {
    auto It = AssumKey.find(Key);
    if (It != AssumKey.end()) return It->second;
    int Id = (int)AssumTab.size();
    O["id"] = (int64_t)Id;
    AssumTab.push_back(std::move(O));
    AssumKey[Key] = Id;
    return Id;
  }
  json::Array assumptionsJSON() {
    json::Array A;
    for (auto &O : AssumTab) A.push_back(json::Object(O));
    return A;
  }

  json::Array indirectJSON() {
    json::Array A;
    for (auto &R2 : Indirect) A.push_back(json::Object(R2.O));
    return A;
  }

  json::Value report(bool &AllAgree, const std::function<void(json::Object &&)> &EmitEntry) {
    json::Array Entries;
    std::vector<std::string> Extra(OptEntry.begin(), OptEntry.end());
    std::string ExtraKind = OptEntryKind.empty() ? (P.Kinds.empty() ? "" : P.Kinds[0].Name) : OptEntryKind.getValue();
    if (P.Kinds.empty()) P.Kinds.push_back(EntryKind{"default", {}, {}, "all"});
    json::Array Unmatched;
    for (auto &K : P.Kinds) {
      auto Sel = select(K.Select, K.Name == ExtraKind ? Extra : std::vector<std::string>{});
      for (Function *F : Sel) EmitEntry(entryReport(F, K, AllAgree));
    }
    if (!Extra.empty()) {
      for (auto &E : Extra) {
        bool Any = false;
        for (Function *F : Defined) if (nameMatches(F, E)) { Any = true; break; }
        if (!Any) {
          // Maybe only a declaration (inlined away or in a library).
          bool Decl = false;
          for (Function &F : M) if (F.isDeclaration() && nameMatches(&F, E)) Decl = true;
          json::Object U;
          U["pattern"] = E;
          U["reason"] = Decl ? "declared but has no body in the module" : "no function matches (inlined or removed)";
          Unmatched.push_back(std::move(U));
        }
      }
    }
    json::Object Top;
    Top["tool"] = "fn-effects";
    Top["target"] = OptTarget.empty() ? M.getModuleIdentifier() : OptTarget.getValue();
    Top["policy"] = P.Name;
    json::Array PF;
    for (auto &F : P.Files) PF.push_back(F);
    Top["policy_files"] = std::move(PF);
    json::Array CL;
    for (auto &C : P.Classes) {
      if (C.Hidden) continue;
      json::Object O;
      O["name"] = C.Name;
      if (!C.Description.empty()) O["description"] = C.Description;
      if (C.Informational) O["informational"] = true;
      if (C.Engine) O["engine"] = true;
      CL.push_back(std::move(O));
    }
    Top["classes"] = std::move(CL);
    json::Array GA;
    for (auto &A : P.GlobalAssumptions) GA.push_back(A);
    Top["global_assumptions"] = std::move(GA);
    json::Array Libs;
    for (auto &L : S.Libraries) Libs.push_back(L);
    Top["summaries"] = std::move(Libs);
    json::Object MS;
    MS["functions_defined"] = (int64_t)Defined.size();
    MS["address_taken"] = (int64_t)R.NumAddrTaken;
    MS["indirect_virtual"] = (int64_t)R.NumVirtual;
    MS["indirect_fnptr_type"] = (int64_t)R.NumFnptr;
    MS["indirect_const_table"] = (int64_t)R.NumTable;
    MS["indirect_unresolved"] = (int64_t)R.NumUnresolved;
    MS["indirect_open"] = (int64_t)R.NumOpen;
    MS["vtables_with_type_metadata"] = (int64_t)R.VTables.size();
    MS["vtables_without_type_metadata"] = (int64_t)R.VTablesNoMeta;
    if (R.VTablesNoMeta) {
      json::Array VA;
      for (auto &N : R.VTablesNoMetaNames) VA.push_back(N);
      MS["vtables_without_type_metadata_names"] = std::move(VA);
      errs() << "fn-effects: " << R.VTablesNoMeta << " vtable(s) without type metadata: every virtual call is open (verdict unknown)\n";
    }
    {
      // rule (e): every inline assembly block, and whether it is on the harmless list
      json::Array AA;
      int64_t NH = 0;
      for (auto &B : AsmBlocks) {
        if (!B.Harmless) NH++;
        if (AA.size() < 200) AA.push_back(json::Object{{"function", B.F->getName().str()}, {"asm", B.Text}, {"harmless", B.Harmless}});
      }
      MS["inline_asm"] = std::move(AA);
      MS["inline_asm_blocks"] = (int64_t)AsmBlocks.size();
      MS["inline_asm_not_harmless"] = NH;
      // rule (d): attributes and module flags the tool does not know
      json::Object UA;
      for (auto &[K, N] : UnknownFnAttrs) UA[K] = N;
      MS["unknown_function_attributes"] = std::move(UA);
      json::Array UF;
      SmallVector<Module::ModuleFlagEntry, 16> Flags;
      M.getModuleFlagsMetadata(Flags);
      for (auto &Fl : Flags)
        if (!P.KnownModuleFlags.count(Fl.Key->getString().str())) UF.push_back(Fl.Key->getString().str());
      if (!UnknownFnAttrs.empty() || !UF.empty())
        errs() << "fn-effects: " << UnknownFnAttrs.size() << " function attribute kind(s) and " << UF.size()
               << " module flag(s) the policy does not know (module.unknown_function_attributes, unknown_module_flags)\n";
      MS["unknown_module_flags"] = std::move(UF);
      MS["emulated_tls"] = OptEmulatedTLS || Triple(M.getTargetTriple()).hasDefaultEmulatedTLS();
    }
    bool PIC = M.getPICLevel() != PICLevel::NotPIC, PIE = M.getPIELevel() != PIELevel::Default;
    MS["relocation"] = PIE ? "pie" : PIC ? "pic" : "static";
    json::Array DC;
    for (auto *CB : R.DynLoadSites) {
      json::Object O;
      O["kind"] = "dynamic-loading";
      O["symbol"] = asFunction(CB->getCalledOperand())->getName().str();
      O["site"] = siteJSON(CB);
      DC.push_back(std::move(O));
    }
    for (auto *CB : R.ExecMapSites) {
      json::Object O;
      O["kind"] = "exec-mapping";
      O["symbol"] = asFunction(CB->getCalledOperand())->getName().str();
      O["site"] = siteJSON(CB);
      DC.push_back(std::move(O));
    }
    MS["dynamic_code"] = std::move(DC);
    MS["dlsym_present"] = R.HasDlsym;
    MS["exit_handlers"] = (int64_t)R.ExitHandlers.size();
    MS["fnptr_match"] = P.FnptrMatch;
    MS["precision"] = P.Precision == 0 ? "type" : P.Precision == 1 ? "rta" : "pta";
    MS["rta_dropped_vtable_targets"] = (int64_t)R.RTADropped;
    MS["signature_mismatches"] = SigMismatch;
    if (OptIncomplete > 0) {
      MS["incomplete_translation_units"] = (int64_t)OptIncomplete;
      MS["incomplete_note"] = std::string("translation units did not compile: RTA pruning and the closed-hierarchy rule are off, every vtable "
                                          "of the module counts as installed") + (R.MissingVTables ? "; a missing unit defines a vtable: virtual calls on objects from outside the module are open" : "");
    }
    json::Object RS;
    for (auto &[K, V] : R.RefineStats) RS[K] = (int64_t)V;
    MS["points_to_sites"] = std::move(RS);
    if (PT) {
      auto St = PT->stats();
      MS["points_to"] = json::Object{{"nodes", (int64_t)St.Nodes}, {"objects", (int64_t)St.Objects}, {"elements", (int64_t)St.Elems},
                                     {"constraints", (int64_t)St.Constraints}, {"iterations", (int64_t)St.Iterations},
                                     {"escaped_objects", (int64_t)St.EscapedObjects}, {"escaped_functions", (int64_t)St.EscapedFunctions},
                                     {"inttoptr", (int64_t)St.IntToPtr}, {"inttoptr_unknown", (int64_t)St.IntToPtrUnknown}, {"collapsed_objects", (int64_t)St.CollapsedObjects},
                                     {"fallback_rounds", (int64_t)St.FallbackRounds}, {"over_budget", St.OverBudget},
                                     {"prepass_objects", (int64_t)St.PreObjects}, {"prepass_relevant", (int64_t)St.PreRelevant},
                                     {"prepass_classes", (int64_t)St.PreClasses}, {"cycle_merged_nodes", (int64_t)St.Merged}, {"prepass_ms", (int64_t)(St.PrepassS * 1000)},
                                     {"no_provenance_stores", (int64_t)St.NoProvStores}, {"forged_pointer_escaped", St.ForgedEscaped},
                                     {"model_check_done", ModelCheckDone}, {"resolutions_without_models", ModelFree},
                                     {"scope", PtaComponent ? "component" : "module"}, {"components", (int64_t)PtaComponents},
                                     {"cut_calls", (int64_t)PT->cutCalls()}, {"module_solve_over_budget", PtaModuleOverBudget}};
    }
    Top["module"] = std::move(MS);
    if (!Unmatched.empty()) Top["unmatched_entries"] = std::move(Unmatched);
    return Top;
  }
};

bool writeJSON(const std::string &Path, json::Value V) {
  std::error_code EC;
  if (Path == "-") {
    outs() << formatv("{0:1}", V) << "\n";
    return true;
  }
  raw_fd_ostream OS(Path, EC);
  if (EC) { errs() << "fn-effects: cannot write " << Path << ": " << EC.message() << "\n"; return false; }
  OS << formatv("{0:1}", V) << "\n";
  return true;
}

struct FnEffectsPass : PassInfoMixin<FnEffectsPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    auto T0 = std::chrono::steady_clock::now();
    Policy P;
    std::string Err;
    if (OptPolicy.empty()) { errs() << "fn-effects: -fx-policy is required\n"; exit(2); }
    if (!loadPolicyFile(OptPolicy, P, Err)) { errs() << "fn-effects: " << Err << "\n"; exit(2); }
    if (!OptPrecision.empty()) P.Precision = OptPrecision == "type" ? 0 : OptPrecision == "rta" ? 1 : 2;
    Summaries S;
    for (auto &F : OptSummary)
      if (!S.load(F, Err)) { errs() << "fn-effects: " << Err << "\n"; exit(2); }
    Analysis A(M, P, S);
    if (!OptMissing.empty()) {
      auto Buf = MemoryBuffer::getFile(OptMissing);
      if (!Buf) { errs() << "fn-effects: cannot read " << OptMissing << "\n"; exit(2); }
      auto J = json::parse((*Buf)->getBuffer());
      if (!J) { errs() << "fn-effects: " << toString(J.takeError()) << "\n"; exit(2); }
      if (auto *O = J->getAsObject())
        for (auto &KV : *O)
          if (auto S2 = KV.second.getAsString()) {
            A.Missing[KV.first] = S2->str();
            if (StringRef(KV.first).starts_with("_ZTV")) A.R.MissingVTables = true;
          }
    }
    A.run();
    bool Agree = A.SigMismatch == 0;
    // The report is streamed: one entry is built, written and freed at a time.
    std::error_code EC;
    std::unique_ptr<raw_fd_ostream> FOS;
    raw_ostream *OS = &outs();
    if (OptOut != "-") {
      FOS = std::make_unique<raw_fd_ostream>(OptOut, EC);
      if (EC) { errs() << "fn-effects: cannot write " << OptOut << ": " << EC.message() << "\n"; exit(2); }
      OS = FOS.get();
    }
    auto TE = std::chrono::steady_clock::now();
    {
      json::OStream J(*OS, 1);
      J.objectBegin();
      J.attributeBegin("entries");
      J.arrayBegin();
      json::Value Top = A.report(Agree, [&](json::Object &&E) { J.value(json::Value(std::move(E))); });
      J.arrayEnd();
      J.attributeEnd();
      auto T1 = std::chrono::steady_clock::now();
      if (auto *O = Top.getAsObject())
        for (auto &KV : *O) J.attribute(KV.first, KV.second);
      J.attribute("indirect_calls", A.indirectJSON());
      J.attribute("assumptions", A.assumptionsJSON());
      {
        json::Array FT;
        for (auto &N : A.FuncTab) FT.push_back(N);
        J.attribute("functions", std::move(FT));
      }
      J.attribute("table_agrees", Agree);
      J.attribute("analysis_ms", (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(T1 - T0).count());
      json::Object TO;
      for (auto &[K, V] : A.Timings) TO[K] = V;
      TO["entries"] = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(T1 - TE).count();
      J.attribute("phase_ms", std::move(TO));
      J.objectEnd();
    }
    *OS << "\n";
    OS->flush();
    if (!OptEffects.empty())
      if (!writeJSON(OptEffects, A.effectsJSON())) exit(2);
    if (!Agree) errs() << "fn-effects: internal check failed: effect table and entry search disagree\n";
    return PreservedAnalyses::all();
  }
  static bool isRequired() { return true; }
};

}  // namespace

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "FnEffects", "0.1", [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM, ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "fn-effects") { MPM.addPass(FnEffectsPass()); return true; }
                  return false;
                });
          }};
}
