# Stage 4: desktop app and in-app actions

## 1. Goal
Ship the Flutter desktop app, Linux first, then macOS and Windows. A user can pair a puck, sync it, read transcripts, and review, edit, confirm and **run actions entirely inside the app**. Each integration has its own editor, validator, preview, executor and (where the service allows it) undo. The app never hands work to another app, a compose sheet, `mailto:` or a browser to finish a task.

**Exit criterion (from PLAN.md):** "Email Jeff about X" and "review PR #N" each go recording → editor → confirm → executed, without leaving the app.

## 2. Depends on / unblocks
- **Depends on:** 2 (C ABI, store, jobs) and 3 (items, action drafts, registry).
- **Unblocks:** 5 (mobile reuses everything here except the platform bridges), 5b (packaging).

## 3. Deliverables
```
app/lib/
  main.dart, app.dart, router.dart
  core/            # FFI wrapper: MicnoteCore (isolate-safe), event stream, typed models (freezed)
  state/           # Riverpod providers
  features/
    onboarding/    # first run, models, privacy choices
    devices/       # pairing (SAS LED compare), status, firmware update
    inbox/         # recordings → transcript view (+ audio playback), items
    actions/       # queue, ActionScreen host, confirm sheet, result view, history
    settings/      # accounts, privacy, models, vocabulary, about
  connectors/
    connector.dart           # interface + registry
    email/{gmail.dart, graph.dart, smtp_imap.dart, email_editor.dart, email_connector.dart}
    github/{api.dart, issue_editor.dart, comment_editor.dart, pr_review_editor.dart, diff_view/, github_connector.dart}
    calendar/ tasks/         # desktop: Google/Graph; native EventKit/CalendarContract land in stage 5
  auth/            # OAuth (PKCE, loopback/embedded), GitHub device flow, token store
  util/{idempotency.dart, markdown.dart, redact_view.dart}
app/test/, app/integration_test/
```

## 4. Design detail

### 4.1 App architecture
- **State management:** Riverpod (code generation). Immutable models with `freezed`/`json_serializable`, generated from the core's JSON method schemas where practical.
- **FFI threading:** core calls go through `MicnoteCore`, which runs `mn_call` on a background isolate (`Isolate.run`) so the UI isolate never blocks.
  - Core events arrive on core threads. They are delivered with `NativeCallable.listener` (async callbacks, safe from any thread) into a `StreamController<CoreEvent>`.
  - The secret-provider callbacks need synchronous answers, so they use `NativeCallable.isolateLocal` with a pre-loaded in-memory cache that is filled from `flutter_secure_storage` before `mn_context_create`. *Verify* the `NativeCallable` threading semantics against the Dart version in use.
- **Navigation:** `go_router`. Deep links `micnote://action/<id>` are used by notifications in stage 5.
- **Design:** Material 3 with adaptive layouts. Desktop gets a three-pane layout (list / transcript / action); mobile gets a stacked layout.

### 4.2 Action lifecycle (state machine shared by all connectors)
```
            ┌────────── regenerate(instruction) ──────────┐
            ▼                                              │
 DRAFTED ──open──► EDITING ──confirm──► CONFIRMING ──ok──► EXECUTING ──► DONE ──undo──► UNDONE
    │                 │  ▲                   │ cancel           │
    │                 │  └───────────────────┘                  ├──► FAILED_RETRYABLE ──retry──► EXECUTING
    └──discard──► DISCARDED                                     └──► FAILED (editable: back to EDITING)
```
- **Revisions:** every edit is saved as a new `action_revisions` row, debounced, with `author=user`. Only the revision the user confirmed can be executed: `executions.rev` must equal the confirmed rev.
- **CONFIRMING** shows the confirm sheet (§4.4). Moving to EXECUTING requires a **fresh `validate()` pass** and, for high-risk actions, a hold-to-confirm gesture (1.5 s).
- **EXECUTING:** runs in the Dart connector. It is in Dart, not core, because the HTTP clients and OAuth tokens live in the app. Core records `executions` and `audit` entries via `mn_call("actions.record_execution", …)`.
- **Idempotency:** `actions.idempotency_key` is a UUIDv7 created when the action is drafted. Each connector uses it as follows:
  - Email: a `Message-ID` derived from the key, checked against Sent before retrying.
  - GitHub: there's no native idempotency, so before retrying a create, search for an issue with the hidden marker `<!-- micnote:KEY -->` in the body.
  - Calendar: `iCalUID` = KEY.
