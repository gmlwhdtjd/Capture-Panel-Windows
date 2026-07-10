# Steinberg ASIO SDK interface files

Capture Panel vendors only the interface headers required to build its native
Windows ASIO host:

- SDK version: 2.3.4
- Release archive: `ASIO-SDK_2.3.4_2025-10-15.zip`
- Official source: <https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip>
- Archive size: 8,910,208 bytes
- SHA-256: `D5EBF0C20DD2C5F43771FD0C1418F4B361BF52434EE670097CFA6B3A335E2ECA`

The files under `common/` preserve the SDK source content and notices. Capture
Panel uses them under the GNU General Public License version 3 alternative in
the bundled Steinberg `LICENSE.txt`, consistently with this project's
GPL-3.0-only license.

No third-party ASIO driver is redistributed by this repository. Users install
the hardware vendor's driver separately.
