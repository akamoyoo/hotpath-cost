// See PointsTo.h. The README ("Points-to analysis") states the algorithm and the soundness argument.
#include "PointsTo.h"

#include "llvm/ADT/SetVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Operator.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>
#include <tuple>

using namespace llvm;

namespace fneffects {

static constexpr int64_t ANY = INT64_MIN;
static constexpr uint64_t INF = UINT64_MAX;
static constexpr uint32_t NONE = UINT32_MAX;

struct PointsTo::Impl {
  Module &M;
  const DataLayout &DL;
  std::function<PtrModel(const Function &)> ModelOf;
  std::function<bool(const Function &)> FromOutside;
  std::function<std::vector<Function *>(const CallBase &)> Fallback;

  // ------------------------------------------------------------------------------------------ objects
  // ONoProv: "bytes without provenance" (an integer computed from no pointer, bytes from outside the
  // process). It flows like provenance through integers and memory; as a pointer it means the same as UNK.
  // The Steensgaard pre-pass ignores it (README, "Provenance").
  enum ObjKind { OFunc, OGlobal, OAlloca, OHeap, OExt, OUnk, OVa, ONoProv };
  struct Reader {
    int64_t K;
    uint64_t S;
    uint32_t Dst;
  };
  struct Slot {
    uint32_t Node;
    uint64_t Size;
  };
  struct Obj {
    const Value *V = nullptr;
    ObjKind Kind = OHeap;
    bool Collapsed = false, Escaped = false;
    DenseSet<int64_t> Offs;
    std::map<int64_t, Slot> Slots;
    std::vector<Reader> Readers;
  };
  std::deque<Obj> Objs;
  DenseMap<const Value *, uint32_t> ObjOf;
  DenseMap<std::pair<const Value *, unsigned>, uint32_t> HeapOf;
  DenseMap<const Function *, uint32_t> VaObjOf;
  uint32_t ExtObj = 0, UnkObj = 0, NoProvObj = 0;
  bool unkLike(uint32_t O) const { return Objs[O].Kind == OUnk || Objs[O].Kind == ONoProv; }
  // NOPROV per TBAA access type (index 0: generic, written untyped or as char)
  std::vector<uint32_t> NoProvByType;
  DenseMap<uint32_t, uint32_t> NoProvTypeOf;  // object -> type id
  uint32_t noProvObj(uint32_t Ty) {
    if (Ty == 0) return NoProvObj;
    if (Ty < NoProvByType.size() && NoProvByType[Ty]) return NoProvByType[Ty];
    if (NoProvByType.size() <= Ty) NoProvByType.resize(Ty + 1, 0);
    uint32_t O = newObj(nullptr, ONoProv);
    NoProvByType[Ty] = O;
    NoProvTypeOf[O] = Ty;
    return O;
  }
  // TBAA (the scalar type DAG Clang emits, old format): ids of access types and their ancestors.
  DenseMap<const MDNode *, uint32_t> TypeId;
  std::vector<const MDNode *> TypeNode{nullptr};
  std::vector<std::vector<uint32_t>> TypeAnc{{}};
  static bool isScalarTypeNode(const MDNode *N) {
    return N && N->getNumOperands() >= 2 && isa<MDString>(N->getOperand(0)) && isa_and_nonnull<MDNode>(N->getOperand(1).get()) &&
           N->getNumOperands() <= 3;
  }
  // The access type of an instruction's !tbaa, as an id; 0 for none, char, the root, a new-format or
  // an unrecognised tag (generic: may alias anything).
  uint32_t tbaaType(const Instruction &I) {
    const MDNode *Tag = I.getMetadata(LLVMContext::MD_tbaa);
    if (!Tag) return 0;
    const MDNode *Acc = nullptr;
    if (Tag->getNumOperands() >= 3 && isa_and_nonnull<MDNode>(Tag->getOperand(0).get()) && isa_and_nonnull<MDNode>(Tag->getOperand(1).get()))
      Acc = cast<MDNode>(Tag->getOperand(1));
    else
      Acc = Tag;
    if (!isScalarTypeNode(Acc)) return 0;
    // "omnipotent char" is a type of its own here, as in LLVM's TBAA: it may alias every type below it
    // (all of them but "vtable pointer"), so bytes written as char stay visible to every other load.
    auto It = TypeId.find(Acc);
    if (It != TypeId.end()) return It->second;
    std::vector<uint32_t> Anc;
    uint32_t Id = (uint32_t)TypeNode.size();
    TypeId[Acc] = Id;
    TypeNode.push_back(Acc);
    TypeAnc.push_back({});
    // ancestors: the parent chain up to the root
    std::vector<const MDNode *> Chain;
    for (const MDNode *X = Acc; X && Chain.size() < 64;) {
      Chain.push_back(X);
      if (!isScalarTypeNode(X)) break;
      X = cast<MDNode>(X->getOperand(1));
    }
    for (const MDNode *X : Chain) {
      auto J = TypeAncKey.find(X);
      uint32_t K;
      if (J == TypeAncKey.end()) { K = (uint32_t)TypeAncKey.size() + 1; TypeAncKey[X] = K; }
      else K = J->second;
      TypeAnc[Id].push_back(K);
    }
    return Id;
  }
  DenseMap<const MDNode *, uint32_t> TypeAncKey;
  bool tbaaMayAlias(uint32_t A, uint32_t B) {
    if (A == B || !A || !B) return true;
    const auto &AA = TypeAnc[A], &BB = TypeAnc[B];
    if (AA.empty() || BB.empty()) return true;
    if (AA.back() != BB.back()) return true;  // different roots: nothing is known
    // one access type is an ancestor of the other (the first entry of each chain is the type itself)
    for (uint32_t X : BB) if (X == AA.front()) return true;
    for (uint32_t X : AA) if (X == BB.front()) return true;
    return false;
  }

  uint32_t newObj(const Value *V, ObjKind K) {
    Objs.emplace_back();
    Objs.back().V = V;
    Objs.back().Kind = K;
    return (uint32_t)Objs.size() - 1;
  }
  uint32_t objOf(const Value *V, ObjKind K) {
    auto It = ObjOf.find(V);
    if (It != ObjOf.end()) return It->second;
    uint32_t O = newObj(V, K);
    ObjOf[V] = O;
    return O;
  }
  uint32_t heapOf(const Value *Site, unsigned Tag = 0) {
    auto Key = std::make_pair(Site, Tag);
    auto It = HeapOf.find(Key);
    if (It != HeapOf.end()) return It->second;
    uint32_t O = newObj(Site, OHeap);
    HeapOf[Key] = O;
    static const char *T = getenv("FX_TRACE_OBJ");
    if (T)
      if (auto *In = dyn_cast_or_null<Instruction>(Site))
        if (In->getFunction()->getName().contains(T)) TraceObjs.insert(O);
    return O;
  }

  // ------------------------------------------------------------------------------------------- elems
  std::vector<std::pair<uint32_t, int64_t>> ElemOf;
  std::map<std::pair<uint32_t, int64_t>, uint32_t> ElemId;
  size_t Collapsed = 0;
  bool FieldInsensitiveHeap = getenv("FX_PTA_HEAP_INSENSITIVE") != nullptr;
  bool UseCycles = getenv("FX_PTA_NO_CYCLES") == nullptr;
  size_t MaxOffsets = getenv("FX_PTA_MAX_OFFSETS") ? (size_t)atoi(getenv("FX_PTA_MAX_OFFSETS")) : 64;

  // Objects that can never hold a code pointer, nor a pointer to an object that does (Steensgaard pre-pass),
  // are all represented by one field-insensitive object DATA.
  std::vector<char> Relevant;  // per object at the time of the pre-pass; later objects are relevant
  uint32_t DataObj = UINT32_MAX;
  bool isData(uint32_t O) const { return O < Relevant.size() && !Relevant[O]; }
  uint32_t elem(uint32_t O, int64_t K) {
    if (isData(O)) { O = DataObj; K = ANY; }
    // heap and stack objects field-insensitive (one element per object); globals keep their offsets
    if (FieldInsensitiveHeap && K != ANY && (Objs[O].Kind == OHeap || Objs[O].Kind == OAlloca)) K = ANY;
    if (K == ANY && Objs[O].Kind == OFunc) return elem(UnkObj, ANY);
    if (K != ANY) {
      Obj &Ob = Objs[O];
      // Provenance rule: an address computed from a function's address (any offset but 0) is of
      // unknown provenance; it is not that function.
      if (Ob.Kind == OFunc && K != 0) return elem(UnkObj, ANY);
      if (Ob.Collapsed || Ob.Kind == OExt || Ob.Kind == OUnk || Ob.Kind == ONoProv || Ob.Kind == OFunc) {
        K = Ob.Kind == OFunc ? 0 : ANY;  // a function is one object; EXT and UNK have no fields
      } else if (!Ob.Offs.count(K)) {
        if (Ob.Offs.size() >= MaxOffsets || K > (1 << 24) || K < -(1 << 24)) {
          Ob.Collapsed = true;  // too many offsets (pointer arithmetic in a loop): field-insensitive from now on
          Collapsed++;
          K = ANY;
        } else {
          Ob.Offs.insert(K);
        }
      }
    }
    auto Key = std::make_pair(O, K);
    auto It = ElemId.find(Key);
    if (It != ElemId.end()) return It->second;
    uint32_t E = (uint32_t)ElemOf.size();
    ElemOf.push_back(Key);
    ElemId[Key] = E;
    return E;
  }

