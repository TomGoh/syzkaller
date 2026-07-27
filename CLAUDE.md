# CLAUDE.md — project working rules

## Root-cause & code-reading discipline (MANDATORY — hard requirements, not suggestions)

In this project I have **more than once** presented a confident but **wrong** root cause built from partial `grep` output instead of reading the actual code, then built fixes/plans on top of it. That wasted the human's time and had to be corrected by them and by reviewers. These rules exist to stop that recurring:

1. **Read the whole function before claiming a mechanism.** Open and read the *complete* body of every function on the path — handler, caller, and callee — before asserting how anything behaves. A `grep` shows only what I searched for. **Never infer "X does not happen" from the absence of a grep match.** (Past failures: missed `t->kcov = kcov` at the KCOV_REMOTE_ENABLE site because I grepped the wrong range; missed the `queue.DefaultOpts` merge that already OR'd in `DedupCover`; misattributed pre-fuzzing feature-probe execs to gen triage.)

2. **Code reading yields a HYPOTHESIS, never a conclusion.** A mechanism inferred only from source is unverified. Say so explicitly. Do **not** call it "the root cause," and do **not** choose fixes/configs/plans based on it, until it is empirically confirmed.

3. **"Located" requires empirical evidence.** A root cause counts as located only when backed by a reproduction, a trace, or instrumentation that shows the **actual values** at the relevant points — pointers, handles, PIDs, counts, states. Prefer identity-tracked evidence (follow one object/handle/PID through its whole lifecycle) over reasoning about aggregate counts or control-flow.

4. **When evidence or a reviewer contradicts me, re-read the cited file:line FIRST.** Assume I misread. Read the exact lines, verify, and retract cleanly and immediately if I was wrong — never defend a code-only theory.

5. **Every status report separates proven from assumed.** Label each claim as "verified (hardware / trace / full-code read)" or "hypothesis — not yet tested." Never present the latter as the former.

6. **No fix, config change, commit, or plan built on an unverified mechanism.** If asked to act on a not-yet-confirmed theory, confirm it empirically first, or state plainly that it is unconfirmed and what would confirm it.

Speed comes from being right the first time, not from guessing fast. When unsure, gather evidence before concluding.
