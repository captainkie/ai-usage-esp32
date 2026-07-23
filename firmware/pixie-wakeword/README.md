# "Pixie" custom wake word (microWakeWord)

An on-device wake word for **"Pixie"** so the bar wakes on voice, not just a tap.
Trained with [microWakeWord](https://github.com/kahrendt/microWakeWord) — the same
framework ESPHome / Home Assistant Voice use — because Espressif's stock esp-sr
WakeNet (bundled with Arduino-ESP32) only ships "Hi ESP"; "Pixie"/"Jarvis" are not
in it and can't be added without ESP-IDF.

## Files
- `pixie.tflite` — INT8 quantized **streaming** model, ~61 KB. Input `[1, 3, 40]`
  int8 (3 frames × 40 log-mel features), output `[1, 1]` uint8 (probability = value/256).
- `pixie.json` — manifest (esphome micro_wake_word **v2** format). `probability_cutoff`
  is the tuning knob — **start high (0.95) and lower only if it won't trigger.**

## How it was trained (reproducible)
1. `pip install microwakeword` (+ torch, piper-phonemize) in a **Python 3.12** venv —
   `tensorflow>=2.18`, `pymicro-features>=2` (mainline, not the mac fork), `datasets<4`
   (v5 needs torchcodec to decode even WAV), and `tensorboard` (train.py logs scalars).
2. **Positives:** 1000 "pixie" clips via Piper sample generator (multi-speaker
   `en_US-libritts_r-medium` — diverse voices so *anyone* can trigger it; the female
   persona is the *name*, not a voice restriction). Needs
   `torch.load(..., weights_only=False)` on torch ≥ 2.6.
3. **Negatives:** microWakeWord's pre-computed spectrogram feature sets from
   HuggingFace `kahrendt/microwakeword` (`dinner_party`, `no_speech`, `dinner_party_eval`).
   The large `speech` set was **dropped for disk** on the training machine, and the
   FMA/AudioSet/RIR audio augmentation was skipped (datasets/torchcodec + disk).
4. `mixednet` model, 10 000 steps → quantize → streaming TFLite.

## ⚠️ Honest caveats
- Training metrics were **~perfect (recall 100 %, 0 ambient false-positives)** but that
  is **optimistic**: the eval positives are TTS (same distribution as training) and the
  negative/ambient coverage was reduced (no `speech` set, no background-noise aug). So:
  **detecting "Pixie" should work; real-world false-accepts are likely higher than the
  metrics imply.** Verify on the board and tune `probability_cutoff` up if it triggers
  on random speech. Retrain with the `speech` set + background noise for a v2.
- **Not yet verified on hardware.** Push-to-talk (tap the orb on screen ④) stays the
  reliable default; the wake word is additive.

## Retrain / improve
Re-run the microWakeWord `basic_training_notebook.ipynb` with: the `speech` negative set
added back, FMA/AudioSet + MIT RIR background augmentation enabled (install `torchcodec`
or keep `datasets<4`), and more positive samples / phonetic variants of "pixie". Then
drop the new `pixie.tflite` here — the firmware loads it by file, no code change.