  // ------------------------------------------------------------------------------------------- nodes
  enum EdgeMode { MCopy = 0, MShift = 1, MAny = 2, MAnyKeepFunc = 3 };  // MAnyKeepFunc: offset lost, but a function stays itself (a callback argument)
  struct Edge {
    uint32_t To;
    int Mode;
    int64_t C;
  };
  enum ConKind { CLoad, CStore, CCall, CSelf };
  struct Con {
    ConKind Kind;
    uint32_t A = NONE;  // load: destination; store: source
    uint64_t Size = 8;
    const CallBase *CB = nullptr;  // call: the reported site (indirect calls only)
    const Value *At = nullptr;     // call: the instruction, names heap objects
    std::vector<uint32_t> Args;
    uint32_t Ret = NONE;
  };
  struct Node {
    SparseBitVector<> Pts, Prev;
    std::vector<Edge> Out;
    std::vector<uint32_t> Cons;
    bool Queued = false;
    bool Narrow = false;  // a value narrower than a pointer: cannot hand a pointer to library code
    bool NoProv = false;  // carries no provenance at all (floating point, 16/32-bit integers)
    // Bytes without provenance carry the TBAA type they were stored as (NOPROV per type) only while
    // they sit in memory. A load drops those of a type its own TBAA type cannot alias (strict aliasing)
    // and makes the others generic (Generic): every SSA value holds only generic NOPROV.
    uint32_t LoadTag = 0;
    bool Generic = false;
    // DropNP: the link pointers a container model dereferences (README, R2): bytes without provenance
    // in the same object (a length, a node colour) are not among them
    bool DropNP = false;
    // RetagTo: the source node of a store of this TBAA type: the bytes without provenance it carries are
    // written as that type
    uint32_t RetagTo = 0;
  };
  std::deque<Node> Nodes;
  std::vector<Con> Cons;
  DenseMap<const Value *, uint32_t> NodeOf;
  DenseMap<const Function *, uint32_t> RetOf, VaOf;
  std::set<std::tuple<uint32_t, uint32_t, int, int64_t>> EdgeSet;
  std::deque<uint32_t> Work;
  uint32_t ESC = 0, THROWN = 0, UNKSTORE = 0;
  size_t IntToPtr = 0, Iterations = 0, FallbackRounds = 0, IntToPtrUnknown = 0;
  double PrepassS = 0;
  std::vector<uint32_t> IntToPtrNodes;
  DenseMap<uint32_t, const Value *> NodeVal;
  // tracing (FX_TRACE_OBJ=<substring of the function owning an alloca>): first predecessor of each node
  // that received an element of a traced object
  DenseSet<uint32_t> TraceObjs;
  DenseMap<uint32_t, uint32_t> TraceParent;
  DenseMap<uint32_t, std::pair<uint32_t, int64_t>> SlotOf;
  bool hasTraced(const SparseBitVector<> &S) {
    if (TraceObjs.empty()) return false;
    for (unsigned E : S) if (TraceObjs.count(ElemOf[E].first)) return true;
    return false;
  }
  std::vector<uint32_t> FirstPtrNP;
  void traceEdge(uint32_t From, uint32_t To, const SparseBitVector<> &S) {
    if (TraceObjs.empty() || TraceParent.count(To) || !hasTraced(S)) return;
    TraceParent[To] = From;
    auto It = NodeVal.find(To);
    if (It != NodeVal.end() && isa<LoadInst>(It->second) && It->second->getType()->isPointerTy() && Nodes[To].LoadTag == 0 &&
        FirstPtrNP.size() < 200)
      FirstPtrNP.push_back(To);
  }
  std::string nodeDesc(uint32_t N) {
    auto It = NodeVal.find(N);
    if (It != NodeVal.end()) {
      std::string W;
      raw_string_ostream OS(W);
      if (auto *In = dyn_cast<Instruction>(It->second)) OS << In->getFunction()->getName().substr(0, 60) << ": ";
      else if (auto *A = dyn_cast<Argument>(It->second)) OS << "arg " << A->getArgNo() << " of " << A->getParent()->getName().substr(0, 60) << ": ";
      It->second->print(OS);
      return W.substr(0, 200);
    }
    auto Is = SlotOf.find(N);
    if (Is != SlotOf.end()) return "slot of " + objName(Is->second.first) + " @" + (Is->second.second == ANY ? std::string("*") : std::to_string(Is->second.second));
    for (auto &[F, R] : RetOf) if (R == N) return "return of " + F->getName().str().substr(0, 80);
    if (N == ESC) return "ESC";
    return "node " + std::to_string(N);
  }
  void traceBack(uint32_t N) {
    errs() << "trace back from " << nodeDesc(N) << "\n";
    for (int i = 0; i < 40; i++) {
      auto It = TraceParent.find(N);
      if (It == TraceParent.end()) { errs() << "   (origin)\n"; break; }
      N = It->second;
      errs() << "   <- " << nodeDesc(N) << "\n";
    }
  }
  std::map<std::string, int> EscapeCalls;
  std::string objName(uint32_t O) {
    auto &Ob = Objs[O];
    if (Ob.Kind == OExt) return "EXT";
    if (Ob.Kind == OUnk) return "UNK";
    if (Ob.Kind == ONoProv) return O == NoProvObj ? "NOPROV" : "NOPROV:" + std::to_string(NoProvTypeOf.lookup(O));
    std::string N = Ob.V ? Ob.V->getName().str() : "?";
    if (auto *In = dyn_cast_or_null<Instruction>(Ob.V)) N = (Ob.Kind == OHeap ? "heap@" : "alloca@") + In->getFunction()->getName().str().substr(0, 50);
    return N;
  }
  void dumpBig() {
    std::vector<std::pair<size_t, uint32_t>> V;
    for (uint32_t i = 0; i < Nodes.size(); i++) V.push_back({Nodes[i].Pts.count(), i});
    std::sort(V.rbegin(), V.rend());
    for (int k = 0; k < 3 && k < (int)V.size(); k++) {
      uint32_t N = V[k].second;
      std::string What = "?";
      auto It = NodeVal.find(N);
      if (It != NodeVal.end()) {
        raw_string_ostream OS(What);
        if (auto *In = dyn_cast<Instruction>(It->second)) OS << In->getFunction()->getName().substr(0, 40) << ": ";
        if (auto *Ar = dyn_cast<Argument>(It->second)) OS << "arg of " << Ar->getParent()->getName().substr(0, 120) << ": ";
        It->second->print(OS);
        What = What.substr(0, 220);
      }
      std::map<std::string, int> ByObj;
      for (unsigned E : Nodes[N].Pts) ByObj[objName(ElemOf[E].first)]++;
      std::vector<std::pair<int, std::string>> T;
      for (auto &[K2, C] : ByObj) T.push_back({C, K2});
      std::sort(T.rbegin(), T.rend());
      errs() << "  big node " << N << " size " << V[k].first << " objs " << ByObj.size() << " : " << What << "\n    top:";
      for (int j = 0; j < 5 && j < (int)T.size(); j++) errs() << " " << T[j].second << " x" << T[j].first;
      errs() << "\n";
    }
  }
  // Exposed objects: those whose address became an integer (ptrtoint) or escaped. A pointer of unknown
  // provenance can point only into one of them.
  uint32_t EXPOSED = 0;
  std::vector<uint32_t> ExposedObjs, UnkLoads, UnkStores;
  DenseSet<uint32_t> ExposedSet, UnkLoadSet, UnkStoreSet;
  // A pointer of unknown provenance that reached library code (ForgedEscaped) may be any exposed
  // object's address: every exposed object then escapes too.
  bool ForgedEscaped = false;
  void expose(uint32_t O) {
    if (!ExposedSet.insert(O).second) return;
    ObjKind K = Objs[O].Kind;
    if (K == OUnk || K == ONoProv || K == OFunc || K == OExt) return;
    ExposedObjs.push_back(O);
    // a store through a forged pointer cannot write a constant (undefined behaviour): vtables, typeinfo
    if (auto *GV = dyn_cast_or_null<GlobalVariable>(Objs[O].V))
      if (GV->isConstant()) {
        for (uint32_t D : std::vector<uint32_t>(UnkLoads)) addReader(O, ANY, INF, D);
        if (ForgedEscaped) escapeObj(O);
        return;
      }
    for (uint32_t D : std::vector<uint32_t>(UnkLoads)) addReader(O, ANY, INF, D);
    // what is stored through a pointer of unknown provenance (one hub node for all such stores)
    if (!Component) addEdge(UNKSTORE, slot(O, ANY, INF));
    // Component mode: an exposed object escapes (always sound). It then holds EXT, which stands for
    // every escaped value, and whatever is stored through an unknown pointer escapes; a call whose
    // callee may be EXT falls back to its own rule in this mode, so the hub is not needed.
    if (ForgedEscaped || Component) escapeObj(O);
  }

  // Online cycle collapse: nodes on a cycle of copy edges have equal sets; they are merged (union-find).
  std::vector<uint32_t> NRep;
  size_t Merged = 0, CycleChecks = 0;
  std::set<std::pair<uint32_t, uint32_t>> CycleChecked;
  std::vector<std::pair<uint32_t, uint32_t>> PendingCycles;
  uint32_t nrep(uint32_t N) {
    if (N == NONE) return N;
    uint32_t R = N;
    while (NRep[R] != R) R = NRep[R];
    while (NRep[N] != R) { uint32_t X = NRep[N]; NRep[N] = R; N = X; }
    return R;
  }
  uint32_t NPSRC = 0;  // a node holding only NOPROV: the source of every store of bytes without provenance
  bool special(uint32_t N) { return N == ESC || N == THROWN || N == UNKSTORE || N == EXPOSED || N == NPSRC || SpecialNodes.count(N); }
  void mergeNodes(uint32_t R, uint32_t M) {
    R = nrep(R);
    M = nrep(M);
    if (R == M || special(R) || special(M)) return;
    NRep[M] = R;
    Merged++;
    Node &NR = Nodes[R], &NM = Nodes[M];
    NR.Pts |= NM.Pts;
    NR.Prev &= NM.Prev;  // only what both propagated is done along both sets of edges and constraints
    NR.Out.insert(NR.Out.end(), NM.Out.begin(), NM.Out.end());
    NR.Cons.insert(NR.Cons.end(), NM.Cons.begin(), NM.Cons.end());
    NR.Narrow = NR.Narrow && NM.Narrow;
    // the merged node reads as the most permissive of the two: no filter unless both have the same,
    // generic if either is (it only gains elements)
    if (NR.LoadTag != NM.LoadTag) NR.LoadTag = 0;
    NR.DropNP = NR.DropNP && NM.DropNP;
    if (NR.RetagTo != NM.RetagTo) {
      // generic is the weaker claim: the merged node keeps every NOPROV generic
      NR.RetagTo = 0;
      NR.Generic = true;
      SparseBitVector<> G;
      for (unsigned E : NR.Pts) G.set(Objs[ElemOf[E].first].Kind == ONoProv ? elem(NoProvObj, ANY) : E);
      NR.Pts = G;
    }
    if (NM.Generic && !NR.Generic) {
      NR.Generic = true;
      SparseBitVector<> G;
      for (unsigned E : NR.Pts) {
        uint32_t O = ElemOf[E].first;
        G.set(Objs[O].Kind == ONoProv ? elem(NoProvObj, ANY) : E);
      }
      NR.Pts = G;
    }
    NM.Out.clear();
    NM.Out.shrink_to_fit();
    NM.Cons.clear();
    NM.Cons.shrink_to_fit();
    NM.Pts.clear();
    NM.Prev.clear();
    push(R);
  }
  // Is there a path of copy edges from B back to A? Merge it (with A->B it is a cycle).
  void collapseCycle(uint32_t A, uint32_t B) {
    CycleChecks++;
    A = nrep(A);
    B = nrep(B);
    if (A == B) return;
    DenseMap<uint32_t, uint32_t> Parent;
    std::vector<uint32_t> St{B};
    Parent[B] = B;
    size_t Visits = 0;
    bool Found = false;
    while (!St.empty() && Visits < 4000) {
      uint32_t X = St.back();
      St.pop_back();
      Visits++;
      for (auto &Ed : Nodes[X].Out) {
        if (Ed.Mode != MCopy) continue;
        uint32_t Y = nrep(Ed.To);
        if (Parent.count(Y)) continue;
        Parent[Y] = X;
        if (Y == A) { Found = true; break; }
        St.push_back(Y);
      }
      if (Found) break;
    }
    if (!Found) return;
    std::vector<uint32_t> Path;
    for (uint32_t X = A; X != B; X = Parent[X]) Path.push_back(X);
    Path.push_back(B);
    for (uint32_t X : Path)
      if (special(X) || Nodes[X].NoProv || Nodes[X].DropNP) return;
    for (uint32_t X : Path) mergeNodes(A, X);
  }
  uint32_t newNode() {
    Nodes.emplace_back();
    NRep.push_back((uint32_t)Nodes.size() - 1);
    return (uint32_t)Nodes.size() - 1;
  }
  void push(uint32_t N) {
    N = nrep(N);
    if (!Nodes[N].Queued) {
      Nodes[N].Queued = true;
      Work.push_back(N);
    }
  }
  void addElem(uint32_t N, uint32_t E) {
    N = nrep(N);
    if (Nodes[N].NoProv) return;
    if (Nodes[N].DropNP && Objs[ElemOf[E].first].Kind == ONoProv) return;
    if (Nodes[N].Pts.test_and_set(E)) push(N);
  }
  bool unionInto(const SparseBitVector<> &S, uint32_t To, int Mode, int64_t C) {
    To = nrep(To);
    if (S.empty()) return false;
    uint32_t LT = Nodes[To].LoadTag;
    bool Gen = Nodes[To].Generic, Drop = Nodes[To].DropNP;
    uint32_t RT = Nodes[To].RetagTo;
    if (Mode == MCopy && !LT && !Gen && !Drop && !RT) return Nodes[To].Pts |= S;
    SparseBitVector<> T;
    for (unsigned E : S) {
      auto [O, K] = ElemOf[E];
      if (Drop && Objs[O].Kind == ONoProv) continue;
      if (Objs[O].Kind == ONoProv && O != NoProvObj) {
        uint32_t Ty = NoProvTypeOf.lookup(O);
        if (LT && !tbaaMayAlias(Ty, LT)) continue;  // strict aliasing: not readable as this type
        if (Gen) E = elem(NoProvObj, ANY);
      }
      if (RT && Objs[O].Kind == ONoProv) E = elem(noProvObj(RT), ANY);
      if (Mode == MCopy) { T.set(E); continue; }
      if (Mode == MAnyKeepFunc && Objs[O].Kind == OFunc) { T.set(E); continue; }
      int64_t NK = (Mode == MAny || Mode == MAnyKeepFunc || K == ANY) ? ANY : K + C;
      T.set(elem(O, NK));
    }
    return Nodes[To].Pts |= T;
  }
  void addEdge(uint32_t From, uint32_t To, int Mode = MCopy, int64_t C = 0) {
    if (From == NONE || To == NONE) return;
    From = nrep(From);
    To = nrep(To);
    if (From == To && Mode == MCopy) return;
    // A value narrower than a pointer carries no provenance: it can become a pointer again only by being
    // added to a base pointer, whose provenance is tracked (README, assumptions of the analysis).
    if (Nodes[From].NoProv || Nodes[To].NoProv) return;
    if (To == ESC && Nodes[From].Narrow) return;  // bytes cannot hand a pointer to library code
    if (Mode != MShift) C = 0;
    if (Mode == MShift && C == 0) Mode = MCopy;
    if (!EdgeSet.insert({From, To, Mode, C}).second) return;
    Nodes[From].Out.push_back({To, Mode, C});
    traceEdge(From, To, Nodes[From].Pts);
    if (unionInto(Nodes[From].Pts, To, Mode, C)) push(To);
  }
  uint32_t addCon(uint32_t Owner, Con C) {
    uint32_t Id = (uint32_t)Cons.size();
    Cons.push_back(std::move(C));
    Nodes[Owner].Cons.push_back(Id);
    // the existing points-to set of the owner must be processed for the new constraint
    for (unsigned E : Nodes[Owner].Prev) processCon(Id, E);
    return Id;
  }

