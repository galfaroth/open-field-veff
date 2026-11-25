# Dropping a lifted tape cut straight onto veffgranular's page: what was found, and why it is not in the pack

OS 1.7.3+0. Everything here was read in the firmware on a computer (the Blackfin disassembler of the open-field
toolchain over the firmware's own records). **Nothing of it was tried on a unit.** Addresses are 1.7.3+0's.

## What the owner asked for

Lift a cut on the tape, go to a synth sound that plays `veffgranular`, press drop on its page, and have the cut become
the sample, as it does on the sampler's and the drum sampler's pages.

## How the sampler's own drop works

1. **The page.** The synth sampler's screen has a method for the drop, `0x8c0d609c` (a word of its table of methods
   at `0x84013870`). It takes nothing. It asks `0x8f00b168()` whether the tape clipboard is empty and returns if it
   is; otherwise it calls `0x8f00b564(slot)` with the unit's current synth slot, the word at `0x11904704`.
2. **The request.** `0x8f00b564(slot)` posts a request to a worker thread: a queue at `0x84bdd8a8` (its read place at
   + 0x88, its write place at + 0x8c, 0x44 bytes a request). It posts only while the queue is empty; with a request
   waiting it returns having done nothing. The request holds the code 9 and the slot (at + 0x1c). The function that
   posts the drum's request was not traced.
3. **The worker.** `0x8f00bcbc` takes a request and jumps through a table of twelve at `0x848c5e7c`. Code 9 is
   `0x8f00b7b0(slot)`, the synth's drop; code 10 is `0x8f00ba38`, the drum's.
4. **The drop itself, `0x8f00b7b0(slot)`:**
   - asks `0x8f00b78c()` whether the clipboard holds one track (several tracks take another path, not read here);
   - takes the slot's record, `0x080235dc + slot * 0x34`, reads its text (`0x8d4f3734`) and compares it with the
     word `sampler` (`0x8d4ea7b2`, the text at `0x848c5b2c`);
   - **only when the slot's sound is a `sampler`**: takes the slot's sample holder (`0x8f01aea4(slot)`), and calls the
     import, `0x8f004204(object)`, with the holder's object. The import has that one argument (open-field's own
     stand-in for it in `8 tracks` is written with the same one). It renders the clipboard into the object in place
     and rebuilds the overview. Then `0x8d4eb6b2(slot)` marks the slot changed;
   - in every case it ends by calling the record's own method number 6.

## Why the pack does not do it

**1. Stock's own request does nothing for our sound.** An engine of a pack stands on `dsynth`, so the slot's record
says `dsynth`, and step 4 skips the import. Posting code 9 for the slot is harmless and useless.

**2. Calling the import ourselves is a call from the wrong thread.** The firmware only ever runs `0x8f004204` on the
worker thread, between requests. It reads the tape clipboard from the disk and works on the clipboard's state, which
that same thread uses for a lift and a drop on the tape. A pack's functions run on the display thread (frame), the key
thread (input) or the audio thread. From any of them the import would run beside the worker, with no lock we know of.
Whether that is safe cannot be told from reading: it needs the state of two threads at the moment of the call. The
price of being wrong is a unit that hangs or a clipboard file written half way, so no guess is shipped.

**3. A pack cannot make the round trip for the owner either.** The base has no call that changes a slot's engine, so
the pack cannot switch the sound to `sampler`, let stock drop, and switch back.

**4. The drop key's number is not known.** open-field's table of key numbers names the help, metronome, mode and T
keys, shift and the knobs. Version 2 of the pack therefore writes one line in the log for every key that goes down
while its page is up and that it does not take (`veffgranular: key <n> went down while the page was up`). Press drop
once on the page and the log has the number.

## What would unblock it

