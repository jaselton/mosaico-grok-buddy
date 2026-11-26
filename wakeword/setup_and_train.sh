#!/bin/zsh
# One-shot environment setup + training for the "Hey Grok" wake word. Re-runnable.
set -eu
cd "${0:A:h}"
PY=/opt/homebrew/bin/python3.11
if [[ ! -d .venv ]]; then
  $PY -m venv .venv
fi
source .venv/bin/activate
python -m pip install -q --upgrade pip wheel
if [[ ! -f .deps_done ]]; then
  # pymicro-features >= 2 has macOS wheels and the process_samples API microWakeWord uses.
  pip install -q 'pymicro-features>=2.0.2'
  pip install -q 'git+https://github.com/whatsnowplaying/audio-metadata@d4ebb238e6a401bb1a5aaaac60c9e2b3cb30929f'
  [[ -d microWakeWord ]] || git clone -q https://github.com/kahrendt/microWakeWord
  pip install -q -e ./microWakeWord
  [[ -d piper-sample-generator ]] || git clone -q -b mps-support https://github.com/kahrendt/piper-sample-generator
  mkdir -p piper-sample-generator/models
  [[ -f piper-sample-generator/models/en_US-libritts_r-medium.pt ]] || \
    curl -L --fail -o piper-sample-generator/models/en_US-libritts_r-medium.pt \
      https://github.com/rhasspy/piper-sample-generator/releases/download/v2.0.0/en_US-libritts_r-medium.pt
  # datasets 4.x needs torchcodec for audio; 3.x decodes with soundfile.
  pip install -q tensorboard torch torchaudio piper-phonemize-cross==1.2.1 "datasets<4" librosa scipy tqdm pyyaml soundfile
  # PyTorch >= 2.6 defaults torch.load(weights_only=True); the official piper
  # generator checkpoint is a full pickled model from rhasspy's release.
  sed -i '' 's/torch.load(model_path)/torch.load(model_path, weights_only=False)/' \
    piper-sample-generator/generate_samples.py 2>/dev/null || \
    sed -i 's/torch.load(model_path)/torch.load(model_path, weights_only=False)/' \
    piper-sample-generator/generate_samples.py
  touch .deps_done
fi
python -u train_hi_grok.py
