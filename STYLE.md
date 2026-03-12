# Commenting and Architecture Notes (MDVZ)

## Goal
Comments must explain intent, interactions, threading, and real-time constraints.
Code remains the source of truth for mechanics.

## Scope
Apply these rules to all files in `Source/`:
- File/module header: role, boundaries, dependencies, thread model.
- Non-trivial class: usage contract, invariants, ownership.
- Public API: `@brief` + parameters/return + execution context.
- Inline comments in function bodies: only for non-obvious decisions.

## Doxygen format
Use declaration-site docs in headers (`.h`):
- `@brief` mandatory for public class/function.
- Add `@param` and `@return` when relevant.
- First sentence is the summary. Put details after a blank line.

## Content rules
- Explain intention and rationale, not line-by-line mechanics.
- Keep comments short, specific, and stable over time.
- Avoid comments that restate code.
- If comments can drift, prefer invariant statements over implementation details.

## Mandatory for MIDI real-time code
For each critical API, document explicitly:
- Thread: `audio`, `message`, or `worker`.
- RT-safe: `yes/no`.
- RT constraints when `yes`: no locks, no allocations, no file/network I/O.
- Temporal behavior: immediate, next block, quantized tick, deferred in PPQ, etc.

For interacting parameters, document:
- Ordering (`A` before `B`).
- Priority/conflicts.
- Side effects (state flush, retrigger, bypass behavior).

## Pattern block (required for key components)
For architecture-critical classes, include this 5-item block:
- Pattern
- Problem solved
- Participants
- Flow (2 to 5 steps)
- Pitfalls (threading, coupling, RT safety, testability)

## Code review checklist
- Module header exists and states role + thread model + RT constraints.
- Class docs state invariants and ownership.
- Public APIs have Doxygen contract and execution context.
- Parameter interactions are documented where they affect behavior.
- No comment paraphrases obvious code.
