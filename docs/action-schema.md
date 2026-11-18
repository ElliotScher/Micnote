# Action schema

> **Stub.** Stage 3 fills this in, and from then on it is the authoritative definition of every action type. Until then, the design lives in [stage 3 §4.2](stages/03-planner.md#42-action-schema-registry).

An **action** is the typed, editable draft the planner produces for one item in a memo: "email Jeff about the budget" becomes a `send_email` draft with recipients, subject and body. The user reviews it in that connector's editor, confirms it, and the app executes it through the service's API. Connectors describe their action types in JSON, and the same definitions drive the local LLM's grammar, Claude's structured outputs, the validator and the editors.

## Sections to write in stage 3

1. **Registry format:** a connector's definition (`connector`, `version`, `actions[]`) and each action's `type`, `description`, `risk`, `context_fetchers` and `params_schema`.
2. **Schema rules:** `additionalProperties: false` everywhere; no recursion and no numeric or length bounds in the schema, so one schema works for llama.cpp grammars and Claude structured outputs; bounds are enforced by the validator instead.
3. **The draft envelope:** `{connector, type, schema_version, params, rationale, risk, confidence, missing_fields[]}`, and how `missing_fields` replaces invented values.
4. **Validation:** JSON Schema plus per-type semantic checks; the error format the editors show.
5. **Risk levels:** low, medium and high, and the confirmation each one needs (see [threat model §5](threat-model.md#5-prompt-injection)).
6. **Versioning:** what happens to stored drafts when a connector bumps its schema version.
7. **v1 action types**, each with its full schema and an example:

   | Connector | Action types |
   |---|---|
   | email | `send_email` |
   | github | `create_issue`, `comment`, `review_pr` |
   | calendar | `create_event` |
   | tasks | `create_reminder` |