- **In the base (the clean way):** an engine of a pack that stands on `sampler` (the slot's record then says
  `sampler`, stock's own drop, recording and trim work, and the saved sound carries the sample); or a helper that runs
  the import on the worker thread for a slot. Either is a change to open-field itself.
- **An experiment on a unit, as a build of its own, not in the listed pack:** on the drop key, from the frame
  function, when the clipboard is not empty (`0x8f00b168()` answers 0), holds one track (`0x8f00b78c()`), and the
  worker's queue is empty and idle (the two words of the queue equal), call `0x8f004204(object)` and then
  `0x8d4eb6b2(slot)`. Guarded by a sum of the firmware's bytes at the call site `0x8f00b838` to `0x8f00b846` (not at
  the import's entry: `8 tracks` puts a jump there). What the log would have to show before it could go into the pack:
  the drop key's number; that the import returns and the pool's `words` and sum change as after a drop on the
  sampler; that the tape still lifts and drops afterwards; and the same again while the tape plays and while a lift
  is under way, which is where two threads would meet.

## What the pack does instead

The sample is the synth sampler's own, by default. To give the sound new material: shift and T1, `sampler`, drop,
shift and T1, `veffgranular`. A slot with nothing in it says so on the page.

# The experiment: version 3 (branch `veffgranular-drop`)

**An experiment for one owner's unit, not for the listing.** It makes the call the section above says cannot be shown
safe by reading. It was built and its decisions tested on a computer; the call itself has never run anywhere.

## What it does, against what stock does

| Stock, for a sampler sound | Version 3 |
| --- | --- |
| The sampler page's drop method asks whether the clipboard is empty (`0x8f00b168`) and gives up if so | The same two words are read (`0x84bdd9e4`, `0x84bdda00`); both 0: the page says `NOTHING LIFTED` |
| It posts request 9 with the slot, only while the worker's queue is empty | No request is posted (stock's would do nothing, the sound being `dsynth`). The queue's two places are read and must be equal, at the key and again in the frame of the call |
| The worker asks whether the clipboard holds one track; several go another way | One track, or it is refused (reason 6). The other way was not read |
| The worker compares the slot's sound with `sampler` | **Left out.** This is the point of the experiment |
| It takes the slot's holder (`0x8f01aea4(slot)`) and its object | The same object, read as the sampler source reads it, `[[0x0802f73c + 4 * slot]]`, with that source's checks, and its data must be the slot's pool. The accessor is not called |
| It calls the import, `0x8f004204(object)` | **The same call, at the same entry**, so that `8 tracks`' wrapper there, when that pack is on the unit, still handles a lifted track 5 to 8 as it does for stock |
| It marks the slot changed (`0x8d4eb6b2(slot)`: a byte a slot, and a call of `0x8fca1ed2`) | **Skipped.** It tells the unit a sampler sound changed; ours is a `dsynth` sound, and what the unit then does with it was not read |
| It calls method 6 of the slot's record | **Skipped**, for the same reason |

Around the call the pack silences its own grains of that slot (the source is not valid from the key on, three frames
before the call, so the grains have faded), and afterwards reads the object again and publishes the sample as new,
which also starts the overview afresh.

## Thread, and what was checked of the worker

The call is made from the pack's **frame function, on the display thread**, once, three frames after the key. The
input function only writes one word. Stock makes this call on its worker thread. No request of the worker's was found
that reaches the import without the name check (the only other caller of the import is the drum's drop, into the
drum's own sample), so the worker could not be used.

Idle, as far as it can be read: the queue's read place equals its write place. The worker moves the read place on
only after a request's work is done, so equal places mean no lift, drop or import of stock's is waiting or running.
What is **not** covered: a request posted after that check, while our call runs. To make that less likely the pack
takes the lift, drop and split keys (10 to 12, and the drop key) while a drop of its own is under way, on its own
page. A request from elsewhere (MIDI, another pack) is not held back.

## The key

The drop key's number is **taken to be 11, and that is a reading, not a finding**: open-field's table names 0 to 9
(help, metronome, the four mode keys, T1 to T4), and says the keys that play notes and shift are 13 to 53, which
leaves 10, 11 and 12 for lift, drop and split in the order they lie on the unit. The firmware's key dispatch was not
traced. Version 2's line still tells: every key the pack does not take, pressed on the page, is written once in the
log with its number. If drop is not 11, put the right number in a file `dropkey.txt` in the folder `veffgranular`
(the number alone) and leave disk mode; no rebuild is needed. Shift and drop is never taken: it stays stock's.

## Refusals

`DROP REFUSED n` on the page, and a line in the log: 1 the firmware is not OS 1.7.3+0 (the sum of 14 bytes at the
worker's call site `0x8f00b838` is not `64cae65e`; then nothing of the firmware's is read or called, and the key is
not even taken), 2 the page is not up for a synth sound that plays the engine, 3 disk mode is open, 4 the slot's
sample object is not where it should be, 6 the clipboard holds more or fewer than one track, 7 the worker has a
request. 5 is `NOTHING LIFTED`.

## What the log says

- once a start: `veffgranular: drop experiment: the call site sums to 64cae65e, as OS 1.7.3+0's: armed; the drop key is taken as 11`
- before the call: `veffgranular: drop: calling the import for synth <n>, object <address>, words <w> before, clipboard <frames> frames`
- after it: `veffgranular: drop: the import answered <r> after <ms> ms; words <w>, stereo <s> now; <d> done, <r> refused`

**It worked** when the line after the call is there, `words` is the clipboard's length plus 8 (twice that for a
stereo lift), the line `veffgranular: sampler source of synth <n>: frames 8 to <w> of ...` follows, and the page
shows the cut's shape. A log that ends at the line before the call means the unit hung inside the import.

