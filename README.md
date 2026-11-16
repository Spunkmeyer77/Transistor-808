# Ticco 808 Calibration Auto r2 engine source

This source is extracted from the Auto r2 release. The standalone engine
build requires CMake 3.22+ and a C++17 compiler. On macOS, Apple Command
Line Tools supply the compiler and LLDB. See engine/LICENSE and
engine/README.md for the license, upstream revision and modification notice.

## Build and debug a kick hit

From this folder:

```sh
cmake -S engine -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --parallel 4
./build-debug/voicerun kick.wav --rate 44100 --seconds 0.5 --hit 0.05:bd
```

With LLDB:

```sh
lldb -- ./build-debug/voicerun kick.wav --rate 44100 --seconds 0.5 --hit 0.05:bd
```

Set a breakpoint with `breakpoint set --name 'tr808::VoiceBoard::setLines'`
to inspect the trigger, or use `tr808::VoiceBoard::prepare` to inspect board
setup. Run with `run`. The command-line renderer can run without Ableton,
Max, or a ROM. It generates its netlist header during the build.

For a kick tuned one octave above stock:

```sh
./build-debug/voicerun kick-up.wav --rate 44100 --seconds 0.5 --hit 0.05:bd --cal bd.c41=7.5e-9 --cal bd.c42=7.5e-9
```

## Where to look

- `engine/machines/tr808/core/voiceboard.cpp` routes triggers, prepares
  circuits and renders the board.
- `engine/machines/tr808/core/circuit.cpp` implements the circuit solver.
- `engine/machines/tr808/core/netlists/bd.cir` describes the kick circuit.
- `source/lemus808_tilde/lemus808.cpp` contains the Max bridge and maps
  calibration values to the board. `makeCalibration` converts Kick Tune
  into the C41 and C42 values. `applyLiveCalibration` applies calibration
  changes on the audio thread in real time.
- `source/lemus808_tilde/calibration.inc` lists the user-facing calibration
  defaults, limits and units.

To build the Max external too, run `bash build-macos.sh` on macOS. This
fetches the version of the Max SDK used by the upstream bridge build.

## CPU changes (r3 engine)

- The voices now render across worker threads (`tr808::Parallel`, 2 helpers on
  a Mac with 4+ cores). The audio thread still takes jobs itself, so a block
  never waits on a slow worker. The workers get real-time priority, so macOS
  keeps them on the performance cores. Output is bit-identical.
- Flush-to-zero is on in the audio callback and the workers, so decaying
  tails can't hit the slow denormal path.
- A calibration rebuild attaches the workers to the new board only at the
  swap, so its `prepare()` never shares them with the audio thread.

## Live calibration (r4 engine)

- Every calibration value now acts in real time, on the running board, with
  no rebuild. The metal squares, duty, noise trim, clap offset and trigger
  width were already runtime values. Kick Tune, Claves Feedback and the two
  revision switches change real parts through the new
  `Circuit::setValue(element, value)`: capacitor voltages and currents are
  kept, so a ringing kick retunes on the fly.
- `VoiceBoard::setLive*` and `setLivePart(voice, element, value)` are called
  from the audio thread between `render()` calls; no allocation, no locks.
- In the Max external, `cal` messages store atomics and each audio block
  applies what moved. `applycal` only answers `calibrated`.
- Checked against full rebuilds: a live Kick Tune of -12/0/+12 gives the same
  25/50/99 Hz tail as a rebuilt board, and a hit after a live change matches a
  rebuilt board's within measurement noise.
