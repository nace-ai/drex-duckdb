# Contributing

Bug reports, documentation fixes, and focused pull requests are welcome. For substantial
API changes, open an issue to discuss the intended behavior first.

## Set up

Follow the [source build instructions](README.md#getting-started). Keep the pinned
submodule revisions unless your change explicitly updates DuckDB compatibility.
See [Updating DuckDB](docs/UPDATING.md) for that process.

## Check a change

```sh
python3 -m unittest discover -s test/mock -v
make test
make format-check
```

`make test` alone skips tests that need the API mock. Follow [the mock setup](test/README.md#the-mock-drex-api)
to run the contract suite locally without an API key or billed requests. The test guide
also covers transport, stress, Python, and sanitizer checks for changes in those areas.
Live tests are opt-in and call a billed external API.

Include a regression test for behavior changes, update examples when the API changes,
and describe the checks you ran in the pull request. Keep unrelated changes separate.

## Reports and private material

Include the platform, DuckDB version, extension revision, and a minimal reproduction.
Use synthetic inputs and placeholder credentials. Do not include API keys, customer data,
private logs, or internal planning documents in issues, pull requests, or commits.

Store internal notes outside the repository. `.gitignore` only helps prevent accidental
new additions; it does not remove files from Git history.