- **Undo window:** for `send_email`, EXECUTING first enters `PENDING_SEND` for 10 s with a visible countdown and an Undo button. The API call happens only when the window ends. If the app quits during the window, the action goes back to CONFIRMING. It is never sent silently.

### 4.3 Connector interface (`connectors/connector.dart`)
```dart
abstract interface class Connector {
  String get id;                                   // "email", "github", ...
  int get version;
  List<ActionTypeDef> get actionTypes;             // JSON schemas → registered with core planner
  Map<String, ContextFetcher> get fetchers;        // read-only lookups (planner.fetch)
  Future<List<Account>> accounts();                // connected accounts for this connector

  Widget editor(ActionDraft draft, ValueChanged<ActionDraft> onChanged);
  Future<List<FieldError>> validate(ActionDraft draft);         // local + remote checks (e.g. repo exists)
  Future<ConfirmPreview> preview(ActionDraft draft);            // human-readable summary + exact payload
  Future<ExecutionResult> execute(ActionDraft draft, ExecutionContext ctx); // ctx: idempotencyKey, cancel token
  Future<UndoResult>? undo(ExecutionResult result);             // null if not supported
}
```
- `ActionDraft` = `{actionId, type, rev, params (Map), missingFields, rationale, risk}`.
- Editors are typed per action type. Each editor converts between `params` and its form model, so the user never sees JSON. An "Advanced" tab shows the raw payload read-only, for transparency.
- **Registry:** `ConnectorRegistry` registers each enabled connector with core at startup (`planner.register_connector`) and answers `planner.fetch` events.

### 4.4 Confirm sheet
- **Header:** "You're about to: Send an email from elliot@… to jeff@…".
- **Body:** the exact payload rendered as it will appear (for email, the full rendered message; for a PR review, the verdict plus the number of comments, expandable).
- **Risk badge:** for high risk, the button reads "Hold to send". Medium risk uses a normal "Confirm" button. Low-risk actions can be set to "skip confirm sheet" in settings; the default is still to show it.
- **Explicit warnings:**
  - recipients outside your domain or contacts ("not in your contacts");
  - attachments;
  - a PR **APPROVE** verdict;
  - any field changed by the planner after your last edit (that can't happen, but it's checked anyway).

### 4.5 Email connector
**Accounts:**
| Provider | Auth | Send | Contacts / context |
|---|---|---|---|
| Gmail | OAuth 2 PKCE, scopes `gmail.send` + `contacts.readonly` (+ `gmail.readonly` only if "check Sent for retry" is enabled) | `users.messages.send` with an RFC 2822 message, base64url | People API |
| Microsoft 365 / Outlook | OAuth 2 PKCE (MSAL-compatible), scopes `Mail.Send`, `Contacts.Read`, `offline_access` | Graph `POST /me/sendMail` | Graph contacts |
| Any other (Fastmail, iCloud, self-hosted) | IMAP/SMTP with an app password, stored in secure storage | SMTP submission (587 STARTTLS / 465), then IMAP `APPEND` to Sent | CardDAV (later) |

- **Libraries:** `enough_mail` (IMAP/SMTP/MIME) for the generic path and for building MIME for Gmail. The `googleapis` package or plain REST via `http`. *Verify* package health before adopting.

**Editor fields:**
- From: an account picker.
- To/Cc/Bcc: chips with autocomplete from `contacts.lookup`. Validation: email format, at least one recipient, and a warning for unknown addresses.
- Subject.
- Body: a markdown editor with live preview, sent as `multipart/alternative` (text + HTML rendered from markdown). There's an AI rewrite toolbar ("shorter", "more formal", "friendlier"), which calls `planner.redraft` and creates a new revision. The signature is appended from the account settings.
- Attachments: a file picker on desktop.
- Missing fields from the planner are highlighted (e.g. `to` empty because "Jeff" was ambiguous). The suggestions show the candidate contacts.

**Execute:** build the MIME message, wait out the undo window, send, and return `{provider, messageId, threadUrl?}`.
**Undo:** only during the window (not possible after sending).

### 4.6 GitHub connector
- **Auth:** OAuth device flow, using a GitHub App or OAuth App owned by the project. The device flow suits desktop and mobile without a redirect server.
  - Scopes: `repo` (or fine-grained permissions if a GitHub App is used: Issues RW, Pull requests RW, Contents R).
  - The token goes to secure storage.
  - *Decision:* a GitHub App with user-to-server tokens is preferred (fine-grained, revocable). *Verify* that its device-flow support covers what we need.
