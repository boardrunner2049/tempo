# Examples

Run these from the repository root, with the library built
(`meson setup build && ninja -C build`) and the package installed
(`pip install -e .`).

| Example | What it shows |
| --- | --- |
| [`motor_and_sensor.py`](motor_and_sensor.py) | The motivating configuration: firmware → world → firmware, and the causal chain recovered from the trace afterwards. |
| [`same_time_chain.py`](same_time_chain.py) | Zero-delay connections, and why `(time, microstep)` is the scheduler's clock rather than time alone. |
| [`two_firmware.toml`](two_firmware.toml) | The same federation as a TOML experiment file, for `tempo-run`. |

```console
$ python examples/motor_and_sensor.py
$ python examples/same_time_chain.py
$ tempo-run examples/two_firmware.toml --stats --dump-trace 20
$ tempo-run examples/two_firmware.toml --explain
```

Every participant here is a `Mock`. That is deliberate: the scheduler's
semantics are pinned down against synthetic participants before any real
simulator is attached, so that when FMI and FastDyn arrive there is something
trustworthy to attach them to.
