; attack-ir/19, at the IR level: an indirect call with value profiling as the front end emits it before
; the InstrProfiling lowering (-fprofile-instr-generate). The lowering turns the intrinsic into a call of
; __llvm_profile_instrument_target, which allocates the site's value nodes with calloc on first use.
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@__profn_entry = private constant [5 x i8] c"entry"
@hook = global ptr @a

define i64 @a(i64 %x) {
  %r = add i64 %x, 1
  ret i64 %r
}

define i64 @entry(i64 %x) {
  %f = load volatile ptr, ptr @hook
  %t = ptrtoint ptr %f to i64
  call void @llvm.instrprof.value.profile(ptr @__profn_entry, i64 12345, i64 %t, i32 0, i32 0)
  %r = call i64 %f(i64 %x)
  ret i64 %r
}

define i32 @main() {
  %r = call i64 @entry(i64 1)
  ret i32 0
}

declare void @llvm.instrprof.value.profile(ptr, i64, i64, i32, i32)