  // ------------------------------------------------------------------------------- memory: slots
  static bool overlap(int64_t KR, uint64_t SR, int64_t KS, uint64_t SS) {
    if (KR == ANY || KS == ANY) return true;
    __int128 ER = SR == INF ? (__int128)INT64_MAX * 4 : (__int128)KR + SR;
    __int128 ES = SS == INF ? (__int128)INT64_MAX * 4 : (__int128)KS + SS;
    return (__int128)KS < ER && (__int128)KR < ES;
  }
  static int readMode(int64_t KR, uint64_t SR, int64_t KS) {
    if (SR < 8) return MAny;  // part of a pointer: provenance kept, offset lost
    if (KR == ANY || KS == ANY) return MCopy;
    return ((KR - KS) % 8 == 0) ? MCopy : MAny;
  }
  uint32_t slot(uint32_t O, int64_t K, uint64_t Size) {
    Obj &Ob = Objs[O];
    if (K != ANY && Ob.Collapsed) K = ANY;
    auto It = Ob.Slots.find(K);
    bool New = It == Ob.Slots.end();
    uint32_t N;
    uint64_t OldSize = 0;
    if (New) {
      N = newNode();
      SlotOf[N] = {O, K};
      Objs[O].Slots[K] = {N, Size};
    } else {
      N = It->second.Node;
      OldSize = It->second.Size;
      if (Size <= OldSize) return N;
      It->second.Size = Size;
    }
    // connect to the readers that now overlap
    std::vector<Reader> Rs = Objs[O].Readers;
    for (auto &R : Rs)
      if (overlap(R.K, R.S, K, Size) && (New || !overlap(R.K, R.S, K, OldSize))) addEdge(N, R.Dst, readMode(R.K, R.S, K));
    return N;
  }
  std::set<std::tuple<uint32_t, int64_t, uint64_t, uint32_t>> ReaderSet;
  void addReader(uint32_t O, int64_t K, uint64_t S, uint32_t Dst) {
    Obj &Ob = Objs[O];
    if (K != ANY && Ob.Collapsed) K = ANY;
    if (!ReaderSet.insert({O, K, S, Dst}).second) return;
    Ob.Readers.push_back({K, S, Dst});
    std::vector<std::pair<int64_t, Slot>> Ss(Ob.Slots.begin(), Ob.Slots.end());
    for (auto &[KS, Sl] : Ss)
      if (overlap(K, S, KS, Sl.Size)) addEdge(Sl.Node, Dst, readMode(K, S, KS));
  }

  // ----------------------------------------------------------------------------------------- escape
  std::vector<const Function *> EscapedFuncs;
  std::vector<uint32_t> ExtCallCons;  // call constraints whose callee may be EXT
  size_t ExtConnections = 0;
  DenseSet<std::pair<const Function *, uint32_t>> Connected;

  void escapeObj(uint32_t O) {
    if (Objs[O].Escaped) return;
    Objs[O].Escaped = true;
    if (Objs[O].Kind == OUnk || Objs[O].Kind == ONoProv) {
      // library code got a pointer of unknown provenance: it may point into any exposed object
      ForgedEscaped = true;
      for (uint32_t X : std::vector<uint32_t>(ExposedObjs)) escapeObj(X);
      return;
    }
    if (getenv("FX_PTA_TRACE")) {
      std::string N = Objs[O].Kind == OExt ? "EXT" : Objs[O].V ? Objs[O].V->getName().str() : "?";
      if (N.empty() && Objs[O].V) { raw_string_ostream OS(N); Objs[O].V->printAsOperand(OS, false); }
      if (auto *In = dyn_cast_or_null<Instruction>(Objs[O].V)) N += " in " + In->getFunction()->getName().str();
      errs() << "escape " << N << "\n";
    }
    // what the object holds escapes too, now and later
    addReader(O, ANY, INF, ESC);
    // library code may write any escaped pointer, or its own pointers, into it: EXT stands for both
    uint32_t S = slot(O, ANY, INF);
    addElem(S, elem(ExtObj, ANY));
    if (Objs[O].Kind == OFunc) {
      const Function *F = cast<Function>(Objs[O].V);
      EscapedFuncs.push_back(F);
      if (!F->isDeclaration()) {
        for (const Argument &A : F->args()) {
          uint32_t N = node(&A);
          if (Nodes[N].Narrow) continue;  // library code cannot pass a pointer in fewer than 64 bits
          addElem(N, elem(ExtObj, ANY));
        }
        addEdge(retNode(F), ESC);
        addElem(vaNode(F), elem(ExtObj, ANY));
      }
      for (uint32_t C : std::vector<uint32_t>(ExtCallCons)) connectCall(F, C, 2);
    }
    // library code may reveal the address (print it, hand it back as an integer): exposed
    expose(O);
  }

  // ---------------------------------------------------------------------------------- value nodes
  DenseMap<const Constant *, uint32_t> ConstNode;

  // Can the bits of this integer constant be (part of) the address of an object? Zero cannot; an 8-byte
  // value below 65536 (vm.mmap_min_addr) or above 2^57 (beyond any user address, 5-level paging
  // included) cannot. Narrower values can be part of an address, unless zero.
  static bool addrLike(uint64_t V) { return V >= 65536 && V < (1ULL << 57); }
  bool constNoProv(const Constant *C) {
    if (auto *CI = dyn_cast<ConstantInt>(C)) {
      const APInt &A = CI->getValue();
      if (A.isZero()) return false;
      unsigned W = A.getBitWidth();
      if (W < 64) return true;
      for (unsigned B = 0; B + 64 <= W; B += 64)
        if (addrLike(A.extractBitsAsZExtValue(64, B))) return true;
      return W % 64 != 0;
    }
    if (auto *FP = dyn_cast<ConstantFP>(C)) {
      APInt A = FP->getValueAPF().bitcastToAPInt();
      if (A.isZero()) return false;
      if (A.getBitWidth() < 64) return true;
      return addrLike(A.extractBitsAsZExtValue(64, 0));
    }
    if (auto *CD = dyn_cast<ConstantDataSequential>(C)) {
      for (unsigned I = 0; I < CD->getNumElements(); I++)
        if (constNoProv(CD->getElementAsConstant(I))) return true;
      return false;
    }
    if (isa<ConstantAggregateZero>(C) || isa<ConstantPointerNull>(C) || isa<UndefValue>(C)) return false;
    if (auto *CA = dyn_cast<ConstantAggregate>(C)) {
      for (auto &Op : CA->operands())
        if (constNoProv(cast<Constant>(Op))) return true;
      return false;
    }
    return false;
  }

  void constPts(const Constant *C, SparseBitVector<> &Out, int Depth = 0) {
    if (Depth > 64) { Out.set(elem(UnkObj, ANY)); return; }
    if (isa<ConstantInt>(C) || isa<ConstantDataSequential>(C)) {
      if (constNoProv(C)) Out.set(elem(NoProvObj, ANY));
      return;
    }
    if (isa<ConstantPointerNull>(C) || isa<UndefValue>(C) || isa<ConstantFP>(C) || isa<ConstantAggregateZero>(C) ||
        isa<BlockAddress>(C) || isa<ConstantTokenNone>(C))
      return;
    if (auto *F = dyn_cast<Function>(C)) { Out.set(elem(objOf(F, OFunc), 0)); return; }
    if (auto *GA = dyn_cast<GlobalAlias>(C)) { constPts(GA->getAliasee(), Out, Depth + 1); return; }
    if (isa<GlobalIFunc>(C)) { Out.set(elem(UnkObj, ANY)); return; }
    if (auto *GV = dyn_cast<GlobalVariable>(C)) { Out.set(elem(objOf(GV, OGlobal), 0)); return; }
    if (auto *E = dyn_cast<DSOLocalEquivalent>(C)) { constPts(E->getGlobalValue(), Out, Depth + 1); return; }
    if (auto *E = dyn_cast<NoCFIValue>(C)) { constPts(E->getGlobalValue(), Out, Depth + 1); return; }
    if (auto *CE = dyn_cast<ConstantExpr>(C)) {
      if (auto *GEP = dyn_cast<GEPOperator>(CE)) {
        SparseBitVector<> B;
        constPts(cast<Constant>(GEP->getPointerOperand()), B, Depth + 1);
        APInt Off(DL.getIndexTypeSizeInBits(GEP->getType()), 0);
        bool Const = GEP->accumulateConstantOffset(DL, Off);
        for (unsigned E : B) {
          auto [O, K] = ElemOf[E];
          Out.set(elem(O, (!Const || K == ANY) ? ANY : K + Off.getSExtValue()));
        }
        return;
      }
      if (CE->isCast()) {
        if (CE->getOpcode() == Instruction::IntToPtr) {
          SparseBitVector<> B;
          constPts(CE->getOperand(0), B, Depth + 1);
          // a constant address in the first page is a null-pointer access (undefined behaviour): no object
          auto *CI = dyn_cast<ConstantInt>(CE->getOperand(0));
          bool NullPage = CI && CI->getValue().ult(4096);
          // provenance rule: an integer turned into a pointer is of unknown provenance, whatever
          // provenance the integer carries (a constant in the first page is a null-pointer access)
          if (!NullPage) Out.set(elem(UnkObj, ANY));
          Out |= B;
          return;
        }
        if (CE->getOpcode() == Instruction::PtrToInt) {
          SparseBitVector<> B;
          constPts(CE->getOperand(0), B, Depth + 1);
          for (unsigned E : B) addElem(EXPOSED, E);
          Out |= B;
          return;
        }
        constPts(CE->getOperand(0), Out, Depth + 1);
        return;
      }
      // arithmetic on addresses (relative vtables, tagged pointers): provenance kept, offset lost
      for (auto &Op : CE->operands()) {
        SparseBitVector<> B;
        constPts(cast<Constant>(Op), B, Depth + 1);
        for (unsigned E : B) Out.set(elem(ElemOf[E].first, ANY));
      }
      return;
    }
    if (isa<ConstantAggregate>(C)) {
      for (auto &Op : C->operands()) constPts(cast<Constant>(Op), Out, Depth + 1);
      return;
    }
    // anything else is not understood: unknown provenance
    Out.set(elem(UnkObj, ANY));
  }

  uint32_t node(const Value *V) {
    if (!V) return NONE;
    if (isa<MetadataAsValue>(V) || isa<InlineAsm>(V) || isa<BasicBlock>(V)) return NONE;
    if (auto *C = dyn_cast<Constant>(V)) {
      auto It = ConstNode.find(C);
      if (It != ConstNode.end()) return It->second;
      uint32_t N = newNode();
      ConstNode[C] = N;
      SparseBitVector<> P;
      constPts(C, P);
      if (!P.empty()) {
        Nodes[N].Pts |= P;
        push(N);
      }
      return N;
    }
    auto It = NodeOf.find(V);
    if (It != NodeOf.end()) return It->second;
    uint32_t N = newNode();
    NodeOf[V] = N;
    NodeVal[N] = V;
    setKind(N, V->getType());
    return N;
  }
  bool narrow(Type *T) {
    if (T->isIntegerTy() || T->isFloatingPointTy()) return T->getPrimitiveSizeInBits().getFixedValue() < 64;
    return false;
  }
  // No provenance: floating-point values (reading pointer storage through a floating-point lvalue breaks
  // strict aliasing, and LLVM moves pointer bytes as integers or pointer vectors) and integers of 2 to 63
  // bits (likewise; a byte-wise copy uses i8, which keeps provenance through loads and stores).
  bool noProv(Type *T) {
    if (T->isFloatingPointTy()) return true;
    if (T->isIntegerTy()) { unsigned W = T->getIntegerBitWidth(); return W != 8 && W < 64; }
    return false;
  }
  void setKind(uint32_t N, Type *T) {
    Nodes[N].Narrow = narrow(T);
    Nodes[N].NoProv = noProv(T);
  }
  // Context of constraint generation (0: the shared copy of every function; >0: a clone for one call site)
  uint32_t GenCtx = 0, NumCtx = 0;
  unsigned GenDepth = 0, MaxCloneDepth = 2, MaxCloneSize = 40;
  size_t CloneInsts = 0, MaxCloneInsts = 2000000;
  std::map<std::pair<const Value *, uint32_t>, uint32_t> CtxNode, CtxRet, CtxVa, CtxAlloca;
  uint32_t nodeC(const Value *V) {
    if (GenCtx == 0 || !V || isa<Constant>(V) || isa<MetadataAsValue>(V) || isa<InlineAsm>(V) || isa<BasicBlock>(V)) return node(V);
    auto Key = std::make_pair(V, GenCtx);
    auto It = CtxNode.find(Key);
    if (It != CtxNode.end()) return It->second;
    uint32_t N = newNode();
    setKind(N, V->getType());
    NodeVal[N] = V;
    return CtxNode[Key] = N;
  }
  uint32_t retNodeC(const Function *F) {
    if (GenCtx == 0) return retNode(F);
    auto Key = std::make_pair((const Value *)F, GenCtx);
    auto It = CtxRet.find(Key);
    if (It != CtxRet.end()) return It->second;
    uint32_t N = newNode();
    setKind(N, F->getReturnType());
    return CtxRet[Key] = N;
  }
  uint32_t vaNodeC(const Function *F) {
    if (GenCtx == 0) return vaNode(F);
    auto Key = std::make_pair((const Value *)F, GenCtx);
    auto It = CtxVa.find(Key);
    if (It != CtxVa.end()) return It->second;
    return CtxVa[Key] = newNode();
  }
  uint32_t traceIfWanted(uint32_t O, const AllocaInst *AI) {
    static const char *T = getenv("FX_TRACE_OBJ");
    if (T && AI->getFunction()->getName().contains(T)) TraceObjs.insert(O);
    return O;
  }
  uint32_t allocaObj(const AllocaInst *AI) {
    if (GenCtx == 0) return traceIfWanted(objOf(AI, OAlloca), AI);
    auto Key = std::make_pair((const Value *)AI, GenCtx);
    auto It = CtxAlloca.find(Key);
    if (It != CtxAlloca.end()) return It->second;
    return CtxAlloca[Key] = traceIfWanted(newObj(AI, OAlloca), AI);
  }
  uint32_t retNode(const Function *F) {
    auto It = RetOf.find(F);
    if (It != RetOf.end()) return It->second;
    uint32_t N = newNode();
    setKind(N, F->getReturnType());
    return RetOf[F] = N;
  }
  uint32_t vaNode(const Function *F) {
    auto It = VaOf.find(F);
    if (It != VaOf.end()) return It->second;
    return VaOf[F] = newNode();
  }
  uint32_t srcNode(uint32_t E) {
    uint32_t N = newNode();
    addElem(N, E);
    return N;
  }

