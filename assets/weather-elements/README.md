# Weather sounds

From **"Weather Elements" by Helton Yan** (https://heltonyan.itch.io/weatherelements), licensed under
[Creative Commons Attribution 4.0](http://creativecommons.org/licenses/by/4.0/) (`LICENSE.txt` as shipped with the pack).

Modified by DesertStormFix: a choice of the pack's files (rain outside / inside, light / heavy; wind whistle light /
heavy; eight thunder claps), loops crossfaded at the seam, inside rain resampled to 22.05 kHz, thunder tails trimmed,
re-encoded as MS-ADPCM. Made by `games/ds1/scripts/gen_weather_sounds.py` from the unzipped pack (the file list is
in the script). Built into `dsfix.weather.dll` (`games/ds1/plugins/weather/weather_sounds.rc`); credited in the player
readme.

The `.adpcm` files are RIFF / WAVE files (format 2, MS-ADPCM) with their own extension because the repo ignores `*.wav`.
