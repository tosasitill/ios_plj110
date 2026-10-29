"""Load this repository's existing device-independent ADT/payload code."""
import importlib.util
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]


def load(name, relative):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    # Legacy scripts prepend their own directory for sibling imports. Do not
    # let that redirect later X9 imports (notably mkadt) into the X676C tools.
    search_path = sys.path[:]
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path[:] = search_path
    return module


adt = load('adt', 'x676c/tools/adt.py')
payload = load('x9_shared_payload', 'x676c/tools/mkpayload.py')
