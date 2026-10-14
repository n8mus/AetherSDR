---
title: "KAT500 Tuner Control"
slug: "/kat500-tuner-control"
description: "Connect an Elecraft KAT500 and use its operating, configuration, backup and memory controls."
status: "Experimental"
applies_to: ["All radios"]
platforms: ["Linux", "macOS", "Windows"]
---

:::info[Status]

**Status:** Experimental · **Applies to:** All radios · **Platforms:** Linux, macOS, Windows

:::

The **KT500** applet controls an independently connected Elecraft KAT500 tuner. Its values come from tuner readbacks. Connecting or opening the applet does not send Tune, key a radio, or recall stored client-side tuner settings.

## Requirements

Use a local USB serial port or a raw TCP serial bridge such as ser2net or aether-serial-bridge. Only one application can own the tuner connection. Configuration, EEPROM and memory tools support the verified firmware **02.16** schema; other firmware shows an availability reason. Firmware updating uses Elecraft's official Utility.

## Setup

1. Open **Settings → Radio Setup... → Peripherals → Elecraft KAT500**.
2. Select **Serial** and the tuner port/baud (4800, 9600, 19200 or 38400), or **Network** and the raw TCP host/port. The network bridge owns baud and 8N1 line settings. Telnet/RFC 2217 and Elecraft Remote authentication are not supported.
3. Click **Connect**. **Auto-connect** reconnects using the saved client connection preferences when enabled.
4. The **KT500** button appears after KAT500 identification. It remains available during logical power-off or stale replies and disappears when the transport closes.

## Using the applet

**ON/OFF** shows confirmed power. Click it to request the opposite state; the label changes after readback. Silence is stale, not OFF. **Tune** requests tuner search only after your explicit click; supply RF according to the Elecraft manual. **Cancel** requests cancellation while the tuner reports an active search. AetherSDR does not generate a radio carrier for these controls.

**Auto**, **Manual** and **Bypass** select the tuner mode. **ANT 1**, **ANT 2** and **ANT 3** remain visible. The applet automatically reads antenna availability for the confirmed band after connecting and when a new band is reported. Buttons become available after those replies; antennas disabled in the tuner configuration remain dimmed with a reason. Confirmed mode and antenna selections remain visible with dimmed text while actions are unavailable. **Clear fault** clears the current fault, not stored fault history.

**Mini** is about half the height of Full. It keeps **ON/OFF**, **Tune/Cancel** and **Full**, with readouts for mode, antenna, frequency/band, SWR and fault/readiness. Switch to **Full** to change mode or antenna, clear faults, view the physical-state pills, or open a drawer. The normal docked view uses compact 22 px buttons without scrolling. Expanded drawers and short popped-out windows scroll when needed. **BYP** and **ATT** show physical bypass and attenuator state. Click either pill to request the opposite state. **KEY PASS** means the KAT500 key-line relay is connected; click to block amplifier keying. **KEY BLOCK** means it is interrupted; click to allow keying. These three pills are toggles: they retain confirmed state until the tuner replies and are unavailable during RF, tuning, maintenance or missing readback. Hover over each for its full function, current state and next action. This affects the amplifier only if its key line passes through the tuner, and does not indicate amplifier transmission; READY, WAIT, TUNING, OFF or STALE describes availability. In Mini, the middle pill combines mode, selected antenna and availability. **READY** means the tuner can accept an operating request; it does not indicate a tuned match. The pill is yellow when Bypass mode or physical bypass is active. Hover over a pill for its full description.

The single **SWR** meter shows adjusted SWR. Press and hold its pill (or hold **Space** while it has keyboard focus) to preview the last bypass SWR in the same meter; the pill becomes yellow and reads **BYP**. Release to return to adjusted SWR. This is a display-only comparison: it does not switch the tuner into bypass. Readings remain after RF stops; the tooltip identifies adjusted SWR without a fresh RF indication, and bypass preview always shows the last observed reading. **—** means no reading is available. The meter uses the TX Controls scale: 1–3, amber from 2 and red from 2.5, with labeled major ticks every 0.5 and minor ticks every 0.1. The pill still shows the exact reading above 3.