## How it could go wrong

- **The unit hangs or faults inside the import.** It may wait for something only the worker thread is given, or need
  more stack than the display thread has (the worker's own function around it sets aside over 200 bytes before it
  even calls). The log then ends at the line before the call. Switch off and on.
- **The clipboard's file on the disk** (`clipboard/track_<n>.raw`) is read by the import while the worker could be
  writing it, if a lift starts in the middle. The import reads; the file should not be harmed, but the sample may be
  half old and half new. Lift again.
- **The clipboard's state in memory** (its list of tracks, its length) is read while the worker could change it: a
  length that no longer fits the file. The pool cannot be overrun by our side, since the import writes it, with its
  own bounds; that those bounds hold when its state changes under it is not known.
- **The slot's pool** is rewritten in place while the audio thread may read it: stock lives with that for the sampler,
  and our grains are silenced first.
- **The display stops** for as long as the import takes (the log line says how long); audio goes on.
- **The sampler does not know.** The slot's changed mark is not set, so the unit may not keep the new sample with the
  slot's state over a restart. The pool holds it until then.
- **Another pack's wrapper at the import's entry** runs on the display thread too, which its author did not plan for.

# The first run on a unit, and version 4

Version 3 ran once on one unit (OS 1.7.3+0, base 37, `8 tracks` loaded), on an empty slot with one track of 255,039
frames lifted. Key 11 is the drop key: confirmed. The log said the import answered 1 "after 0 ms" with `words` 0, and
the page showed no sample.

What that meant, read again in the firmware:

- **The import is the right function and takes the one argument we gave it.** `0x8f004204(object)` asks
  `0x8f00b78c()` (1 when the clipboard holds at most one track) and goes on into one of two renders,
  `0x8f003e8c(object)` or `0x8f003c78(object)`. The worker hands it `[holder]`, the object, in R0 and nothing else;
  neither render reads another register of the caller's. With `8 tracks` on the unit the entry jumps to that pack's
  function, which hands a clipboard of tracks 1 to 4 on to the firmware's own code; its counter of renders of its own
  stayed 0, so the firmware's render ran.
- **The render has no early way out but an empty clipboard.** `0x8f003e8c` takes the clipboard's length
  (`0x8f009710()`, the word at `0x84bdd9e4`) and the object's `frames` and renders the lesser of the two; it reads the
  object's stereo byte and never writes it (an empty slot's is 1, so the cut was rendered as stereo); it zeroes the
  frames it will render, opens the lifted tracks' stores, reads and writes the pool from its first word, closes them
  and ends with the overview (`0x8f0037be`). It tests no flag of the object or the holder, and nothing says "this
  slot is a sampler". The work is done inside it; the calls the worker makes afterwards only mark.
- **"0 ms" said nothing.** The base's clock moves once a frame, so a call inside one frame always measures 0.
- **`words` is not the sample's length.** The word at object + 0xd76c is written in two places only, both in the
  load of a sound's file (`0x8f0035c4`, `0x8f003624`). Neither render of a drop writes it, nor `frames`, and
  recording does not either. Version 3 read the length from `words`, found 0, and called the slot empty: **the drop
  very likely worked and the pack hid it.** That is not shown yet: version 3 did not look at the pool.
- **The second press 46 s later** was logged as a key not taken. Of the cheap checks only two can say no to key 11 on
  an armed unit: a drop already under way (then the key is taken and dropped, with no line) or shift held. So shift
  was held, and shift and drop is left to the unit on purpose. Version 4 says so in its line.

What version 4 does differently:

- **Where the sample ends** no longer comes from `words` (see `sampler_region.h`): the pool up to its last frame that
  is not silent; after a drop of the pack's own, exactly the frames that drop rendered (the clipboard's length, at
  most what the sample may hold), for as long as the pool's sum stays what it was right after the call.
- **The page says `DROPPED` only when the pool's sum changed**, else `DROP: NOTHING CHANGED`.
- **The log** has the object's fields before and after (frames, words, stereo, kind, its three unknown words), the
  pool's sum before and after and whether it changed, the frame the sound reaches, the worker's queue before and
  after, and 40 frames later the sum and the queue once more (should anything go on working after the call).
- The call, its argument, its thread and every check are as in version 3. No field of the object or the holder is
  written by the pack: stock's own drop writes none either.

**It worked** when the line after the call says `the pool changed` and `sounding to frame` is near the clipboard's
length, the line `veffgranular: sampler source of synth <n>: frames 0 to <length> of ..., a drop of our own` follows,
and the page shows the cut. `THE POOL IS AS IT WAS` with a clipboard that is not silent would mean the import does
refuse us after all, by a test not found in this reading.
