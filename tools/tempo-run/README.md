# tempo-run

`tempo-run` is a Python entry point, not a native binary, so it lives in
[`python/tempo/cli.py`](../../python/tempo/cli.py) and is installed by
`pip install -e .`.

That split is deliberate. Building a federation means reading TOML, resolving
participant types, and reporting configuration errors — all control-plane
work. Converting a trace is also control-plane work, but it has to be possible
without Python installed at all, so `tempo-trace` is native and lives in
[`../tempo-trace/`](../tempo-trace/).

```console
$ tempo-run experiment.toml --stats
$ tempo-run experiment.toml --explain
$ tempo-run experiment.toml --until 5ms
$ tempo-run experiment.toml --trace run.trace --dump-trace 20
```

See [`../../docs/configuration.md`](../../docs/configuration.md).
