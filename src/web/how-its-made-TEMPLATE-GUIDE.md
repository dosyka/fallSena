# How It's Made - template guide

## Status: filled (#W20-FILL, 2026-09-29)

`how-its-made.html` is now finished copy. Every text slot has been replaced
with real `<p>` paragraphs and every placeholder card and slot comment has
been removed. The copy only uses verified facts (KiCad, Fusion 360, case +
lid enclosure, the known hardware list, the grace window and caretaker
alert, the planned next steps) plus what is plainly visible in the photos.
The slot table and swap procedure below are kept as reference for adding
more photos later.

Photos used (all live in `images/`, not `images/build/`):

| Section | Photo | Replaced slot |
| --- | --- | --- |
| 2. 3D Modeling the Enclosure | `images/caseCAD.png` (case CAD) | `cad-front.jpg` |
| 2. 3D Modeling the Enclosure | `images/lidCAD.png` (lid CAD) | `cad-exploded.jpg` |
| 3. PCB Design | `images/virtualSchematic.png` | `pcb-schematic.jpg` |
| 3. PCB Design | `images/boardLayout.png` | `pcb-layout.jpg` |
| 4. Soldering and Assembly | `images/bareBoard.jpg` | `board-bare.jpg` |
| 4. Soldering and Assembly | `images/solderedBoard.jpg` | `board-finished.jpg` |
| 5. The Final Product | `images/EncasedDevice.jpg` | `device-final.jpg` |

The `enclosure-empty.jpg`, `board-soldering.jpg`, and `device-worn.jpg`
slots were removed because no photo exists for them. CAD, schematic, and
layout renders use `build-figure__img--contain` (no cropping); the finished
device photo uses `build-figure__img--natural` inside a
`build-gallery--single` gallery.

Optional enrichments the owner could still add (none of these are on the
page today, because they are not yet known facts):

- Enclosure fabrication method and material.
- Board dimensions and layer count.
- Soldering notes (tools, technique, anything tricky).
- A mid-soldering photo (old `board-soldering.jpg` slot).
- A photo of the device worn at the waist (old `device-worn.jpg` slot).
- A photo of the empty enclosure (old `enclosure-empty.jpg` slot).

To add a new photo, add a `<figure class="build-figure">` with an
`<img class="build-figure__img" ... loading="lazy">` and a `<figcaption>` to
the relevant section's `.build-gallery` (drop `build-gallery--single` from
section 5 if it gains a second photo).

`how-its-made.html` is a fill-in template, not finished copy. Every text
block and every photo is an explicit "slot": a styled placeholder card with
an HTML comment above it that names the slot. Nothing on the page is meant
to ship as-is except the section headings and the small set of pre-filled
sentences marked "Suggested text - edit or replace" below.

Find every remaining slot at any time by grepping the file:

```
grep -n "FILL IN\|IMAGE SLOT" src/web/how-its-made.html
```

## Slot reference

