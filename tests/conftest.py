import pathlib
import sys
import types

HEDGER_DIR = pathlib.Path(__file__).resolve().parent.parent / "hedger"
sys.path.insert(0, str(HEDGER_DIR))

try:
    import nats  # noqa: F401
except ImportError:
    _stub = types.ModuleType("nats")

    async def _connect(*args, **kwargs):
        raise RuntimeError("nats stub: unit tests must not touch the network")

    _stub.connect = _connect
    sys.modules["nats"] = _stub
