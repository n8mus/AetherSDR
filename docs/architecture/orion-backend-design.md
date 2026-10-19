# Ten-Tec Orion / Omni VII backend with an SDRplay RSP2 panadapter

Status: **design, not built** (fork session 1, 2026-10-17).
Scope: N8EM's fork only. Upstream submission is not planned.

## Station and goal

The station has two parts:

- a Ten-Tec Orion 565/566 (or an Omni VII) on serial CAT; and
- a separate SDRplay RSP2, receive-only, fed from the same antenna path,
  which gives the panadapter and the receive audio.

TX audio goes from the PC to a SignaLink USB codec ("USB AUDIO CODEC" in
PipeWire), and PTT goes by CAT. The aim is for AetherSDR's own UI to run
this station: its slices tune the radio, its panadapter shows the RSP2,
and its CAT and TCI servers serve the station's other programs.

The behaviour reference is the operator's own program,
`~/orion3-sdr-console`, read-only. It is the reference for **protocol and
hardware facts only**, never for UI. Where it cites a public document
(the Ten-Tec Orion and Omni VII programmer's references, the SDRplay API
v3 specification, the K1EL WinKeyer 3 datasheet, the TelePost LP-100A
manual), that document is the protocol authority. Constitution IV
(clean room) is met: the reference program is the operator's own work,
and nothing is decompiled.

## Q1. One backend, two devices

**Do backends assume the spectrum comes from the radio?** The seam does
not. `IRadioBackend.h` says DSP location is invisible and that workers are
the backend's private business (threading rule 3). In practice, though,
every existing backend takes its spectrum from the device it controls:

| Backend | Spectrum source |
|---|---|
| Flex | Radio's VITA-49 |
| HL2, ANAN | Host FFT of the radio's own IQ |
| Icom | Radio's scope sweeps |
| RTL | Host FFT of the dongle's own IQ |

There is no "external SDR as panadapter" concept anywhere in the repo.
The nearest is the `websdr-sourced-slice.md` proposal. It is audio only,
and it explicitly refuses to draw a foreign receiver on the radio's pan.

**Design:** one family, `orion`, with one class, `orion::OrionBackend`.
It privately owns both devices, and above the seam it is one radio.

```
                      ┌──────────────── OrionBackend (owner thread) ────────────────┐
 RadioModel  ◄─seam─► │ CatEngine ── OrionCat  (ASCII, /dev/orion)                   │
                      │           └─ Omni7Cat  (binary RTS/CTS, /dev/omni7)          │
                      │ SdrEngine ── SdrPlayWorker (API v3 callback thread)          │
                      │           ├─ IqRing → spectrum FFT → spectrumFrameReady      │
                      │           └─ per-slice DDC → WdspChannel → audioFrameReady   │
                      │ TxAudioSink ── PipeWire stream → SignaLink                   │
                      └──────────────────────────────────────────────────────────────┘
```

- **CatEngine** owns a `QSerialPort` on the owner thread. QSerialPort is
  already asynchronous, so the CAT link needs no worker thread. Two
  drivers sit behind one small internal interface, modelled on the
  reference program's `RadioController`:
  - `OrionCat`: ASCII, CR-terminated, 57600 8N1, no handshake, **DTR and
    RTS asserted**. The Orion goes silent with DTR low (live-found).
  - `Omni7Cat`: binary, 57600 8N1 **with RTS/CTS**. Replies are framed by
    an expected length per prefix, because 0x0D can appear inside a
    payload.

  The poll scheduler is part of CatEngine. It replaces the reference
  program's poll loop in `MainWindow.cpp`; see the CAT facts below.
- **SdrEngine** owns the RSP2 through the SDRplay API v3. The stream
  callback only copies samples into a bounded ring (2 s, drop oldest). A
  worker does the FFT and the DDCs, and its results hop back to the owner
  thread by queued connection before any seam signal is emitted
  (threading rule 2).
- **Frequencies above the seam are always absolute RF.** The offset LO
  is hidden inside SdrEngine.

### Offset-LO arithmetic

These facts are from the reference program's `MainWindowInternal.h` and
`MainWindowTuning.cpp`:

- Capture is 2 MS/s with hardware decimation 2, giving 1 MS/s. That is
  1 MHz of usable bandwidth, using the RSP2's 1.536 MHz IF filter.
- `kLoOffsetHz = 260000`. The RSP2 LO is tuned **260 kHz above** the
  dial, so the zero-IF DC hump never lands on the tuned signal. The
  largest view clear of the hump is 1000 − 2·260 = 480 kHz.
- Bin to RF: `absHz = LO + (bin − N/2) · binHz`. With a 16384-point FFT,
  a bin is about 61 Hz.
- `edgesFromRig(mode, bw, pbt)` gives the passband edges relative to the
  dial. AM uses the full width; USB is +half and LSB is −half; PBT is
  signed by mode. Port this as a pure function, with a unit test.
- LO policy:
  - Classic: LO = dial + 260 kHz.
  - CTUN-style hold: keep the LO while |LO − dial| ≤ 440 kHz.
  - Band frames: LO = frame centre + span/2 + 20 kHz guard.

  In AetherSDR this policy lives behind `setPanCenter(panId, hz,
  intent)`:
  - **Drag** moves only the RSP2 LO (the view). The radio is not retuned.
  - **Range** sets the view, subject to the 480 kHz limit.
  - A slice tuned outside the view triggers an automatic recentre
    (compare `recenter-policy.md`).
- The pan reports `panCenterBandwidthChanged` and `SpectrumCoverage` in
  absolute RF, with the DC-hump region excluded from the coverage
  bounds. ("Bounds describe the genuine usable samples.")
- Amplitude is **not** calibrated dBm. Set `panAmplitude` honestly, as
  the Icom backend does (its Gap D). An S-meter calibration offset
  (radio dB − SDR dB) can be offered later as an extension.

### RSP2 facts to carry over

- **Device and front end.** Pick the first device with
  `hwVer == SDRPLAY_RSP2_ID` and use Tuner A. Antenna A is the Orion
  feed; antenna B is the Omni feed. Make the antenna a backend setting.
