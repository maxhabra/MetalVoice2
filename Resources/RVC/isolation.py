"""Confine the trusted worker before loading third-party libraries or accepting audio."""
import ctypes
import os
from pathlib import Path
import resource
import sys



def confine(config):
    # macOS seatbelt parameters are data, including paths containing quotes.
    # Only system code and the installed runtime may supply executable mappings.
    # -S on Python 3.11 leaves sys.prefix pointing to the base interpreter for a venv.
    site = Path(sys.executable).parent.parent / "lib" / f"python{sys.version_info.major}.{sys.version_info.minor}" / "site-packages"
    params = {
        "PYTHON": Path(sys.base_prefix).resolve(),
        "SITE": site.resolve(),
        "CODE": Path(__file__).resolve().parent,
        "HARVEST": Path(__file__).resolve().parents[2] / "Frameworks/libWorldHarvest.dylib",
        "MODEL": Path(config["model"]).resolve(),
        "ENCODER": (Path(config["assets"]) / "contentvec/contentvec-f.onnx").resolve(),
        "PITCH": (Path(config["assets"]) / "rmvpe/rmvpe_20231006.onnx").resolve(),
        "INDEX": Path(config["index"]).resolve() if config.get("index") else Path("/dev/null"),
    }
    profile = b'''(version 1)
(deny default)
(import "dyld-support.sb")
(allow sysctl-read file-read-metadata)
(allow file-read* file-map-executable
    (subpath "/System/Library") (subpath "/usr/lib") (subpath "/Library/Apple")
    (subpath (param "PYTHON")) (subpath (param "SITE")) (literal (param "HARVEST")))
(allow file-read* (subpath (param "CODE")) (subpath "/usr/share")
    (literal "/dev/null") (literal "/dev/urandom") (literal "/dev/random")
    (literal (param "MODEL")) (literal (param "INDEX"))
    (literal (param "ENCODER")) (literal (param "PITCH")))'''
    os.chdir("/")
    for limit, target in ((resource.RLIMIT_CORE, 0), (resource.RLIMIT_NOFILE, 256),
                          (resource.RLIMIT_FSIZE, 0)):
        soft, hard = resource.getrlimit(limit)
        cap = min([target] + [v for v in (soft, hard) if v != resource.RLIM_INFINITY])
        resource.setrlimit(limit, (cap, cap))
    values = [os.fsencode(item) for pair in params.items() for item in pair]
    argv = (ctypes.c_char_p * (len(values) + 1))(*values, None)
    sandbox = ctypes.CDLL("/usr/lib/libsandbox.1.dylib")
    error = ctypes.c_char_p()
    sandbox.sandbox_init_with_parameters.argtypes = [
        ctypes.c_char_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_char_p)]
    if sandbox.sandbox_init_with_parameters(profile, 0, argv, ctypes.byref(error)) != 0:
        raise RuntimeError("Could not isolate the voice worker")
    # The app launches with -S. Add only this interpreter's own packages, without .pth hooks.
    if str(site) not in sys.path:
        sys.path.append(str(site))