  // ------------------------------------------------------------------------------- calls and models
  // Record: 1 = a target found by the analysis, 2 = an escaped function reached through an EXT callee,
  // 0 = a fallback connection (not reported as resolved).
  void connectCall(const Function *F, uint32_t CId, int Record = 1) {
    Con C = Cons[CId];  // copy: Cons may grow
    if (Record == 2 && C.CB && F->getFunctionType() != C.CB->getFunctionType()) return;  // wrong type: UB
    if (C.CB && Record == 1) SiteFuncs[C.CB].insert(const_cast<Function *>(F));
    if (C.CB && Record == 2) SiteExtFuncs[C.CB].insert(const_cast<Function *>(F));
    if (Record == 2) {
      // Through a library pointer: the arguments escape at this call and the result is EXT; an escaped
      // function's parameters already hold EXT (any escaped value) and its result escapes. Binding them
      // would add nothing the sets do not already stand for.
      ExtConnections++;
      return;
    }
    if (!Connected.insert({F, CId}).second) return;
    if (F->isDeclaration() || modeled(*F)) {
      applyModel(*F, C.Args, C.Ret, C.At ? C.At : (const Value *)F);
      return;
    }
    if (cut(F, C)) {
      // another component: exactly as an unknown library function with the default model (the callee's
      // parameters hold EXT already, and its result escapes)
      CutCalls++;
      for (uint32_t A : C.Args) if (A != NONE) addEdge(A, ESC);
      if (C.Ret != NONE) addElem(C.Ret, elem(ExtObj, ANY));
      return;
    }
    unsigned I = 0;
    for (const Argument &A : F->args()) {
      if (I >= C.Args.size()) break;
      addEdge(C.Args[I++], node(&A));
    }
    for (; I < C.Args.size(); I++) addEdge(C.Args[I], vaNode(F));
    if (C.Ret != NONE) addEdge(retNode(F), C.Ret);
  }

  void makeGeneric(uint32_t N) {
    N = nrep(N);
    if (Nodes[N].Generic) return;
    Nodes[N].Generic = true;
    bool Any = false;
    for (unsigned E : Nodes[N].Pts) if (Objs[ElemOf[E].first].Kind == ONoProv && ElemOf[E].first != NoProvObj) { Any = true; break; }
    if (!Any) return;
    SparseBitVector<> G;
    for (unsigned E : Nodes[N].Pts) G.set(Objs[ElemOf[E].first].Kind == ONoProv ? elem(NoProvObj, ANY) : E);
    Nodes[N].Pts = G;
    push(N);
  }
  void load(uint32_t Ptr, uint32_t Dst, uint64_t Size) {
    if (Ptr == NONE || Dst == NONE) return;
    makeGeneric(Dst);  // a value read from memory holds only generic NOPROV (memcpy gives bytes any type)
    Con C;
    C.Kind = CLoad;
    C.A = Dst;
    C.Size = Size;
    addCon(Ptr, C);
  }
  void store(uint32_t Ptr, uint32_t Src, uint64_t Size) {
    if (Ptr == NONE || Src == NONE) return;
    Con C;
    C.Kind = CStore;
    C.A = Src;
    C.Size = Size;
    C.At = CurInst;
    addCon(Ptr, C);
  }
  // the source node of bytes without provenance written by a store of TBAA type Ty (0: generic)
  DenseMap<uint32_t, uint32_t> NPSrcOf;
  uint32_t npSrc(uint32_t Ty) {
    if (!Ty) return NPSRC;
    auto It = NPSrcOf.find(Ty);
    if (It != NPSrcOf.end()) return It->second;
    uint32_t N = newNode();
    addElem(N, elem(noProvObj(Ty), ANY));
    SpecialNodes.insert(N);
    return NPSrcOf[Ty] = N;
  }
  DenseSet<uint32_t> SpecialNodes;
  uint32_t callCon(uint32_t Callee, const CallBase *CB, std::vector<uint32_t> Args, uint32_t Ret, const Value *At = nullptr) {
    Con C;
    C.Kind = CCall;
    C.CB = CB;
    C.At = At ? At : CB;
    C.Args = std::move(Args);
    C.Ret = Ret;
    return addCon(Callee, C);
  }

  std::set<std::string> ModelsUsed;
  // printf-like format: which conversions it may contain (a non-constant format: all of them)
  void formatConversions(const CallBase *CB, int Idx, bool &HasN, bool &HasP) {
    HasN = HasP = true;
    if (!CB || Idx < 0 || (unsigned)Idx >= CB->arg_size()) return;
    StringRef Str;
    if (!getConstantStringInfo(CB->getArgOperand(Idx), Str)) return;
    HasN = HasP = false;
    for (size_t I = 0; I < Str.size(); I++) {
      if (Str[I] != '%') continue;
      size_t J = I + 1;
      while (J < Str.size() && StringRef("-+ #0123456789.*'$hlLqjztI").contains(Str[J])) J++;
      if (J >= Str.size()) break;
      if (Str[J] == 'n') HasN = true;
      if (Str[J] == 'p') HasP = true;
      I = J;
    }
  }
  // A byte copy of Len bytes from Src to Dst. Short constant copies (a struct assignment) go in 8-byte
  // chunks, so that each field keeps its own slot: an integer and a pointer of the source stay apart in
  // the destination. Longer or variable ones copy everything in the range into the whole range.
  void copyBytes(uint32_t Dst, uint32_t Src, uint64_t Len) {
    if (Dst == NONE || Src == NONE) return;
    if (Len != INF && Len <= 512) {
      for (uint64_t Off = 0; Off < Len; Off += 8) {
        uint64_t N = std::min<uint64_t>(8, Len - Off);
        uint32_t S = Src, D = Dst;
        if (Off) {
          S = newNode();
          addEdge(Src, S, MShift, (int64_t)Off);
          D = newNode();
          addEdge(Dst, D, MShift, (int64_t)Off);
        }
        uint32_t T = newNode();
        load(S, T, N);
        store(D, T, N);
      }
      return;
    }
    uint32_t T = newNode();
    load(Src, T, Len);
    store(Dst, T, Len);
  }
  void applyModel(const Function &F, const std::vector<uint32_t> &Args, uint32_t Ret, const Value *Site) {
    PtrModel PM = ModelOf(F);
    auto arg = [&](size_t I) { return I < Args.size() ? Args[I] : NONE; };
    uint32_t ExtE = elem(ExtObj, ANY);
    const CallBase *CB = dyn_cast_or_null<CallBase>(Site);
    bool Effects = !PM.WritesBytes.empty() || !PM.Exposes.empty() || !PM.Sends.empty() || !PM.Permutes.empty();
    if (PM.K != PtrModel::Escape || Effects || !PM.Calls.empty()) ModelsUsed.insert(F.getName().str());
    // the variadic arguments (or the pointers a va_list holds)
    unsigned NFixed = F.getFunctionType()->getNumParams();
    auto varargs = [&]() {
      uint32_t VA = newNode();
      for (size_t J = NFixed; J < Args.size(); J++) addEdge(Args[J], VA);
      if (PM.VaList >= 0) {
        uint32_t T = newNode();
        load(arg(PM.VaList), T, INF);
        load(T, VA, INF);
      }
      return VA;
    };
    auto targets = [&](int Idx) {
      std::vector<uint32_t> R;
      if (Idx >= 0) { if (arg(Idx) != NONE) R.push_back(arg(Idx)); }
      else if (Idx == -1) R.push_back(varargs());
      else if (Idx <= -1000) {
        uint32_t B = arg((size_t)(-1000 - Idx));
        if (B == NONE) return R;
        uint32_t L1 = newNode(), L2 = newNode();
        load(B, L1, INF);
        load(L1, L2, INF);
        R = {B, L1, L2};
      }
      else {
        // every object reachable from the variadic arguments, two levels (an iovec's buffers)
        uint32_t VA = varargs(), L1 = newNode(), L2 = newNode();
        load(VA, L1, INF);
        load(L1, L2, INF);
        R = {VA, L1, L2};
      }
      return R;
    };
    bool HasN = true, HasP = true;
    if (PM.Format >= 0) formatConversions(CB, PM.Format, HasN, HasP);
    bool Fill = true;
    if (PM.FillValue >= 0 && CB && (unsigned)PM.FillValue < CB->arg_size())
      if (auto *V = dyn_cast<ConstantInt>(CB->getArgOperand(PM.FillValue))) Fill = !V->isZero();
    for (auto &[Idx, Sz] : PM.WritesBytes) {
      if (Idx < 0 && PM.Format >= 0 && !HasN) continue;  // %n is the only conversion that writes
      if (!Fill) continue;
      for (uint32_t T : targets(Idx)) { store(T, NPSRC, Sz ? Sz : INF); NoProvStores++; }
    }
    for (int Idx : PM.Exposes) {
      if (Idx < 0 && PM.Format >= 0 && !HasP) continue;  // %p is the only conversion that prints an address
      for (uint32_t T : targets(Idx)) addEdge(T, EXPOSED);
    }
    for (int Idx : PM.Sends)
      for (uint32_t T : targets(Idx)) {
        uint32_t C = newNode();
        load(T, C, INF);
        addEdge(C, EXPOSED);
      }
    for (int Idx : PM.Permutes) {
      uint32_t C = newNode(), P = newNode();
      load(arg(Idx), C, INF);
      addEdge(arg(Idx), P, MAny);
      store(P, C, INF);
    }
    // the result: an integer computed by the library has no provenance; a pointer the model does not
    // define is a library pointer
    Type *RT = F.getReturnType();
    if (Ret != NONE && !RT->isPointerTy() && PM.K != PtrModel::Escape) computed(Ret);
    if (Ret != NONE && RT->isPointerTy() && (PM.K == PtrModel::None || PM.K == PtrModel::Throw)) addElem(Ret, ExtE);
    switch (PM.K) {
    case PtrModel::Escape:
      for (uint32_t A : Args) addEdge(A, ESC);
      if (Ret != NONE) addElem(Ret, ExtE);
      break;
    case PtrModel::None:
      break;
    case PtrModel::Fresh:
      if (Ret != NONE) {
        addElem(Ret, elem(heapOf(Site), 0));
        if (PM.FreshUnknown) store(srcNode(elem(heapOf(Site), 0)), NPSRC, INF);
      }
      break;
    case PtrModel::Realloc: {
      if (Ret == NONE) break;
      uint32_t H = srcNode(elem(heapOf(Site), 0));
      uint32_t T = newNode();
      load(arg(0), T, INF);
      store(H, T, INF);
      addEdge(H, Ret);
      addEdge(arg(0), Ret);
      break;
    }
    case PtrModel::RetArg0:
      if (Ret != NONE) addEdge(arg(PM.RetArg), Ret, MAny);
      break;
    case PtrModel::Copy: {
      uint64_t Len = INF;
      if (CB && CB->arg_size() > 2 && (F.getName().contains("mem")))
        if (auto *LC = dyn_cast<ConstantInt>(CB->getArgOperand(2))) Len = LC->getZExtValue();
      copyBytes(arg(0), arg(1), Len);
      if (Ret != NONE) addEdge(arg(0), Ret, MAny);  // stpcpy, mempcpy: a pointer into argument 0
      break;
    }
    case PtrModel::Data: {
      // the fresh buffers of container internals are the shared DATA object when there is one (it keeps a
      // real slot: pointer bytes copied into any buffer stay visible from every buffer)
      uint32_t FreshObj = DataObj != UINT32_MAX ? DataObj : heapOf(Site, 1);
      if (PM.Self) {
        uint32_t Fresh = elem(FreshObj, ANY);
        Con SC;
        SC.Kind = CSelf;
        SC.A = Fresh;
        SC.Size = PM.Size ? PM.Size : INF;
        if (arg(0) != NONE) addCon(arg(0), SC);
        // The characters it copies (from another string, a char pointer, or its own old buffer) land in
        // the fresh buffer: pointer bytes stored in a string survive any copy the library makes.
        uint32_t Src = newNode(), Bytes = newNode(), FreshPtr = srcNode(Fresh);
        Nodes[Src].DropNP = true;  // the string's own pointer member (the length and capacity are integers)
        for (size_t J = 0; J < Args.size(); J++) {
          if (J > 0) addEdge(Args[J], Src);
          load(Args[J], Src, SC.Size);
        }
        load(Src, Bytes, INF);
        // the characters may also be computed (to_string, append(n, c)): bytes without provenance
        computed(Bytes);
        for (size_t J = 1; J < Args.size(); J++) addEdge(Args[J], Bytes);  // a character argument
        store(FreshPtr, Bytes, INF);
        if (PM.InlineSize) {
          // and into the object's own character buffer (std::string's small-string buffer), only there
          uint32_t T = newNode();
          addEdge(arg(0), T, MShift, (int64_t)PM.InlineOffset);
          store(T, Bytes, PM.InlineSize);
        }
        if (Ret != NONE) {
          // a pointer result is the string's own pointer or a fresh buffer, never its length or characters
          uint32_t R = Ret;
          if (F.getReturnType()->isPointerTy()) { R = newNode(); Nodes[R].DropNP = true; addEdge(R, Ret); }
          addEdge(arg(0), R);
          load(arg(0), R, SC.Size);
          addElem(R, Fresh);
        }
        break;
      }
      // Container internals (std::string members, red-black tree and list node links): the function reads
      // and writes pointers only within the first Size bytes of the objects it is given (and, with Closure,
      // of the nodes linked from them), and stores only those pointers or fresh heap pointers.
      uint64_t N = PM.Size ? PM.Size : INF;
      uint32_t C = newNode();
      Nodes[C].DropNP = true;  // the link pointers (a colour or a count in the header is an integer)
      addElem(C, elem(FreshObj, ANY));
      auto sel = [&](const std::vector<int> &L) {
        std::vector<uint32_t> R;
        if (L.empty()) { for (uint32_t A : Args) R.push_back(A); }
        else for (int I : L) R.push_back(arg(I));
        return R;
      };
      for (uint32_t A : sel(PM.Reads)) { addEdge(A, C); load(A, C, N); }
      if (PM.Closure) load(C, C, N);
      for (uint32_t A : sel(PM.Writes)) store(A, C, N);
      if (PM.WriteReached) store(C, C, N);
      if (Ret != NONE) addEdge(C, Ret);
      break;
    }
    case PtrModel::RetExt:
      if (Ret != NONE) addElem(Ret, ExtE);
      break;
    case PtrModel::Throw:
      addEdge(arg(0), THROWN);
      break;
    case PtrModel::Catch:
      if (Ret != NONE) { addEdge(THROWN, Ret, MAny); addElem(Ret, ExtE); }
      break;
    }
    for (auto &[Fi, ArgIdx] : PM.Calls) {
      // a library callback may get a pointer to any element of what it was given (qsort's comparator)
      std::vector<uint32_t> CA;
      for (int J : ArgIdx) {
        uint32_t T = newNode();
        addEdge(arg(J), T, MAnyKeepFunc);
        CA.push_back(T);
      }
      callCon(arg(Fi), nullptr, CA, NONE, Site);
    }
  }

