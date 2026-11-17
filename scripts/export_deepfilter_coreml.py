#!/usr/bin/env python3
"""Export the pinned DFN3 ONNX weights as one native streaming Core ML step.

The temporal convolution caches and five GRU states persist across 10 ms calls.
Decoder history advances only when upstream's local-SNR thresholds enable it.
No audio feature extraction or synthesis is converted/reimplemented here.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tarfile

import coremltools as ct
from coremltools.converters.mil import Builder as mb
from coremltools.converters.mil.mil import types
from coremltools.converters.mil.mil.block import curr_block
import numpy as np
import onnx
from onnx import helper, numpy_helper

ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "ThirdParty/DeepFilterNet/upstream/models/DeepFilterNet3_onnx.tar.gz"
STATE_SIZE = 26304
STATE_SHAPES = [(1, 1, 2, 32), (1, 2, 2, 96)] + [(1, 256)] * 5 + [(1, 64, 4, 96)]


def const(x):
    return x if isinstance(x, np.ndarray) else np.asarray(x)


class StreamingGraph:
    def __init__(self, history, conditional=True):
        self.history = history
        self.offset = 0
        self.updates = []
        self.states = []
        self.conditional = conditional

    def state(self, shape, name):
        count = int(np.prod(shape))
        if isinstance(self.history, list):
            handle = self.history[len(self.states)]
            value = mb.cast(x=mb.read_state(input=handle), dtype="fp32")
        else:
            value = mb.slice_by_size(x=self.history, begin=[0, self.offset], size=[1, count])
            value = mb.reshape(x=value, shape=shape)
        self.states.append(dict(name=name, shape=list(shape), offset=self.offset, count=count))
        self.offset += count
        return value

    def update(self, previous, candidate, gate):
        if gate is not None and self.conditional:
            candidate = mb.select(cond=gate, a=candidate, b=previous)
        if isinstance(self.history, list):
            mb.coreml_update_state(state=self.history[len(self.states)-1], value=mb.cast(x=candidate, dtype="fp16"))
        else:
            self.updates.append(mb.reshape(x=candidate, shape=[1, -1]))

    def run(self, graph, inputs, prefix, gate=None):
        env = {x.name: numpy_helper.to_array(x).copy() for x in graph.initializer}
        env.update(inputs)
        for node in graph.node:
            args = [env[x] if x else None for x in node.input]
            args = [np.asarray(x.val) if hasattr(x, "val") and x.val is not None else x for x in args]
            attr = {a.name: helper.get_attribute_value(a) for a in node.attribute}
            op = node.op_type
            name = prefix + re.sub(r"[^a-zA-Z0-9_]", "_", node.name)
            if op == "Constant":
                value = numpy_helper.to_array(attr["value"])
            elif op == "Shape":
                value = np.array(args[0].shape, dtype=np.int64)
            elif op == "ConstantOfShape":
                scalar = numpy_helper.to_array(attr["value"]).reshape(-1)[0]
                value = np.full(tuple(args[0]), scalar)
            elif op == "Cast":
                assert isinstance(args[0], np.ndarray), node.name
                value = args[0].astype(helper.tensor_dtype_to_np_dtype(attr["to"]))
            elif op == "Gather":
                assert isinstance(args[0], np.ndarray), node.name
                value = np.take(args[0], args[1], axis=attr.get("axis", 0))
            elif op == "Concat":
                value = np.concatenate(args, axis=attr["axis"]) if all(isinstance(x, np.ndarray) for x in args) else mb.concat(values=args, axis=attr["axis"])
            elif op == "Reshape":
                shape = [int(v) for v in args[1]]
                shape = [args[0].shape[i] if v == 0 else v for i, v in enumerate(shape)]
                value = np.reshape(args[0], shape) if isinstance(args[0], np.ndarray) else mb.reshape(x=args[0], shape=shape)
            elif op == "Transpose":
                value = np.transpose(args[0], attr["perm"]) if isinstance(args[0], np.ndarray) else mb.transpose(x=args[0], perm=attr["perm"])
            elif op in ("Unsqueeze", "Squeeze"):
                axes = [int(x) for x in (args[1] if len(args) > 1 else attr.get("axes", [i for i, d in enumerate(args[0].shape) if d == 1]))]
                if isinstance(args[0], np.ndarray):
                    value = np.expand_dims(args[0], tuple(axes)) if op == "Unsqueeze" else np.squeeze(args[0], tuple(axes))
                else:
                    value = mb.expand_dims(x=args[0], axes=axes) if op == "Unsqueeze" else mb.squeeze(x=args[0], axes=axes)
            elif op == "Slice":
                x = args[0]
                axes = args[3] if len(args) > 3 else np.arange(len(args[1]))
                steps = args[4] if len(args) > 4 else np.ones(len(axes), dtype=int)
                slices = [slice(None)] * len(x.shape)
                for axis, start, end, step in zip(axes, args[1], args[2], steps):
                    slices[int(axis)] = slice(int(start), None if abs(int(end)) > 2**60 else int(end), int(step))
                if isinstance(x, np.ndarray):
                    value = x[tuple(slices)]
                else:
                    normalized = [s.indices(int(d)) for s, d in zip(slices, x.shape)]
                    value = mb.slice_by_index(x=x, begin=[s[0] for s in normalized], end=[s[1] for s in normalized], stride=[s[2] for s in normalized])
            elif op == "Pad":
                x = args[0]
                pads = [int(p) for p in args[1]]
                assert len(pads) == 8 and pads[2] in (2, 4) and sum(pads) == pads[2], (node.name, pads)
                shape = list(x.shape)
                shape[2] = pads[2]
                previous = self.state(shape, name)
                value = mb.concat(values=[previous, x], axis=2)
                begin = [0, 0, 1, 0]
                self.update(previous, mb.slice_by_size(x=value, begin=begin, size=shape), gate)
            elif op in ("Conv", "ConvTranspose"):
                pads = attr.get("pads", [0, 0, 0, 0])
                p = [pads[0], pads[2], pads[1], pads[3]]
                kw = dict(x=args[0], weight=args[1], strides=attr["strides"], dilations=attr["dilations"], groups=attr["group"], pad_type="custom", pad=p, name=name)
                if len(args) > 2:
                    kw["bias"] = args[2]
                if op == "Conv":
                    value = mb.conv(**kw)
                else:
                    # ONNX output_padding extends the high edge. Core ML uses
                    # the explicit output shape for this asymmetric final bin.
                    shape = list(args[0].shape)
                    shape[1] = args[1].shape[1] * attr["group"]
                    for i in range(2):
                        shape[i+2] = (shape[i+2]-1)*attr["strides"][i]-pads[i]-pads[i+2]+attr["dilations"][i]*(args[1].shape[i+2]-1)+attr.get("output_padding", [0,0])[i]+1
                    value = mb.conv_transpose(**kw, output_shape=shape)
            elif op == "Einsum":
                assert attr["equation"] == b"btgi,gih->btgh", node.name
                x, w = args
                groups, channels, outputs = w.shape
                assert tuple(x.shape[:2]) == (1, 1)
                # Grouped 1x1 convolution maps efficiently onto Apple engines.
                weight = np.ascontiguousarray(w.transpose(0, 2, 1).reshape(groups*outputs, channels, 1, 1))
                value = mb.conv(x=mb.reshape(x=x, shape=[1, groups*channels, 1, 1]), weight=weight, groups=groups, pad_type="valid", name=name)
                value = mb.reshape(x=value, shape=[1, 1, groups, outputs])
            elif op == "GRU":
                assert attr["linear_before_reset"] == 1 and attr["hidden_size"] == 256
                x, w, r, bias = args[:4]
                assert tuple(x.shape[:2]) == (1, 1)
                previous = self.state([1, 256], name)
                gx = mb.linear(x=mb.reshape(x=x, shape=[1, -1]), weight=w[0], bias=bias[0, :768])
                gh = mb.linear(x=previous, weight=r[0], bias=bias[0, 768:])
                xz, xr, xn = mb.split(x=gx, axis=-1, num_splits=3)
                hz, hr, hn = mb.split(x=gh, axis=-1, num_splits=3)
                z = mb.sigmoid(x=mb.add(x=xz, y=hz))
                reset = mb.sigmoid(x=mb.add(x=xr, y=hr))
                candidate = mb.tanh(x=mb.add(x=xn, y=mb.mul(x=reset, y=hn)))
                hidden = mb.add(x=mb.mul(x=mb.sub(x=np.float32(1), y=z), y=candidate), y=mb.mul(x=z, y=previous))
                self.update(previous, hidden, gate)
                env[node.output[0]] = mb.reshape(x=hidden, shape=[1, 1, 1, 256])
                if len(node.output) > 1:
                    env[node.output[1]] = mb.reshape(x=hidden, shape=[1, 1, 256])
                continue
            elif op == "MatMul":
                value = mb.matmul(x=args[0], y=args[1], name=name)
            elif op in ("Add", "Mul"):
                value = (mb.add if op == "Add" else mb.mul)(x=args[0], y=args[1], name=name)
            elif op in ("Relu", "Sigmoid", "Tanh"):
                value = {"Relu": mb.relu, "Sigmoid": mb.sigmoid, "Tanh": mb.tanh}[op](x=args[0], name=name)
            else:
                raise ValueError(f"Unsupported ONNX op {op}: {node.name}")
            assert len(node.output) == 1
            env[node.output[0]] = np.asarray(value) if isinstance(value, np.generic) else value
        return {v.name: env[v.name] for v in graph.output}

    def conditional_stage(self, graph, inputs, prefix, gate, output_name, output_shape, count):
        """Skip inactive decoders entirely, retaining their previous history."""
        assert not isinstance(self.history, list)
        offset = self.offset
        recorded = []
        def run_stage():
            child = StreamingGraph(self.history, conditional=False)
            child.offset = offset
            outputs = child.run(graph, inputs, prefix)
            assert child.offset == offset + count
            recorded.extend(child.states)
            return outputs[output_name], mb.concat(values=child.updates, axis=1)
        def keep_history():
            return mb.const(val=np.zeros(output_shape, np.float32)), mb.slice_by_size(x=self.history, begin=[0, offset], size=[1, count])
        output, updated = mb.cond(pred=mb.reshape(x=gate, shape=np.array([], dtype=np.int32)), _true_fn=run_stage, _false_fn=keep_history)
        self.states.extend(recorded)
        self.offset += count
        self.updates.append(updated)
        return output


def export(destination, stateful, fp16, conditional, separate=False, mixed=False, skip_inactive=False):
    assert not mixed or (fp16 and not stateful), "Mixed precision needs explicit Float32 encoder state"
    assert not skip_inactive or (conditional and not stateful and not fp16), "Conditional execution uses explicit full-precision history"
    with tarfile.open(ARCHIVE) as archive:
        graphs = {name: onnx.load_model_from_string(archive.extractfile(f"tmp/export/{name}.onnx").read()).graph for name in ["enc", "erb_dec", "df_dec"]}
    dtype = types.fp16 if fp16 else types.fp32
    specs = [mb.TensorSpec((1, 1, 1, 32), types.fp32), mb.TensorSpec((1, 2, 1, 96), types.fp32)]
    if separate:
        assert stateful and fp16
        specs.extend(mb.StateTensorSpec(shape, types.fp16) for shape in STATE_SHAPES)
    else:
        specs.append(mb.StateTensorSpec((1, STATE_SIZE), dtype) if stateful else mb.TensorSpec((1, STATE_SIZE), types.fp32))
    metadata = []

    def build(feat_erb, feat_spec, history):
        previous = history if separate else mb.read_state(input=history) if stateful else history
        previous = mb.cast(x=previous, dtype="fp32") if fp16 and stateful and not separate else previous
        stream = StreamingGraph(previous, conditional)
        enc = stream.run(graphs["enc"], {"feat_erb": feat_erb, "feat_spec": feat_spec}, "enc")
        lsnr = enc["lsnr"]
        erb_gate = mb.logical_and(x=mb.greater_equal(x=lsnr, y=np.float32(-10)), y=mb.less_equal(x=lsnr, y=np.float32(30)))
        df_gate = mb.logical_and(x=mb.greater_equal(x=lsnr, y=np.float32(-10)), y=mb.less_equal(x=lsnr, y=np.float32(20)))
        snr = mb.identity(x=lsnr, name="lsnr")
        if mixed:
            for op in curr_block().operations:
                op.name = "encoder_" + op.name
        if skip_inactive:
            erb = {"m": stream.conditional_stage(graphs["erb_dec"], enc, "erb", erb_gate, "m", (1, 1, 1, 32), 512)}
            df = {"coefs": stream.conditional_stage(graphs["df_dec"], enc, "df", df_gate, "coefs", (1, 1, 96, 10), 25088)}
        else:
            erb = stream.run(graphs["erb_dec"], enc, "erb", mb.reshape(x=erb_gate, shape=[1]))
            df = stream.run(graphs["df_dec"], enc, "df", mb.reshape(x=df_gate, shape=[1]))
        assert stream.offset == STATE_SIZE, stream.offset
        metadata.extend(stream.states)
        updated = mb.concat(values=stream.updates, axis=1, name="preserve_state") if not separate else None
        if stateful and not separate:
            updated = mb.cast(x=updated, dtype="fp16") if fp16 else updated
            mb.coreml_update_state(state=history, value=updated)
        gains = mb.identity(x=erb["m"], name="gains")
        coefs = mb.identity(x=df["coefs"], name="coefs")
        return (gains, coefs, snr) if stateful else (gains, coefs, snr, mb.identity(x=updated, name="next_history"))

    if separate:
        @mb.program(input_specs=specs, opset_version=ct.target.macOS15)
        def program(feat_erb, feat_spec, erb_history, spec_history, enc_gru, erb_gru0, erb_gru1, df_gru0, df_gru1, df_conv):
            return build(feat_erb, feat_spec, [erb_history, spec_history, enc_gru, erb_gru0, erb_gru1, df_gru0, df_gru1, df_conv])
    else:
        @mb.program(input_specs=specs, opset_version=ct.target.macOS15)
        def program(feat_erb, feat_spec, history):
            return build(feat_erb, feat_spec, history)

    precision = ct.transform.FP16ComputePrecision(op_selector=lambda op: not op.name.startswith("encoder_") and op.name not in ("preserve_state", "next_history")) if mixed else ct.precision.FLOAT16 if fp16 else ct.precision.FLOAT32
    model = ct.convert(program, convert_to="mlprogram", minimum_deployment_target=ct.target.macOS15,
                       compute_precision=precision,
                       skip_model_load=True)
    model.short_description = "DeepFilterNet3 streaming mono 48 kHz / 480 samples, pinned upstream weights"
    model.author = "DeepFilterNet authors; streaming Core ML export by MetalVoice2"
    model.license = "MIT OR Apache-2.0 (upstream code and model weights)"
    model.user_defined_metadata["source_sha256"] = hashlib.sha256(ARCHIVE.read_bytes()).hexdigest()
    model.save(str(destination))
    destination.with_suffix(".json").write_text(json.dumps(dict(state_size=STATE_SIZE, states=metadata, stateful=stateful, separate=separate, fp16=fp16, mixed=mixed, conditional=conditional, skip_inactive=skip_inactive, source_sha256=model.user_defined_metadata["source_sha256"]), indent=2)+"\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--stateful", action="store_true")
    parser.add_argument("--fp16", action="store_true")
    parser.add_argument("--always-run-decoders", action="store_true")
    parser.add_argument("--separate-states", action="store_true")
    parser.add_argument("--mixed-precision", action="store_true")
    parser.add_argument("--skip-inactive-decoders", action="store_true")
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    export(args.output, args.stateful, args.fp16, not args.always_run_decoders, args.separate_states, args.mixed_precision, args.skip_inactive_decoders)