- **Gain.**
  - Hardware AGC is off. DC and IQ correction are on.
  - gRdB is clamped to 20–59 and LNA to 0–8. Defaults are gRdB 40 /
    LNA 6, because the shared feed is hot.
  - Live changes use `sdrplay_api_Update`: `Tuner_Gr`, `Tuner_Frf`,
    `Rsp2_AntennaControl`, `Rsp2_RfNotchControl`.
- **Overload.** Acknowledge `PowerOverloadChange` with
  `Update_Ctrl_OverloadMsgAck`. During TX, park the gain at 59/8 (TX
  desense guard) and restore it after a 1–3 s hang.
- **Broadcast notch** only works at LNA ≤ 2. "Notch mode" parks the gain
  at LNA 1 / gRdB 59. Any manual gain change leaves notch mode.
- **Auto-gain** (`AutoGain.h`) is a small pure state machine and ports
  as is:
  - Peak above −4 dBFS adds one LNA step.
  - Below −12 dBFS for 30 s removes one step.
  - 2.5 s hold between steps, 10-minute lockout after an attack,
    minimum LNA 3.
- **Performance.** Build optimised (RelWithDebInfo or Release). A Debug
  DSP build makes the SDRplay service silently drop transfers to about
  40%. Report the delivery rate through `linkStats()` / `healthSnapshot()`
  and raise `configurationWarning` below 95%.
- **Service wedge.** After an unclean exit, the sdrplay_apiService
  handshake can wedge and the pan stays dark. The reference program has
  no recovery code. Here, `connectRadio` should report the failure
  clearly; whether to retry `sdrplay_api_Open` once is an open question.
- **Not bundled (fork rule 5).** Add an `ENABLE_SDRPLAY` option on the RTL
  pattern: find `sdrplay_api.h` and `libsdrplay_api`, and if they are
  missing, build CAT-only. The packaging scripts must not copy the
  library.

## Q2. Slices

| | Orion 565/566 | Omni VII |
|---|---|---|
| `maxSlices` | 2 | 1 |
| Slice 0 | Main RX = VFO A (`*AF`, `R M…`) | VFO A |
| Slice 1 | Sub RX = VFO B (`*BF`, `R S…`) | — (VFO B used only for split TX) |
| `maxPanadapters` | 1 | 1 |

**Audio.** Both slices are demodulated by the host from the RSP2 capture,
through AetherSDR's WDSP (`WdspChannel`). This matches the reference
program, which already uses AetherSDR's vendored WDSP 2.10. Watch out for
two things:

- **WDSP handedness.** The reference program conjugates the RSP2's IQ
  before WDSP. Verify this against `WdspChannel` with a test tone.
- **Rates.** Integer rate ratios and nested power-of-two block sizes are
  required. Both were ASan findings in the reference program.

If slice 1 is tuned outside the 1 MHz capture, the radio is still tuned,
but slice 1 has no SDR audio. The backend reports this, and AetherSDR's
UI dims the slice. (The UI rule is "dimmed, not hidden".) The operator
has accepted this behaviour.

**Split.**

- `setTxSlice(1)` maps to Orion `*KV[mainRx][subRx][tx]` with main RX on
  A and TX on B (the reference program writes this as `*KVA?B`; `N` means
  unassigned). Check the sub-RX letter it sends before porting. On the
  Omni VII it maps to `*N1`.
- The operator's habit is kept. He sets split by hand, tuning B onto a
  clear spot. When TX moves to B and B is not locked, park B at
  **A + 1 kHz (CW) or A + 5 kHz (voice)** with `*BF`. Do not send a mode,
  because the radio's menu copies it.
- Snapshot the previous routing **before** the CAT call. The Omni driver
  reports routing synchronously (live-found).
- VFO lock (`*AL/*AU/*BL/*BU`) only freezes the radio's knob; CAT still
  tunes. The backend therefore enforces the lock on slice tuning itself.

**Radio is authoritative (Constitution II/III).** Like Icom, this backend
declares no client settings domains and is never pushed a restored state.
Echo guards from the reference program:

- 1.5 s after a slice edit, ignore the polled frequency.
- 2 s for filter and notch.
- A jump of more than 100 kHz needs two agreeing reads.
- A polled mode change needs two agreeing reports.

### CAT facts to carry over (Orion)

**Frequency and mode**

- Frequency: `*AF<hz>` / `*BF<hz>`, read back with `?AF` / `?BF`. Gate
  frequencies to 100 kHz–60 MHz on both send and parse.
- Mode: `*RMM<n>`, with USB 0, LSB 1, UCW 2, LCW 3, AM 4, FM 5.
  - `*RSM` is dead on v3 firmware.
  - Send a mode command **120 ms after** a retune. The Orion drops
    commands while busy.

**Filter, AGC and front end**

- Filter: `*R<M|S>F<100–9000>`. AM allows up to 9000 Hz; other modes stop
  at 6000. The radio **rejects** out-of-range values rather than clamping
  them, so clamp before sending.
- PBT: `*R<M|S>P<±8000>`. A passband (lo, hi) becomes width = hi − lo and
  PBT = (hi + lo)/2. This maps directly onto `setSliceFilter`.
- AGC: `*R<M|S>A<F|M|S|P|O>`.
  - Programmable AGC also has threshold (`AT`, µV), hang (`AH`, 0–10 s)
    and decay (`AD`, 5–500).
  - Decay values under 5 are silently rejected.
- RF gain `G` 0–100. Attenuator `T` 0–3. Preamp `E`.
- DSP: `NR` (v3 moved it from `NN`), `NB`, `NA` 0–9, `NH`.
- Manual notch, undocumented: `NC` 20–4000, `NW` 10–300, `NM`, `NS`.
  Clamp before sending.
- AF volume `*UM/*US` is a **byte scale, 0–255**.

**Routing**

- Antenna routing: `*KA[ant1][ant2][rxant]`. TX follows the main
  receiver's port.

**Meters**

