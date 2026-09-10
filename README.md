# Guitar Rack

Guitar Rack runs the Guitarix DSP engine as a supervised child process on PipeWire. The supervisor and Qt status interface stay available while the DSP process restarts after an audio-server or engine failure.

## Development

```sh
nix develop
cargo run --bin guitar-rack-engine
cargo run --bin guitar-rack
```
