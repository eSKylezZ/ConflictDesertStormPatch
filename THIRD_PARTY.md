# Third-party code

Libraries vendored in `third_party/` (unmodified) and where they end up. Art and icon credits are in
`dist/DesertStormFix-README.txt` ("Credits"), which ships with the release.

| Library | Version | Licence | Used by | Ships in the release package |
| --- | --- | --- | --- | --- |
| [Lua](https://www.lua.org) (`third_party/lua`) | 5.4.7 | MIT (`third_party/lua/LICENSE`) | `dsfix.dll` (mission scripts, pse.lua), `pse_build.exe` (`missions --check`), `pse_tests` | yes, in `dsfix.dll` - credited in the player readme |
| [Zydis](https://github.com/zyantific/zydis) + Zycore (`third_party/zydis`) | 4.1.0 (amalgamated release; details, source URL and hashes in `third_party/zydis/VERSION.txt`) | MIT (`third_party/zydis/LICENSE`, `LICENSE-zycore`) | the mission translator `pse/build/mission2lua` (x86 decoder): `pse_build.exe`, dev builds of `dsfix.dll` (`Dev\MissionSource`), `pse_tests` | no: release builds compile the in-game translator out (`DS_DIST`), and `pse_build.exe` is not in the package. When it ships (first-boot generation, DESIGN.md §12.2), add the Zydis / Zycore notice to the player readme's credits. |

Both are MIT-licensed: the copyright notice and the permission notice must go with every copy that contains them.