- S-meter: `@SRM<m>S<s>`, raw 0–300 for each receiver. Convert to dB
  relative to S9 with the Hamlib TT565 V2 table:

  ```
  {10,-48},{24,-42},{38,-36},{47,-30},{61,-24},{70,-18},{79,-12},{84,-6},
  {94,0},{103,10},{118,20},{134,30},{147,40},{161,50}
  ```

  Interpolate linearly and clamp at both ends. Port this as a pure
  function and emit it through `meterUpdate`.
- TX meter: `@STF<fwd>R<ref>S<swr>`. On v3, SWR is 8.8 fixed point
  (÷256) unless the field contains a '.'.
  - Example: `@STF024R004S608` is 24 W forward, 4 W reflected, SWR 2.38.
  - Hold the TX meter face for 1.5 s. In CW the radio answers in RX form
    between elements.

**Polling**

- 200 ms tick in a five-phase rotation:
  - `?S` twice per rotation (2 Hz).
  - `?AF` once at a time, re-armed after 600 ms.
  - A slow wheel covers the rest: mode, filter, `?BF`, AGC, gain, `?KV`,
    `?KA`, volume, TX settings, locks, CW, notch. A full lap is about
    70 s.
- Burst mode: within 800 ms of a dial move, give the S-meter slots to
  `?AF`.
- The Orion services its UART about every 100 ms. Round trips are
  bimodal: about 17 ms median with a 110 ms tail.
- Coalesce drag writes at 40 ms.

**Recovery.** The reference program has no reopen logic. Add one here:
`ioError` leads to `disconnected()`, and AetherSDR's normal reconnect
takes over.

### CAT facts to carry over (Omni VII)

- Frequency: `*A` / `*B` with a 4-byte big-endian value.
- Mode: `*M<A><B>` sets **both VFOs at once**, first character for A.
  The programmer's table is wrong here. Codes: AM 0, USB 1, LSB 2,
  CWU 3, FM 4, CWL 5, FSK 6.
- Filter: `*W<idx>`, the nearest of 38 presets from 14000 down to 200 Hz.
- PBT: `*P`, signed 16-bit, ±8192.
- AGC: `*G` '0'–'3'.
- RF gain `*I` 0–127. Despite the documentation, the scale is **not**
  inverted.
- AF volume `*U` 0–127. NB/NR/AN `*K` (all three at once). Split `*N`.
- **PTT:** `*T` 0x04 d0. Send a keepalive every **2 s**, because the radio
  drops TX after 5 s of silence. Unkey is `*T 0x00 0x00`.
- S-meter: the "SABCD" ASCII reply converts to dB relative to S9.
- The `C1`/`C2` TX group only works in REMOTE mode. In RADIO mode, sets
  return `Z` and queries go unanswered.
- On a band change, re-query the antenna (`?C1V`). The Omni recalls a
  per-band antenna.
- The Omni VII Ethernet (UDP REMOTE and RIP audio) is **out of scope for
  v1**.

## Q3. TX audio and PTT

**Capabilities:** `canTransmit = true`, `hostModulates = false` (the
radio modulates), `takesTxAudioOverSeam = true`. This is the same shape
as Icom. AetherSDR's mic chain (`TxVoiceProcessor`), and TCI/DAX audio
from WSJT-X and fldigi, therefore reach `submitTxAudio()` as 24 kHz Int16
stereo. Existing backends send that audio over the radio's own link. Here
the backend sends it to the **SignaLink**:

- `TxAudioSink` opens a PipeWire playback stream on the SignaLink node.
  - Use `pw_stream` under `HAVE_PIPEWIRE_NATIVE` where available,
    otherwise a `QAudioSink` matched by device description.
  - Never use `plughw`; it would lock other programs out.
  - Match the full node name, `BurrBrown_from_Texas_Instruments_USB_AUDIO_CODEC`.
    The bare "USB AUDIO CODEC" also matched an Alesis mixer's PCM2900
    (live-found).
  - Convert 24 kHz to 48 kHz mono once, at connect.
- `finishTxAudio()` returns the milliseconds still queued in the stream,
  so PTT is held through the tail. The reference program drains 250 ms.
- **Two TX audio paths, both kept (operator decision).** The radio's TX
  input stays set to BOTH on the front panel, and the path is chosen by
  gain through CAT, as the reference program's DIG and FreeDV buttons do:
  - **Radio mic** (front-panel jack): aux 0, plus the saved mic and
    processor levels. AetherSDR sends no TX audio.
  - **PC audio** (SignaLink): mic 0, processor 0, aux 100. This is used
    for digital modes, FreeDV, and voice from the PC mic. A PC mic is
    needed for AetherSDR's TX EQ and processing to apply, so in this path
    the radio's own processor is off.

  The operator picks the path with one AetherSDR control (an extension
  verb), not automatically by `TxAudioSource`. Digital and FreeDV
  operation select PC audio; the reference program's
  `audio/voiceMic` (51) and `audio/voiceSpeech` (2) defaults seed the
  radio-mic levels, held in this fork's own settings.
- **The Orion must still work stand-alone.** On disconnect, on exit, and
  in the destructor, put back the radio-mic profile (PTT off first). The
  radio is then usable from its front panel and mic with the PC off.
  Never change the front-panel input selection or other menu settings.

**PTT** is in `setKeying(on, op, completion)`:

| Radio | Key | Unkey |
|---|---|---|
| Orion | `*TK` | `*TU` |
| Omni VII | `*T` (keepalive every 2 s) | `*T 0x00 0x00` |

- The keep/deny decision is made above the seam (Constitution VI). The
  backend only translates it, and **never refuses an unkey**.
- Key, then start audio after **300 ms** (the reference program's
  lead-in).
- Force PTT off on disconnect and in the destructor.
- Set `hasRadioPttReadback = true` only if the TX state can really be
  read. The Orion's `@STF` replies during TX are indirect evidence;
  confirm this on the air before claiming it.
- During TX, SdrEngine applies the desense guard (gain 59/8) and keeps
  clipped IQ out of the decoders.

