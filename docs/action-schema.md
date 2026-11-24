# Action schema

> **In plain terms.** An *action* is a ready-to-run task the app drafts from something you said, such as an email or a GitHub issue. A *schema* is the exact description of what each kind of action contains: which fields it has, which are required, and what values are allowed.
>
> The point of writing these descriptions down precisely, once, is that everything else follows from them: the AI is only allowed to produce drafts that fit them, the app checks every draft against them, and each editor screen is built from them. The definitions themselves are the files in `core/src/planner/connectors/`, and this page explains them.
>
> *Unfamiliar words are explained in the [glossary](glossary.md).*

An **action** is the typed, editable draft the planner produces for one item in a memo: "email Jeff about the budget" becomes a `send_email` draft with recipients, subject and body. The user reviews it in that connector's editor, confirms it, and the app executes it through the service's API.

**Status:** the registry and the validator exist (`core/src/planner/schema_registry.*`, `action_validator.*`, tested in `core/tests/planner_schema_test.cpp`). The planner that writes drafts, and the editors and executors that use them, come in the rest of stage 3 and in stage 4.

## 1. Registry format

A connector's definition is one JSON object:

```json
{"connector": "email", "version": 1,
 "actions": [{"type": "send_email", "description": "...", "risk": "high",
              "context_fetchers": ["contacts.lookup"], "params_schema": {}}]}
```

- **`connector`**, **`type`** and property names use `a-z`, `0-9` and `_`, up to 64 characters.
- **`risk`** is `low`, `medium` or `high` (§5). **`context_fetchers`** lists the read-only lookups the planner may ask for.
- Other keys are refused, so a misspelled one is noticed.
- Registering a connector again replaces it, if the version is the same or higher. A bad definition changes nothing.

## 2. Schema rules

`params_schema` is a small part of JSON Schema that works for llama.cpp's grammars and for Claude's structured outputs. Anything else is refused when the connector registers:

| Type | Also allowed |
|---|---|
| `object` | `properties`, `required` and `additionalProperties: false`, all three always there |
| `array` | `items` (always there) |
| `string` | `enum`; `format` of `email` or `date-time` |
| `integer`, `boolean` | |

Every type can have a `description`. There are no length or number limits (the validator has those), no `$ref`, and at most 8 levels.

## 3. The draft

```json
{"connector": "email", "type": "send_email", "schema_version": 1,
 "params": {}, "rationale": "...", "confidence": 0.8, "missing_fields": ["to"]}
```

- **`missing_fields`** names the top-level fields the planner couldn't fill from what it was given. Such a field is left out, and the editor asks for it. The core works this list out from what the model left blank; the model doesn't write it.
- **There's no `risk` in a draft.** It always comes from the registry: a draft can be shaped by text someone slipped into a recording or a web page, and must not be able to call itself harmless.

## 4. Validation

Each problem is `{path, code, message}`: `path` points into `params` (`/to/0`), or is empty for the whole draft; `code` is stable, for programs; `message` is for people. A draft is checked twice:

- **As a draft:** fields in `missing_fields` may be absent or empty. Anything that is filled in is checked.
- **Before it runs:** everything must be there.

The schema is checked first, then the rules under each action below. Lengths count characters. An email address can't hold spaces, line breaks, commas or angle brackets, and a subject or title can't hold a line break, so neither can smuggle in another recipient or mail header. A date and time is RFC 3339 with an offset (`2026-11-26T15:00:00-05:00`).

**Codes:** `type`, `missing`, `unknown`, `enum`, `format`, `too_long`, `too_many`, `range`, `not_in_diff`, `unknown_action`, `schema_version`, `missing_fields`.

## 5. Risk levels

| Risk | What confirming needs |
|---|---|
| low, medium | Confirm, after the exact payload is shown |
| high | Hold to confirm, after the exact payload is shown |

