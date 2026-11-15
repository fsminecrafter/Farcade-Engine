# Audio (engine/audio)

Sound for the AudioListener, AudioStream and AudioSampler modules
(`engine/scene/modules.h`). Public header: `engine/audio/audio.h` (it pulls in
no decoder and no OS header). Build: `mk/audio.mk`.

    AudioDevice  -> where the sound goes
    Mixer        -> 32 voices, resampling, saturating mix
    AudioSystem  -> the ECS side: modules, spatialization, pushing to the device

## Formats

WAV (PCM 8/16/24/32 bit, float, ADPCM: dr_wav), MP3 (dr_mp3), Ogg Vorbis
(stb_vorbis). Chosen by the first bytes of the file (`RIFF`/`RF64` = WAV,
`OggS` = Vorbis, anything else = MP3). Mono and stereo play as they are; files
with more channels keep their first two. Any sample rate: the mixer resamples
linearly to the device rate (and by `pitch`). Files that cannot be decoded are
rejected with a log line (`Clip::decode` returns nullptr).

Decoding is from memory (no stdio): the host reads the file (`AudioHost::
read_file`, so the editor resolves project paths) and the audio system owns
the bytes.

- **Sampler** clips (`AudioSampler`) are decoded completely once to int16 and
  cached by path (`stats().clips_cached`).
- **Streams** (`AudioStream`) keep the compressed file in memory and decode
  on the fly into a 4096-frame FIFO per voice; they loop by rewinding the
  decoder.

## Devices

`AudioDevice::init(Backend, rate)`:

| Backend | |
|---|---|
| `MinimalOS` | one stereo int16 stream (`mos_audio_stream_open(48000, 2)`, then 44100 if the rate is refused). Fails when there is no sound card (`os_error()` is `SYS_ERR_GENERIC`). |
| `Null` | plays nothing; keeps what it is given and drops what the caller says was "played" (`null_advance(frames)`), so tests are deterministic. `capture()` reads back everything written. |
| `Auto` | Minimal-OS stream if it can be opened, else the Null device following the clock (real time) - the host build and machines without a sound card. |

Queue model: the OS stream holds 16384 frames; `write()` never blocks and
returns the frames taken. `queued()`, `writable()` and `underruns()` (the
kernel's count) are available.

## Pushing audio

No audio thread. `AudioSystem::update()` (registered in `Phase::Store`, or
call it yourself) tops the device queue up to about 80 ms every frame: it
mixes `target - queued` frames in chunks of 1024 and writes them. At 60 fps
that is ~800 frames per frame. A frame that takes longer than ~80 ms makes the
device run dry (an underrun, counted); the next update then refills the full
80 ms at once. If the game calls update less often than every ~80 ms the
sound breaks up: call it from the frame loop.

## ECS behaviour

Each `update()`:

1. reads the current AudioListener (`World::current`): its world position,
   its right axis and `volume` (the master volume). No listener: master 1, and
   `spatial` sources play flat.
2. finds every entity with an AudioStream or AudioSampler; the first time it
   sees an AudioStream it starts it if `play_on_start` (so streams start with
   the world's first frame; entities created later start when first seen).
3. syncs live voices with the modules: volume, pitch, loop, spatial.
4. ends finished one-shot voices, forgets destroyed entities (their voices
   stop).
5. mixes and pushes.

Spatial (`spatial = true`): distance `d` from the listener. Gain is 1 up to
`min_distance`, then inverse distance `min/d`, shifted and scaled so that it
reaches exactly 0 at `max_distance` (silent beyond). Pan = the direction to
the source on the listener's right axis (-1 left .. +1 right), faded to 0
within `min_distance`. Mono sources pan with a constant-power law (centre
= -3 dB per side, so a centred mono sound is 0.707 per channel); stereo
sources use balance. Gain and pan change smoothly (ramped over a 256-frame
block).

API for scripts and the editor (all on `AudioSystem`):
`play_sampler(e)` (pitch = `pitch * (1 + pitch_variation * random(-1..1))`, at
most `voices` overlapping per entity, the oldest is cut), `play_stream(e)`
(restarts), `stop_stream(e)`, `pause_stream(e, paused = true)`,
`is_playing(e)`, `stop_all()`, `restart()` (forget what started: use it when
Play restarts and the world's entities are reused), `stats()`
(`voices_active`, `underruns`, `mix_ms`, `mix_ms_max`, `frames_pushed`,
`clips_cached`).

Missing or undecodable files: one log line per path (`audio: cannot read
'x'`, `audio: cannot decode 'x'`), `play_*` returns false, nothing else
happens.

## Integration

mk: add `$(AUDIO_SRC)` to the editor's source lists (host and OS) together
with `$(ECS_SRC) $(FLECS_SRC) $(CORE_SRC)`; `mk/audio.mk` gives the audio
objects their flags. Init order:

```cpp
World world;  world.init();
AudioDevice dev;  dev.init();                       // Backend::Auto
AudioHost host;  host.read_file = ...;  host.log = ...;  host.user = ...;
AudioSystem audio;  audio.init(&world, &dev, host); // after the script runtime
// ... world.progress(dt) every frame runs audio.update() in Phase::Store
audio.shutdown();  dev.shutdown();                  // before world.shutdown()
```

`read_file` returns `malloc`'d bytes (the system `free`s them) or nullptr.
Lua's `obj.audio_sampler:play()` calls `audio.play_sampler(entity)`.

## Tests and numbers

`tests/host/audio_tests.cpp` (131 checks, through the Null device; the MP3
and Ogg files are 2-4 KB tones in `tests/data/*.inc`, made with ffmpeg).
`build/os/bin/audio_smoke.run` plays 440 Hz for one second through the real
device and prints `FE AUDIO PASS <frames> <underruns>`, `FE AUDIO PASS
nodevice` (no sound card) or `FE AUDIO FAIL <why>`.

Mixing 32 voices for 1 s of audio takes ~4 ms on the host (-O2), ~0.4% of
real time.
