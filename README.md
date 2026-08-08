dexed-cli - headless Dexed renderer
===================================

> **Note:** This is **not** the original Dexed. This branch (`cli`) is a
> customized fork of [Dexed](https://github.com/asb2m10/dexed) reduced to a
> command line audio renderer for [Real2Dex](https://github.com/real2dex).
> There is no GUI and no plugin format here. For the VST3/CLAP/standalone
> build, use the `real2dex` branch.

It keeps the DX7 synth engine
([msfa](https://github.com/google/music-synthesizer-for-android)) and the
parameter model, and drops everything else: the editor, the cartridge manager,
MIDI hardware I/O, the JSON socket server and the plugin wrappers. What is left
renders `.syx` patches to WAV, in parallel, from a job file.

Building
--------

```
$ git clone -b cli <this repo> dexed-cli
$ cd dexed-cli
$ git submodule update --init --recursive
$ cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
$ cmake --build build --config Release
```

The binary lands in `build/Source/dexed-cli_artefacts/Release/dexed-cli`.

### Linux / Docker

No X server, no `DISPLAY`, no Xvfb and no sound card are needed: nothing in
this build ever constructs a window, and JUCE only dlopens libX11 when it has
to open one. `juce_audio_processors` still drags `juce_gui_basics` in through
its module dependencies, so the one non-obvious build dependency is freetype:

```dockerfile
RUN apt-get update && apt-get install -y build-essential cmake libfreetype-dev
```

At runtime only `libfreetype6` and the usual C++ runtime are required.

Usage
-----

```
# one clip
dexed-cli render --syx rom1a.syx --program 3 --note 60 -o out.wav

# a dataset, across every core
dexed-cli batch jobs.jsonl --threads 8

# jobs streamed in from another process (read to EOF, then rendered)
generate_jobs.py | dexed-cli batch - --threads 8

# exact parameter names for the "params" field
dexed-cli params
```

A job file is one JSON object per line:

```json
{"output": "clips/000.wav", "syx": "rom1a.syx", "program": 3, "note": 60, "velocity": 0.8}
{"output": "clips/001.wav", "syx": "rom1a.syx", "program": 3, "note": 72, "params": {"ALGORITHM": 0.5, "FEEDBACK": 0.0}}
```

Only `output` is required; every other field falls back to the matching command
line option. `params` values are normalised 0..1, exactly like the plugin's host
parameters. Run `dexed-cli --help` for the full option list.

Clip format
-----------

The defaults reproduce the previous `renderClipToFile()` byte for byte: 120 BPM,
4 beats held plus a 4 beat tail, 48 kHz / 32-bit float / mono, 192000 samples
(4.0 s). `--rate`, `--bits`, `--channels`, `--bpm`, `--sound-beats` and
`--tail-beats` change that when needed.

Reproducibility
---------------

Rendering is deterministic: identical jobs produce identical bytes regardless of
thread count, job order, or which machine ran them. Three things had to change
to make that true, and they are the only ways output can differ from the plugin
build:

* **No `Dexed.xml`.** The plugin loaded engine type, pitch bend range and
  velocity normalisation from a per-user preferences file, so its output
  depended on the machine it ran on. The CLI never reads it. In particular the
  engine now comes from `--engine` (default `markI`); pass `--engine modern` if
  you need to match clips a plugin build produced with `engineType=0`.
* **The feedback buffer is initialised.** `Dx7Note::fb_buf_` was never cleared
  by the constructor or by `init()`, so the first ~20 ms of any patch with
  FEEDBACK > 0 was synthesised from whatever the heap happened to contain.
* **The LFO starts from a defined phase.** `Lfo::reset()` does not touch
  `phase_`, and `keydown()` only rewinds it when LFO KEY SYNC is on.

Everything else is bit-identical: verified against the `real2dex` standalone
build across programs and notes, with the non-patch parameters (filter, output
gain, master tune, mono mode) pinned on both sides.

`--skip-silent-tail` stops running the FM engine once it can prove the rest of
the clip is silent -- every envelope frozen, and a stretch of exactly-zero
output longer than one full LFO cycle. The filter keeps running on those zeros,
so the written file is unchanged; it is purely a speed option.

Performance
-----------

320 clips of 4.0 s, one cartridge, 8 logical cores, writing to a local SSD:

| | clips/s |
|---|---|
| plugin build, serial JSON `batch` | ~95 |
| dexed-cli, 1 thread | ~98 |
| dexed-cli, 4 threads | ~250 |
| dexed-cli, 8 threads | ~200-280 |

Synthesis is no longer the limit -- per-file filesystem overhead is. The same
run against a faster volume reaches ~800 clips/s, and shrinking the clip length
by 8x only moves 8-thread throughput from ~800 to ~1130 clips/s, which is the
file-creation ceiling rather than a DSP one. If you need to go faster than that,
the next step is writing many clips into one packed file instead of one WAV per
clip.

Licensing
---------

Dexed is licensed on the GPL v3. The msfa component (acronym for music
synthesizer for android, see msfa in the source folder) stays on the Apache 2.0
license to be able to collaborate between projects.

Credits & thanks
----------------
* DX Synth engine : Raph Levien and the [msfa](https://github.com/google/music-synthesizer-for-android) team
* [Surge Synth Team](https://surge-synth-team.org/) for substantial contributions like microtuning and MPE support.
* [Sentinel77](https://github.com/Sentinel77) for numerous engine fixes
* LP Filter : Filatov Vadim (2DaT); taken from the excellent [Obxd](https://obxd.wordpress.com) project
* PPPlay : Great [OPL3](https://github.com/stohrendorf/ppplay) implementation, with documented code :D
* DX7 program compilation : Jean-Marc Desprez (author of [SynprezFM](http://www.synprez.com/SynprezFM))
* DX7 programs : Dave Benson, Frank Carvalho, Tim Conrardy, Jack Deckard, Chris Dodunski, Tim Garrett, Hitaye, Stephan Ibsen, Christian Jezreel, Narfman, Godric Wilkie
* falkTX [distrho](http://distrho.sourceforge.net/)
