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
