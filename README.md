# Drex for DuckDB

Ask typed questions about your data directly from SQL. `drex` connects DuckDB to
[Drex](https://console.nace.ai/docs) to classify text, score rows, and return probabilities
as native SQL values.

```sql
SELECT prompt_drex(
    'I was charged twice. Please refund the duplicate payment.',
    'Is the customer asking for a refund?'
) AS refund_probability;
```

Use it for support-ticket routing, document classification, and scoring data you already
query with DuckDB. Inputs can be text, JSON, or images on supported models.

[Getting started](#getting-started) · [Examples](#examples) · [API reference](docs/API.md) ·
[Contributing](CONTRIBUTING.md)

## Getting started

This checkout targets **DuckDB v2.0.0**. Build from source using the pinned submodules;
the instructions below do not require a community extension release. Extension binaries
must match the DuckDB version and platform that load them. This is a local DuckDB
extension; it does not provide a server-side MotherDuck function or a browser/Wasm build.

### 1. Build

You need Git, CMake, Make, a C++17 compiler, Python 3.10+, and the OpenSSL and curl development
libraries. Ninja is optional. The build fetches `httpfs` for HTTPS support.

```sh
git clone --recurse-submodules https://github.com/nace-ai/drex-duckdb.git
cd drex-duckdb
# Also run this if you cloned without --recurse-submodules:
git submodule update --init --recursive
```

On Ubuntu/Debian:

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build python3 libssl-dev libcurl4-openssl-dev
GEN=ninja make release
```

On macOS, install the Xcode command-line tools and Homebrew dependencies:

```sh
xcode-select --install # Only needed if the command-line tools are not installed
brew install cmake ninja openssl@3 curl
OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" GEN=ninja make release
```

### 2. Load and authenticate

Get an API key from the [Nace console](https://console.nace.ai). Set `NACE_API_KEY` in
your environment through your secret manager or a hidden shell prompt before starting
DuckDB. Avoid putting the key in SQL files or shell history.

From the repository root, start the bundled shell:

```sh
./build/release/duckdb -unsigned
```

Then run:

```sql
LOAD 'build/release/extension/httpfs/httpfs.duckdb_extension';
LOAD 'build/release/extension/drex/drex.duckdb_extension';
CREATE SECRET (TYPE nace, PROVIDER env);
```

`-unsigned` allows locally built extensions; only load binaries you trust. The environment
provider reads `NACE_API_KEY` and optional `NACE_BASE_URL` when you create the secret.
The default endpoint is `https://console.nace.ai`.

### 3. Query

```sql
CREATE TABLE tickets AS
SELECT * FROM (VALUES
    (1, 'I was charged twice. Please refund the duplicate payment.'),
    (2, 'How do I change my email address?')
) AS t(id, body);

CREATE TABLE ticket_answers AS
SELECT id, prompt_drex(body, 'Is the customer asking for a refund?') AS refund_probability
FROM tickets;

SELECT * FROM ticket_answers WHERE refund_probability > 0.8;
```

Answers come from the API and can vary. Saving them in a table lets you query the results
without sending the same rows again.

## Examples

### Classify a row

```sql
SELECT id, prompt_drex(
    body,
    'Which team should handle this ticket?',
    choice := ['billing', 'technical', 'account']
) AS category
FROM tickets;
```

The result is a struct with `choice`, `probabilities`, and `confidence` fields.

### Ask several questions in one request per row

```sql
SELECT id, prompt_drex(body, questions := {
    'refund': {'type': 'noul', 'instructions': 'Is the customer asking for a refund?'},
    'urgency': {'type': 'score', 'instructions': 'How urgent is this ticket?',
                'criteria': ['low', 'medium', 'high']}
}) AS answers
FROM tickets;
```

This returns a struct with `refund` and `urgency` fields. See the [API reference](docs/API.md)
for return types, model selection, images, and the lower-level `drex_decide` function.

## Requests, privacy, and cost

The extension sends evaluated inputs and question instructions to the configured API.
Use data you are authorized to send to that service. A custom `NACE_BASE_URL` receives
both the API key and the request data; use a trusted endpoint. HTTPS is required except
for `localhost`, `127.0.0.1`, and `[::1]`.

Each evaluated non-NULL row normally produces one decision request; retries can add
attempts. Model validation also requests model metadata. Answered requests may be billed
even when their rows do not appear in the final result.

- A plain `LIMIT n` sends n rows in the tested query shape. `ORDER BY ... LIMIT` can evaluate
  every row; `OFFSET` and filters on an AI answer can evaluate whole chunks or more.
- Select the input rows first, then call `prompt_drex`. For an explicit boundary, materialize
  a limited input table before making API calls.
- Reading a view, a Python relation, or a `NOT MATERIALIZED` CTE again can repeat requests.
  Save answers with `CREATE TABLE ... AS` to reuse them.

These behaviors are covered by the [semantics tests](test/README.md); SQL execution plans
can affect how many inputs are evaluated.

## Configuration

| Setting | Default | Purpose |
| --- | --- | --- |
| `drex_max_concurrency` | `8` | Default cap on in-flight requests across the database; `concurrency := N` overrides it for a call. |
| `drex_max_rpm` | `0` | Database-wide requests per minute; `0` means no client-side limit. |

```sql
SET drex_max_concurrency = 4;
SET drex_max_rpm = 60;
```

Rate-limit responses are retried with the server's requested wait, within the client's
retry budget. See [error behavior](docs/API.md#error-behavior) for failures and NULL results.

## Development and support

See [CONTRIBUTING.md](CONTRIBUTING.md) for development checks and [test/README.md](test/README.md)
for the mock, transport, stress, and opt-in live suites. Basic SQL tests run with `make test`,
but API-dependent tests require the local mock to exercise their behavior.

Report bugs and request features through [GitHub Issues](https://github.com/nace-ai/drex-duckdb/issues).
Include your DuckDB version, platform, and a minimal example with credentials and private data removed.

Maintained by Nace AI. Built from the [DuckDB extension template](https://github.com/duckdb/extension-template).
Licensed under [MIT](LICENSE).
