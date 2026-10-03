# TAP playback design and verification

File > Load and drag-and-drop attach a TAP to cassette port 1 without resetting
the machine. File > Tape supplies Play, Stop, Rewind, and Eject. The user enters
LOAD in BASIC and presses Play; all decoding and subsequent reads use the ROM.

PetTape owns validated v0/v1 pulse intervals, position, a remaining-cycle counter,
and transport state. Invalid attachments leave the previous image untouched.
The header's capture clock is converted to the PET's 1 MHz clock with carried
rounding remainder. Legacy platform-0 headers use C64 timing. PET headers use
1 MHz. Version-0 overflow has no exact duration and is represented as 2048 source
cycles. Unsupported platforms and half-wave v2 images produce a visible error.

Pet2001IO advances the transport each emulated cycle only while Play is pressed
and PIA1 CB2 is an active-low output. Each full-wave record generates a CA1 read
event through the existing PIA edge logic; PA4 reports the active-low Play switch.
Port 2 remains released. CPU reset stops playback but retains image and position.
Rewind stops and resets position; EOF stops without rewinding. Host callbacks
expose transport actions/state without exposing PET internals to the window code.

Recording, port 2, motor inertia, tape sound, and v2 half-waves are outside this
first implementation. PRG/T64 direct loading and IEEE disk images remain separate.

Verification: `petemu/tests/run_tap_tests.bat` checks parsing, invalid media,
clock conversion, pause/resume, EOF, sense, motor gating, CA1 interrupt delivery,
and reset behavior. Supplying the absolute `roms/pet2001n/` directory additionally
captures a BASIC SAVE at the VIA write pin, then loads and runs the resulting tape
using each installed PET ROM profile. An optional second argument saves the test
TAP for interactive checks. The fixture contains `10 PRINT"TAP OK"`.

References:
- https://vice-emu.sourceforge.io/vice_17.html (TAP format and capture clocks)
- https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/pet/petpia1.c (port wiring)
- https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/datasette/datasette.c (write-edge polarity)
