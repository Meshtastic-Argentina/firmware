# MeshArg Firmware

> **A community fork of [Meshtastic®](https://meshtastic.org) firmware**, maintained by the MeshArg group.
> **Not affiliated with, endorsed by, or sponsored by Meshtastic LLC.** Meshtastic® is a registered
> trademark of Meshtastic LLC; this is an independent, modified derivative.

Modified fork of the official **[meshtastic/firmware](https://github.com/meshtastic/firmware)**,
distributed under the **GNU GPL-3.0** — the same license as upstream (see [LICENSE](LICENSE)).

### What's different from upstream

This branch is based on upstream stable `v2.7.26.54e0d8d`. Its default build includes a **ReplyBot** auto-responder that replies to text "slash commands" broadcast on the secondary channel named exactly `ambaBOTs`: `/ping` `/hello` `/test` (signal mic-check) · `/net` (+ `/info`, bare `net`/`info` — mesh
health) · `/node` (device status) · `/route` (traceroute the path back to you) · `/help`, plus
bare-word `test`/`ping`. Replies are threaded; `/route` uses a 30 s timeout. Full list →
**[CHANGES.md](CHANGES.md)**.

These changes are **app-layer only** (text replies) — no changes to the mesh packet format, routing,
or encryption — so MeshArg nodes interoperate normally with stock Meshtastic nodes on the same major
version. Please be considerate with auto-replies on busy public channels.

### Build & flash (PlatformIO)

```sh
git clone --branch mesharg-stable-2.7.26 --recurse-submodules https://github.com/Meshtastic-Argentina/firmware.git && cd firmware
pio run -e heltec-v3 -t upload     # Heltec V3 (ESP32-S3): build + flash over USB
pio run -e rak3401-1watt           # RAK WisMesh 1W: build, then drag the .uf2 onto the
                                   #   UF2 bootloader drive (double-tap reset to mount it)
```

---

<sub>The original upstream Meshtastic README is preserved below for attribution.</sub>

---

<div align="center" markdown="1">

<img src=".github/meshtastic_logo.png" alt="Meshtastic Logo" width="80"/>
<h1>Meshtastic Firmware</h1>

![GitHub release downloads](https://img.shields.io/github/downloads/meshtastic/firmware/total)
[![CI](https://img.shields.io/github/actions/workflow/status/meshtastic/firmware/main_matrix.yml?branch=master&label=actions&logo=github&color=yellow)](https://github.com/meshtastic/firmware/actions/workflows/ci.yml)
[![CLA assistant](https://cla-assistant.io/readme/badge/meshtastic/firmware)](https://cla-assistant.io/meshtastic/firmware)
[![Fiscal Contributors](https://opencollective.com/meshtastic/tiers/badge.svg?label=Fiscal%20Contributors&color=deeppink)](https://opencollective.com/meshtastic/)
[![Vercel](https://img.shields.io/static/v1?label=Powered%20by&message=Vercel&style=flat&logo=vercel&color=000000)](https://vercel.com?utm_source=meshtastic&utm_campaign=oss)

<a href="https://trendshift.io/repositories/5524" target="_blank"><img src="https://trendshift.io/api/badge/repositories/5524" alt="meshtastic%2Ffirmware | Trendshift" style="width: 250px; height: 55px;" width="250" height="55"/></a>

</div>

</div>

<div align="center">
	<a href="https://meshtastic.org">Website</a>
	-
	<a href="https://meshtastic.org/docs/">Documentation</a>
</div>

## Overview

This repository contains the official device firmware for Meshtastic, an open-source LoRa mesh networking project designed for long-range, low-power communication without relying on internet or cellular infrastructure. The firmware supports various hardware platforms, including ESP32, nRF52, RP2040/RP2350, and Linux-based devices.

Meshtastic enables text messaging, location sharing, and telemetry over a decentralized mesh network, making it ideal for outdoor adventures, emergency preparedness, and remote operations.

### Get Started

- 🔧 **[Building Instructions](https://meshtastic.org/docs/development/firmware/build)** – Learn how to compile the firmware from source.
- ⚡ **[Flashing Instructions](https://meshtastic.org/docs/getting-started/flashing-firmware/)** – Install or update the firmware on your device.

Join our community and help improve Meshtastic! 🚀

## Stats

![Alt](https://repobeats.axiom.co/api/embed/8025e56c482ec63541593cc5bd322c19d5c0bdcf.svg "Repobeats analytics image")
