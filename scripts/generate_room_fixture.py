#!/usr/bin/env python3
"""Regenerate the room DSP oracle using pinned NARA-WPE + NumPy, never native DSP.
Requires NumPy (the model export environment works). No microphone or networking.
"""
import ast
import hashlib
import json
from pathlib import Path
import numpy as np

root = Path(__file__).resolve().parent.parent
source = root / 'ThirdParty/NaraWPE/wpe.py'
# Execute only these upstream functions, avoiding NARA's unrelated CLI dependencies.
tree = ast.parse(source.read_text())
namespace = {'np': np}
functions = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in
             ('online_wpe_step', '_stable_positive_inverse')]
exec(compile(ast.Module(body=functions, type_ignores=[]), str(source), 'exec'), namespace)
step = namespace['online_wpe_step']
base = np.fromfile(root / 'Tests/Fixtures/DPDFNet2.input.f32', dtype='<f4')
rng = np.random.default_rng(823)
rir = np.zeros(24000)
rir[0] = 1
for ms, gain in [(17,.15),(37,.2),(51,.2),(73,.17),(98,.13)]: rir[ms*48] = gain
t = np.arange(21600) / 48000
rir[2400:] += rng.normal(size=len(t)) * np.exp(-np.log(1000)*t/.6)*.013
n = 1 << (len(base) + len(rir) - 2).bit_length()
audio = np.fft.irfft(np.fft.rfft(base,n)*np.fft.rfft(rir,n),n)[:len(base)+24000]
audio = np.pad(audio, (0, (-len(audio)) % 480 + 480)).astype('<f4')
window = np.sin(np.pi * (np.arange(960) + .5) / 960)
history = np.zeros((15, 481), complex)
cov = np.tile(np.eye(12), (481,1,1)).astype(complex)
filters = np.zeros((481,12,1), complex)
power = np.zeros(481); gain = np.ones(481); cursor=0
previous = np.zeros(480); overlap=np.zeros(480); out=[]
for block in audio.reshape(-1,480):
 y = np.fft.rfft(np.concatenate([previous,block])*window)
 x = history[(cursor + 15 - 3 - np.arange(12)) % 15].T
 power = .9 * power + .1 * np.abs(y)**2
 # The first 12 slots supply reversed predictor taps to the upstream function;
 # its last slot supplies the observation. We manage the exact 30 ms gap above.
 buffer = np.zeros((16,481,1), complex)
 buffer[:12,:,0] = x[:,::-1].T
 buffer[-1,:,0] = y
 residual, next_cov, next_filters = step(buffer, np.maximum(power,1e-6), cov, filters, .999, 12, 3)
 active = power > 1e-6
 cov[active] = next_cov[active]; filters[active] = next_filters[active]
 history[cursor] = y; cursor=(cursor+1)%15
 target = np.clip(np.abs(residual[:,0])/np.maximum(abs(y),1e-9), .4, 1)
 gain += np.where(target>gain,.85,.5)*(target-gain)
 gain[0]=gain[-1]=1
 synthesis=np.fft.irfft(y*gain)*window
 out.append(synthesis[:480]+overlap)
 overlap=synthesis[480:];previous=block.copy()
output=np.concatenate(out).astype('<f4')
paths=[root/'Tests/Fixtures/RoomDereverb.input.f32',root/'Tests/Fixtures/RoomDereverb.reference.f32']
for p,x in zip(paths,[audio,output]):x.tofile(p)
manifest={'reference':'NARA-WPE online_wpe_step + independent NumPy STFT/mask',
 'upstreamCommit':'5fbfc3639274e5a70dffcf08e20615a38c372ebb',
 'sourceSHA256':hashlib.sha256(source.read_bytes()).hexdigest(),
 'baseSHA256':hashlib.sha256((root/'Tests/Fixtures/DPDFNet2.input.f32').read_bytes()).hexdigest(),
 'sampleRate':48000,'delaySamples':480,'samples':len(audio),'generator':'scripts/generate_room_fixture.py',
 'files':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}}
(root/'Tests/Fixtures/RoomDereverb.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