  // ------------------------------------------------------------------------------- processing
  DenseMap<const CallBase *, SetVector<Function *>> SiteFuncs, SiteExtFuncs;
  DenseSet<const CallBase *> SiteExt, SiteUnk, IndirectSites;
  DenseSet<const CallBase *> FallbackDone;

  void connectFallback(uint32_t CId) {
    const CallBase *CB = Cons[CId].CB;
    if (!CB || !Fallback || !FallbackDone.insert(CB).second) return;
    for (Function *F : Fallback(*CB)) connectCall(F, CId, 0);
  }

  void processCon(uint32_t CId, unsigned E) {
    auto [O, K] = ElemOf[E];
    ConKind Kind = Cons[CId].Kind;
    if (Kind == CLoad) {
      uint32_t Dst = Cons[CId].A;
      uint64_t Size = Cons[CId].Size;
      if (unkLike(O)) {
        // A load through a pointer of unknown provenance gives UNK: whatever an exposed object holds is
        // then represented by UNK, and every call whose callee may be UNK falls back to the rta rule.
        addElem(Dst, elem(UnkObj, ANY));
        return;
      }
      if (Objs[O].Kind == OExt) { addElem(Dst, elem(ExtObj, ANY)); return; }  // EXT holds EXT
      addReader(O, K, Size, Dst);
    } else if (Kind == CSelf) {
      ObjKind OK = Objs[O].Kind;
      if (OK == OExt || OK == OUnk || OK == ONoProv || OK == OFunc) return;
      uint32_t S = slot(O, K, Cons[CId].Size);
      addElem(S, elem(O, ANY));
      addElem(S, Cons[CId].A);
    } else if (Kind == CStore) {
      uint32_t Src = Cons[CId].A;
      if (unkLike(O)) {
        // a store through a pointer of unknown provenance: into every exposed object (the hub), and
        // what is stored escapes
        static int DbgN = getenv("FX_TRACE_UNKSTORE") ? 30 : 0;
        if (DbgN > 0 && UnkStoreSet.insert(Src).second) { DbgN--; errs() << "store through " << objName(O) << " of " << nodeDesc(Src) << " at ";
          if (auto *In = dyn_cast_or_null<Instruction>(Cons[CId].At)) { std::string W; raw_string_ostream OS(W); OS << In->getFunction()->getName().substr(0, 80) << ": "; In->print(OS); errs() << W.substr(0, 300); }
          errs() << "\n"; }
        addEdge(Src, ESC);
        addEdge(Src, UNKSTORE);
        return;
      }
      if (Objs[O].Kind == OFunc) return;  // writing into code: undefined behaviour
      // writing into library memory: the value escapes (EXT stands for it from now on)
      if (Objs[O].Kind == OExt) { addEdge(Src, ESC); return; }
      // writing into a constant global: undefined behaviour
      if (auto *GV = dyn_cast_or_null<GlobalVariable>(Objs[O].V))
        if (GV->isConstant()) return;
      addEdge(Src, slot(O, K, Cons[CId].Size));
    } else {
      const CallBase *CB = Cons[CId].CB;
      ObjKind OK = Objs[O].Kind;
      if (OK == OFunc) {
        connectCall(cast<Function>(Objs[O].V), CId);
      } else if (OK == OExt) {
        if (CB) SiteExt.insert(CB);
        ExtCallCons.push_back(CId);
        for (const Function *F : std::vector<const Function *>(EscapedFuncs)) connectCall(F, CId, 2);
        // a library pointer may be library code: model the call as an unknown library call
        for (uint32_t A : Cons[CId].Args) addEdge(A, ESC);
        if (Cons[CId].Ret != NONE) addElem(Cons[CId].Ret, elem(ExtObj, ANY));
      } else if (OK == OUnk || OK == ONoProv) {
        if (CB) SiteUnk.insert(CB);
        connectFallback(CId);
        for (uint32_t A : Cons[CId].Args) addEdge(A, ESC);
        if (Cons[CId].Ret != NONE) addElem(Cons[CId].Ret, elem(ExtObj, ANY));
      }
      // a data object as callee: calling it is undefined behaviour
    }
  }

  double BudgetS = 0;
  bool OverBudget = false;
  std::chrono::steady_clock::time_point Start = std::chrono::steady_clock::now();
  void run() {
    bool Prof = getenv("FX_PROFILE") != nullptr;
    while (!Work.empty()) {
      uint32_t N = Work.front();
      Work.pop_front();
      Nodes[N].Queued = false;
      if (nrep(N) != N) continue;  // merged into another node
      Iterations++;
      if (BudgetS > 0 && (Iterations & 0xFF) == 0 &&
          std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count() > BudgetS) {
        OverBudget = true;  // checked often: one iteration can take seconds on a large module
        Work.clear();
        return;
      }
      if ((Iterations & (Prof ? 0x3FFF : 0xFFFFF)) == 0) {
        double T = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
        if (Prof) {
          size_t PtsSum = 0, Big = 0;
          for (auto &Nd : Nodes) { size_t C = Nd.Pts.count(); PtsSum += C; if (C > 1000) Big++; }
          errs() << "pta: " << (int)T << " s, " << Iterations << " iterations, work " << Work.size() << ", nodes " << Nodes.size()
                 << ", elems " << ElemOf.size() << ", edges " << EdgeSet.size() << ", pts total " << PtsSum << ", nodes>1000 " << Big
                 << ", readers " << ReaderSet.size() << ", unkstore " << Nodes[UNKSTORE].Pts.count() << ", esc " << Nodes[ESC].Pts.count()
                 << ", thrown " << Nodes[THROWN].Pts.count() << ", collapsed " << Collapsed << ", merged " << Merged << ", inttoptr " << IntToPtr
                 << ", ext-callee calls " << ExtCallCons.size() << ", escaped functions " << EscapedFuncs.size() << ", ext connections " << ExtConnections << "\n";
          dumpBig();
          if (!TraceObjs.empty()) {
            uint32_t Best = 0; size_t BS = 0;
            for (uint32_t i = 0; i < Nodes.size(); i++) if (i != ESC && Nodes[i].Pts.count() > BS && TraceParent.count(i)) { BS = Nodes[i].Pts.count(); Best = i; }
            traceBack(Best);
          }
        }
        if (BudgetS > 0 && T > BudgetS) { OverBudget = true; Work.clear(); return; }
      }
      SparseBitVector<> Diff = Nodes[N].Pts;
      Diff.intersectWithComplement(Nodes[N].Prev);
      if (Diff.empty()) continue;
      Nodes[N].Prev |= Diff;
      if (N == ESC)
        for (unsigned E : Diff) escapeObj(ElemOf[E].first);
      if (N == EXPOSED)
        for (unsigned E : Diff) expose(ElemOf[E].first);
      for (size_t i = 0; i < Nodes[N].Out.size(); i++) {
        Edge Ed = Nodes[N].Out[i];
        uint32_t T = nrep(Ed.To);
        if (T == N && Ed.Mode == MCopy) continue;
        traceEdge(N, T, Diff);
        if (unionInto(Diff, T, Ed.Mode, Ed.C)) push(T);
        // lazy cycle detection: equal sets across a copy edge suggest a cycle
        if (UseCycles && Ed.Mode == MCopy && Nodes[T].Pts == Nodes[N].Pts && CycleChecked.insert({N, T}).second)
          PendingCycles.push_back({N, T});
      }
      auto T0 = std::chrono::steady_clock::now();
      size_t NC = Nodes[N].Cons.size();
      for (size_t i = 0; i < NC; i++) {
        uint32_t C = Nodes[N].Cons[i];
        for (unsigned E : Diff) processCon(C, E);
      }
      for (auto [A, B] : PendingCycles) collapseCycle(A, B);
      PendingCycles.clear();
      size_t Cs = NC;
      if (Prof) {
        double D = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
        if (D > 0.5)
          errs() << "pta: slow node " << N << " diff " << Diff.count() << " cons " << Cs << " out " << Nodes[N].Out.size() << " took " << D
                 << " s; readers " << ReaderSet.size() << " edges " << EdgeSet.size() << "\n";
      }
    }
  }

