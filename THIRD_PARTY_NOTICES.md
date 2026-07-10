# Third-party notices

## Steinberg ASIO SDK 2.3.4

Copyright (c) 2025 Steinberg Media Technologies GmbH. All rights reserved.

Upstream archive:
<https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip>

- Archive size: 8,910,208 bytes
- SHA-256: `D5EBF0C20DD2C5F43771FD0C1418F4B361BF52434EE670097CFA6B3A335E2ECA`
- Redistributed files: `common/asio.h`, `common/asiosys.h`,
  `common/iasiodrv.h`, and `LICENSE.txt`

This repository includes the ASIO interface header source content needed by
the host. It is used under the GNU General Public
License version 3 alternative offered by Steinberg. The exact SDK notice is
preserved in every header and its license is included at
`third_party/asio/LICENSE.txt`. Binary release bundles also place this text at
`licenses/Steinberg-ASIO-SDK-LICENSE.txt`.

ASIO is a trademark of Steinberg Media Technologies GmbH. Capture Panel is an
independent open-source host and is not affiliated with or endorsed by
Steinberg.

## .NET 10 runtime

Self-contained Windows desktop releases include the Microsoft .NET runtime and
Microsoft Windows Desktop Runtime selected by the .NET 10 SDK. These components
are licensed under the MIT License and include software under additional
third-party notices.

The release workflow copies the exact license material from the restored
runtime packs into `licenses/dotnet-runtime-LICENSE.txt`,
`licenses/dotnet-runtime-THIRD-PARTY-NOTICES.txt`, and
`licenses/windows-desktop-runtime-LICENSE.txt`. Development builds that depend
on an installed .NET runtime do not redistribute these runtime files.

## Hardware drivers

Capture Panel does not include or redistribute Darkglass, Focusrite, Realtek,
or any other vendor's ASIO driver. Those drivers remain subject to their
vendors' own licenses and must be installed separately by the user.
