# MusicPlayer

Audio-reactive MP3/WAV player for the Nicolai Electronics Tanmatsu (ESP32-P4).

Application ID: `nl.daandobber.musicplayer`

The built-in **MilkDrip** visualizer is a compact, low-resolution homage to
MilkDrop. It combines PCM-driven feedback, procedural rendering, continuously
evolving global colour palettes and fluid field-morph transitions designed for
the ESP32-P4.

## Features

- Recursively finds MP3 and WAV files on the microSD card.
- MP3 and 8/16/24/32-bit mono/stereo WAV playback through the ES8156 codec.
- Artist/album library with ID3v1/ID3v2 and WAV `LIST/INFO` metadata, plus filename/folder fallbacks.
- Editable play queue: replace it with an artist or album, append selections, and remove tracks.
- Hardware volume control and speaker/headphone handling through the Tanmatsu BSP.
- 12-band FFT, RMS, peak and beat analysis of the PCM that is sent to I2S.
- 128 audio-reactive effects: 64 MilkDrop-style feedback presets plus 54 independent procedural, minimal, hybrid and demoscene-inspired renderers.
- Eighteen global colour palettes continuously blend independently of the active algorithm.
- Slow fluid transitions advect the previous image through a vector field while the next algorithm emerges.
- Instant preset switching without an expensive full-frame crossfade.
- Persistent settings for timed/beat-based automatic effect switching, shuffle, visual intensity, brightness and idle dimming.
- Real JPEG album art from MP3 ID3 `APIC` or nearby `cover.jpg`, `folder.jpg`, or `front.jpg`; generated art is the fallback.

## Controls

| Screen | Key | Action |
| --- | --- | --- |
| Library | Left / Right or Tab | Select artists, albums, or playlist pane |
| Library | Up / Down | Select an item in the active pane |
| Library | Page Up / Page Down | Move eight items |
| Artists / albums | Return | Replace playlist and start playing |
| Artists / albums | Space | Append selection to playlist |
| Playlist | Return / Space | Play selected track |
| Playlist | Backspace | Remove selected track |
| Player | Left / Right | Previous / next playlist track |
| Player | Return / Space | Play / pause |
| Player | F2 / F3 or Down / Up | Previous / next effect |
| Player | F4 or Menu | Hide / show the compact now-playing card |
| Any screen | F5 | Open / close settings |
| Settings | Up / Down, Left / Right | Select and change a setting |
| Settings | `Test dimming`, Right | Immediately test the configured dim level |
| Player | Escape | Return to library while audio keeps playing |
| Any | Side `+` / `-` | Volume |
| Any | F1 | Stop audio and return to launcher |

## Build environment

The project targets ESP-IDF 5.5.1 and ESP32-P4. It uses managed components for
`badge-bsp`, PAX, `esp_audio_codec`, and `esp-dsp`. SDK and toolchain paths are
kept outside this repository.

The plasma and metaballs algorithms are adapted from the MIT-0 licensed
`esp_effects` project by Mika Tuupola. The rendering and all audio-reactive
control code in this application are Tanmatsu/PAX-specific implementations.

`Mind Is Growing` is an original cellular-automaton effect inspired by the
techniques Linus Akesson documented for his 256-byte C64 demo
[A Mind Is Born](https://linusakesson.net/scene/a-mind-is-born/). No original
demo code is included. Copper bars, Kefrens bars, rotozoom, shadebobs and the
twister are original implementations of classic demoscene effect families.