Expand **Manual** for C/L totals and eight relay checkboxes each. Checkbox labels show component values, largest first: capacitance in pF and inductance in nH, matching the Elecraft Utility. Numeric **Set** maps to the closest available relay combination. These requests can change the signal path. **Match frequency**, in kHz, starts at the confirmed tuner frequency and selects the frequency used by **Recall** and **Memorize**. Once you edit it, status refreshes preserve your choice. Both actions require 1800–54000 kHz, matching the [KAT500 operating range](https://elecraft.com/products/kat500-high-power-autotuner); an unknown or unsupported frequency sends no command. Recall retrieves the stored match; Memorize stores and verifies a match for that frequency. **Full tune without saving** requests FTNS explicitly.

**History** plots last-observed SWR and forward/reflected ADC counts for five minutes. Each graph includes a trace legend, latest value, vertical scale and time labels; gaps mean missing replies. ADC counts are not watts. SWR can describe an earlier physical RF measurement. The graphs grow and shrink with the popout; smaller windows scroll when the expanded controls no longer fit.

**Settings → Signal path** contains **Capacitor on antenna side**; checked places the capacitor on the antenna side. The **BYP**, **ATT** and **KEY** controls are directly available as pills in Full view. **BYP** operates only the matching-network bypass relays and leaves Auto/Manual mode unchanged; recall or tuning can engage the network again. Use the main **Bypass** mode button to disable tuning. All signal-path controls require confirmed readback and the same fresh, idle, no-RF conditions as Manual.

Settings also shows identity, installed firmware, serial, DIRECT/NETWORK endpoint, baud where applicable, freshness, availability explanations and the last operation result. **Last observed** exposes the tuner readings:

| Reading | Meaning |
|---|---|
| ATU selection (F) | Frequency used to select a stored match; also shown in the applet header |
| XCVR / CAT (FX) | Last frequency received by the tuner through CAT or Elecraft AUXBUS |
| RF counter (FC) | Last measured RF frequency, in 8 kHz steps |
| ATU bin (FY) | Frequency range covered by the selected tuning-memory bin |
| Adjusted / bypass SWR | Last readings with the selected match and with the matching network bypassed |

F, FX and FC are displayed in kHz; FY is a range in kHz. They can remain after RF stops. A raw zero frequency is not a usable frequency; **—** means no reply has been received. Milliseconds since the SWR reply measure communication freshness, not the age of the physical RF measurement.

**Debug** offers normal state/errors or **Verbose** wire and wake traffic, with copy, atomic save, clear and paused display. New entries stay in view when scrolled to the bottom. Scroll up to inspect older entries without jumping, or pause the display; resuming catches up to the newest entry. Debug TX means serial writes, not RF. Drawer triangles point down when expanded and right when collapsed.

**Follow TX frequency** defaults off. Enabling it follows the effective TX slice frequency, including XIT and split offsets. When eligibility returns, a frequency change that has not yet been attempted can proceed. A failed or canceled request is not retried for the same frequency; change the effective TX frequency or switch Follow off and on to authorize a fresh attempt. It does not key a radio. The applet’s own **Tune** button remains a tuner-only request.

### TX Controls integration

**Use KAT500 for TX Controls** is on by default in both the applet's **Settings** and **Peripherals → Elecraft KAT500**. When the tuner reports **Auto** or **Manual**, the TX Controls applet uses it instead of the internal radio ATU. Explicit **Bypass** mode or switching this option off restores the radio controls. Physical bypass can be the tuner's chosen match in Auto and does not change the selected route.

Right-click **AUTO** or **ATU** for **Pre-tune bands…** and **Clear all KAT500 memories…**. Pre-tune uses the same Step/Auto sweep dialog, with KAT500 memory-bin spacing and the active band plan. Each point runs a fresh KAT500 tune while keeping the radio's internal ATU bypassed. Aborting stops the carrier, cancels the tuner search and restores the original radio frequency. A lost tuner connection or changed route stops the sweep without falling back to the radio ATU. Changing between the radio and KAT500 invalidates an open pre-tune window, including a paused sweep; close and reopen it to use the new tuner.

**Clear all KAT500 memories…** asks for confirmation before erasing stored matches on every band and antenna; it preserves configuration and the radio's internal memories. Elecraft does not document single-frequency erasure. Left-click **AUTO** or **ATU** for a fresh tune at the current frequency instead.

- In **Auto**, the gold **AUTO** button starts a fresh tune and leaves the tuner in Auto. In **Manual**, the button reads **ATU**. Clicking either authorizes a radio tune carrier at the existing **Tune Pwr** setting; AetherSDR does not increase that setting.
- The sequence waits for internal radio ATU bypass readback, selects the TX frequency, then starts the carrier only after the KAT500 confirms the search request. **CANCEL**, completion, timeout, lost connection or changed authorization stops the carrier. A failed attempt is not automatically restarted.
- **SAVE** replaces **MEM** and stores the current matching network for the selected antenna and TX frequency, with readback verification. SAVE generates no RF.
- A **KAT500** pill occupies the former Mem indicator position. **Success** and **Byp** describe tuner readback; the SWR meter above still measures SWR at the radio. Success clears when the matching context changes.
- If the selected KAT500 stops replying, the external controls remain selected but unavailable. They do not silently fall back to the internal ATU.

Connect the amplifier key line through the KAT500 if its key interrupt is to protect an amplifier during tuning. AetherSDR does not switch the amplifier to Standby for this sequence. The context menu targets KAT500 while this routing is selected. Use Advanced for band/antenna-specific erasure and memory inspection.

## Using Advanced

Open **Peripherals → Elecraft KAT500 → Advanced**, or **Settings → Open Advanced settings** in the applet. Opening the panel sends no maintenance request. Sections fit the detail pane and scroll vertically; wide fault and memory tables scroll within their own table.

### Configuration

Click **Read configuration** before editing. Double-click an **Edit** cell and enter a documented numeric value (0/1 for off/on). Choose an individual band or **All bands**. **Mixed** stays mixed until you edit that field; editing it expands only that field across eleven bands. **Readback** remains device readback. Compact setting names have tooltips with units, ranges and special values. **Apply changed fields** sends deliberate changes and verifies each GET; **Cancel edits** discards staging. At least one antenna must remain enabled, and a preferred antenna must be enabled.

| Category | Values |
| --- | --- |
| Amplifier key interrupt power | 0–1500 W; 1500 means unlimited interrupt threshold |
| Power on when DC applied | 0/1 |
| Sleep when idle | 0/1; the connection handles wake handshakes |
| Enabled/preferred antennas | ANT 1–3 enabled; preferred 0 means last used |
| ATU slots per bin | 1–6; six physical slots are shared among antennas |
| SWR thresholds | Auto tune 1.5–99.99; bypass/key interrupt 1–99.99 |
| Auto fine tune | 0/1 per band |
| Additional retune distance | 0–65535 |

**Optimize for KPA500** explicitly stages the 1500 unlimited interrupt threshold. Click **Apply changed fields** to send it. It is not a transmit-power target and is never applied merely because an amplifier is present. Fixed-point readbacks remain authoritative. On a partial failure, review confirmed fields and unresolved staging before retrying; already confirmed writes cannot be undone by cancellation.

### Backup and restore

**Read and save backup...** acquires both EEPROM regions (66,560 bytes) and saves an atomic native `.KAT500Config` file. **Choose backup...** validates bounded size, complete coverage and checksum locally, then previews identity and firmware. Choosing a file does not change the tuner.

Restore requires a complete firmware 02.16 backup matching the connected tuner serial/firmware, fresh idle state and zero forward ADC count. Review the replacement explanation and check **Confirm backup replacement**, then click **Restore and verify**. Each permitted block is verified by exact readback; identity and fault history ranges are excluded. Progress and **Cancel operation** are available. Cancellation or failure leaves confirmed blocks applied; no automatic write retry, replay or reboot occurs. Manually power-cycle after success to load restored settings.

### Fault history, memories and reset

**Read fault history** shows newest records first, elapsed device seconds, decoded fault, ADC counts, SWR and raw bytes. Power On records differ from the current no-fault state. Unknown records remain visible. **Export CSV...** saves the captured records.

**Inspect bin** reads matches for a selected kHz frequency, or **Current frequency**. Stored antenna/C/L rows do not alter live controls. To erase, select band and antenna, review the displayed erase target, check **Confirm selected erase**, and click **Erase selected memories**. Completion confirms the command barrier; inspect the bin to verify the resulting contents. **Confirm factory reset** has a separate explanation of erased settings/matches, restart and default-baud reconnection. Neither action runs on connect or page open.

## Troubleshooting

### Controls are disabled

Open **Settings** for the availability explanation, or read the control's tooltip or accessible description. Connect and identify the tuner, wait for fresh replies, turn it on, and keep it idle with no RF before writes. Configuration and memory tools require firmware 02.16. Bootloader identity directs you to the official Utility.

### A requested value is not confirmed

Operating setters allow a finite GET-only settling window for transient old replies. The setter is not resent. If the operation fails, read the current device state before another deliberate request. Reconnecting discards queued mutations and staged edits.

### Memory verification disconnects the tuner

If a memory response is incomplete, AetherSDR closes the stream so leftover memory data cannot be mistaken for live status. Open **Settings → Radio Setup... → Peripherals → Elecraft KAT500** and click **Connect**. The failed command is not retried. Check **Match frequency** before making another explicit Recall or Memorize request.

### A file dialog is already open

Return to the existing dialog to complete or cancel file selection. Advanced permits one asynchronous native file dialog at a time; closing the panel safely closes its owned dialog.

## See also

- [Peripherals](./peripherals.md)
- [Amplifiers](./amplifiers.md)
- [Settings and Backups](./settings-and-backups.md)
- [Elecraft KAT500 software](https://elecraft.com/pages/kat500-software)

## Screenshots

Current Full and Mini applets, captured from the Mac connected to a KAT500 with no RF:

<img src="/img/kat500/kat500-applet-full.png" width="320" alt="KAT500 Full applet with confirmed Auto mode, antenna 1, SWR and signal-path toggles." />

<img src="/img/kat500/kat500-applet-mini.png" width="320" alt="KAT500 Mini applet with power, Tune, fault status, mode and SWR." />
