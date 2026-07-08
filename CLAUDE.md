# CLAUDE.md — House Rules for giga-vla

Field notes on getting a language model to write code we will not rewrite.
The throughline: the model is fast at generating plausible code and slow to
notice that plausible is not the same as correct. Discipline comes from the
process below.

## I. Read before you write

The biggest source of bad model-written code is writing before reading the
codebase. Read the files you are about to touch — read, not skim. Copy the
patterns that already exist, and check the imports to see what the project
actually depends on. If you can't find a pattern, ask instead of guessing.

## II. Think before you code

Figure out what you are doing before you type. State your assumptions
("add authentication" is five different things, so name the one you picked)
and name the tradeoffs. If something is genuinely confusing, stop and ask
rather than filling the gap with plausible-looking code — that is exactly
the code that passes a casual review and fails when it matters.

## III. Simplicity

Write the minimum code that solves the problem in front of us now — not the
minimum that could solve every future version of it. Resist premature
abstraction, skip error handling for errors that cannot occur, and hardcode
values until there is a real reason to configure them. The test: if the only
reason something is abstracted is "in case we need to," it is over-built.

## IV. Surgical changes

The diff should be as small as the task allows. Do not touch what you were
not asked to touch: match the existing style, do not reformat. A formatter
pass buries the three lines that matter inside three hundred that do not.
The test: can we justify every changed line by the task? If a line is there
because "while I was in there," revert it.

## V. Verification

The gap between code that works and code we think works is testing. When
fixing a bug, write the failing test first, watch it fail, then fix it —
that is the only proof we fixed the cause and not the symptom. Test behavior
that can actually break, not that a constructor sets a field. If something
is hard to test, that is information about the design, not permission to
skip it.

## VI. Goal-driven execution

Every task needs a success criterion before code is written. "Add
validation" becomes "reject a missing or malformed email, return 400 with a
clear message, and test both cases." For anything multi-step, state the plan
first so the wrong approach dies before an hour is spent building it.

## VII. Debugging

When something breaks, investigate; do not guess. Read the whole error and
the stack trace, reproduce the problem before changing anything, and change
one thing at a time. Do not paper over an unexpected null with a null check:
find out why it is null, or the bug just moves somewhere quieter.

## VIII. Dependencies

Every dependency is permanent code we do not control. Before adding one,
ask whether the project or the standard library already covers it. When you
would add one, say why, so the choice is visible rather than smuggled into
the manifest.

## IX. Communication

Say what you did and why, not just a block of code. Flag concerns even when
you did exactly what was asked, and be precise about uncertainty: "I am not
sure this library supports streaming" tells the user what to verify; "I
think this should work" does not.

## X. Common failure modes

A few patterns recur often enough to name: the **Kitchen Sink**
(restructuring half the codebase while you are at it), the **Wrong
Abstraction** (copy-paste twice before you abstract), the **Optimistic
Path** (the happy path handled and the 500 ignored), and the **Runaway
Refactor** (a fix that cascades across files). Catch yourself in any of
these and the right move is to stop — not to push through.

---

## Project-specific notes

- This repo has two build targets: a Python host (VLA inference) and
  Arduino GIGA R1 firmware (real-time servo control). See
  `docs/ARCHITECTURE.md` for the v1 decision record — read it before
  changing the wire protocol, control loop, or safety behavior.
- Rule III applied here: v1 is one arm, one policy, one transport. Anything
  serving a hypothetical second robot is over-built.
- Rule X applied here: the Optimistic Path is physical on this project — an
  unhandled link drop moves a real robot. Safety behavior (watchdog, limits,
  e-stop) is MCU-side and is never bypassed for convenience during bring-up.
- Rule V applied here: every milestone must be verifiable without the
  physical robot first (loopback/sim), then with it.
