from __future__ import annotations

import importlib
import sys


sys.dont_write_bytecode = True
main = importlib.import_module("qiven_operator").main


if __name__ == "__main__":
    raise SystemExit(main())