  // --------------------------------------------------------------------------------- constraints
  uint64_t sizeOf(Type *T) {
    if (!T->isSized()) return INF;
    TypeSize TS = DL.getTypeStoreSize(T);
    if (TS.isScalable()) return INF;
    return TS.getFixedValue();
  }

  void initGlobal(GlobalVariable &GV) {
    uint32_t O = objOf(&GV, OGlobal);
    bool Escapes = GV.isDeclaration() || GV.hasExternalWeakLinkage() || GV.hasWeakAnyLinkage() || GV.hasCommonLinkage() ||
                   GV.isInterposable();
    bool PICnotPIE = M.getPICLevel() != PICLevel::NotPIC && M.getPIELevel() == PIELevel::Default;
    if (PICnotPIE && !GV.hasLocalLinkage() && GV.hasDefaultVisibility() && !GV.isConstant()) Escapes = true;
    if (Component && !GV.isConstant()) Escapes = true;  // component mode: other components read and write it
    if (Escapes) addElem(ESC, elem(O, 0));
    if (GV.hasInitializer() && !GV.isDeclaration()) walkInit(O, GV.getInitializer(), 0);
  }
  void walkInit(uint32_t O, const Constant *C, int64_t Off) {
    if (isa<ConstantAggregateZero>(C) || isa<ConstantPointerNull>(C) || isa<UndefValue>(C)) return;
    if (isa<ConstantDataSequential>(C) || isa<ConstantInt>(C) || isa<ConstantFP>(C)) {
      // provenance rule: initial bytes that may be part of an address
      if (constNoProv(C)) addElem(slot(O, Off, sizeOf(C->getType())), elem(NoProvObj, ANY));
      return;
    }
    if (auto *CS = dyn_cast<ConstantStruct>(C)) {
      const StructLayout *SL = DL.getStructLayout(CS->getType());
      for (unsigned I = 0; I < CS->getNumOperands(); I++)
        walkInit(O, CS->getOperand(I), Off + (int64_t)SL->getElementOffset(I).getFixedValue());
      return;
    }
    if (isa<ConstantArray>(C) || isa<ConstantVector>(C)) {
      Type *ET = isa<ConstantArray>(C) ? cast<ArrayType>(C->getType())->getElementType()
                                       : cast<VectorType>(C->getType())->getElementType();
      int64_t ES = (int64_t)DL.getTypeAllocSize(ET).getFixedValue();
      for (unsigned I = 0; I < C->getNumOperands(); I++) walkInit(O, cast<Constant>(C->getOperand(I)), Off + I * ES);
      return;
    }
    SparseBitVector<> P;
    constPts(C, P);
    if (P.empty()) return;
    uint32_t S = slot(O, Off, sizeOf(C->getType()));
    if ((Nodes[S].Pts |= P)) push(S);
  }

  DenseMap<const Function *, bool> ModeledCache;
  bool modeled(const Function &F) {
    auto It = ModeledCache.find(&F);
    if (It != ModeledCache.end()) return It->second;
    return ModeledCache[&F] = !F.isDeclaration() && ModelOf(F).Bodies;
  }
  void genFunction(Function &F) {
    if (modeled(F)) return;  // replaced by its pointer model
    GenCtx = 0;
    GenDepth = 0;
    genFunctionBody(F);
  }
  void genFunctionBody(Function &F) {
    for (Instruction &I : instructions(F)) genInst(F, I);
  }
  bool shouldClone(Function &Fn) {
    if (Fn.isDeclaration() || modeled(Fn) || Fn.isVarArg()) return false;
    if (GenDepth >= MaxCloneDepth || CloneInsts > MaxCloneInsts) return false;
    if (Fn.getInstructionCount() > MaxCloneSize) return false;
    for (const Instruction &I : instructions(Fn))  // directly recursive: no
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (CB->getCalledOperand()->stripPointerCasts() == &Fn) return false;
    return true;
  }

  void anyFrom(Instruction &I) {
    uint32_t N = nodeC(&I);
    for (auto &Op : I.operands()) addEdge(nodeC(Op), N, MAny);
  }
  // A computed integer: its bits are not a copy of a pointer (provenance rule), whatever provenance
  // its operands give it.
  void computed(uint32_t N) { if (N != NONE) addElem(N, elem(NoProvObj, ANY)); }
  // Does a value of this type hold bits that the analysis does not track (no provenance by type)?
  bool hasNoProvPart(Type *T) {
    if (noProv(T)) return true;
    if (auto *VT = dyn_cast<VectorType>(T)) return hasNoProvPart(VT->getElementType());
    if (auto *AT = dyn_cast<ArrayType>(T)) return hasNoProvPart(AT->getElementType());
    if (auto *ST = dyn_cast<StructType>(T)) {
      for (Type *E : ST->elements()) if (hasNoProvPart(E)) return true;
      return false;
    }
    return false;
  }
  size_t NoProvStores = 0;
  // Provenance rule, at a store: bits of a type that carries no provenance (a 32-bit integer, a
  // floating-point value) other than zero make the bytes written "no provenance".
  void storeValue(uint32_t Ptr, Value *V, uint64_t Size, uint32_t Tag = 0) {
    Type *T = V->getType();
    if (hasNoProvPart(T)) {
      auto *C = dyn_cast<Constant>(V);
      if (!(C && (C->isNullValue() || isa<UndefValue>(C) || !constNoProv(C)))) {
        store(Ptr, npSrc(Tag), Size);
        NoProvStores++;
      }
      if (noProv(T)) return;
    }
    uint32_t Src = nodeC(V);
    if (Tag && !isa<Constant>(V)) {
      // the bytes are written as the store's TBAA type
      uint32_t R = newNode();
      Nodes[R].RetagTo = Tag;
      addEdge(Src, R);
      Src = R;
    }
    store(Ptr, Src, Size);
  }

  const Value *CurInst = nullptr;  // debugging: the instruction whose constraints are being generated
  void genInst(Function &F, Instruction &I) {
    CurInst = &I;
    if (auto *AI = dyn_cast<AllocaInst>(&I)) { addElem(nodeC(AI), elem(allocaObj(AI), 0)); return; }
    if (auto *L = dyn_cast<LoadInst>(&I)) {
      Nodes[nodeC(L)].LoadTag = tbaaType(*L);
      Nodes[nodeC(L)].Generic = true;
      load(nodeC(L->getPointerOperand()), nodeC(L), sizeOf(L->getType()));
      return;
    }
    if (auto *S = dyn_cast<StoreInst>(&I)) {
      storeValue(nodeC(S->getPointerOperand()), S->getValueOperand(), sizeOf(S->getValueOperand()->getType()), tbaaType(*S));
      return;
    }
    if (auto *G = dyn_cast<GetElementPtrInst>(&I)) {
      APInt Off(DL.getIndexTypeSizeInBits(G->getType()), 0);
      if (!G->getType()->isVectorTy() && G->accumulateConstantOffset(DL, Off))
        addEdge(nodeC(G->getPointerOperand()), nodeC(G), MShift, Off.getSExtValue());
      else
        addEdge(nodeC(G->getPointerOperand()), nodeC(G), MAny);
      return;
    }
    if (auto *C = dyn_cast<CastInst>(&I)) {
      unsigned Op = C->getOpcode();
      bool Copy = Op == Instruction::BitCast || Op == Instruction::AddrSpaceCast || Op == Instruction::PtrToInt ||
                  Op == Instruction::IntToPtr;
      addEdge(nodeC(C->getOperand(0)), nodeC(C), Copy ? MCopy : MAny);
      if (!Copy) computed(nodeC(C));  // widened, narrowed or converted: not a copy of a pointer
      if (Op == Instruction::PtrToInt) addEdge(nodeC(C->getOperand(0)), EXPOSED);
      if (Op == Instruction::IntToPtr) {
        IntToPtr++;
        // provenance rule: a pointer made from an integer is of unknown provenance (and keeps whatever
        // provenance the integer carries)
        addElem(nodeC(C), elem(UnkObj, ANY));
        IntToPtrNodes.push_back(nodeC(C));
      }
      return;
    }
    if (auto *P = dyn_cast<PHINode>(&I)) {
      for (Value *V : P->incoming_values()) addEdge(nodeC(V), nodeC(P));
      return;
    }
    if (auto *S = dyn_cast<SelectInst>(&I)) {
      addEdge(nodeC(S->getTrueValue()), nodeC(S));
      addEdge(nodeC(S->getFalseValue()), nodeC(S));
      return;
    }
    if (isa<ExtractValueInst>(I) || isa<InsertValueInst>(I) || isa<ExtractElementInst>(I) || isa<InsertElementInst>(I) ||
        isa<ShuffleVectorInst>(I) || isa<FreezeInst>(I)) {
      uint32_t N = nodeC(&I);
      for (auto &Op : I.operands()) addEdge(nodeC(Op), N);
      return;
    }
    if (auto *A = dyn_cast<AtomicRMWInst>(&I)) {
      load(nodeC(A->getPointerOperand()), nodeC(A), sizeOf(A->getType()));
      uint32_t V = nodeC(A->getValOperand());
      if (A->getOperation() == AtomicRMWInst::Xchg) {
        store(nodeC(A->getPointerOperand()), V, sizeOf(A->getType()));
      } else {
        uint32_t T = newNode();
        addEdge(V, T, MAny);
        addEdge(nodeC(A), T, MAny);
        computed(T);
        if (hasNoProvPart(A->getType())) store(nodeC(A->getPointerOperand()), NPSRC, sizeOf(A->getType()));
        store(nodeC(A->getPointerOperand()), T, sizeOf(A->getType()));
      }
      return;
    }
    if (auto *X = dyn_cast<AtomicCmpXchgInst>(&I)) {
      load(nodeC(X->getPointerOperand()), nodeC(X), sizeOf(X->getNewValOperand()->getType()));
      storeValue(nodeC(X->getPointerOperand()), X->getNewValOperand(), sizeOf(X->getNewValOperand()->getType()));
      return;
    }
    if (isa<LandingPadInst>(I) || isa<CatchPadInst>(I) || isa<CleanupPadInst>(I)) {
      addEdge(THROWN, nodeC(&I), MAny);
      addElem(nodeC(&I), elem(ExtObj, ANY));
      return;
    }
    if (isa<VAArgInst>(I)) {
      addEdge(vaNodeC(&F), nodeC(&I));
      addElem(nodeC(&I), elem(ExtObj, ANY));
      return;
    }
    if (auto *R = dyn_cast<ReturnInst>(&I)) {
      if (R->getReturnValue()) addEdge(nodeC(R->getReturnValue()), retNodeC(&F));
      return;
    }
    if (auto *CB = dyn_cast<CallBase>(&I)) { genCall(F, *CB); return; }
    if (isa<CmpInst>(I) || I.isTerminator() || I.getType()->isVoidTy() || isa<FenceInst>(I)) return;
    if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      // provenance rule: the result of integer arithmetic is not a copy of a pointer
      computed(nodeC(BO));
      if (BO->getType()->isIntOrIntVectorTy(8)) return;  // byte arithmetic (checksums): no pointer operand
      switch (BO->getOpcode()) {
      case Instruction::Add:
      case Instruction::Or:
      case Instruction::And:
      case Instruction::Xor:
        anyFrom(I);  // offsets and tags: provenance of both operands
        return;
      case Instruction::Sub:
        addEdge(nodeC(BO->getOperand(0)), nodeC(BO), MAny);  // p - c, or p - q (a distance): p's provenance
        return;
      default:
        // multiplication, division, remainder, shifts, floating-point arithmetic: a scaled or shifted
        // address is not a pointer; it becomes one again only added to a base pointer
        return;
      }
    }
    anyFrom(I);  // everything else: provenance of every operand, offset lost, and computed
    computed(nodeC(&I));
  }