- **`create_issue` editor:** repo picker (search across the user's repos, with recent ones first), title, markdown body with preview, labels (fetched per repo), assignees, milestone. Execute: `POST /repos/{owner}/{repo}/issues`, with the body ending in the hidden idempotency marker. Undo: `PATCH state=closed, state_reason=not_planned` plus an optional comment.
- **`comment` editor:** target (issue or PR picker), markdown body. Execute: `POST /repos/{o}/{r}/issues/{n}/comments`. Undo: delete the comment.
- **`review_pr` editor** (the most complex screen):
  - **Layout:** a file tree on the left; a diff view on the right with unified/split toggle, syntax highlighting (`flutter_highlight` or `re_highlight`), and a choice of whitespace handling. Lazy-load large files.
  - **Comments:** the AI-drafted inline comments show as anchored cards on lines. Each one can be edited, deleted, moved (drag to another line), or turned into a suggestion block. "Add comment" works on any line.
  - **Summary** comment, a markdown editor.
  - **Verdict:** COMMENT / APPROVE / REQUEST_CHANGES, defaulting to whatever the planner chose, with COMMENT as a fallback.
  - **Stale diff check:** the PR's head SHA is pinned when the draft is made. If the PR has moved on, the editor shows "PR changed since draft" and offers to re-anchor (comments on lines that still exist are kept; the rest are flagged).
  - **Execute (GraphQL):**
    1. `addPullRequestReview` (pending, `commitOID` = the pinned SHA).
    2. `addPullRequestReviewThread` for each inline comment (path, line, side).
    3. `submitPullRequestReview` (event + body).

    On failure after step 1, delete the pending review (`deletePullRequestReview`) so no half-submitted review is left behind.
    *Alternative:* a single REST call, `POST /repos/{o}/{r}/pulls/{n}/reviews` with `comments[]` and `event`. It is atomic. **Prefer REST for atomicity**; use GraphQL only if REST can't express a comment type.
  - **Undo:** reviews can't be deleted after submission. "Dismiss" only applies to approvals and needs admin rights, so undo is not offered and the confirm sheet says so.

### 4.7 Calendar and tasks on desktop (minimal in this stage)
- `create_event` uses the Google Calendar API or Graph, with an editor for title, start/end + timezone, attendees, location/video link and notes. Native EventKit and CalendarContract arrive in stage 5.
- `create_reminder` uses Google Tasks or Microsoft To Do.

### 4.8 Auth and token storage
- **Desktop OAuth:** an embedded webview where available (`desktop_webview_window` / `webview_windows`). Fallback: the system browser plus a loopback redirect on `127.0.0.1:<random port>` with PKCE (RFC 8252).
  - Using the browser for the one-time account connection is acceptable; **running actions never leaves the app.**
  - Google may block embedded webviews for OAuth. If so, the loopback + system browser method is the compliant path (*verify*).
- **Mobile:** ASWebAuthenticationSession / Custom Tabs via `flutter_appauth` (stage 5).
- **Storage:** tokens are stored in `flutter_secure_storage` under the key `conn/<connector>/<account>`. Refresh tokens are rotated whenever a provider rotates them.
- **Error handling:** refresh is attempted on 401. If refresh fails, the account is marked as needing to reconnect, and the action goes to `FAILED_RETRYABLE` with a "Reconnect account" button.

### 4.9 Other screens
- **Devices:**
  - pairing wizard: scan → select puck → "Hold the button 5 s" → SAS color comparison (the colors shown large) → done;
  - status: battery, storage, firmware, last sync;
  - firmware update: from the bundled image (stage 5b) via MCUmgr over BLE/USB. Uses `mcumgr_flutter` on mobile, and SMP implemented in core on desktop (*decide*).
- **Inbox:**
  - recordings grouped by day, with a time-confidence badge;
  - transcript view with audio playback synced to the text (tap a segment to seek);
  - markers shown as separators;
  - full-text search (core FTS).
- **Actions queue:** pending drafts grouped by recording, with bulk discard, and a "Needs info" filter for drafts that have `missing_fields`.
- **History:** executed actions with links (opened in an in-app webview, not an external browser), undo where available, and audit details.
- **Privacy settings:** cloud on/off; API key; redaction rules (§3 of the planner doc); vocabulary list; "network activity" log showing every outbound host contacted in the last 30 days.

### 4.10 The no-external-launch guarantee
- The lint rule `avoid_url_launcher_in_connectors` (a custom lint via `custom_lint`) forbids importing `url_launcher` and `Process.run` inside `lib/connectors/**` and `lib/features/actions/**`.
- An integration test wraps the execution layer with a fake platform channel that fails the test if `launchUrl` or a share intent is invoked.
- The only allowed external launches are OAuth fallbacks (`lib/auth/**`, which needs the browser for the one-time sign-in) and "Open on GitHub" links in History, which open an in-app webview by default.

## 5. Task breakdown
**PR 1: foundation**
- [ ] M: `MicnoteCore` wrapper (isolate calls, event stream, secret provider)
- [ ] M: Riverpod setup, router, theming, three-pane layout
- [ ] M: onboarding + settings skeleton

**PR 2: devices and inbox**
- [ ] M: pairing wizard with SAS + status screen
- [ ] M: inbox, transcript view, playback (`just_audio` with a custom source that streams decrypted Opus from core, or a decode-to-temp file in app data; *decide*)
- [ ] S: search

**PR 3: action framework**
- [ ] M: action state machine, revisions autosave, confirm sheet, hold-to-confirm
- [ ] M: connector interface + registry + fetcher bridge
- [ ] S: queue + history screens
- [ ] S: no-external-launch lint + integration test

**PR 4: email**
- [ ] M: auth for Gmail (PKCE loopback) + Graph
- [ ] M: editor + validation + contact autocomplete
- [ ] M: MIME build, send, Sent append (SMTP), undo window
- [ ] M: IMAP/SMTP generic accounts

**PR 5: GitHub**
- [ ] M: device-flow auth, repo/label/assignee fetchers
- [ ] M: issue + comment editors and executors, idempotency marker, undo
- [ ] L: PR review editor (diff view, anchored comments, re-anchoring) + atomic REST submit

**PR 6: calendar and tasks (desktop APIs)**
- [ ] M: `create_event`, `create_reminder` editors and executors

**PR 7: polish**
- [ ] M: macOS and Windows builds pass the same end-to-end flows; keyboard shortcuts (⌘/Ctrl+Enter confirm, Esc cancel)
- [ ] S: accessibility pass (screen-reader labels, contrast, focus order)

## 6. Testing and verification
- **Widget tests:** each editor's validation and missing-field highlights; confirm-sheet content for each action type; hold-to-confirm; the undo countdown.
- **Connector unit tests** with recorded HTTP fixtures (`http_mock_adapter` or a fake `http.Client`): Gmail send, Graph sendMail, the SMTP flow against a local test server (`maildev`/GreenMail in CI), GitHub issue create/close, the review REST submit, and the 401 → refresh → retry path.
- **Integration tests** (`integration_test/`, Linux in CI, using a fake core):
  - full lifecycle DRAFTED → DONE for each type;
  - a network drop mid-execute → retry is idempotent (no duplicate issue or email);
  - the no-external-launch guard.
- **Live sandbox runs** (manual, before each release):
  - a test GitHub repo `micnote-sandbox`: issue, comment and review on a seeded PR;
  - a test mailbox: send, then verify it arrived and appears in Sent.
- **Exit demo:** record both memos from the exit criterion on the Thingy:53 → auto-sync in the desktop app → edit → confirm → done, with the app staying in the foreground the whole time.

## 7. Risks and mitigations
| Risk | Mitigation |
|---|---|
| Google OAuth verification is needed for `gmail.send` (a sensitive scope) for public distribution | Personal use and testers stay in "testing" mode (100 users); start the verification process before 5b; the IMAP/SMTP path works without Google review |
| The PR diff view performs poorly on big PRs | Virtualized list, lazy file loading, an "only files with comments" filter |
| Embedded webview OAuth gets blocked | Loopback + system browser method (one-time, at setup only) |
| `NativeCallable` threading pitfalls | All core→Dart traffic goes through `listener` callbacks; synchronous callbacks are limited to the cached secret lookup |
| Users trust AI drafts too readily | Confirm sheet with explicit warnings; high-risk actions need hold-to-confirm; the draft's rationale is visible |

## 8. Open questions
- SMP firmware update: in core (shared with the CLI) or via `mcumgr_flutter`? Leaning core, for one implementation everywhere.
- Audio playback path (§5 PR 2).
- Should low-risk actions ever auto-run without confirmation? The default is no, and it stays a user setting at most.

## 9. Out of scope
- Mobile background sync and native calendar/reminders (stage 5).
- Installers and auto-update (stage 5b).
- Slack, Linear, Jira, Notion connectors (post-v1, same interface).
