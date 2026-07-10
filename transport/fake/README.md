# transport/fake

In-process fake transport implementing `pulselink::Transport`: simulates a
gateway + N nodes within a single test binary, with injectable frame drops,
duplicates, and delays.

This is where most protocol-logic test coverage lives (CLAUDE.md, TRD.md §9,
D-009) — host-native tests in `test/` drive the fake transport instead of
real hardware.

Built starting Phase 1 (PLAN.md).
