# Dependencies and source provenance

Orbit application code and modifications: GPL-3.0-or-later. No Sony proprietary SDK or Discord SDK is included.

- PSL1GHT `samples/graphics/videoTest/rsxutil.c` and `.h`, authors including Youness Alaoui: GPL v3. Original source: https://github.com/ps3dev/PSL1GHT/tree/master/samples/graphics/videoTest . See file headers; Orbit modifies the renderer integration.
- Monocypher 4.0.2: https://github.com/LoupVaillant/Monocypher/tree/4.0.2 . Dual BSD-2-Clause / CC0, source and full license in `vendor/`. Used for Argon2id, constant-time comparison and memory wiping.
- cJSON 1.7.19: https://github.com/DaveGamble/cJSON/tree/v1.7.19 . MIT, source and full license in `vendor/`.
- DejaVu Sans: https://dejavu-fonts.github.io/ . Bitstream Vera license plus public-domain DejaVu changes. Font copied without modification from the development environment's DejaVu Sans; renamed filename only. Full notices in `vendor/FONT-LICENSE`.
- Mbed TLS 2.28.10: https://github.com/Mbed-TLS/mbedtls/tree/v2.28.10 . Apache-2.0. PS3 static libraries provided by the toolchain below; native tests built against the same release. TLS-PSK AES-128-GCM-SHA256, no custom encryption protocol. Full license in `vendor/MBEDTLS-LICENSE`.
- FreeType, libpng and zlib static libraries supplied by ps3dev portlibs. FreeType: https://freetype.org/ (FreeType license option); libpng: https://www.libpng.org/pub/png/libpng.html ; zlib: https://zlib.net/ . Corresponding notices are in `vendor/`.
- PSL1GHT system wrappers, Pthreads-embedded, newlib and compiler runtime are supplied by the public ps3dev toolchain. Source/build manifests: https://github.com/ps3dev/ps3dev , https://github.com/ps3dev/PSL1GHT , https://github.com/ps3dev/ps3libraries , https://github.com/ps3dev/ps3toolchain . Pthreads-embedded is LGPL-2.1-or-later; the application is provided in source form and can be relinked using these open-source tools.

Toolchain release used: https://github.com/ps3dev/ps3dev/releases/tag/nightly-2026-10-12 . The toolchain itself is not included in the source ZIP due to its size. It must be installed separately to rebuild. Build commands are in the README and Makefile.

Audio diagnostic only:
- `vendor/voice-import/exports.S` and `libexport.c`: public PSL1GHT SPRX import templates, MIT, copyright (c) 2011 PSL1GHT Development Team. Source: https://github.com/ps3dev/PSL1GHT/tree/master/ppu/sprx/common . Full notice in `vendor/PSL1GHT-LICENSE`. The new config and FNID list target cellVoice.
- Minimal voice ABI declarations were written for this diagnostic using public interface facts in https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellVoice.h and call signatures in cellVoice.cpp. FNID derivation follows the public algorithm in https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/PPUModule.cpp ; validated against cellAudioInit. No emulator implementation is bundled. Many emulator voice functions are stubs; emulator success would not verify real sound.
- OSK length behavior was checked against https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellOskDialog.cpp . Hardware verification of the correction remains necessary.
- PS3 audio device selection: https://manuals.playstation.net/document/en/ps3/current/settings/audiodevice.html . System audio output is a separate setting: https://manuals.playstation.net/document/en/ps3/current/settings/audiooutput.html .

0.5 / audio probe 0.2:
- `vendor/mic-import` uses the same MIT PSL1GHT import templates; the separate probe now imports nine cellMic functions, not cellVoice. Public ABI, raw PCM format byte order and read signatures: https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellMic.h and https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellMic.cpp . No emulator implementation is included. Hardware device support still needs verification.
- `0x80310809` is named CELL_VOICE_ERROR_NOT_IMPLEMENTED in the public cellVoice header. The user's reported failure occurred at creation of the microphone port; this release does not infer a defective headset from that result.
