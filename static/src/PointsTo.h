// Inclusion-based (Andersen-style) points-to analysis over a whole LLVM module, field-sensitive by byte
// offset, context-insensitive, with an on-the-fly call graph. Its purpose is to resolve indirect calls
// (function pointers, std::function, virtual calls through stored vtable pointers) more precisely than
// "every address-taken function of that type", without giving up soundness: whatever it cannot follow is
// represented explicitly (EXT: memory and code of libraries without bitcode; UNK: a pointer of unknown
// provenance), and a call whose callee may be EXT or UNK, or whose callee set is empty, is reported so
// that the caller can fall back to the type rule for that call only.
#pragma once
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"
#include <functional>
#include <string>
#include <vector>

namespace fneffects {

// How a function without a body treats the pointers given to it (from the policy, abi.pointer_models).
struct PtrModel {
  enum Kind {
    Escape,   // default: every pointer argument escapes to library code; the result is EXT
    None,     // reads its arguments, keeps no pointer, returns no pointer (strlen, printf, free, mutexes)
    Fresh,    // returns a new heap object (malloc, operator new)
    Realloc,  // returns a new heap object holding the contents of argument 0
    RetArg0,  // returns a pointer into argument RetArg (strchr, memset: 0; bsearch: 1)
    Copy,     // memcpy-like: copies the bytes of argument 1 into argument 0, returns argument 0
    Data,     // container internals: links argument objects to each other and to fresh heap objects only
    RetExt,   // returns a pointer to library memory, keeps nothing (getenv, __errno_location)
    Throw,    // __cxa_throw: argument 0 becomes the in-flight exception, argument 2 is called on it
    Catch,    // __cxa_begin_catch and friends: returns the in-flight exception
  };
  Kind K = Escape;
  // Callbacks the function makes now or later: (index of the function-pointer argument, indices of the
  // arguments it is called with). The pointer arguments are not escaped.
  std::vector<std::pair<int, std::vector<int>>> Calls;
  uint64_t Size = 0;  // Data: the bytes of each object the function may touch (0: all)
  bool Bodies = false; // also for functions with a body in the module (the model replaces the body here)
  // Data: which arguments' objects are read and written (empty: all), whether links are followed
  // (closure), and whether the objects reached through links are written too.
  std::vector<int> Reads, Writes;
  bool Closure = false, WriteReached = false;
  // Data with Self: the function writes into each object it is given (argument 0) only pointers into that
  // same object and fresh heap pointers (std::string members); nothing crosses between objects.
  bool Self = false;
  int RetArg = 0;  // RetArg0: the argument the result points into
  uint64_t InlineOffset = 0, InlineSize = 0;  // Data with Self: the object's own character buffer (std::string: 16, 16)
  // Effects added to any kind (each from the function's documented contract):
  // WritesBytes: the objects these arguments point to receive bytes from outside or computed bytes
  // (read, recv, fread, snprintf's output, a timespec, a mutex's internals): Size bytes from the
  // pointer (0: to the end of the object), which then hold "no provenance" (a pointer loaded from
  // them is of unknown provenance). Index -1: every variadic argument (printf's %n), -2: every pointer
  // reachable from the variadic arguments or the va_list.
  std::vector<std::pair<int, uint64_t>> WritesBytes;
  // Exposes: the addresses these arguments point to become known as integers (printf's %p).
  // Sends: the pointers stored in the bytes these arguments point to leave the process and may come
  // back as bytes (write, send, fwrite): the objects they point to are exposed.
  std::vector<int> Exposes, Sends;
  std::vector<int> Permutes;  // qsort: the object's pointers move to any offset within it
  bool FreshUnknown = false;  // Fresh: the new object's contents are bytes from outside (mmap)
  int VaList = -1;            // the argument that is a va_list (vprintf): its pointers are the variadic arguments
  int Format = -1;            // printf-like format argument: a constant format without %n writes nothing, without %p exposes nothing
  int FillValue = -1;         // memset-like: WritesBytes applies only when this argument is not the constant 0
  std::string Note;           // the one-line justification from the contract (policy "note")
};

class PointsTo {
public:
  struct SiteResult {
    std::vector<llvm::Function *> Funcs;
    bool Ext = false;    // the callee may be a pointer created by library code
    bool Unk = false;    // the callee may be a pointer of unknown provenance
    bool Empty = false;  // no function reaches the callee
  };
  struct Stats {
    size_t Nodes = 0, Objects = 0, Elems = 0, Constraints = 0, Iterations = 0, EscapedObjects = 0,
           EscapedFunctions = 0, IntToPtr = 0, CollapsedObjects = 0, FallbackRounds = 0;
    bool OverBudget = false;
    size_t IntToPtrUnknown = 0, NoProvStores = 0;
    bool ForgedEscaped = false;
    size_t PreObjects = 0, PreRelevant = 0, PreClasses = 0, Merged = 0;
    double PrepassS = 0;
  };

  PointsTo(llvm::Module &M, std::function<PtrModel(const llvm::Function &)> ModelOf,
           std::function<bool(const llvm::Function &)> CalledFromOutside);
  ~PointsTo();
  // Component mode: solve the module as the disjoint union of its components (call before solve).
  // ComponentOf gives each defined function its component; a call between components is treated as a
  // call of an unknown function, every function as called from outside, every mutable global as visible
  // outside. Sound for any partition. A site whose result has Ext set may then be a pointer from another
  // component: the caller must fall back to its own rule for it.
  void setComponents(std::function<unsigned(const llvm::Function &)> ComponentOf);
  size_t cutCalls() const;
  // FallbackTargets(CB): the targets the caller's own rule gives for an indirect call; used for the
  // arguments of calls the analysis cannot resolve, so that the analysis itself stays sound.
  void solve(const std::function<std::vector<llvm::Function *>(const llvm::CallBase &)> &FallbackTargets, double BudgetS = 0);
  SiteResult site(const llvm::CallBase &CB) const;
  bool escaped(const llvm::Function *F) const;
  // Functions without a body whose pointer model (not the default escape) the solution used.
  std::vector<std::string> modelsUsed() const;
  Stats stats() const;
  std::string describe(const llvm::Value *V) const;  // debugging: the points-to set of a value

private:
  struct Impl;
  Impl *I;
};

}  // namespace fneffects