  void genCall(Function &F, CallBase &CB) {
    std::vector<uint32_t> Args;
    for (Value *A : CB.args()) Args.push_back(nodeC(A));
    uint32_t Ret = CB.getType()->isVoidTy() ? NONE : nodeC(&CB);
    Value *Callee = CB.getCalledOperand();
    if (CB.isInlineAsm()) {
      for (uint32_t A : Args) addEdge(A, ESC);
      if (Ret != NONE) { addElem(Ret, elem(ExtObj, ANY)); computed(Ret); }
      return;
    }
    Value *S = Callee->stripPointerCasts();
    if (auto *GA = dyn_cast<GlobalAlias>(S)) S = const_cast<GlobalObject *>(GA->getAliaseeObject());
    if (auto *Fn = dyn_cast_or_null<Function>(S)) {
      if (Fn->isIntrinsic()) { genIntrinsic(F, CB, Fn->getIntrinsicID(), Args, Ret); return; }
      if (Fn->isDeclaration() && getenv("FX_PROFILE") && ModelOf(*Fn).K == PtrModel::Escape) EscapeCalls[Fn->getName().str()]++;
      if (shouldClone(*Fn)) {
        // Context sensitivity for small functions: this call site gets its own copy of the callee's
        // constraints (parameters, locals, return value); the shared copy keeps the indirect callers.
        uint32_t Saved = GenCtx;
        unsigned SavedDepth = GenDepth;
        GenCtx = ++NumCtx;
        GenDepth++;
        CloneInsts += Fn->getInstructionCount();
        genFunctionBody(*Fn);
        unsigned I = 0;
        for (const Argument &A : Fn->args()) {
          if (I >= Args.size()) break;
          addEdge(Args[I++], nodeC(&A));
        }
        for (; I < Args.size(); I++) addEdge(Args[I], vaNodeC(Fn));
        uint32_t R = retNodeC(Fn);
        GenCtx = Saved;
        GenDepth = SavedDepth;
        if (Ret != NONE) addEdge(R, Ret);
        return;
      }
      uint32_t CId = callCon(srcNode(elem(objOf(Fn, OFunc), 0)), nullptr, Args, Ret, &CB);
      connectCall(Fn, CId, 0);  // now, so that the pre-pass sees the callee's (or the model's) constraints
      return;
    }
    IndirectSites.insert(&CB);
    callCon(nodeC(Callee), &CB, Args, Ret, &CB);
  }

  void genIntrinsic(Function &F, CallBase &CB, Intrinsic::ID ID, std::vector<uint32_t> &Args, uint32_t Ret) {
    auto arg = [&](size_t I) { return I < Args.size() ? Args[I] : NONE; };
    auto constLen = [&](size_t I) -> uint64_t {
      if (I < CB.arg_size())
        if (auto *C = dyn_cast<ConstantInt>(CB.getArgOperand(I))) return C->getZExtValue();
      return INF;
    };
    switch (ID) {
    case Intrinsic::memcpy:
    case Intrinsic::memcpy_inline:
    case Intrinsic::memmove:
    case Intrinsic::memcpy_element_unordered_atomic:
    case Intrinsic::memmove_element_unordered_atomic: {
      uint64_t Len = constLen(2);
      copyBytes(arg(0), arg(1), Len);
      return;
    }
    case Intrinsic::memset:
    case Intrinsic::memset_inline:
    case Intrinsic::memset_element_unordered_atomic: {
      // filling with a byte other than a constant 0 writes bytes without provenance
      auto *V = dyn_cast<ConstantInt>(CB.getArgOperand(1));
      if (!(V && V->isZero())) { store(arg(0), NPSRC, constLen(2)); NoProvStores++; }
      return;
    }
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
    case Intrinsic::assume:
    case Intrinsic::type_test:
    case Intrinsic::public_type_test:
    case Intrinsic::invariant_start:
    case Intrinsic::invariant_end:
    case Intrinsic::donothing:
    case Intrinsic::sideeffect:
    case Intrinsic::prefetch:
    case Intrinsic::stackrestore:
    case Intrinsic::experimental_noalias_scope_decl:
    case Intrinsic::vaend:
      return;
    case Intrinsic::stacksave:
      return;
    case Intrinsic::threadlocal_address:
    case Intrinsic::launder_invariant_group:
    case Intrinsic::strip_invariant_group:
    case Intrinsic::ssa_copy:
    case Intrinsic::expect:
    case Intrinsic::expect_with_probability:
      if (Ret != NONE) addEdge(arg(0), Ret);
      return;
    case Intrinsic::ptrmask:
      if (Ret != NONE) addEdge(arg(0), Ret, MAny);
      return;
    case Intrinsic::type_checked_load:
    case Intrinsic::type_checked_load_relative: {
      uint32_t T = newNode();
      if (auto *C = dyn_cast<ConstantInt>(CB.getArgOperand(1)))
        addEdge(arg(0), T, MShift, C->getSExtValue());
      else
        addEdge(arg(0), T, MAny);
      if (ID == Intrinsic::type_checked_load) load(T, Ret, 8);
      else addElem(Ret, elem(UnkObj, ANY));
      return;
    }
    case Intrinsic::load_relative:
      if (Ret != NONE) addElem(Ret, elem(UnkObj, ANY));
      return;
    case Intrinsic::trap:
    case Intrinsic::ubsantrap:
    case Intrinsic::debugtrap:
      return;
    case Intrinsic::vastart: {
      auto It = VaObjOf.find(&F);
      uint32_t O;
      if (It == VaObjOf.end()) {
        O = newObj(&F, OVa);
        VaObjOf[&F] = O;
        uint32_t S = slot(O, ANY, INF);
        addEdge(vaNodeC(&F), S);
        addElem(S, elem(ExtObj, ANY));
      } else {
        O = It->second;
      }
      store(arg(0), srcNode(elem(O, ANY)), 24);
      return;
    }
    case Intrinsic::vacopy: {
      uint32_t T = newNode();
      load(arg(1), T, 24);
      store(arg(0), T, 24);
      return;
    }
    case Intrinsic::masked_load:
    case Intrinsic::masked_gather:
    case Intrinsic::masked_expandload:
      if (Ret != NONE) load(arg(0), Ret, INF);
      return;
    case Intrinsic::masked_store:
      store(arg(1), arg(0), INF);
      return;
    case Intrinsic::masked_scatter:
      store(arg(1), arg(0), INF);
      return;
    case Intrinsic::masked_compressstore:
      store(arg(1), arg(0), INF);
      return;
    default:
      // other intrinsics compute values from their operands (provenance kept, offset lost, and
      // computed); one that may write memory or call is treated as an unknown library call
      if (!CB.onlyReadsMemory() && !CB.onlyAccessesInaccessibleMemory() && !CB.doesNotAccessMemory()) {
        for (uint32_t A : Args) addEdge(A, ESC);
        if (Ret != NONE) addElem(Ret, elem(ExtObj, ANY));
      }
      if (Ret != NONE) {
        for (uint32_t A : Args) addEdge(A, Ret, MAny);
        computed(Ret);
      }
      return;
    }
  }

  Impl(Module &M, std::function<PtrModel(const Function &)> ModelOf, std::function<bool(const Function &)> FromOutside)
      : M(M), DL(M.getDataLayout()), ModelOf(std::move(ModelOf)), FromOutside(std::move(FromOutside)) {}

  // Component mode (README, "Points-to per component"): the module is solved as the disjoint union of
  // its components. A call from one component into another is not bound to the callee: it is treated
  // as a call of an unknown function (the arguments escape, the result comes from outside), every
  // function may be called from outside (its parameters hold EXT, its result escapes), and every
  // mutable global is visible outside. Each of the three over-approximates what the cut removes, so the
  // result is sound for any partition; the partition decides only the precision.
  std::function<unsigned(const Function &)> Component;  // empty: whole-module mode
  size_t CutCalls = 0;
  bool cut(const Function *Callee, const Con &C) {
    if (!Component) return false;
    const Value *At = C.At ? C.At : C.CB;
    auto *I = dyn_cast_or_null<Instruction>(At);
    if (!I) return true;
    return Component(*I->getFunction()) != Component(*Callee);
  }

  void build() {
    ExtObj = newObj(nullptr, OExt);
    UnkObj = newObj(nullptr, OUnk);
    NoProvObj = newObj(nullptr, ONoProv);
    if (getenv("FX_TRACE_NOPROV")) TraceObjs.insert(NoProvObj);  // debugging: where NOPROV comes from
    ESC = newNode();
    NPSRC = newNode();
    addElem(NPSRC, elem(NoProvObj, ANY));
    THROWN = newNode();
    UNKSTORE = newNode();
    EXPOSED = newNode();
    addElem(THROWN, elem(ExtObj, ANY));
    // EXT memory holds EXT pointers and everything that escaped; EXT itself is escaped
    addElem(ESC, elem(ExtObj, ANY));
    for (GlobalVariable &GV : M.globals()) initGlobal(GV);
    for (Function &F : M) {
      if (F.isIntrinsic()) continue;
      if (!F.isDeclaration() && (Component || FromOutside(F))) addElem(ESC, elem(objOf(&F, OFunc), 0));
    }
    for (Function &F : M)
      if (!F.isDeclaration()) genFunction(F);
  }

  // ------------------------------------------------------------------- Steensgaard pre-pass (scale)
  // Unification over the same constraints the inclusion solver uses. Every rule of the inclusion
  // analysis is mirrored by a join, so the result over-approximates it: if an object's class can never
  // contain a code pointer (a function, EXT or UNK) directly or through pointers, no execution and no
  // flow of the inclusion analysis can load a code pointer from it or through it.
  std::vector<uint32_t> UF;
  std::vector<int64_t> SPt;
  uint32_t CellObj0 = 0;
  size_t PreJoins = 0, PreClasses = 0, PreRelevant = 0, PreObjects = 0, PrePasses = 0;
  uint32_t find(uint32_t X) {
    uint32_t R = X;
    while (UF[R] != R) R = UF[R];
    while (UF[X] != R) { uint32_t N = UF[X]; UF[X] = R; X = N; }
    return R;
  }
  uint32_t newCell() {
    UF.push_back((uint32_t)UF.size());
    SPt.push_back(-1);
    return (uint32_t)UF.size() - 1;
  }
  uint32_t ptOf(uint32_t C) {
    C = find(C);
    if (SPt[C] < 0) { uint32_t P = newCell(); SPt[find(C)] = P; return P; }
    return find((uint32_t)SPt[C]);
  }
  void join(uint32_t A, uint32_t B) {
    std::vector<std::pair<uint32_t, uint32_t>> St{{A, B}};
    while (!St.empty()) {
      auto [X, Y] = St.back();
      St.pop_back();
      X = find(X);
      Y = find(Y);
      if (X == Y) continue;
      PreJoins++;
      int64_t PX = SPt[X], PY = SPt[Y];
      UF[Y] = X;
      if (PX < 0) SPt[X] = PY;
      else if (PY >= 0) St.push_back({(uint32_t)PX, (uint32_t)PY});
    }
  }
  uint32_t objCell(uint32_t O) { return CellObj0 + O; }

  // node -> its cell (nodes created during the pre-pass get fresh cells)
  size_t PreNN = 0;
  DenseMap<uint32_t, uint32_t> LateNodeCell;
  uint32_t nodeCell(uint32_t N) {
    if (N < PreNN) return N;
    auto It = LateNodeCell.find(N);
    if (It != LateNodeCell.end()) return It->second;
    uint32_t C = newCell();
    LateNodeCell[N] = C;
    return C;
  }
  uint32_t pn(uint32_t N) { return ptOf(nodeCell(N)); }