**CW goes through the WinKeyer only (operator decision).** The backend
does not use the Orion's internal keyer (`/c` commands), so it declares
`hasRadioSideCwKeyer = false`. AetherSDR's CW text, the cwdaemon port and
the paddles all go to the WinKeyer peripheral (see Q4). Rule 3 applies:
no unattended TX or CW tests.

## Q4. Other station devices and services

| Device / service | AetherSDR today | Plan |
|---|---|---|
| LP-100A wattmeter (`/dev/lpmeter`, 115200, `;P?`) | **Has it:** `LpMeterProtocol`, `LpMeterConnection`, `LpMeterApplet` (a peripheral, outside the seam) | Use theirs. Compare with the reference program before relying on it: the 43-byte frame, `F` not `M`, Z valid only at ≥0.5 W, 5-poll dead timeout |
| PowerMaster (38400, CRC-8 0xB1) | No | Low priority. If needed, a peripheral beside the LP-100A code, which is a hook-up point |
| WinKeyer WK3 (FTDI `A904QF5Z`, 1200 8N2) | No (host keyer `IambicKeyer`, `CwxLocalKeyer` only) | Port as a peripheral: open sequence, `isWk3()`, re-send only adopted settings, never write the mode register unless opted in |
| DCU-3 rotor (`/dev/portS4`, 4800, streams while turning) | No (Green Heron only) | Port the DCU-3 driver plus a rotctld server on :4533, so cqrlog and Not1MM keep working |
| rigctld :4532 (fldigi, cqrlog, WSJT-X, Pat) | **Has it:** `CatPort` Rigctld dialect, default slot 0 on **4532** | Use theirs (operator decision), provided it passes **Pat's exact sequence** from the reference program's `tests/rigctldtest.cpp` (one-line `dump_caps`, `chk_vfo`) and cqrlog's `+` extended form. Write that as a test |
| TCI :50001 | Has it | Bonus: WSJT-X and others can use TCI with audio |
| cwdaemon UDP :6789 (cqrlog CW) | No | Port, driving the WinKeyer. Prosign mapping is in the reference program's `CwWindow.cpp` |
| DX-cluster server :7300 (skimmer spots, "HELLO") | Client only | Only needed if the skimmer is ported. Later |
| cqrlog UDP ADIF :2334 | No | Port if the operator logs from this program. Later |
| Pat websocket monitor (blocks voice TX during Winlink) | No | Port. It is a TX-inhibit input, so it must feed the above-seam TX gate, not the backend |
| Not1MM log reader | No | Later |
| CW decoder / skimmer, RADE | AetherSDR has its own CW decode and FreeDV | Use theirs first (brief: "optional, later") |

Services that are not in the backend belong outside
`src/core/backends/orion/`. Each one is a peripheral and a hook-up point,
so each needs an entry in `FORK-NOTES.md`. Consider a single
`src/fork/` folder so they stay together for merging.

## Q5. Hook-up points (expected)

The minimum to make the backend selectable:

1. `CMakeLists.txt`: an `ENABLE_ORION` block (sources, plus an optional
   `ENABLE_SDRPLAY` with `find_library(sdrplay_api)`). Copy the RTL block
   at about line 2867.
2. `src/models/RadioModel.cpp`, `makeBackend()`: one `orion` branch.
   `populateFamilyParams()`: device paths, model, and SDR antenna in
   `req.params`.
3. `src/gui/ConnectionPanel.{h,cpp}`: a family constant, the manual
   radio-type combo entry, and family normalisation. The device path
   travels in `RadioInfo.serial` or `params`.
4. `tests/tests.cmake`, plus the family lists in
   `tests/backend_family_switch_test.cpp` and
   `tests/backend_seam_affinity_test.cpp`.
5. `docs/architecture/radio-capabilities-map.md`: an `orion` profile
   table. Upstream requires every field to be set explicitly.

Later: discovery (`LocalRadioDiscoverySource.cpp`, optional), settings
widgets, peripherals. Note that `TciServer.cpp:2424/2684` special-cases
`family()=="icom"` for unkey settle. Check whether `orion` needs the same.

Constraints from upstream that still apply:

- Don't use `QSettings`; use `AppSettings`.
- No `core/` file may include a `gui/` header (CI ratchet EB1).
- No vendor headers above the seam (EB3).
- No `family()` branches above the seam beyond the factory.
- No `dynamic_cast` to the backend.
- New capabilities go in as `std::optional` records.

**Settings never touch the reference program's QSettings**
(`n8mus/tentec-console`), per fork rule 7.

## Q6. First buildable step

**Step 1: CAT-only Orion backend against a fake radio.** No SDR, no TX.

- Add `src/core/backends/orion/`:
  - `OrionBackend` (seam implementation, capabilities with
    `canTransmit=false` for now)
  - `CatLink` (QSerialPort, line framing, DTR/RTS)
  - `OrionCat` (command builders and reply parser, pure and
    unit-testable)
  - `OrionPollScheduler`
  - `SMeterCal` (the table above)
- Wire hook-up points 1–4 and document them in `FORK-NOTES.md`.
- Add `tests/orion/fake_orion.py`, a pty fake that **answers**: `?AF`,
  `?BF`, `?S`, mode and filter queries, using a small state machine. The
  reference program's `fake_radio.py` never answers, so this is new. Also
  add Qt unit tests for the parser and the S-meter table.
- Done when:
  - `cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo` builds.
  - The tests pass.
  - With `fake_orion.py` on a pty, AetherSDR connects as family `orion`.
    Slice 0 follows `?AF`, tuning slice 0 sends `*AF…`, the S-meter
    moves, and disconnect is clean.
  - No real radio, no TX.

**Step 1 on-air result (2026-10-17, real Orion on `/dev/orion`):** both
VFOs appear and follow the radio, the mouse wheel tunes it, and the S-meter
works with slight lag. VFO B tuning was sluggish: the poller sped up only
for dial A. Fixed by giving the active slice's dial the fast slots and
lending the burst slots to whichever dial is moving.

