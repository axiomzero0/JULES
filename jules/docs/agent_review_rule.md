# Agent Review Rule (enforced)

**Status:** Mandatory. Instituted 2026-09-24 after a review pass caught real
defects that the authoring agent had marked "done".

## The rule

Every artifact an agent produces — source code, tests, scripts, docs,
commit messages — must be reviewed by a **second, independent agent** before
it is considered delivered. No exceptions for "small" or "obvious" changes.

## Protocol

1. The authoring agent finishes its work and records it in the worklog.
2. A review agent (fresh context, no authorship bias) receives the artifact
   paths, the design contract, and the project rules. It returns findings
   with severity (blocker / should-fix / note).
3. Every finding is either **fixed** or **explicitly dispositioned** with a
   written rationale. "Fixed later" is not a disposition.
4. The worklog entry records: reviewer findings, fixes applied, dispositions.

## Why

- The authoring agent is biased toward its own intent: it reads what it
  meant to write. The reviewer reads what is there.
- Stale-context bugs (placeholders left from refactors, half-applied edits,
  API drift between design and code) are systematically invisible to the
  author and trivially visible to a fresh reader.
- The founding incident: a superoptimizer ISA table was delivered with a
  broken shift-semantics placeholder and a wrong carry-flag model; a review
  pass caught both. Self-review had passed them.

## Cost

One agent pass per deliverable. The alternative — shipping broken code and
rediscovering the bug through a miscompile — is strictly more expensive.
