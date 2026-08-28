---
name: morning-brief
description: Morning brief from Linear, Calendar, and Gmail — a planned day, not a report.
version: 0.4.0
---

# Morning brief

Start-of-day plan for Athena. Timezone **Asia/Almaty**. Working hours **09:00–18:00**. Load this when the user asks for a morning brief, "what's on my plate today," or the `morning-brief` cron job fires.

Also load `google-workspace` and follow its `daily-brief` reference for Gmail and Calendar. Linear is the `linear` MCP server, not a skill.

You are acting as a secretary: the job is to say **what to do, when, and how** — not to report the contents of three systems. Calendar events are the fixed walls of the day; Linear issues are what you place between them.

## The user

- **Bilingual (Russian / English).** Mixing the two in one message is correct and needs no apology or normalization. Keep every Linear issue title in the language Linear holds it in, and write the surrounding text in whichever language reads naturally for that line. Never translate a title to match the rest of a sentence.
- **Offline work Tuesday, Wednesday, and Thursday, 09:00–18:00.** See the placement rules in step 4 — this constrains what may be scheduled, not just what is convenient.
- Linear projects are exactly three: **Work**, **Personal**, **Aquila**.

## Procedure

### 1. Resolve the day

Compute the local date, the weekday, and the half-open window `[day_start, next_day_start)` in Asia/Almaty, plus the UTC equivalent, and use them for every query. Do **not** print them — working state, not output.

### 2. Fetch Linear

Use Linear MCP tools. `list_issues` with assignee `"me"` is the default for "my issues."

Pull enough to judge the day, not the whole backlog:

1. Issues assigned to **me** that are not completed/canceled: In Progress first, then Todo / Backlog that is due today, overdue, due tomorrow, in the **current cycle**, or Urgent/High priority.
2. Current cycle via `list_cycles` when a team is known; skip if cycles are unused.
3. Due today, due tomorrow, and overdue. Include blocked issues only if the user can unblock them today.
4. `get_issue` only when title + state + due date is not enough to place the work in the day (recent comments, blockers, a description that names the concrete next action).

Always carry each issue's **project** (Work / Personal / Aquila) — step 4 cannot place work without it.

Rank by: overdue → due today → in progress and due tomorrow → urgent/high due tomorrow → current cycle. Cap **TO DO** at 5 issues; everything below the cap is context.

If Linear tools are missing, say so in one line and continue with Google. Do not invent issues.

### 3. Fetch calendar and mail

Invoke `google-workspace`'s `scripts/google_api.py` with the **Hermes venv interpreter** — it is the only Python on this machine carrying the Google client libraries:

```bash
~/.hermes/hermes-agent/venv/bin/python \
  ~/.hermes/skills/productivity/google-workspace/scripts/google_api.py <subcommand>
```

Bare `python` does not exist here, and system `python3` fails with `ModuleNotFoundError: No module named 'googleapiclient'`. Do **not** try to `pip install` the dependencies to work around it: the system environment is externally managed and the install will fail. Subcommand args are positional — `gmail search "<query>" --max N`, `calendar list --start <iso> --end <iso>`.

Pull all calendars in the day window, with start/end, location, and any video link. Then mail — only threads that could change today's plan. Skip newsletters, promotions, and Google security/account notices about Athena's own access.

If Google returns `TOKEN_REVOKED` or `NOT_AUTHENTICATED`, put one line at the top of **HEADS-UP** saying calendar and mail are missing and that re-auth is needed. Never guess at events.

### 4. Build the plan

This is the real work of the brief. Place the ranked Linear issues into the gaps between fixed calendar events.

**Offline-work days — Tuesday, Wednesday, Thursday.** Between 09:00 and 18:00 the user is at their offline workplace. In that window place **only issues from the `Work` project**. `Personal` and `Aquila` issues go before 09:00 or after 18:00 on those days, however urgent they are — an urgent errand does not become possible just because it is urgent. If a Personal or Aquila issue genuinely cannot wait for the evening, do not silently schedule it inside working hours: put it in HEADS-UP saying it needs a slot the offline day does not have. On Monday, Friday, Saturday, and Sunday any project may take any hour.

Then:

- **Fixed events are walls.** Every calendar event appears in the plan at its real time, marked `🔒`. Never schedule work across one.
- **Chain errands by location — this outranks the priority sort.** When two out-of-house tasks are near each other, they belong in one outing, back to back, even if their priorities are far apart. Two trips across town on one day is a planning failure, not a strict reading of urgency.
- **Deep work gets the longest uninterrupted block**, earliest available. In-progress issues before not-started ones — finishing beats starting.
- **Blocks run 45 minutes to 2 hours.** Nothing longer than 2h without a gap. Use a Linear estimate when the issue carries one.
- **Leave slack** — a ~1h midday gap, and do not fill every minute to 18:00. Slack is an **absent line**, not a scheduled one: never print a block called "перерыв", "buffer", or "free time". The gap speaks for itself.
- **Travel costs time.** If a fixed event has a location, leave a gap before it rather than ending a block at its start time.
- At most ~7 plan lines. If the ranked work does not fit the day, that is a HEADS-UP, not a longer plan.

