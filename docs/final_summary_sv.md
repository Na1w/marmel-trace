# Slutlig sammanfattning (svenska) — Datadriven scen i C11-raytracern

**Projekt:** Photorealistic Outdoor Raytracer (C11, noll tredjepartsberoenden)
**Arbetsyta:** `/Users/fredrikandersson/Experiments/marmel-0.8.0/raytracer`
**Leverabel:** `docs/final_summary_sv.md`
**Status:** **KLART — inga kvarstående defekter.**

Detta dokument sammanfattar den avslutade uppgiften: att göra renderarens scen
**datadriven**. Varje påstående nedan är kontrollerat mot filerna i repot
(`README.md`, `docs/scene_format.md`, `docs/scene_inventory.md`,
`scenes/*.scene`, `src/*.c`, `src/*.h`, `Makefile`) samt mot de två oberoende
verifieringsrapporterna `docs/validation_report_scene.md` och
`docs/validation_report_final.md`.

---

## 1. Vad som gjordes

Scenen (kamera, himmel, material, primitiver samt procedurella träd/buskar)
beskrivs nu av ett **strukturerat textdokument som läses vid start**. Formatet är
radorienterat och redigerbart för hand; grammatiken är fryst i
`docs/scene_format.md`.

- Standardscenen levereras som `scenes/default.scene`. **Samma text är inbäddad
  i binären** via `src/default_scene_text.h` och tolkas vid start med
  `scene_desc_load_string()`. Programmet kör därför fristående **utan**
  `scenes/`-katalogen.
- Den tidigare hårdkodade scenbyggaren `scene_build()` är **borttagen**.
  `scene_build_from_desc()` är nu den **enda** konstruktionsvägen, och allt
  (inklusive de befintliga testerna) går via beskrivningsvägen.

### Centrala leverabler

| Del | Filer |
| --- | --- |
| Format + inventering | `docs/scene_format.md`, `docs/scene_inventory.md` |
| Parser / datamodell / skrivare | `src/scene_desc.h`, `src/scene_desc.c`, `src/scene_desc_write.c` |
| Scenkonstruktion | `src/scene.h`, `src/scene.c` (`scene_build_from_desc`, `scene_default_desc`); `scene_build()` raderad |
| Data + inbäddning | `scenes/default.scene`, `src/default_scene_text.h`, `scenes/example_sunset.scene` |
| CLI | `src/main.c`: `--scene <fil>`, `--write-scene <fil>`, uppdaterad `--help`, `--opt värde` **och** `--opt=värde`, icke-noll exitkod + tydligt felmeddelande vid saknad/ogiltig scenfil, fallback till inbäddad standardscen |
| Tester | `tests/*.c` |
| Dokumentation | `README.md` uppdaterad; `docs/render_notes.md` avstämd mot den faktiska artefakten |

### CLI i korthet (`src/main.c`)

- `--scene PATH` tolkar och renderar en scenbeskrivning; utan flaggan används den
  inbäddade standardbeskrivningen.
- `--write-scene PATH` skriver standardbeskrivningen och avslutar **utan** att
  rendera.
- Både `--opt value` och `--opt=value` accepteras.
- Exitkoder: `0` vid framgång, `2` vid CLI-fel, `1` när scenen inte kan läsas
  (t.ex. `--scene missing.scene`) eller vid render-/skrivfel. Diagnostik går till
  **stderr**.

---

## 2. Verifierat resultat

Samtliga siffror nedan är bekräftade av de två oberoende valideringarna.

| Kontroll | Resultat |
| --- | --- |
| `make` / `make threads` | 0 varningar, 0 fel (`-std=c11 -O2 -Wall -Wextra`) |
| `make test` | **483 godkända / 0 misslyckade** (båda byggena), `ALL TESTS PASSED` |
| Byte-identitets-GATE | **PASSERAR** |
| `scene_build`-symbol | Finns **inte** kvar (endast `scene_build_from_desc`) |
| Parser-fuzzing | Tusentals malformade indata: 0 krascher, 0 hängningar |
| Minnesläckor | **0** på alla vägar (`leaks -atExit`) |
| README-arbetsflöde | Verifierat ordagrant, ingen avvikelse |
| `docs/render_notes.md` | Samtliga figurer reproducerbara från artefakten |

### Byte-identitets-GATE

Den inbyggda standardscenen (inbäddad text, tolkad), `--scene scenes/default.scene`
och den medföljande `output/scene.bmp` renderar **byte-identiskt**. Detta gäller
både BMP och rå RGB, vid full och reducerad upplösning.

Referens (1920×1080, 16 spp, djup 6, seed 1337):

```text
sha256(output/scene.bmp) = 752d5f7c5818beb907f465fdebe20091bd8baa968452faaab417b5697f45ce28
```

1280×720-varianten (16 spp, djup 6, seed 1337):

```text
sha256 = 8537ed5d7518c0d2ef8c198d70dd38104dd365973ce7c0fb9afe6380c280ff2d
```

Den inbäddade texten är dessutom byte-identisk med `scenes/default.scene`, och
kroppen som `--write-scene` skriver ut är identisk med standardfilens kropp
(endast den inledande kommentarsbannern skiljer).

---

## 3. Problem som hittades och åtgärdades under arbetet

1. **`tree`/`bush`-direktiven var stubbade** — de är nu fullt implementerade och
   genererar deterministiskt samma geometri som den inbyggda procedurella
   generatorn (1286 primitiver inkl. växtprimitiver för standardscenen).
2. **Skrivarens `%.6g` klarade inte bit-exakt rundtur** för `sky.sun_dir` och
   dammens box-`center.y` — skrivaren gjordes precisionsbevarande (eskalerar
   först när så krävs) och specen §9.4 uppdaterades i `docs/scene_format.md`.
3. **Den inbäddade scenetexten var död kod** — den kompileras nu verkligen in och
   tolkas vid start.
4. **`docs/render_notes.md` beskrev en 1920×1080-artefakt medan den incheckade
   filen var 1280×720** — artefakten regenererades och dokumentationen stämdes av.

Alla fyra punkter är stängda och verifierade.

---

## 4. Oberoende verifiering

- `docs/validation_report_scene.md` (oberoende QA, t-110): bekräftar
  **BYTE-IDENTITY GATE: PASSES** samt att `scenes/default.scene` reproducerar den
  inbyggda scenen bit-för-bit (25 890 fältjämförelser, 0 avvikelser). Rapportens
  båda flaggade avvikelser (stubbade växtdirektiv, skrivarprecision) bedömdes som
  **ACCEPTABLE**.
- `docs/validation_report_final.md` (oberoende QA, t-114): bekräftar att båda
  tidigare defekterna (död hårdkodad byggare / icke-inbäddad standard, samt
  inaktuell artefakt / icke-reproducerbara anteckningar) är **lösta**, att bygget
  är varningsfritt, testsviten grön (483/0), att `output/scene.bmp` är
  byte-identisk med en oberoende omrendering vid dokumenterade inställningar, och
  att varje siffra i `docs/render_notes.md` kan reproduceras. Slutomdöme:
  **PROJECT COMPLETE AND CORRECT. No defect remains.**

---

## 5. Slutomdöme

Uppgiften är **avslutad**. Renderarens scen är fullt datadriven, den hårdkodade
byggaren är borta, standardscenen är inbäddad och programmet fungerar fristående.
Bygget är varningsfritt, hela testsviten passerar, byte-identitets-gaten håller och
två oberoende verifieringar rapporterar att **inga defekter kvarstår**.