  void prepass() {
    size_t NN = Nodes.size(), NO = Objs.size();
    PreNN = NN;
    PreObjects = NO;
    UF.clear();
    SPt.clear();
    for (size_t i = 0; i < NN + NO; i++) newCell();
    CellObj0 = (uint32_t)NN;
    uint32_t ExtC = objCell(ExtObj);
    join(ptOf(ExtC), ExtC);             // EXT memory holds EXT
    join(pn(ESC), ExtC);              // what flows into ESC is reachable by library code
    join(objCell(UnkObj), pn(EXPOSED));  // UNK points into exposed objects
    join(pn(ESC), pn(EXPOSED));          // escaped objects are exposed
    join(ptOf(objCell(UnkObj)), ExtC);
    for (uint32_t N : IntToPtrNodes) { join(pn(N), pn(EXPOSED)); join(pn(N), objCell(UnkObj)); }
    for (size_t n = 0; n < NN; n++) {
      for (unsigned E : Nodes[n].Pts)
        if (Objs[ElemOf[E].first].Kind != ONoProv) join(pn((uint32_t)n), objCell(ElemOf[E].first));  // NOPROV: not mirrored
      for (auto &Ed : Nodes[n].Out) join(pn((uint32_t)n), pn(Ed.To));
      for (uint32_t CId : Nodes[n].Cons) {
        const Con &C = Cons[CId];
        if (C.Kind == CLoad) join(pn(C.A), ptOf(pn((uint32_t)n)));
        else if (C.Kind == CStore && C.A != NPSRC && !SpecialNodes.count(C.A)) join(ptOf(pn((uint32_t)n)), pn(C.A));
        else if (C.Kind == CSelf) {
          uint32_t K = pn((uint32_t)n);
          join(ptOf(K), K);
          join(ptOf(K), objCell(ElemOf[C.A].first));
        }
      }
    }
    for (auto &[N, SO] : SlotOf) join(pn(N), ptOf(objCell(SO.first)));
    // calls (and escaped functions) until no new function reaches a call
    std::set<std::pair<uint32_t, uint32_t>> Done;
    DenseSet<const Function *> ParamsEscaped;
    while (true) {
      PrePasses++;
      bool Any = false;
      std::map<uint32_t, std::vector<uint32_t>> FuncsIn;
      for (size_t o = 0; o < NO; o++)
        if (Objs[o].Kind == OFunc) FuncsIn[find(objCell((uint32_t)o))].push_back((uint32_t)o);
      uint32_t ExtRep = find(ExtC);
      // functions reachable by library code: parameters and results are EXT
      for (uint32_t o : FuncsIn[ExtRep]) {
        auto *F = cast<Function>(Objs[o].V);
        if (F->isDeclaration() || !ParamsEscaped.insert(F).second) continue;
        Any = true;
        for (const Argument &A : F->args()) join(pn(node(&A)), ExtC);
        join(pn(retNode(F)), ExtC);
        join(pn(vaNode(F)), ExtC);
      }
      for (size_t n = 0; n < NN; n++)
        for (uint32_t CId : Nodes[n].Cons) {
          const Con &C = Cons[CId];
          if (C.Kind != CCall) continue;
          uint32_t K = find(pn((uint32_t)n));
          bool CalleeExt = K == find(ExtC);
          std::vector<uint32_t> Fs = FuncsIn.count(K) ? FuncsIn[K] : std::vector<uint32_t>();
          for (uint32_t o : Fs) {
            if (!Done.insert({CId, o}).second) continue;
            Any = true;
            auto *F = cast<Function>(Objs[o].V);
            if (F->isDeclaration() || modeled(*F)) {
              // a library function reached through a pointer: as an unknown library call (escape),
              // unless the call was connected already (its model's constraints are in the graph)
              if (!Connected.count({F, CId})) {
                for (uint32_t A : C.Args) if (A != NONE) join(pn(A), ExtC);
                if (C.Ret != NONE) join(pn(C.Ret), ExtC);
              }
              continue;
            }
            unsigned I = 0;
            for (const Argument &A : F->args()) {
              if (I >= C.Args.size()) break;
              if (C.Args[I] != NONE) join(pn(C.Args[I]), pn(node(&A)));
              I++;
            }
            for (; I < C.Args.size(); I++) if (C.Args[I] != NONE) join(pn(C.Args[I]), pn(vaNode(F)));
            if (C.Ret != NONE) join(pn(retNode(F)), pn(C.Ret));
          }
          if (CalleeExt && Done.insert({CId, UINT32_MAX}).second) {
            Any = true;
            for (uint32_t A : C.Args) if (A != NONE) join(pn(A), ExtC);
            if (C.Ret != NONE) join(pn(C.Ret), ExtC);
          }
        }
      if (!Any) break;
    }
    // relevance: a class holds code (a function, EXT, UNK) or points to a class whose objects are relevant
    std::map<uint32_t, char> HasCode, Memo;
    for (size_t o = 0; o < NO; o++) {
      ObjKind K = Objs[o].Kind;
      if (K == OFunc || K == OExt || K == OUnk) HasCode[find(objCell((uint32_t)o))] = 1;
    }
    auto relevantClass = [&](uint32_t K) -> bool {
      // Q(K) = HasCode(P(K)) or Q(P(K)), along the chain of pointees
      std::vector<uint32_t> Path;
      std::set<uint32_t> Seen;
      uint32_t C = find(K);
      bool R = false;
      while (true) {
        auto M = Memo.find(C);
        if (M != Memo.end()) { R = M->second; break; }
        if (!Seen.insert(C).second) break;  // cycle without code
        Path.push_back(C);
        if (SPt[C] < 0) break;
        uint32_t P = find((uint32_t)SPt[C]);
        if (HasCode.count(P)) { R = true; break; }
        C = P;
      }
      for (uint32_t X : Path) Memo[X] = R;
      return R;
    };
    Relevant.assign(NO, 1);
    std::set<uint32_t> Classes;
    for (size_t o = 0; o < NO; o++) {
      ObjKind K = Objs[o].Kind;
      Classes.insert(find(objCell((uint32_t)o)));
      if (K == OFunc || K == OExt || K == OUnk || K == ONoProv) continue;
      Relevant[o] = relevantClass(objCell((uint32_t)o)) ? 1 : 0;
    }
    PreClasses = Classes.size();
    for (char R : Relevant) PreRelevant += R;
    // DATA, and rewrite what the build already propagated
    DataObj = newObj(nullptr, OHeap);
    Relevant.push_back(1);
    Objs[DataObj].Collapsed = true;
    uint32_t DataElem = elem(DataObj, ANY);
    size_t NE = ElemOf.size();
    std::vector<uint32_t> Remap(NE);
    for (uint32_t E = 0; E < NE; E++) Remap[E] = isData(ElemOf[E].first) ? DataElem : E;
    for (auto &Nd : Nodes) {
      bool Hit = false;
      for (unsigned E : Nd.Pts) if (E < Remap.size() && Remap[E] != E) { Hit = true; break; }
      if (!Hit) continue;
      SparseBitVector<> NP;
      for (unsigned E : Nd.Pts) NP.set(E < Remap.size() ? Remap[E] : E);
      Nd.Pts = NP;
    }
    for (size_t o = 0; o < NO; o++)
      if (!Relevant[o])
        for (auto &[K, Sl] : std::map<int64_t, Slot>(Objs[o].Slots)) addEdge(Sl.Node, slot(DataObj, ANY, INF));
    UF.clear();
    UF.shrink_to_fit();
    SPt.clear();
    SPt.shrink_to_fit();
  }

  void descNodes() {
    if (getenv("FX_TRACE_NOPROV"))
      for (size_t i = 0; i < FirstPtrNP.size() && i < 6; i++) traceBack(FirstPtrNP[i]);
    const char *D = getenv("FX_DESC_NODES");
    if (!D) return;
    for (StringRef R = D; !R.empty();) {
      auto [A, B] = R.split(',');
      R = B;
      unsigned N;
      if (A.getAsInteger(10, N) || N >= Nodes.size()) continue;
      errs() << "node " << N << " = " << nodeDesc(N) << " rep " << nrep(N) << " retag " << Nodes[nrep(N)].RetagTo << " generic " << Nodes[nrep(N)].Generic << "\n  pts:";
      int K = 0;
      for (unsigned E : Nodes[nrep(N)].Pts) { if (K++ > 12) break; errs() << " " << objName(ElemOf[E].first) << "@" << (ElemOf[E].second == ANY ? std::string("*") : std::to_string(ElemOf[E].second)); }
      errs() << "\n";
    }
  }
  void solveAll() {
    if (!getenv("FX_PTA_NO_PREPASS")) {
      auto T0 = std::chrono::steady_clock::now();
      prepass();
      PrepassS = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
      if (getenv("FX_PROFILE"))
        errs() << "prepass: " << PrepassS << " s, objects " << PreObjects << ", relevant " << PreRelevant << ", classes " << PreClasses
               << ", joins " << PreJoins << ", passes " << PrePasses << "\n";
    }
    if (getenv("FX_PROFILE")) {
      std::vector<std::pair<int, std::string>> V;
      for (auto &[K, C] : EscapeCalls) V.push_back({C, K});
      std::sort(V.rbegin(), V.rend());
      errs() << "escape-model calls: " << V.size() << " functions\n";
      for (size_t i = 0; i < 40 && i < V.size(); i++) errs() << "  " << V[i].first << " " << V[i].second.substr(0, 120) << "\n";
    }
    run();
    // Calls whose callee set stayed empty: connect them to the fallback targets and solve again, so that
    // the analysis does not depend on a call it reports as resolved by the fallback rule.
    while (true) {
      bool Any = false;
      for (uint32_t N0 : IntToPtrNodes)
        if (uint32_t N = nrep(N0); Nodes[N].Pts.empty()) {
          if (getenv("FX_PROFILE")) {
            auto It = NodeVal.find(N0);
            if (It != NodeVal.end())
              if (auto *In = dyn_cast<Instruction>(It->second)) {
                std::string W;
                raw_string_ostream OS(W);
                OS << In->getFunction()->getName().substr(0, 70) << " :: ";
                if (auto *Op = dyn_cast<Instruction>(In->getOperand(0))) Op->print(OS);
                else In->getOperand(0)->print(OS);
                errs() << "unknown inttoptr: " << W.substr(0, 220) << "\n";
              }
          }
          addElem(N, elem(UnkObj, ANY));
          IntToPtrUnknown++;
          Any = true;
        }
      for (uint32_t CId = 0; CId < Cons.size(); CId++) {
        if (Cons[CId].Kind != CCall || !Cons[CId].CB) continue;
        const CallBase *CB = Cons[CId].CB;
        if ((SiteFuncs.count(CB) && !SiteFuncs[CB].empty()) || SiteExt.count(CB) || SiteUnk.count(CB) || FallbackDone.count(CB))
          continue;
        connectFallback(CId);
        Any = true;
      }
      if (!Any || OverBudget) break;
      FallbackRounds++;
      run();
    }
    descNodes();
  }
};

PointsTo::PointsTo(Module &M, std::function<PtrModel(const Function &)> ModelOf,
                   std::function<bool(const Function &)> CalledFromOutside)
    : I(new Impl(M, std::move(ModelOf), std::move(CalledFromOutside))) {}
PointsTo::~PointsTo() { delete I; }

void PointsTo::setComponents(std::function<unsigned(const Function &)> ComponentOf) { I->Component = std::move(ComponentOf); }
size_t PointsTo::cutCalls() const { return I->CutCalls; }

void PointsTo::solve(const std::function<std::vector<Function *>(const CallBase &)> &FallbackTargets, double BudgetS) {
  I->Fallback = FallbackTargets;
  I->BudgetS = BudgetS;
  I->build();
  I->solveAll();
}

PointsTo::SiteResult PointsTo::site(const CallBase &CB) const {
  SiteResult R;
  auto It = I->SiteFuncs.find(&CB);
  if (It != I->SiteFuncs.end()) R.Funcs.assign(It->second.begin(), It->second.end());
  auto Ie = I->SiteExtFuncs.find(&CB);
  if (Ie != I->SiteExtFuncs.end())
    for (Function *F : Ie->second)
      if (std::find(R.Funcs.begin(), R.Funcs.end(), F) == R.Funcs.end()) R.Funcs.push_back(F);
  R.Ext = I->SiteExt.count(&CB);
  R.Unk = I->SiteUnk.count(&CB);
  R.Empty = !R.Ext && !R.Unk && (It == I->SiteFuncs.end() || It->second.empty());
  return R;
}

std::vector<std::string> PointsTo::modelsUsed() const { return {I->ModelsUsed.begin(), I->ModelsUsed.end()}; }

bool PointsTo::escaped(const Function *F) const {
  auto It = I->ObjOf.find(F);
  return It != I->ObjOf.end() && I->Objs[It->second].Escaped;
}

std::string PointsTo::describe(const Value *V) const {
  std::string Out;
  auto It = I->NodeOf.find(V);
  if (It == I->NodeOf.end()) return "<no node>";
  if (getenv("FX_TRACE_NOPROV")) I->traceBack(I->nrep(It->second));
  for (unsigned E : I->Nodes[I->nrep(It->second)].Pts) {
    auto [O, K] = I->ElemOf[E];
    auto &Ob = I->Objs[O];
    std::string N = Ob.Kind == Impl::OExt ? "EXT" : Ob.Kind == Impl::OUnk ? "UNK" : Ob.Kind == Impl::ONoProv ? "NOPROV" : Ob.V ? Ob.V->getName().str() : "?";
    if (N.empty() && Ob.V) { raw_string_ostream OS(N); Ob.V->printAsOperand(OS, false); }
    Out += N + "@" + (K == ANY ? std::string("*") : std::to_string(K)) + (Ob.Escaped ? "!" : "") + " ";
  }
  return Out;
}

PointsTo::Stats PointsTo::stats() const {
  Stats S;
  S.Nodes = I->Nodes.size();
  S.Objects = I->Objs.size();
  S.Elems = I->ElemOf.size();
  S.Constraints = I->Cons.size();
  S.Iterations = I->Iterations;
  for (auto &O : I->Objs) if (O.Escaped) S.EscapedObjects++;
  S.EscapedFunctions = I->EscapedFuncs.size();
  S.IntToPtr = I->IntToPtr;
  S.IntToPtrUnknown = I->IntToPtrUnknown;
  S.NoProvStores = I->NoProvStores;
  S.ForgedEscaped = I->ForgedEscaped;
  S.CollapsedObjects = I->Collapsed;
  S.FallbackRounds = I->FallbackRounds;
  S.Merged = I->Merged;
  S.OverBudget = I->OverBudget;
  S.PreObjects = I->PreObjects;
  S.PreRelevant = I->PreRelevant;
  S.PreClasses = I->PreClasses;
  S.PrepassS = I->PrepassS;
  return S;
}

}  // namespace fneffects