Each plan line states the concrete next action — *what* happens in that block, not just which issue it belongs to. Keep the line short enough not to wrap on a phone: identifier, a **short label of a few words** — never the issue's full Linear title — then the action. Aim for under 60 characters after the time.

### 5. Compose for Telegram

Plain text, short lines, no tables and no nested bullets. The whole message should be scannable on a phone in about ten seconds.

```
Thu 27 Aug

PLAN
09:00–11:00  RUH-54 teleportation fix → закончить валидацию
11:00–12:30  RUH-52 D435 mount → напечатать, 45° + 30°
13:00–15:00  Университет: рама (RUH-55) + флюорография (RUH-50)
15:30–17:00  RUH-56 моторы esp32 → прогнать тесты
17:30–19:00  🔒 Автошкола
21:00–22:00  🔒 Упражнения, сон

TO DO
• RUH-50 Флюорография — просрочено на 16 дней, блокирует портал
• RUH-55 Забрать раму — due tmr, только в рабочие часы
• RUH-56 Моторы esp32 — due tmr
• RUH-54 Teleportation fix — due tmr, in progress
• RUH-52 D435 mount — due tmr, in progress

HEADS-UP
• RUH-53 mycobot 280 тоже due 28 Aug — в день не влезает

Поставить план в календарь? — ответь «да»
```

- **Date line** — weekday and date. Nothing else: no UTC window, no account name, no greeting.
- **PLAN** — the schedule. Fixed events marked `🔒`.
- **TO DO** — one line per issue: identifier, short title, and the single fact that earns it a place today (overdue by N days, due tomorrow, blocks something). This section carries the **pressure**; PLAN carries the timing. Never repeat a plan line's action text here. No status paragraphs, no URLs unless the issue is unreadable without one.
- **HEADS-UP** — only what changes the plan: work that does not fit, a deadline approaching this week, mail that moves a priority, prep a meeting needs, a broken integration.
- **The calendar offer** — one closing line, only when a PLAN was produced and the plan is not already on the calendar. It is the single permitted trailing line.

### 6. What never appears

Cut everything that is not a decision. Specifically, never print:

- A summary section restating what the other sections already say.
- Empty-state confirmations — "no conflicts detected," "newsletters were skipped," "no cycle returned," "no prep found."
- A calendar section separate from the plan. Events live in PLAN and nowhere else.
- The UTC window, the day window, the Google account, or the timezone.
- A closing note that nothing was sent, created, or mutated.
- **Any section with no content.** Omit the heading entirely. A day with nothing urgent is a two-section message; a clear day is three lines.

Every plan block, TO DO line, and heads-up must trace to a real issue, event, or thread. Name a coverage gap when one exists — once, in HEADS-UP, in one line.

## Writing the plan to Google Calendar

Athena writes **only** to a dedicated calendar, never to the user's primary calendar. Its id lives in
`~/.hermes/.env` as `ATHENA_CALENDAR_ID`. Read the variable and pass its **resolved value** to `--calendar`;
never pass the literal `$ATHENA_CALENDAR_ID` through to a tool. If the variable is unset, say so and write
nothing — do not fall back to the primary calendar.

### Consent

The brief **offers**; the user **decides**. Producing a brief is never authorization to write, and neither is
a general "yes, this feature is good."

Write only after the user has seen this specific plan and approved this specific plan — «да», "ok", "put it
on the calendar" — in the conversation. The user often replies with **edits** ("move the clinic to the
evening", "drop RUH-52"). Apply the edits, show the corrected plan, and write only when they approve the
corrected version. An answer that changes the plan is a revision, not a green light.

Never write pre-emptively, never write "to save a step", and never write on a schedule.

### Writing

1. **Re-list the Athena calendar** for the day (`calendar list --start <iso> --end <iso> --calendar <id>`) so
   you are working against current state, not what you fetched earlier.
2. **Delete the day's existing Athena blocks** with `calendar delete <event_id> --calendar <id>`. Scope is the
   dedicated calendar and today's window — nothing else is ever a delete candidate. The `google_api.py`
   calendar CLI has create/delete/list but no update, so re-planning is delete-then-create.
3. **Create one event per work block** — never for the `🔒` fixed events, which live on the user's own
   calendars and already exist:
   ```bash
   ~/.hermes/hermes-agent/venv/bin/python \
     ~/.hermes/skills/productivity/google-workspace/scripts/google_api.py \
     calendar create --calendar "<ATHENA_CALENDAR_ID>" \
     --summary "RUH-54 teleportation fix" \
     --start "2026-08-27T09:00:00+05:00" --end "2026-08-27T11:00:00+05:00" \
     --description "[athena-plan] RUH-54 — закончить валидацию"
   ```
   - Summary: issue identifier + short label, matching the plan line.
   - Times: ISO 8601 **with the +05:00 offset**, never naive.
   - Description: `[athena-plan]` first, then the block's action. Keep the marker even on the dedicated
     calendar — it is a second line of defence, not the primary one.
4. **Report in one line** — how many blocks were written, and any that failed. Not a table, not a restatement
   of the plan.

If the user asks to clear the plan, delete the day's events on the Athena calendar and create nothing.

This is the **only** write Athena performs from a brief. A brief never sends mail, never mutates Linear, and
never creates, edits, or deletes an event on any calendar other than the dedicated Athena one.