**Step 2 (built and on-air checked 2026-10-17):** RSP2
spectrum through `Rsp2Receiver` (SDRplay API loaded with `dlopen`, never
linked) and `OrionSpectrum` (offset-LO view rules, 16384-point
Blackman-Harris FFT, peak-per-pixel crop). The LO is a function of the
view: a guard above its top edge, so the default 480 kHz view centred on
the dial reproduces the 260 kHz offset exactly. Bench run against the real
RSP2 with a fake CAT radio: LO followed tuning and pan drags, delivery
98-104% per second and 100% over 5 s windows, no overloads, clean release
and reopen. Levels are uncalibrated (`Rsp2Receiver::kLevelOffsetDb`).
On air: stations show in the pan at the frequencies heard on the Orion,
and tuning follows. Tune by double-click (AetherSDR's default; View ->
Single-Click to Tune for one click). SPLIT is refused until step 4.

**Step 3 (built and heard on air 2026-10-17):** slice
A's receive audio from the RSP2 through WDSP (`OrionRxAudio`). The stream
callback copies each IQ block to a queue; the audio thread mixes the dial to
0 Hz, resamples 1 MS/s to 96 kHz with one complex polyphase filter
(x12 / 125: WDSP needs whole-number ratios, and separate I and Q
resamplers would slip against each other), and runs WDSP RXA at 96 / 48 /
24 kHz. Findings pinned by `orion_rxaudio_test`:
- WDSP needs the RSP2's IQ conjugated (as the reference program found).
- WDSP must run in blocking mode on this dedicated thread. Fed in bursts
  non-blocking, its output ring falls out of step and sideband rejection
  drops to about 30 dB; blocking, it is over 150 dB.
- CW tunes the dial minus the pitch and demodulates around the pitch.
First on-air connect failed: WDSP's cold open (FFTW planning, 12 s) ran on
the GUI thread, and the CAT watchdog then took our own stall for a silent
radio. WDSP is now built on the audio thread, and a late poll tick restarts
the silence clock. Slice A only; slice B audio is step 4. Mute, gain and balance apply to the
speaker feed; the per-slice feed (TCI, decoders) is pre-mute.