`review_pr` is high, but medium when its verdict is `COMMENT`. See [threat model §5](threat-model.md#5-prompt-injection).

## 6. Versions

A draft records its connector's version as `schema_version`, and a draft made for another version is refused (`schema_version`). Upgrading stored drafts is stage 4's to design.

## 7. Version 1 actions

### email ([`email.json`](../core/src/planner/connectors/email.json), version 1)

#### `send_email`

Compose and send an email from one of the user's connected accounts. **Risk:** high. **Context fetchers:** `contacts.lookup`, `email.accounts`.

| Field | Type | Required | |
|---|---|---|---|
| `account_id` | text |  | The sending account; the default one if absent. |
| `to` | list of email addresses | yes | Recipients. |
| `cc` | list of email addresses |  | Copied recipients. |
| `bcc` | list of email addresses |  | Blind-copied recipients. |
| `subject` | text | yes | One line. |
| `body_markdown` | text | yes | Markdown. |

**Also checked:** `to` not empty; at most 100 recipients over `to`, `cc` and `bcc`; `subject` one line, up to 998 characters; body up to 100,000.

```json
{
  "to": [
    "jeff@example.com"
  ],
  "subject": "Moving the budget review",
  "body_markdown": "Hi Jeff,\n\nCan we move the review to Thursday?"
}
```

### github ([`github.json`](../core/src/planner/connectors/github.json), version 1)

#### `create_issue`

File a new issue in a GitHub repository. **Risk:** medium. **Context fetchers:** `github.repos`.

| Field | Type | Required | |
|---|---|---|---|
| `repo` | text | yes | The repository, as owner/name. |
| `title` | text | yes | One line. |
| `body_markdown` | text | yes | Markdown. |
| `labels` | list of text |  | Existing label names. |
| `assignees` | list of text |  | GitHub usernames. |

**Also checked:** `repo` is `owner/name`; `title` one line, up to 256; body up to 65,536 (it may be empty).

```json
{
  "repo": "ElliotScher/Micnote",
  "title": "Pairing times out",
  "body_markdown": "Pairing gives up after 60 s."
}
```

#### `comment`

Comment on a GitHub issue or pull request. **Risk:** medium. **Context fetchers:** `github.repos`, `github.pr`.

| Field | Type | Required | |
|---|---|---|---|
| `repo` | text | yes | The repository, as owner/name. |
| `number` | whole number | yes | The issue or pull request number. |
| `body_markdown` | text | yes | Markdown. |

**Also checked:** `repo` is `owner/name`; `number` 1 or more; body not empty, up to 65,536.

```json
{
  "repo": "ElliotScher/Micnote",
  "number": 12,
  "body_markdown": "Fixed on main."
}
```

#### `review_pr`

Review a GitHub pull request: a summary, a verdict and comments on lines. **Risk:** high. **Context fetchers:** `github.repos`, `github.pr`.

| Field | Type | Required | |
|---|---|---|---|
| `repo` | text | yes | The repository, as owner/name. |
| `number` | whole number | yes | The pull request number. |
| `verdict` | `COMMENT` / `APPROVE` / `REQUEST_CHANGES` | yes | The review's outcome. |
| `summary_markdown` | text | yes | Markdown. |
| `comments` | list of objects |  | Comments on lines of the diff. |
| `comments[].path` | text | yes | The file, as in the diff. |
| `comments[].line` | whole number | yes | The line number on that side. |
| `comments[].side` | `LEFT` / `RIGHT` | yes | RIGHT is the new file, LEFT the old. |
| `comments[].body_markdown` | text | yes | Markdown. |

**Also checked:** `repo` is `owner/name`; `number` and every `line` 1 or more; summary up to 65,536; a review that isn't `APPROVE` needs a summary or a comment; a comment's body isn't empty; when the diff is known, each comment is on one of its lines.

```json
{
  "repo": "ElliotScher/Micnote",
  "number": 12,
  "verdict": "COMMENT",
  "summary_markdown": "One question.",
  "comments": [
    {
      "path": "core/src/x.cpp",
      "line": 3,
      "side": "RIGHT",
      "body_markdown": "Can this be null?"
    }
  ]
}
```

### calendar ([`calendar.json`](../core/src/planner/connectors/calendar.json), version 1)

#### `create_event`

Create a calendar event. **Risk:** medium. **Context fetchers:** `contacts.lookup`, `calendar.free_busy`.

| Field | Type | Required | |
|---|---|---|---|
| `title` | text | yes | One line. |
| `start` | date and time | yes | RFC 3339 with an offset. |
| `end` | date and time | yes | RFC 3339 with an offset; after start. |
| `attendees` | list of email addresses |  | People to invite. |
| `location` | text |  | A place or a video link. |
| `notes` | text |  | Plain text. |

**Also checked:** `title` one line, up to 1,000; `end` after `start`.

```json
{
  "title": "Budget review",
  "start": "2026-11-26T15:00:00-05:00",
  "end": "2026-11-26T15:30:00-05:00",
  "attendees": [
    "jeff@example.com"
  ]
}
```

### tasks ([`tasks.json`](../core/src/planner/connectors/tasks.json), version 1)

#### `create_reminder`

Create a reminder or task for the user. **Risk:** low. **Context fetchers:** none.

| Field | Type | Required | |
|---|---|---|---|
| `title` | text | yes | One line. |
| `due` | date and time |  | RFC 3339 with an offset. |
| `list` | text |  | The list to add it to; the default one if absent. |
| `notes` | text |  | Plain text. |

**Also checked:** `title` one line, up to 1,000.

```json
{
  "title": "Buy milk",
  "due": "2026-11-27T18:00:00-05:00"
}
```
