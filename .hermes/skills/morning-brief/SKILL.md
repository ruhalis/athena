---
name: morning-brief
description: Morning brief from Linear, Calendar, and Gmail.
version: 0.2.0
---

# Morning brief

Start-of-day briefing for Athena. Timezone is **Asia/Almaty**. Load this when the user asks for a morning brief, "what's on my plate today," or the `morning-brief` cron job fires.

Also load `google-workspace` and follow its `daily-brief` reference for Gmail and Calendar. Linear is the `linear` MCP server, not a skill.

A brief request is not authorization to send mail, create events, or change Linear issues.

## Procedure

### 1. Resolve the day

State the local date and the half-open window `[day_start, next_day_start)` in Asia/Almaty, plus the UTC equivalent. Done when that window is explicit.

### 2. Fetch Linear (today's work)

Use Linear MCP tools. `list_issues` with assignee `"me"` is the default for "my issues."

Pull enough to judge the day, not the whole backlog:

1. Issues assigned to **me** that are not completed/canceled: In Progress first, then Todo / Backlog that is due today, overdue, in the **current cycle**, or Urgent/High priority.
2. Current cycle via `list_cycles` when a team is known; skip if cycles are unused.
3. Due today and overdue. Include blocked issues only if the user can unblock them today.
4. `get_issue` only when title + state + due date is not enough to recommend (recent comments, blockers).

Cap "do today" at **5** issues. Everything else is context, not a task list. Rank by: overdue → due today → In Progress in the current cycle → Urgent/High → meetings that depend on the issue.

If Linear tools are missing, say so and continue with Google. Do not invent issues.

### 3. Fetch calendar and mail

Invoke `google-workspace`'s `scripts/google_api.py` with the **Hermes venv interpreter** — it is the only Python on this machine carrying the Google client libraries:

```bash
~/.hermes/hermes-agent/venv/bin/python \
  ~/.hermes/skills/productivity/google-workspace/scripts/google_api.py <subcommand>
```

Bare `python` does not exist here, and system `python3` fails with `ModuleNotFoundError: No module named 'googleapiclient'`. Do **not** try to `pip install` the dependencies to work around it: the system environment is externally managed and the install will fail. Subcommand args are positional — `gmail search "<query>" --max N`, `calendar list --start <iso> --end <iso>`.

Follow `google-workspace` `references/daily-brief.md`: all calendars in the day window (conflicts, prep, locations/links), then only mail that changes priority or follow-up. Skip newsletters. If Google is not authenticated, say so.

### 4. Compose for Telegram

Keep it scannable. Use this order:

1. **Summary** — 3–6 bullets: shape of the day (meetings vs focus time), hottest Linear work, any mail/deadline that changes the plan.
2. **Do today** — the Linear issues to actually work, each with identifier, title, state, due/cycle if set, and *why today*. If none, say the board is clear and what the next useful issue is.
3. **Calendar** — today's events, conflicts, tight transitions, prep. Hidden all-day items are still commitments.
4. **Mail** — only threads that change priority or follow-up.
5. **Recommendations** — how to spend the day given the calendar. Name the best focus block(s), what to do before the first meeting, what to defer, and one thing *not* to start. Sequence Linear work around meetings; do not dump a second backlog.

Name coverage gaps (Linear unauthenticated, a calendar missing, mail search failed). Every task and recommendation must trace to a real issue, event, or thread.