| Slot ID | Type | Section | What goes here | Image filename | Aspect ratio |
| --- | --- | --- | --- | --- | --- |
| `section-1-lead` | text (suggested) | 1. Introduction | Pre-filled from verified facts (ESP32-C6, MPU6050, waist-worn, grace window, caretaker app alert). Edit or replace. | - | - |
| `section-1-motivation` | text (empty) | 1. Introduction | 1 to 2 sentences on why you built FallSena and the problem it solves for caretakers. | - | - |
| `images/build/cad-front.jpg` | image | 2. 3D Modeling the Enclosure | CAD model of the enclosure, front view | `images/build/cad-front.jpg` | 4:3 |
| `images/build/cad-exploded.jpg` | image | 2. 3D Modeling the Enclosure | CAD model, exploded/internal view showing where the board and battery sit | `images/build/cad-exploded.jpg` | 4:3 |
| `images/build/enclosure-empty.jpg` | image | 2. 3D Modeling the Enclosure | Fabricated enclosure, empty, before the electronics were installed | `images/build/enclosure-empty.jpg` | 4:3 |
| `section-2-goals` | text (empty) | 2. 3D Modeling the Enclosure | 2 to 3 sentences on what the enclosure needed to do (fit, protection, serviceability) and any constraints you designed around. | - | - |
| `section-2-cad` | text (empty) | 2. 3D Modeling the Enclosure | 1 to 2 sentences naming the CAD software you used and how you approached modeling the enclosure. | - | - |
| `section-2-fabrication` | text (empty) | 2. 3D Modeling the Enclosure | 1 to 2 sentences on how the enclosure was fabricated (method and material) and how you checked the fit against the electronics. | - | - |
| `images/build/pcb-schematic.jpg` | image | 3. PCB Design | PCB schematic diagram | `images/build/pcb-schematic.jpg` | 4:3 |
| `images/build/pcb-layout.jpg` | image | 3. PCB Design | PCB board layout / copper routing view | `images/build/pcb-layout.jpg` | 4:3 |
| `section-3-why-custom` | text (empty) | 3. PCB Design | 2 to 3 sentences on why a custom PCB was needed instead of off-the-shelf breakout modules. | - | - |
| `section-3-components` | text (suggested) | 3. PCB Design | Pre-filled component list (ESP32-C6, MPU6050 over I2C, RGB LED, vibration motor, button, 3.7V 1000mAh LiPo with voltage-divider sensing). Edit or replace. | - | - |
| `section-3-tool-dimensions` | text (empty) | 3. PCB Design | 1 to 2 sentences naming the PCB design tool you used, the final board dimensions, and the layer count. | - | - |
| `section-3-challenges` | text (empty) | 3. PCB Design | 1 to 2 sentences on the main design challenge you ran into fitting everything onto the board. | - | - |
| `images/build/board-bare.jpg` | image | 4. Soldering and Assembly | Bare fabricated board, before any components were soldered on | `images/build/board-bare.jpg` | 4:3 |
| `images/build/board-soldering.jpg` | image | 4. Soldering and Assembly | Board mid-assembly, components being soldered on | `images/build/board-soldering.jpg` | 4:3 |
| `images/build/board-finished.jpg` | image | 4. Soldering and Assembly | Fully soldered, finished board | `images/build/board-finished.jpg` | 4:3 |
| `section-4-bare-board` | text (empty) | 4. Soldering and Assembly | 1 to 2 sentences describing the bare board before any components were soldered on. | - | - |
| `section-4-tools-technique` | text (empty) | 4. Soldering and Assembly | 1 to 2 sentences on the soldering tools and technique you used and anything that made assembly tricky. | - | - |
| `section-4-fit-check` | text (empty) | 4. Soldering and Assembly | 1 sentence on how the finished board fit into the enclosure (button, LED, battery connector alignment). | - | - |
| `images/build/device-final.jpg` | image | 5. The Final Product | Finished device on its own, hero shot | `images/build/device-final.jpg` | 3:4 |
| `images/build/device-worn.jpg` | image | 5. The Final Product | Finished device worn at the waist | `images/build/device-worn.jpg` | 3:4 |
| `section-5-summary` | text (suggested) | 5. The Final Product | Pre-filled from verified facts (waist-worn, senses a fall, grace window, caretaker app alert). Edit or replace. | - | - |
| `section-6-next-steps` | text (empty) | 6. What's Next | 2 to 3 sentences on what comes next for the project (testing, refinements, features) and any open questions. | - | - |

Slot IDs above match the HTML comments in `how-its-made.html` exactly
(`<!-- FILL IN: <slot-id> -->` or `<!-- IMAGE SLOT: <image-filename> -->`).
Do not rename a slot ID in one file without updating the other.

## Two kinds of text slot

- **Empty guidance slots** (`<!-- FILL IN: slot-id -->`) have no suggested
  copy. The card just shows a "Text needed" label and a one-line note on
  what to write and roughly how long. Nothing here is guessed on your
  behalf - write it from scratch.
- **Suggested slots** (`<!-- FILL IN (suggested text below, edit or
  replace): slot-id -->`) contain a short pre-filled sentence or two built
  only from facts that are already known to be true (the ESP32-C6/MPU6050
  hardware, how it's worn, the grace window, the caretaker alert). Read it,
  edit it to match reality, or delete it and write your own - just don't
  leave it unread.

## How to fill in a text slot

1. Find the slot's `<!-- FILL IN ... -->` comment.
2. Write your real paragraph(s) as plain `<p>` tags directly below the
   comment (matching the other `<p>` tags already in that section).
3. Delete the placeholder `<div class="build-figure build-figure--text ...">
   ... </div>` block entirely, including the icon and label inside it.
4. Delete the `<!-- FILL IN ... -->` comment once the slot is filled - it's
   only there so the empty slot is easy to find.

## How to fill in an image slot

1. Save your photo at the exact path named in the `<!-- IMAGE SLOT: ... -->`
   comment (see the table above for the filename and recommended aspect
   ratio; shoot or crop close to that ratio so it fills the card cleanly).
2. Inside that `<figure class="build-figure">`, replace only the
   `<div class="build-figure__visual" aria-hidden="true">...</div>` block
   with:
   ```html
   <img class="build-figure__img" src="images/build/<filename>" alt="<short description>">
   ```
   For the two 3:4 shots in "The Final Product" (`device-final.jpg`,
   `device-worn.jpg`), add a second class so the image keeps its portrait
   ratio instead of the default 4:3:
   ```html
   <img class="build-figure__img build-figure__img--portrait" src="images/build/device-final.jpg" alt="...">
   ```
3. Leave the `<figcaption class="build-figure__label">` as-is, or edit its
   text - it still works as the caption under the real photo.
4. Delete the `<!-- IMAGE SLOT: ... -->` comment once the slot is filled.

This is a one-element swap in both cases: the `<figure>`/`<figcaption>`
wrapper and the surrounding `.build-gallery` grid never change, so the
layout, spacing, and responsive stacking stay exactly as they are today.

## When you're done

A finished page should have zero `FILL IN` and zero `IMAGE SLOT` comments
left. Re-run the grep above to confirm:

```
grep -n "FILL IN\|IMAGE SLOT" src/web/how-its-made.html
```

No output means every slot has been filled.
