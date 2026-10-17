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