**Step 4 (built and on-air checked 2026-10-17):**
- Slice B (sub receiver) audio: a second `OrionRxAudio` on the same RSP2
  stream, mixed with A for the speaker (sample-aligned; a stalled chain
  plays as silence after 0.2 s). Each slice keeps its own pre-mute feed.
  A receiver more than 460 kHz from the LO (the capture's +-500 kHz less
  the resampler's 40 kHz) is silent and reported `inCapture = false`.
- Split: AetherSDR's TX-slice control now reaches the backend
  (`requestSliceControl`), which sends `*KV<main><sub><tx>` and reads it
  back. Moving TX onto B parks B at A + 1 kHz (CW) or + 5 kHz (voice)
  first, unless B is locked; moving it back leaves B alone.
- VFO locks (`?AL`/`?BL`) are read every fast-wheel lap (~7 s), shown on
  the slice, and enforced on CAT tuning (the radio's lock freezes only
  its knob).
Routing only: nothing in step 4 keys the transmitter.
On air (2026-10-17): split works. AetherSDR's right-click SPLIT offsets
(1, 5 and 10 kHz up) all reach the Orion. A plain left-click on SPLIT is
still refused ("Split entry ignored"): that path creates a Flex slice and
the Orion's two receivers are fixed. Both VFOs are heard together from the
RSP2 after one fix: B's demodulator had only been retuned on A's changes,
and AetherSDR's per-slice audio controls arrive as requestSliceAudio.

**Step 6 (built 2026-10-17, bench-tested on a fake radio only; the first
real transmission is the operator's):**
- PTT by CAT (`*TK`/`*TU`). A key needs an operation admitted by the TX
  coordinator; an unkey is never refused. MOX comes only from the radio's
  readback: its `?S` reply turns into `@STF` while it transmits (1.5 s
  hang for CW), which also drives TX:FWDPWR, TX:REFPWR and TX:SWR.
- Two TX audio paths in the Phone applet's mic-source list: MIC (the
  radio's front jack; aux 0, the operator's mic and processor levels) and
  PC (AetherSDR's chain to the SignaLink; mic 0, processor 0, aux 100).
  The list reaches the model through one RadioModel hook (FORK-NOTES).
- SignaLink output (`OrionTxAudio`): its PipeWire node by full name, 48 kHz.
  Audio passes ONLY while we keyed the radio on the PC path and the radio
  reports TX (or 300 ms after the key): a SignaLink keys a radio by itself
  when audio reaches it. Unkeying drops anything queued.
- While transmitting: RX audio muted, RSP2 parked at gRdB 59 / LNA 8,
  restored 1.5 s after.
- Failsafes: a 3-minute key limit; disconnect, link loss and exit unkey
  first, then put the radio-mic profile back (stand-alone rule); a radio
  found left in the PC profile is given its mic back on connect.
- Not yet: the Omni VII's keepalive PTT.
- TUNE (built 2026-10-17, bench-tested on the fake radio only): the
  reference program's manual tune. The TUNE drive (*TP), the main
  receiver to FM for a steady carrier at the dial (*RMM5), then *TK. It
  stops itself after 15 s. Off: *TU FIRST, then the mode and the drive go
  back (RF power moved mid-tune is what goes back). The slice keeps
  showing the operator's mode and the RF power slider never shows the TUNE
  drive (readback held while tuning and 1.5 s after). No PC audio while
  the carrier is up. Disconnect or exit mid-tune unkeys and restores
  (stand-alone rule). TUNE power changes apply live. Offered in CW too,
  since the carrier is FM. On air 2026-10-17 (operator): the TUNE carrier
  keys, and the log shows mode and drive (7%) restored after each one.
- ATU (built 2026-10-17, bench-tested on the fake radio only): the
  internal tuner, as the reference program drives it. ATU = *TT1 then
  *TTT; the radio keys its own carrier for the cycle and drops it when
  done. Status "in progress" until the radio is back in RX, then
  successful if the last TX meter SWR was 2.0 or less, failed if higher.
  No carrier within 6 s = failed, with a notice (tuner not fitted or not
  enabled); a cycle past 30 s is stopped with *TU. Bypass = *TT0 (and
  stops a cycle). The tuner in/out state is read with ?TT on connect and in
  the slow poll wheel, so the front panel's change shows too. A cycle needs
  the TX coordinator's admission and never runs over our own key or TUNE.
  On air 2026-10-17 (operator): the cycle works. The Orion started its
  carrier 2.9 s after *TTT (so the start limit went from 3 s to 6 s), ran
  about 3 s, and ended at SWR 1.0: reported successful.
- MON (built 2026-10-17, bench-tested on the fake radio only): the Phone
  applet's MON button and level drive the radio's own TX monitor (*TO,
  0..100, as the reference program sets it); MON off sends *TO0, and the
  @TO readback shows on/off and the level (0 = off, the level slider kept).
  Until now MON was refused ("Backend refused operation monitor level").
  VOX stays unsupported: the reference program has no Orion VOX command.
  On air (operator): in CW there is no MON; what matters there is the
  sidetone.
- CW sidetone (built 2026-10-17; on air the same day, operator: the level
  and the button change the Orion's sidetone): the CW tab's Sidetone
  button and level drive the radio's own sidetone volume (*CV, 0..100, as
  the reference program sets it); off sends *CV0, a level moved while off
  waits, and @CV is read back on connect and after each change. Pitch
  already works on air (receive tone and the Zero Beat target together;
  the operator matches it to the Orion). Not wired: the CW tab's Delay.
  The reference program's Orion QSK delay is *CQ on a 0..100 scale with no
  stated unit, while AetherSDR's Delay is milliseconds; it needs an on-air
  measurement before any mapping.
- On air (2026-10-17, operator at the radio, dummy load): MOX keys and
  unkeys the Orion by CAT, with the radio's TX/RX readback each time. The
  PC path works end to end: the Alesis mic (PCM2900) -> AetherSDR's TX
  chain -> SignaLink -> Orion, 6.5 W out at a 7 W setting. Operator setup
  it needs, all AetherSDR's own: PC Audio on (title bar), and Radio Setup
  -> Audio -> Input = the PCM2900. A leftover capability line had hidden
  the MIC/PC choice; fixed. Key edges, radio TX/RX reports and path
  changes are logged (aether.orion). The MIC path checked too: on MIC the
  front microphone modulates, on PC it does not (only the SignaLink does),
  as designed.

**Step 7 (built 2026-10-17, bench-tested against fakes only):** the station
peripherals, owned by the Orion session (opened on connect, released on
disconnect).
- WinKeyer (WK3, FTDI `A904QF5Z`, 1200 8N2): the reference program's open
  sequence on timers (no blocking sleeps). AetherSDR's CW text keyer is the
  WinKeyer once it answers; a send needs the TX coordinator's admission and
  completes when the keyer reports idle. No shaping parameter and never the
  mode register is written: the keyer keeps its owner's stored setup. Speed
  from AetherSDR's CW speed; the pot reports back.
- DCU-3 rotor (`/dev/portS4`, 4800 8N1) read as a stream; target, 500 ms
  gap, execute. rotctld on loopback :4533 with the reference program's
  replies (RPRT -5 with no reading, never a fake 0.0).
- cwdaemon on loopback UDP :6789 into the WinKeyer, cwdaemon prosigns
  translated to 0x1B merges. As in the reference program it keys through
  the WinKeyer's own line, not AetherSDR's TX gate; every send is logged.
- Pat monitor (ws://127.0.0.1:8080/ws): WARN ONLY (operator decision). A
  Winlink session raises a notice and an `orion/winlink` extension status;
  keying is never blocked (Pat itself keys through AetherSDR's rigctld).
On air 2026-10-17 (operator): the WinKeyer opened as a WK3; text typed in
the WinKeyer panel sends, Esc and a paddle touch both stop it, and the
panel's Speed changes the keyer (after a fix: the CW speed control was
refused by the backend). The keyer's speed knob now sets the sending speed
(after a second fix: the knob speed is sent back to the keyer, as the
reference program does). Health shows the rotor's heading, rotctld,
cwdaemon and the Pat watch all up. Deferred by the operator ("get the
program working before the accessories"): turning the rotor, cqrlog over
cwdaemon (not used here), Pat's warning, and a rotor control in AetherSDR
(it has none for a DCU-3; rotctld clients only).
Not ported (still optional): PowerMaster, skimmer and DX-cluster server,
cqrlog ADIF sync, Not1MM log reader.

**Panadapter display (2026-10-17, bench-tested only):** the operator found
the pan weaker than the console's. Nothing of AetherSDR's Display panel was
missing (schemes incl. Purple, waterfall gain/black level, FFT controls);
it was mostly set poorly (FFT AVG 0, waterfall gain 29). Three real gaps,
now closed the way the RTL backend does it:
- FFT AVG is AetherSDR's shared time-based averager
  (`SpectrumTemporalAverage`): 10 ms per step, 0 off, 100 = 1 s, the same
  at any FPS; history slides on the RF grid across a retune (a gain/LO
  change reported by the RSP2 still restarts it).
- Wt Avg reaches it (off: average power; on: average dB).
- `backendPanAveraging` with client persistence, so both settings come
  back next session (FFT AVG had been sitting at 0).
The console's own appearance is not brought over (fork rule 2).

**Radio AF volume (2026-10-18, bench-tested on the fake radio only):**
PC Audio decides where a slice's volume and mute act. On: the RSP2 audio
in AetherSDR, as before. Off (listening to the radio): slice A drives the
Orion's main AF (*UM), slice B the sub (*US), on the radio's byte scale
(percent x 255 / 100; *U100 is only ~40 % of full audio, reference
program). Mute = 0 with the level kept; a level moved while muted waits.
?UM / ?US are read on connect, in the slow wheel and when PC Audio turns
off, so the radio's own knobs show on the sliders; each mode keeps its own
level. Disconnect gives a muted receiver its level back (stand-alone
rule). Balance stays RSP2-only. The radio's phones/speaker routing (*UC:
left, right, speaker each A, B or both) is on Radio Setup -> Orion Audio
(the fork's own page, AetherSDR widgets), read with ?UC on connect. PC Audio reaches the backend through the "orion" extension
namespace (audio.pc / audio.pc.state; hook in RadioModel, FORK-NOTES).
Also 2026-10-18: Center Lock + Zero Beat crash fixed (pan centre echo
loop); serial device remembered and startup auto-connect for serial
radios.

**Step 8, CW skimmer stage 1 (2026-10-18, tested on synthetic IQ only):**
the Orion console's skimmer, ported (src/core/backends/orion/skimmer/: its
engine, fldigi and Bayes decoders, and DX-cluster server, as is apart from
the namespace and the engine watching the LO rather than the dial, since
here the LO follows the pan view). The RSP2 callback copies IQ into
OrionSkimmerFeed's ring; its own thread feeds the decoder bank (OpenMP
across cores when found, as the console builds it). Every 2.5 s the RSP2
worker posts the whole capture's spectrum; OrionSkimmer lays the channels
out on the band's CW segment from it, and only then lets blocks taken at
that LO through, so a pan move never feeds the decoders the wrong offsets.
Calls are checked against AetherSDR's cty.dat and cqrlog's MASTER.SCP
(the console's confidence tiers). Finds go to the telnet server on
localhost:7300 (cqrlog, Not1MM) and, as an "orion"/"skimmer.spot"
extension event, onto the panadapter through the RBN spot path (violet,
10 min). Radio Setup -> Orion: on/off (default off), pause outside CW
(default on), Bayes, channels (24), port (7300); it also pauses while the
radio transmits. Health shows status, channels decoding, the telnet feed
and dropped IQ.

**Skimmer stage 2 (2026-10-18, tested on synthetic IQ; the windows not yet
seen on screen):** Tools -> Skimmer band map... and Skimmer waterfall...,
the console's windows re-expressed in AetherSDR's widgets and colours.
- Band map: the skimmer's finds sorted by frequency (call, WPM, age), calls
  in their worked-before colour, channels still hunting as dim "?" rows;
  click a row to tune the active slice there (the Log QSO window then picks
  the call up from the spot at that frequency).
- Waterfall: the console's SkimStft, ported on FFTW (4096-point FFT at
  ~125 ksps, ~30 Hz bins, ~8 ms columns) over a slice of the capture
  centred on the band's CW segment, frequency up the side, time flowing
  left, keying visible; decoded calls and their live copy ride beside the
  traces in worked-before colours; click to tune, wheel to zoom, drag to
  pan, scroll speed x1/x2/x4. Colours: the operator's AetherSDR waterfall
  scheme (read from the panadapter) and AetherSDR's theme, not the
  console's ramp. Its STFT has its own IQ feed thread (every block, any LO)
  that runs only while the window is open.

**Step 9, logbook stage 1 (2026-10-18, tested without cqrlog only):** the
Orion console's station log, ported (src/core/backends/orion/log/: LogDb on
Qt SQL/SQLite with the console's schema, Adif, CqrlogSync), with AetherSDR's
settings (OrionLog/*) and its cty.dat in place of the console's. AetherSDR
keeps its OWN log file (~/.local/share/AetherSDR/logbook.sqlite), separate
from the console's; cqrlog stays the shared award log. Both directions of
the cqrlog bridge: each QSO is mirrored to the bridge (UDP localhost:2334,
one headerless ADIF record), and while cqrlog runs its new QSOs are pulled
in every 2 min through the mysql client on cqrlog's socket (the first pull
brings in cqrlog's whole history; near-duplicates are skipped and imported
QSOs are marked as already uploaded). GUI in AetherSDR's own widgets
(src/gui/orion/OrionLogbookUi): a Logbook menu (Log QSO... Ctrl+Alt+L,
Import/Export ADIF, the two cqrlog switches, the log file shown) and a Log
QSO window: frequency/mode from the active slice, the call from a spot on
that frequency, the UTC clock running until edited, country and zones from
cty.dat, name/QTH/grid carried from the last QSO with the station, a
near-duplicate question, the last 8 QSOs with the station.

**Step 9, logbook stage 2 (2026-10-18, tested without cqrlog or WSJT-X
running):**
- Logbook browser (Logbook -> Logbook... Ctrl+Alt+B), the console's window
  re-expressed in AetherSDR widgets: the whole log as a table (search by
  call, sort, edit in place, delete with a question; bookkeeping columns
  hidden), ADIF in/out, and the console's on-demand "check against cqrlog"
  (lists QSOs cqrlog lacks; sends them only on the operator's yes).
- Worked-before colouring: the console's LogbookIndex, ported, merges this
  log with cqrlog's own worked/confirmed summary (mysql on its socket while
  it runs, cached between runs, so confirmations made in cqrlog later
  show). Spots take the console's colours (red never worked, amber other
  band, green worked this band, grey confirmed) through AetherSDR's spot
  marker colour, after DXCC colouring and below the operator's override;
  markers redraw when the log changes. Menu toggle.
- WSJT-X auto-logging: the console's WsjtxListener, ported (multicast
  224.0.0.1:2237, port shared with AetherSDR's own WSJT-X client): every
  "Logged ADIF" QSO lands in the log (country from cty.dat, resends
  skipped), with a status-bar note; NOT mirrored to cqrlog (cqrlog takes
  WSJT-X's QSOs itself, as with the console). The DX WSJT-X is working
  fills an open, empty Log QSO window (call, grid, mode). Menu toggle.
Later: the contest deck.

**Step 9, online logs (2026-10-18, tested against local stand-ins only;
nothing has been sent to a real service):** the console's QslUploader,
ported: each logged QSO (and WSJT-X's, unless OrionLog/wsjtxPush is off)
goes at once to every enabled service, every outcome announced (accepted,
duplicate = delivered, failed = 'E'), per-QSO state in the log's up_*
columns; one catch-up sweep and one LoTW QSL download shortly after
startup, otherwise Retry (no background retry loop, the operator's call).
LoTW by the tqsl CLI (batched, -l station, -p only with a key password),
eQSL GET, QRZ logbook POST (POTA/SIG stripped), ClubLog realtime POST
(needs an application API key), HRDLOG POST; HRD Logbook (TCP) and N1MM+
(UDP) mirrors; LoTW QSL report -> confirmations. Imported history is
stamped uploaded and never sent. Differences from the console: secrets
(passwords, API keys, upload codes) live in the system keychain
(AetherSDR's only credential store), not settings; program id AetherSDR;
each service's address is a setting (default the real one) so tests use
a stand-in. Logbook -> Online logs... holds the switches, fields, Test
buttons, traffic list, Retry and LoTW QSL sync.

**Step 10, the CW window and the panadapter's two averages (2026-10-19,
bench-tested against the fake radio and the real RSP2, receive only):**
- **The CW window** (Tools -> CW window..., Ctrl+Alt+W), the Orion
  console's `CwWindow` re-expressed on AetherSDR's own plumbing
  (`src/gui/orion/OrionCwWindow`). Fork rule 2 was amended by the operator
  to allow the console's appearance here, because this window replaces
  AetherSDR's CWX panel at this station. From the console: the layout and
  the dark shack-distance styling, the type-ahead line with Word keys and
  Live keys, the DX box, F1-F12 macros on three rows of four with
  right-click editing, and the plain-language macro vocabulary
  (`[his call] DE [my call] UR RST [rst sent]`, with the old %c / %mc
  spellings still keying; `core/backends/orion/CwMacros`, ported). From
  AetherSDR: every byte on the air goes through `CwxModel`, so the TX
  coordinator, the TUNE interlock, the sidetone and the backend's own
  WinKeyer path are exactly the CWX panel's; the decoded CW in the RX pane
  is AetherSDR's `CwRxModel`. Nothing here opens a serial port.
  The CWX indicator opens this window instead of the CWX panel while the
  `orion` family is connected (`OrionCw/replaceCwx`, default on); the CWX
  panel is untouched for every other radio.
- **One QSO, two windows.** The console's `QsoContext`, ported
  (`core/backends/orion/log/QsoContext.h`): the Log QSO window and the CW
  window share one record of the station being worked. A call typed or
  clicked in either one fills the other (with the country line and the
  previous-QSO table), and the reports in the Log QSO window are what
  `[rst sent]` and `[rst rcvd]` key. It is cleared when the QSO is logged
  or the form is wiped, never just because a window opened.
- **The logbook gains the console's v3 `handle` column**: the ON-AIR name,
  kept apart from the callbook's `name` ("the callbook says Robert, the man
  sends BOB"), with its own box in the Log QSO window and `[his handle]` in
  the macros. An older log gains the column by ALTER on open. It rides our
  own ADIF export as `APP_TTC_HANDLE` — the spelling the station's console
  writes and reads, so a file round-trips either way — and never goes to an
  online log: every upload path takes standard ADIF.
- **The panadapter's trace and waterfall now average separately.** The
  backend hands the waterfall its own copy of each frame (as
  same-observation `SpectrumCoverage`), built from the raw frame rather
  than the averaged one, peak-held over the waterfall's row interval so a
  dit between two rows still lands. FFT AVG shapes the trace; a new WF AVG
  row at the top of the Display panel's WATERFALL group shapes the
  waterfall, on the same 10 ms-per-step scale. The row appears only on a
  backend declaring `backendPanWaterfallAveraging`, and the client keeps
  its value per radio and pan. This is what the pause note below asked
  for: averaging the trace no longer smears the waterfall.
- **The pan's vertical axis is declared** (`panAmplitude`: relative levels,
  absolute host-computed bins, client-persisted range). Without that record
  AetherSDR read the Orion as a backend nobody had looked at and gated the
  noise-floor auto-adjust off, so the trace sat wherever the saved range
  left it. Measured on the bench: the capture is flat to within 3 dB across
  the whole +/-500 kHz, and a frame crops to the view correctly at every
  span — what looked wrong on screen was the Display panel in **3D Stacked
  Trace** mode plus a floor position that pinned the trace to the bottom of
  the pane. In 2D, with the floor position around 70, the pan reads
  correctly: flat floor, carriers on their right frequencies, the waterfall
  tracking them.

**PAUSED 2026-10-17 (operator decision).** The fork runs the whole station
on air (CAT, RSP2 pan and both receivers, split, TX on both paths, TUNE,
ATU, WinKeyer CW, sidetone). Open when it resumes: the 2D panadapter (the
backend averaging in 3f529735 smears the waterfall, because AetherSDR builds
waterfall rows from the same frames; the fix chosen in discussion is
trace-only averaging in SpectrumWidget, or a staged port of the console's 2D
behind a Display option with rule 2 amended), Omni VII (step 5), CW Delay,
the rotor/Pat checks. Nothing pushed; the operator asks before any push.

**Later steps, each one an operator on-air checkpoint:**

| Step | Content | Operator check |
|---|---|---|
| 2 | RSP2 `SdrEngine`: spectrum only | Pan shows the right frequencies around the dial; delivery ≥ 95% |
| 3 | WDSP demodulation for slice 0 | Receive audio |
| 4 | Slice 1 and split routing | — |
| 5 | Omni7Cat | — |
| 6 | TX: PTT plus SignaLink audio | **Operator-attended only** |
| 7 | Peripherals: WinKeyer, rotor and rotctld, cwdaemon, Pat monitor; rigctld verified with Pat | — |

## Operator decisions (2026-10-17)

1. **Voice TX audio:** both paths stay. The radio's front mic jack is for
   stand-alone use, and the SignaLink carries PC audio (digital, FreeDV,
   and the PC mic with AetherSDR's EQ), switched by gain as in Q3. The
   Orion must keep working stand-alone.
2. **CW:** WinKeyer only. cwdaemon on :6789 drives the WinKeyer.
3. **Slice 1 outside the RSP2 window:** silent and dimmed is fine.
4. **Rigctld:** AetherSDR's own CatPort on :4532, once it passes Pat's
   sequence.
5. **Folder:** the clone lives at `~/aether-orion`.
