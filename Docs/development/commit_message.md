# Commit Message Guidelines

This repository uses a structured commit message format for
non-trivial changes. The goal is to make review and later incident
analysis easier by capturing the problem, the implementation, and the
verification evidence in the commit itself.

## Format

Use this layout:

```text
<area>: <short summary>

Issue:
------
<Describe the user-visible problem, root cause, and why the old
behavior is wrong. Wrap body text at 72 columns.>

Solution:
---------
<Describe the implementation, important design decisions, compatibility
impact, and behavior after the change. Wrap body text at 72 columns.>

Docs:
<Optional. Describe documentation changes. Wrap at 72 columns.>

Tests:
<Optional. Summarize test strategy and coverage. Wrap at 72 columns.>

Test: <verification command or result>
Test: <verification command or result>

Signed-off-by: Name <email>
Co-Authored-By: GPT 5.5 <noreply@openai.com>
```

## Required Sections

`Issue:` and `Solution:` are required for all non-trivial code changes.

The underline after a section header is part of the format. Its length
must match the header length, including the colon:

```text
Issue:
------

Solution:
---------
```

## Subject

- Use English.
- Prefer `<area>: <summary>`, for example:

  ```text
  mdl: expand LF pinbox index space
  ```

- Keep the subject concise. Aim for 50 characters when practical, and
  do not exceed 72 characters.
- Use imperative mood when it reads naturally.
- Do not end the subject with a period.

## Body Wrapping

Use Google-style wrapping:

- Wrap body text at 72 columns.
- Keep the subject separated from the body by one blank line.
- Keep paragraphs short and focused.
- Do not use very long bullet lines.

You can check wrapping with:

```bash
git log -1 --format=%B HEAD |
  awk 'length($0) > 72 { printf "%4d %s\n", length($0), $0 }'
```

This command should produce no output, except when a deliberately long
literal path, URL, or command is unavoidable.

## Issue Section

The `Issue:` section should explain:

- The user-visible symptom or failure mode.
- The root cause.
- Why existing behavior is incorrect or insufficient.
- Relevant limits, invariants, or source-level facts.

Do not put the implementation details first. Reviewers should
understand the problem before reading the fix.

## Solution Section

The `Solution:` section should explain:

- What changed.
- Why this design was chosen.
- What was intentionally not changed.
- Compatibility and downgrade considerations when relevant.
- Operational impact when relevant.

## Docs And Tests Sections

Use `Docs:` when the commit adds or updates design notes, review
documents, runbooks, or other Markdown artifacts.

Use `Tests:` to summarize the testing strategy. Put concrete
verification commands in separate `Test:` lines.

Example:

```text
Tests:
Add gunit coverage proving the default LF pinbox can allocate past the
old 65535 usable-pin limit. Add MDL coverage for the same behavior.

Test: Built mysys_lf-t, mdl-t, and mysqld with Ninja.
Test: Ran Mysys.LFPinboxAllocatesPastOldLimit.
Test: Ran MDLTest.AllocatesPastOldPinboxLimit.
Test: Ran mtr 1st.
Test: Ran git diff --check.
```

## Sign-Off

Keep sign-off trailers at the end of the commit message. The usual
trailers for AI-assisted changes are:

```text
Signed-off-by: Qingping Zhu <qingping_smile@163.com>
Co-Authored-By: GPT 5.5 <noreply@openai.com>
```

Adjust the human sign-off when the author is different.

## Local Template

The repository provides `.gitmessage` as a local template. Enable it
with:

```bash
git config commit.template .gitmessage
```

The template is a convenience helper. The final commit message must
still be edited to describe the actual issue, solution, docs, and
tests.
