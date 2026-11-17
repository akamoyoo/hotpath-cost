#!/usr/bin/env python3
"""patch.py ORIG OUT: MPMCQueue.h with the turn of each operation computed once, before the atomic
operations, instead of again after them (where the atomic forces capacity_ to be reloaded, so the
compiler divides a second time). Nothing else changes."""
import sys
s = open(sys.argv[1]).read()
R = [
 ("""    auto &slot = slots_[idx(head)];
    while (turn(head) * 2 != slot.turn.load(std::memory_order_acquire))
      ;
    slot.construct(std::forward<Args>(args)...);
    slot.turn.store(turn(head) * 2 + 1, std::memory_order_release);""",
  """    auto &slot = slots_[idx(head)];
    auto const t = turn(head);
    while (t * 2 != slot.turn.load(std::memory_order_acquire))
      ;
    slot.construct(std::forward<Args>(args)...);
    slot.turn.store(t * 2 + 1, std::memory_order_release);"""),
 ("""      auto &slot = slots_[idx(head)];
      if (turn(head) * 2 == slot.turn.load(std::memory_order_acquire)) {
        if (head_.compare_exchange_strong(head, head + 1)) {
          slot.construct(std::forward<Args>(args)...);
          slot.turn.store(turn(head) * 2 + 1, std::memory_order_release);""",
  """      auto &slot = slots_[idx(head)];
      auto const t = turn(head);
      if (t * 2 == slot.turn.load(std::memory_order_acquire)) {
        if (head_.compare_exchange_strong(head, head + 1)) {
          slot.construct(std::forward<Args>(args)...);
          slot.turn.store(t * 2 + 1, std::memory_order_release);"""),
 ("""    auto &slot = slots_[idx(tail)];
    while (turn(tail) * 2 + 1 != slot.turn.load(std::memory_order_acquire))
      ;
    v = slot.move();
    slot.destroy();
    slot.turn.store(turn(tail) * 2 + 2, std::memory_order_release);""",
  """    auto &slot = slots_[idx(tail)];
    auto const t = turn(tail);
    while (t * 2 + 1 != slot.turn.load(std::memory_order_acquire))
      ;
    v = slot.move();
    slot.destroy();
    slot.turn.store(t * 2 + 2, std::memory_order_release);"""),
 ("""      auto &slot = slots_[idx(tail)];
      if (turn(tail) * 2 + 1 == slot.turn.load(std::memory_order_acquire)) {
        if (tail_.compare_exchange_strong(tail, tail + 1)) {
          v = slot.move();
          slot.destroy();
          slot.turn.store(turn(tail) * 2 + 2, std::memory_order_release);""",
  """      auto &slot = slots_[idx(tail)];
      auto const t = turn(tail);
      if (t * 2 + 1 == slot.turn.load(std::memory_order_acquire)) {
        if (tail_.compare_exchange_strong(tail, tail + 1)) {
          v = slot.move();
          slot.destroy();
          slot.turn.store(t * 2 + 2, std::memory_order_release);"""),
]
for a, b in R:
    assert s.count(a) == 1, a[:60]
    s = s.replace(a, b)
open(sys.argv[2], "w").write(s)
