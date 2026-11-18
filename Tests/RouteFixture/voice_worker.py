"""Deterministic voice-stage stand-in for the native route regression."""
from array import array
import sys

sys.stdout.buffer.write(b"OK\n")
sys.stdout.buffer.flush()
while True:
    block = sys.stdin.buffer.read(1920)
    if len(block) != 1920:
        break
    converted = array("f", (-v for v in array("f", block)))
    sys.stdout.buffer.write(converted.tobytes())
    sys.stdout.buffer.flush()
