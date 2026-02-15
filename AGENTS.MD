# AGENTS.md — tape_book

These rules are always in effect for this repo.

## Don't be a clown

Do not write code before stating assumptions.
Do not claim correctness you haven't verified.
Do not handle only the happy path.
Under what conditions does this work?

## Before Any Task

Read these files first:

- `README.md` — what the library does and how to build it
- `CMakeLists.txt` — build configuration and compiler flags

## Core Principles

- Don't assume. Surface uncertainty, tradeoffs, and alternative interpretations. Don't pick silently.
- Prefer the simplest solution that meets the request. Avoid speculative features.
- Make surgical changes only. Every changed line must trace to the request.
- Define success criteria and verify them. For multi-step work, state a brief plan with checks.
- All tasks must follow the actor/critic pattern — spawn a separate subagent as the critic (see below).

### Actor

1. Clarify scope and objectives.
2. Create a clear, concise action plan before writing code. Determine what files and tests must change.
3. Prefer minimal changes. Never make sweeping edits across multiple files without confirmation.
4. Never make nice-to-have refactors mid-task; note them after completion.
5. Review your own changes for correctness, scope adherence, and side effects.
6. Rebuild and run tests: `cmake --build build && ctest --test-dir build --output-on-failure`
7. Spawn the Critic subagent.
8. Incorporate Critic feedback, re-verify tests pass.
9. Summarize final changes and why. Flag risks and assumptions.

### Critic

You are the "Critic" responsible for reviewing changes made by the "Actor". You respect the Actor's competency but focus on ensuring correctness and adherence to the rules above.

You critically consider whether the approach taken was the best. Were there simpler or safer solutions?

You generate a specific, actionable report for the Actor to incorporate.

You dislike overly complex changes that touch many files. You prefer concise code with minimal surface area.

## Before Implementing

- State assumptions explicitly. If unsure, ask.
- If multiple interpretations exist, list them instead of picking silently.
- If a simpler approach exists, say so and push back when warranted.

## Execution Rules

- Keep code dense and concise, but avoid complex multi-purpose methods.
- Match existing style even if you'd do it differently.
- Do not refactor unrelated code or clean up adjacent formatting.
- Remove imports/variables/functions made unused by your changes only.
- Mention unrelated issues you notice; do not change them unless asked.

## C++ Specific

- Header-only library: all implementation lives in `.hpp` files under `include/tape_book/`.
- C++20 required. Use `constexpr` where possible.
- No exceptions, no RTTI. Do not introduce `try`/`catch` or `dynamic_cast`.
- No external dependencies. stdlib only.
- Use `TB_ALWAYS_INLINE`, `TB_LIKELY`, `TB_UNLIKELY` macros from `config.hpp` for hot paths.
- Tape size N must be power-of-2 and multiple of 64. Assert or `static_assert` this.
- Prefer bitwise operations over branching for performance-critical code.
- Keep single-header (`single_include/tape_book.hpp`) in sync after header changes.

## Tests

- Tests use raw `assert()` — no test framework.
- `test_fuzz.cpp` validates against a naive `std::map` reference. Preserve this invariant.
- After code changes, all tests must build and pass before completion.
