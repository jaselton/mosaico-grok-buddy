#!/usr/bin/env python3
"""Train a microWakeWord "Hi Grok" model (adapted from microWakeWord's
basic_training_notebook.ipynb). Each stage is skipped if its output exists,
so the script can be re-run after an interruption.

NOTE: the augmentation/negative datasets carry mixed licenses; per the
upstream notebook, the resulting model is for non-commercial personal use.
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
os.chdir(ROOT)
PHRASES = ["hi grok", "hi grock", "hai grok"]   # spellings that steer the TTS pronunciation
SAMPLES_PER_PHRASE = 1200


def sh(cmd):
    print(f"\n$ {cmd}", flush=True)
    subprocess.run(cmd, shell=True, check=True)


def stage(name):
    print(f"\n========== {name} ==========", flush=True)


# ---- 1. samples -------------------------------------------------------------
stage("generate samples")
if not Path("generated_samples").exists():
    Path("generated_samples").mkdir()
    for i, phrase in enumerate(PHRASES):
        out = f"gen_{i}"
        if not Path(out).exists():
            sh(f'{sys.executable} piper-sample-generator/generate_samples.py "{phrase}" '
               f'--max-samples {SAMPLES_PER_PHRASE} --batch-size 100 --output-dir {out}')
        for j, wav in enumerate(sorted(Path(out).glob("*.wav"))):
            wav.rename(Path("generated_samples") / f"p{i}_{j}.wav")
print("samples:", len(list(Path("generated_samples").glob("*.wav"))), flush=True)

# ---- 2. augmentation audio --------------------------------------------------
stage("augmentation data")
import datasets  # noqa: E402
import numpy as np  # noqa: E402
import scipy.io.wavfile  # noqa: E402
from tqdm import tqdm  # noqa: E402


def write16k(path, arr):
    scipy.io.wavfile.write(path, 16000, (np.asarray(arr) * 32767).astype(np.int16))


if not Path("mit_rirs/.done").exists():
    Path("mit_rirs").mkdir(exist_ok=True)
    for row in tqdm(datasets.load_dataset("davidscripka/MIT_environmental_impulse_responses",
                                          split="train", streaming=True)):
        write16k(f"mit_rirs/{row['audio']['path'].split('/')[-1]}", row["audio"]["array"])
    Path("mit_rirs/.done").touch()

if not Path("audioset_16k/.done").exists():
    # AudioSet moved to parquet shards with embedded audio (2026); one ~690 MB
    # balanced-train shard is plenty of background noise.
    Path("audioset").mkdir(exist_ok=True)
    shard = Path("audioset/09.parquet")
    if not shard.exists():
        sh("curl -L --fail -o audioset/09.parquet "
           "https://huggingface.co/datasets/agkphysics/AudioSet/resolve/main/data/bal_train/09.parquet")
    Path("audioset_16k").mkdir(exist_ok=True)
    # Read the parquet directly: its metadata uses datasets 4.x feature types.
    import io
    import librosa
    import pyarrow.parquet as pq
    import soundfile as sf
    table = pq.read_table(shard, columns=["audio"])
    for i, cell in enumerate(tqdm(table.column("audio").to_pylist())):
        try:
            data, sr = sf.read(io.BytesIO(cell["bytes"]), dtype="float32", always_2d=True)
            mono = data.mean(axis=1)
            if sr != 16000:
                mono = librosa.resample(mono, orig_sr=sr, target_sr=16000)
            write16k(f"audioset_16k/as_{i:05d}.wav", mono)
        except Exception as e:  # a few clips fail to decode; skip them
            print("skip", i, e)
    Path("audioset_16k/.done").touch()

if not Path("fma_16k/.done").exists():
    Path("fma").mkdir(exist_ok=True)
    if not Path("fma/fma_small").exists():
        sh("curl -L --fail -o fma/fma_xs.zip "
           "https://huggingface.co/datasets/mchl914/fma_xsmall/resolve/main/fma_xs.zip")
        sh("cd fma && unzip -q fma_xs.zip && rm fma_xs.zip")
    Path("fma_16k").mkdir(exist_ok=True)
    import librosa
    for i, mp3 in enumerate(tqdm(sorted(Path("fma/fma_small").glob("**/*.mp3")))):
        try:
            mono, _ = librosa.load(str(mp3), sr=16000, mono=True)
            write16k(f"fma_16k/{mp3.stem}.wav", mono)
        except Exception as e:
            print("skip", mp3.name, e)
    Path("fma_16k/.done").touch()

# ---- 3. augmented positive features ----------------------------------------
stage("augmented features")
from microwakeword.audio.augmentation import Augmentation  # noqa: E402
from microwakeword.audio.clips import Clips  # noqa: E402
from microwakeword.audio.spectrograms import SpectrogramGeneration  # noqa: E402
from mmap_ninja.ragged import RaggedMmap  # noqa: E402

if not Path("generated_augmented_features/.done").exists():
    clips = Clips(input_directory="generated_samples", file_pattern="*.wav",
                  max_clip_duration_s=None, remove_silence=False,
                  random_split_seed=10, split_count=0.1)
    augmenter = Augmentation(
        augmentation_duration_s=3.2,
        augmentation_probabilities={
            "SevenBandParametricEQ": 0.1, "TanhDistortion": 0.1, "PitchShift": 0.1,
            "BandStopFilter": 0.1, "AddColorNoise": 0.1, "AddBackgroundNoise": 0.75,
            "Gain": 1.0, "RIR": 0.5,
        },
        impulse_paths=["mit_rirs"], background_paths=["fma_16k", "audioset_16k"],
        background_min_snr_db=-5, background_max_snr_db=10,
        min_jitter_s=0.195, max_jitter_s=0.205)
    for split, name, rep, slide in (("training", "train", 2, 10),
                                    ("validation", "validation", 1, 10),
                                    ("testing", "test", 1, 1)):
        out = Path("generated_augmented_features") / split
        out.mkdir(parents=True, exist_ok=True)
        spec = SpectrogramGeneration(clips=clips, augmenter=augmenter, slide_frames=slide, step_ms=10)
        RaggedMmap.from_generator(out_dir=str(out / "wakeword_mmap"),
                                  sample_generator=spec.spectrogram_generator(split=name, repeat=rep),
                                  batch_size=100, verbose=True)
    Path("generated_augmented_features/.done").touch()

# ---- 4. negative features ---------------------------------------------------
stage("negative datasets")
Path("negative_datasets").mkdir(exist_ok=True)
for fname in ("dinner_party.zip", "dinner_party_eval.zip", "no_speech.zip", "speech.zip"):
    if not Path(f"negative_datasets/{fname}.done").exists():
        sh(f"curl -L --fail -o negative_datasets/{fname} "
           f"https://huggingface.co/datasets/kahrendt/microwakeword/resolve/main/{fname}")
        sh(f"cd negative_datasets && unzip -q -o {fname} && rm {fname}")
        Path(f"negative_datasets/{fname}.done").touch()

# ---- 5. train ---------------------------------------------------------------
stage("train")
import yaml  # noqa: E402

feat = lambda d, w, truth, trunc: {"features_dir": d, "sampling_weight": w, "penalty_weight": 1.0,
                                   "truth": truth, "truncation_strategy": trunc, "type": "mmap"}
config = {
    "window_step_ms": 10,
    "train_dir": "trained_models/hi_grok",
    "features": [
        feat("generated_augmented_features", 2.0, True, "truncate_start"),
        feat("negative_datasets/speech", 10.0, False, "random"),
        feat("negative_datasets/dinner_party", 10.0, False, "random"),
        feat("negative_datasets/no_speech", 5.0, False, "random"),
        feat("negative_datasets/dinner_party_eval", 0.0, False, "split"),
    ],
    "training_steps": [20000],
    "positive_class_weight": [1],
    "negative_class_weight": [20],
    "learning_rates": [0.001],
    "batch_size": 128,
    "time_mask_max_size": [0], "time_mask_count": [0],
    "freq_mask_max_size": [0], "freq_mask_count": [0],
    "eval_step_interval": 500,
    "clip_duration_ms": 1500,
    "target_minimization": 0.9,
    "minimization_metric": None,
    "maximization_metric": "average_viable_recall",
}
with open("training_parameters.yaml", "w") as f:
    yaml.dump(config, f)

sh(f"{sys.executable} -m microwakeword.model_train_eval "
   "--training_config=training_parameters.yaml --train 1 --restore_checkpoint 1 "
   "--test_tf_nonstreaming 0 --test_tflite_nonstreaming 0 --test_tflite_nonstreaming_quantized 0 "
   "--test_tflite_streaming 0 --test_tflite_streaming_quantized 1 --use_weights best_weights "
   "mixednet --pointwise_filters '64,64,64,64' --repeat_in_block '1, 1, 1, 1' "
   "--mixconv_kernel_sizes '[5], [7,11], [9,15], [23]' --residual_connection '0,0,0,0' "
   "--first_conv_filters 32 --first_conv_kernel_size 5 --stride 3")

model = Path("trained_models/hi_grok/tflite_stream_state_internal_quant/stream_state_internal_quant.tflite")
print("\nMODEL:", model, model.exists() and model.stat().st_size, flush=True)
