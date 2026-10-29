# SQL API

Load the extensions and create a `nace` secret as shown in the [README](../README.md#getting-started).

## `prompt_drex`

```sql
prompt_drex(input, instructions [, noul := criteria | choice := criteria | score := criteria]
            [, model := 'drex-v2.0'] [, concurrency := 8])
prompt_drex(input, questions := questions [, model := 'drex-v2.0'] [, concurrency := 8])
```

Instructions, criteria, questions, model, and concurrency must be constant arguments.
`noul`, `choice`, `score`, and `questions` are mutually exclusive. Without a mode,
the function asks a `noul` question: the probability that the answer is true.

| Mode | Result |
| --- | --- |
| `noul` (default) | `DOUBLE` probability |
| `choice` | `STRUCT(choice VARCHAR, probabilities STRUCT(value VARCHAR, probability DOUBLE)[], confidence DOUBLE)` |
| `score` | `STRUCT(score DOUBLE, probabilities STRUCT(index UINTEGER, value VARCHAR, probability DOUBLE)[], confidence DOUBLE)` |
| `questions` | One struct field per question, with the corresponding result type above |

Criteria can be a list of labels, or a list of `{'label': ..., 'description': ...}`
structs. `noul` labels are `true` and `false`. Choice and score accept 2–255 unique
labels; score levels have no descriptions, so describe the scale in the instructions.
`questions` accepts a SQL struct or JSON object mapping IDs to question definitions
with `type`, `instructions`, and optional `criteria`. It accepts 1–512 questions, subject
to the selected model's API limits. `multiple_choice` and `box` are unsupported.

### Inputs and models

`VARCHAR` is sent as text; `JSON` as structured state. A `BLOB` or `BLOB[]` supplies
PNG, JPEG, or WebP images to an image-capable model. For example, with your own image:

```sql
SELECT prompt_drex(content, 'Does this image contain a receipt?', model := 'drex-max')
FROM read_blob('receipt.png');
```

The default model is `drex-v2.0`. `model :=` selects another model exposed by the API.
The extension validates model names using `GET /v1/models`, caching the list per database
and endpoint. NULL inputs return NULL without a decision request.

## `drex_decide`

The lower-level function accepts JSON state and questions and returns the API response as
JSON, without mapping the answers into SQL structs:

```sql
SELECT drex_decide(
    '{"message": "I was charged twice"}'::JSON,
    '{"refund": {"type": "noul", "instructions": "Is a refund needed?"}}'::JSON
);
```

An optional `model := 'drex-v2.0'` selects the model. NULL state or questions return NULL.

## Credentials

Prefer the environment provider:

```sql
CREATE OR REPLACE SECRET (TYPE nace, PROVIDER env);
```

It reads `NACE_API_KEY` and optional `NACE_BASE_URL` at secret creation time. Recreate
the secret after changing either variable. The config provider accepts explicit `API_KEY`
and optional `BASE_URL` arguments, but putting keys directly in SQL can expose them in
history and logs. The key is redacted in `duckdb_secrets()`. `SCOPE` is unsupported.

For OpenRouter, set `NACE_BASE_URL=https://openrouter.ai/api` and use an OpenRouter key
as `NACE_API_KEY`, then recreate the secret. Model availability comes from that endpoint;
unnamespaced model names are resolved under `nace-ai/`.

## Error behavior

Authentication, billing, invalid question definitions, and unknown models fail the query.
Row-specific API refusals such as oversized input or invalid images return NULL with a
warning. Exhausted retryable failures can also return NULL. Treat NULL as an unsuccessful
answer, not a negative classification.

The client retries transient failures within a bounded retry budget and honors retry
headers. TLS failures fail immediately. Request idempotency headers do not provide a
persistent answer cache or guarantee that rerunning a query avoids billing.
