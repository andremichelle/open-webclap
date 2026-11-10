# The factory kits

The TR-808 and TR-909 play these 16-bit 44.1 kHz mono samples until the user brings others (a ReBirth.dll or
sample files dropped on the panel). None of them is ReBirth's: they are free recordings, chosen per slot as the
closest match to the slot's sound, plus a few layers synthesised for the port. `build.sh` embeds them in the module.

| Slots | Source | Author | Licence |
|---|---|---|---|
| 909: every slot but sdn1..3 | [TR-909 JGB pack](https://freesound.org/people/altemark/packs/1643/), a real TR-909 | Janne G:son Berg, cut by altemark | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| 808: BD, CB, CH, OH, RS, CL | [Roland TR08 100% gain, default tuning](https://freesound.org/people/Karman_Lyne/packs/29226/) | Karman_Lyne | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) |
| 808: CP, LT, MT, HT, LC, MC, HC | FluidR3Mono_GM, kit "TR-808" ([MuseScore 3.6.2](https://github.com/musescore/MuseScore/tree/v3.6.2/share/sound)) | Frank Wen, mono by Michael Cowgill | MIT |
| 808: SD1..5, N1..4, CY, MA; 909: sdn1..3 | synthesised by `make-kits.py` | this port | GPL-3.0, as the port |

**Credit:** TR-909 samples by Janne G:son Berg, cut by altemark (freesound.org/people/altemark/packs/1643), CC BY 4.0.
The samples were changed: trimmed, levelled, the 909's snares and the 808's toms and congas retuned.

`make-kits.py <downloads> <references or -> <out>` rebuilds them from the downloaded packs (the Freesound
packs as their public previews). Each sample starts 0.5 ms before its onset and ends where it falls below
-75 dBFS, with a short fade. `kit.json` holds each slot's source, pitch ratio and gain, which were measured once
against ReBirth's samples (its level, so the engine's mix stays as designed, and the pitch of the tuned slots).
With `-` the script uses the stored values and needs no reference.
