# "Hi Grok" wake word

The buddy can wake hands-free on a [microWakeWord](https://github.com/kahrendt/microWakeWord)
model. No model is shipped: train your own and drop it here as
`wakeword/hi_grok.tflite` (git-ignored), then rebuild. Without it the buddy is
tap-to-talk only.

```sh
cd wakeword
zsh setup_and_train.sh        # macOS/Linux, Python 3.11; several hours on a laptop CPU
cp trained_models/hi_grok/tflite_stream_state_internal_quant/stream_state_internal_quant.tflite hi_grok.tflite
```

`train_hi_grok.py` follows microWakeWord's basic training notebook: ~3,600
synthetic samples of "hi grok" (three spellings) from Piper TTS, augmented with
room impulse responses, AudioSet and FMA background audio, trained against
microWakeWord's negative feature sets, then quantized for streaming. Edit
`PHRASES` to train a different phrase.

**License note:** the augmentation and negative datasets carry mixed licenses;
per the upstream notebook, models trained this way are for **non-commercial
personal use**. That is why no model is committed here.

On the device (`components/app_wake`), the TFLite Micro audio frontend is set
up exactly like the training features (30 ms windows every 10 ms, 40 channels,
125–7500 Hz), and detection fires when the mean of the last five model scores
passes 0.90. Detection only runs while the buddy is idle (half duplex), so it
can't hear itself.
