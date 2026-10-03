# The pet, and the avatar

The companion's face and the desk pet are the same small character: a blob with two eyes. Its life is
`components/home/src/home_pet*.c`, its drawing is `components/ui/src/ui_avatar*.c`, its screens are
`ui_pet.c` (the pet page and the idle face in home mode) and `ui_app_companion.c` (the companion).

## The character is a file on the card

`<card>/CATOS/PET/AVATAR.JSON` is read at boot and again whenever the file changes (checked every 3 s). It is a
Bible Strong avatar definition, schema v1: design it in the Studio (avatars.bible-strong.app), export, copy the
file there. If the card has no such file, or it does not parse, the tablet logs one line (`avatar: ...`, visible
in the dev console) and keeps the built-in character, which is ours. `pet avatar` lists what the loaded character
answers with itself and what it borrows.

What is honoured: the body's primary surface (type, width, height, roundness) and its sphere / cube / capsule /
cylinder / cone / diamond / mickey shapes; extra body nodes (up to 16, four different shapes), which turn with the
head and swap in front of or behind it; the two colours; every expression's head turn, eyes (width, height, x, y,
angle) and spacing; per-expression colour overrides, cross-faded; `motion.eyes` (micro-saccades, shake) and
`motion.body` (slow drift, shake, applied to the head); animations with their steps, hold and transition times,
spring / smooth / snappy, loop / once / pingPong, and the blink. Not honoured: `perspective` (always 1 in
practice), node rotation and non-spherical depth, the head's silhouette turning with the head, and the eyes being
clipped to the head where they run off its edge.

Which of the app's meanings the character answers is one table, `MAP` in `ui_avatar.c`: for each (idle, listening,
thinking, speaking, happy, confused, surprised, sleeping, waking, hungry, eating, petted, love ...) the animations
that say it, then the nearest expressions, then the built-in pose of that name worn on the character's own body and
colours.

## Why Strobi and Tabi are not in this repository

The Bible Strong Avatar Lab is AGPL-3.0 and this repository is MIT. The renderer here is written from the
geometry (a face painted on a sphere, turned by the head, foreshortened by how far the surface leans away; its two
constants were fitted to the format's own renders), not translated from theirs, and the built-in character is ours.
A definition made with their tools is a work of theirs, so it lives only on the tablet's card. `.gitignore` keeps
`AVATAR.JSON` and `*.avatar.json` out. To put one on a tablet:

    python tools/tab5_dev.py COM9 "put C:/path/to/thing.avatar.json CATOS/PET/AVATAR.JSON"

## What it holds, and what it costs

The body is baked once at 420 px into two alpha masks (the silhouette, and a dithered soft light on its upper left)
and a mask for each different node, about a byte a pixel: roughly 70 KB for the character on the card, 60 KB for the
built-in one. They are baked when a screen that shows the avatar is on or beside the screen, and given back when
home mode or the companion closes. Parsing the file keeps a compact struct (a few KB) and frees the text at once.
Per frame only the eyes, and the ears when the head turns, are invalidated, at 20 fps at most and 10 fps when calm;
at rest nothing is redrawn.

## The pet's life

`home_pet.c` is pure and host-tested (`test/test_pet.c`). Hunger, energy, mood and affection advance by the clock in
ten-minute steps, so the time the tablet was off is caught up at boot: a week away leaves it hungry and groggy,
never dead (hunger tops out at 0.92, energy bottoms at 0.08, and anything over a week counts as a week). It sleeps
from 22:00 to 7:00 (woken by hand it stays up 20 minutes) and dozes when the room has been quiet for twenty minutes
and it is tired.

State is `<card>/CATOS/PET/state.json`, written through a temp file and a rename, within a couple of seconds of a
change the owner made and at most once a minute for drift; the kv store (`pet`) stands in when no card is in. A
state that failed to load is never written over.

The companion knows the pet: its state and last few diary entries go into every question's context note, a hungry
pet may be mentioned once every three hours (never nagged), and she has the tools `pet_status`, `pet_feed`,
`pet_play` and `pet_rename`. They are hers alone: the technician is never offered them. The diary is the pet's own
(`diary` in the state file); what she learns about the owner goes into her memory pool as before.

## Dev console

    pet                      the state and the diary
    pet show | pet idle      the pet page, the idle face
    pet face <expression>    hold one of the character's expressions (pet face off to go back)
    pet state <name> | pet react <name>   idle quiet listen think speak happy confused surprised sleep wake drowsy
                             sad hungry eat petted love fed curious
    pet set hunger|energy|mood|affection <0..1>, pet set age <days>, pet asleep 0|1
    pet feed <food>, pet play, pet stroke, pet reset, pet avatar
