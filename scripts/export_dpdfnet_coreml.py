#!/usr/bin/env python3
"""Convert the pinned upstream 48 kHz HR streaming model; build-time only."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import numpy as np
import torch
import coremltools as ct
from coremltools.converters.mil import Builder as mb
from coremltools.converters.mil.frontend.torch.torch_op_registry import register_torch_op
from coremltools.converters.mil.frontend.torch.ops import _get_inputs

ROOT = Path(__file__).resolve().parents[1]
UNROLL_GRU = True
UPSTREAM = ROOT / "ThirdParty/DPDFNet/upstream"
sys.path.insert(0, str(UPSTREAM))
from onnx_model.export_dpdfnet_48khz_hr_to_onnx import build_model, DPDFNet48HROnnxWrapper


@register_torch_op
def unsafe_chunk(context, node):
    # GRUCell's three equal gate slices; no mutation/aliasing is used here.
    x, chunks, dim = _get_inputs(context, node, expected=3)
    assert x.shape[int(dim.val)] % int(chunks.val) == 0
    context.add(mb.split(x=x, num_splits=chunks, axis=dim, name=node.name), node.name)


@register_torch_op(override=True)
def gru(context, node):
    # Fuse gate projections and unroll the fixed frequency axis. This avoids
    # the generic converter's repeated while-loop/scatter work without changing
    # the reset-after equation or introducing reduced precision.
    x, h0, weights, bias, layers, dropout, training, bidirectional, batch_first = _get_inputs(context, node, expected=9)
    assert int(layers.val) == 1 and bool(bias.val) and float(dropout.val) == 0
    if batch_first.val:
        x = mb.transpose(x=x, perm=[1,0,2])
    outputs, states = [], []
    for direction in range(2 if bidirectional.val else 1):
        wi, wh, bi, bh = [v.val for v in weights[direction*4:direction*4+4]]
        hidden = mb.gather(x=h0, indices=np.int32(direction), axis=0)
        projected = mb.linear(x=x, weight=wi, bias=bi)
        def step(index, previous):
            gx = mb.gather(x=projected, indices=index, axis=0)
            gh = mb.linear(x=previous, weight=wh, bias=bh)
            xr, xz, xn = mb.split(x=gx, num_splits=3, axis=-1)
            hr, hz, hn = mb.split(x=gh, num_splits=3, axis=-1)
            reset = mb.sigmoid(x=mb.add(x=xr, y=hr))
            update = mb.sigmoid(x=mb.add(x=xz, y=hz))
            candidate = mb.tanh(x=mb.add(x=xn, y=mb.mul(x=reset,y=hn)))
            return mb.add(x=mb.mul(x=mb.sub(x=np.float32(1),y=update),y=candidate), y=mb.mul(x=update,y=previous))
        if UNROLL_GRU:
            sequence = []
            for index in (reversed(range(x.shape[0])) if direction else range(x.shape[0])):
                hidden = step(np.int32(index), hidden)
                sequence.append(mb.expand_dims(x=hidden, axes=[0]))
            if direction:
                sequence.reverse()
            outputs.append(mb.concat(values=sequence, axis=0))
        else:
            def condition(index, previous, sequence):
                return mb.less(x=index, y=np.int32(x.shape[0]))
            def body(index, previous, sequence):
                position = mb.sub(x=np.int32(x.shape[0]-1), y=index) if direction else index
                updated = step(position, previous)
                sequence = mb.scatter(data=sequence, indices=mb.expand_dims(x=position, axes=[0]), updates=mb.expand_dims(x=updated, axes=[0]), axis=0)
                return mb.add(x=index,y=np.int32(1)), updated, sequence
            _, hidden, sequence = mb.while_loop(_cond=condition, _body=body,
                loop_vars=(np.int32(0), hidden, np.zeros((x.shape[0],x.shape[1],wh.shape[1]),np.float32)))
            outputs.append(sequence)
        states.append(mb.expand_dims(x=hidden, axes=[0]))
    y = mb.concat(values=outputs, axis=2) if len(outputs)>1 else outputs[0]
    h = mb.concat(values=states, axis=0) if len(states)>1 else states[0]
    if batch_first.val:
        y = mb.transpose(x=y, perm=[1,0,2])
    context.add(y, node.outputs[0])
    context.add(h, node.outputs[1])


def main():
    global UNROLL_GRU
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=ROOT / "Resources/DPDFNet2.mlpackage")
    parser.add_argument("--fp16", action="store_true", help="Experimental reduced precision, requires new parity qualification")
    parser.add_argument("--loop-gru", action="store_true")
    args = parser.parse_args()
    UNROLL_GRU = not args.loop_gru
    torch.set_num_threads(1)
    model = build_model(argparse.Namespace(dprnn_num_blocks=2,
        checkpoint=UPSTREAM / "models/dpdfnet2_48khz_hr.pth"))
    # Upstream stores this fixed variance as integers. PyTorch sqrt
    # implicitly promotes it; Core ML requires the same promotion explicitly.
    model.erb_norm.var0 = model.erb_norm.var0.float()
    wrapper = DPDFNet48HROnnxWrapper(model).eval()
    state = model.initial_state(dtype=torch.float32)
    spec = torch.zeros(1, 1, 481, 2)
    with torch.no_grad():
        traced = torch.jit.trace(wrapper, (spec, state))
    mlmodel = ct.convert(traced, source="pytorch", convert_to="mlprogram",
        inputs=[ct.TensorType(name="spec", shape=spec.shape, dtype=np.float32),
                ct.TensorType(name="state_in", shape=state.shape, dtype=np.float32)],
        outputs=[ct.TensorType(name="spec_e", dtype=np.float32),
                 ct.TensorType(name="state_out", dtype=np.float32)],
        minimum_deployment_target=ct.target.macOS15,
        compute_precision=ct.precision.FLOAT16 if args.fp16 else ct.precision.FLOAT32,
        skip_model_load=True)
    mlmodel.short_description = "DPDFNet-2 48 kHz HR, official weights, explicit streaming state"
    mlmodel.author = "Ceva Inc.; Core ML conversion by MetalVoice2"
    mlmodel.license = "Apache-2.0"
    mlmodel.save(str(args.output))
    state.numpy().astype('<f4').tofile(args.output.parent / "DPDFNet2.state.f32")
    artifacts = [Path(__file__).resolve(), ROOT / "ThirdParty/DPDFNet/SOURCE_MANIFEST.json",
                 args.output.parent / "DPDFNet2.state.f32"]
    artifacts.extend(p for p in args.output.rglob('*') if p.is_file())
    manifest = dict(model="dpdfnet2_48khz_hr", sample_rate=48000, hop=480,
        delay_samples=2400, state_size=state.numel(), precision="Float16" if args.fp16 else "Float32", fused_gru=True, unrolled_gru=UNROLL_GRU,
        files={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(artifacts)})
    (args.output.parent / "DPDFNet2.manifest.json").write_text(json.dumps(manifest, indent=2)+'\n')
    print(json.dumps(dict(state_size=state.numel(), model=str(args.output))))


if __name__ == "__main__":
    main()
