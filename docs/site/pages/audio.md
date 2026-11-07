title: Audio and the media player
lede: tOS plays WAV, MP3, AAC and M4A files, has system sounds, and lets scripts play audio.
icon: media
---
## Hardware

tOS finds the sound hardware by itself. It tries the **Sound Blaster 16** first and falls back to the **ICH AC97** (the controller VirtualBox provides).

| Where | What to do |
|---|---|
| QEMU | Start with `-soundhw sb16` (or `make run-audio` from the source tree) |
| VirtualBox | In the machine's Settings, Audio, choose **ICH AC97**. Nothing else is needed |
| VMware | Use the machine's sound card setting; tOS looks for a supported one |

`soundinfo` in the Terminal tells you which audio backend is active, or the vendor and device ID of a sound card that tOS does not support yet.

## ![](icon:media) The media player

The **Music** app plays audio files. It has a seek bar, a volume control, a playlist and a loop mode. Opening it for the first time plays a short built-in jingle (a ~40-second chiptune melody made for tOS), so you can hear that sound works without any files.

| To do this | Do this |
|---|---|
| Play and pause | The play button |
| Stop | The stop button |
| Previous and next track | The previous and next buttons |
| Loop | The loop button |
| Seek | Click the seek bar, or press the left and right arrow keys (5 seconds) |
| Volume | The + and - buttons |
| Scroll the playlist | The up and down arrow keys |

Open it from the Launcher, or with `open mediaplayer`.

## Formats

| Format | Notes |
|---|---|
| **WAV** | PCM, 8 or 16 bit, mono or stereo, any sample rate |
| **MP3** | MPEG-1 Layer III, mono or stereo |
| **AAC** | AAC Low Complexity (ADTS) |
| **M4A / MP4** | AAC Low Complexity inside an MP4 container. HE-AAC is not supported |

Everything is played as 8-bit mono at 22050 Hz (the AC97 backend converts it to 16-bit stereo at 48 kHz for the hardware). The decoders were written for tOS.

## Tips

- **No sound at all?** Check `soundinfo`. In QEMU you must start with `-soundhw sb16`; in VirtualBox the audio controller must be ICH AC97. `beep test` plays a click.
- **Playing a file from the Terminal:** `python`, then `import tos` and `tos.play("/path/song.mp3")`. See [MicroPython and T#](/tos/scripting).
- **Putting music on tOS:** copy files to a USB stick, plug it in, and open them from `/Volumes/<label>` in Files, or download them with tFire into Downloads.

## System sounds

tOS plays short sounds when a window opens (a rising tone), when it closes (a falling tone) and when you click a menu or a dock icon. They are made on the fly and played through the same backend as the media player.

```
beep           # shows whether the sounds are on
beep on
beep off
beep test      # plays one test click: a quick way to check that audio works
```

## Playing audio from a script

MicroPython and T# can play files too. `tos.play(path)` figures out the format from the file's bytes, and waits until the file has finished.

```python
import tos
tos.set_volume(70)           # 0 to 100
tos.play("/mnt/song.mp3")    # blocks until the song ends
tos.stop_audio()
tos.audio_playing()
```

See [MicroPython and T#](/tos/scripting).
