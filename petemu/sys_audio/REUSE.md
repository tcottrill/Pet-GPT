Audio and controller code is copied from the user-designated source
`C:/Source2026/AAE_publish_new_vector_test/aae/system/` (2026-11-07).
The original license headers are retained. The build uses these local copies;
it does not require that source checkout to remain installed.

Audio uses the source's portable mixer and native XAudio2 (Windows) or
PipeWire/ALSA (Linux) backends, including its existing volume, resampling,
sample playback and saturation algorithms. No replacement mixing algorithm
or third-party multimedia framework is used.

PET integration adds `mixer_flush()` and the backend `FlushStream()` operation
for modal suspension and resets. It clears pending generated PCM without
releasing stream channels. The frontend mutes with `pause_audio()` and restores
the saved volume with `restore_audio()`. The PET's 735-sample CB2 buffers remain
at 44100 Hz: `stream_set_native_rate(1,44100)` is required after stream creation
because the output backend can negotiate a different hardware sample rate.

The input files are also copied from the same source, retaining native
XInput/DirectInput/WinMM and Linux evdev acquisition plus the shared gamepad
mapping. The only host adaptation is `joystick_set_host_window(void*)`, which
receives the wxWidgets-owned native window handle on Windows instead of
depending on AAE's private window module. It is a no-op on Linux.

The original PET mixer/header and controller/header are preserved under
`backups/legacy-win32` and are not build inputs. Focused PET mixer tests use the actual mixer
with a test output backend to verify 44100-to-48000 conversion, queue flush,
pause/resume and cleanup without requiring audio hardware. The native backend
smoke test is separate and reports a skip when no audio device is available.
