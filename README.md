# VVVVVV Crowd Control Build

This repository is the VVVVVV source tree with a Crowd Control TCP client
integrated into `desktop_version`. It is not a drop-in mod for an unmodified
commercial VVVVVV installation; use a build made from this source tree.

## Requirements

- A desktop VVVVVV build compiled from this repository.
- Crowd Control with the **VVVVVV** pack selected.

`desktop_version\CMakeLists.txt` includes `src\CrowdControl.cpp`, which
contains the game-side connector.

## Setup and usage

1. Build the desktop version from this source tree using its CMake project.
2. Start Crowd Control and select VVVVVV.
3. Launch the built game. Its Crowd Control client attempts a local connection
   automatically.

The game connects to the Crowd Control server at `127.0.0.1:28379`. The
in-game status text reports **CrowdControl Connected** or
**CrowdControl Disconnected [F9]**.

## Troubleshooting

- **Disconnected status:** confirm that Crowd Control is running with the
  VVVVVV pack and that port `28379` is free locally.
- **The first connection fails or is lost:** press **F9** outside the editor
  to request a reconnect.
- **The stock game does not connect:** the connector is compiled into this
  repository's desktop source; run the build produced from this tree.

The upstream project credits, source-license information, and distribution
terms remain in `LICENSE.md` and the original VVVVVV source files.
