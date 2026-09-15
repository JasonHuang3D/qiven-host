from __future__ import annotations

import argparse
import concurrent.futures
from dataclasses import dataclass, asdict
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
CONFIG_PATH = ROOT / ".qiven" / "operator.json"
HEARTBEAT_SECONDS = 5.0
POLL_SECONDS = 0.05
ANSI_GREEN = "\x1b[32m"
ANSI_RED = "\x1b[31m"
ANSI_YELLOW = "\x1b[33m"
ANSI_CYAN = "\x1b[36m"
ANSI_RESET = "\x1b[0m"


def _flag(name: str) -> bool:
    return os.environ.get(name, "").strip().casefold() in {"1", "true", "yes", "on"}


def _color_enabled() -> bool:
    if _flag("QIVEN_OPERATOR_NO_COLOR") or os.environ.get("NO_COLOR") is not None or not sys.stdout.isatty():
        return False
    if os.name != "nt":
        return True
    try:
        import ctypes

        kernel32 = ctypes.windll.kernel32
        handle = kernel32.GetStdHandle(-11)
        mode = ctypes.c_uint()
        if not kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
            return False
        return bool(kernel32.SetConsoleMode(handle, mode.value | 0x0004))
    except Exception:
        return False


@dataclass
class Result:
    name: str
    status: str
    returncode: int
    duration_seconds: float = 0.0
    detail: str = ""
    output: str = ""


class Console:
    def __init__(self, *, json_mode: bool = False, verbose: bool = False, no_color: bool = False) -> None:
        self.json_mode = json_mode
        self.verbose = verbose
        self.color = _color_enabled() and not json_mode and not no_color
        self._lock = threading.Lock()

    def _paint(self, text: str, color: str) -> str:
        return f"{color}{text}{ANSI_RESET}" if self.color else text

    def tag(self, kind: str) -> str:
        mapping = {
            "run": ("[ RUN]", ANSI_CYAN),
            "wait": ("[WAIT]", ANSI_YELLOW),
            "ok": ("[ OK ]", ANSI_GREEN),
            "fail": ("[FAIL]", ANSI_RED),
        }
        text, color = mapping[kind]
        return self._paint(text, color)

    def emit(self, kind: str, message: str) -> None:
        if self.json_mode:
            return
        with self._lock:
            print(f"{self.tag(kind)} {message}", flush=True)

    def block(self, text: str) -> None:
        if self.json_mode or not text:
            return
        with self._lock:
            print(text, end="" if text.endswith("\n") else "\n", flush=True)


class OperatorError(RuntimeError):
    pass


def _load_config() -> dict[str, Any]:
    try:
        data = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise OperatorError(f"missing Operator config: {CONFIG_PATH}") from exc
    except json.JSONDecodeError as exc:
        raise OperatorError(f"invalid Operator config JSON: {exc}") from exc
    if data.get("schema_version") != 1:
        raise OperatorError("unsupported Operator config schema")
    return data


def _run_capture(argv: list[str], *, cwd: Path = ROOT) -> subprocess.CompletedProcess[str]:
    return subprocess.run(argv, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)


def _git(*args: str) -> subprocess.CompletedProcess[str]:
    return _run_capture(["git", *args])


def _toolchain() -> dict[str, str]:
    root = Path(os.environ.get("QIVEN_TOOLCHAIN_ROOT", ROOT.parent / "qiven-toolchain-win")).resolve()
    manifest = root / "toolchain.json"
    if not manifest.is_file():
        raise OperatorError(f"toolchain manifest not found: {manifest}")
    try:
        data = json.loads(manifest.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise OperatorError(f"invalid toolchain manifest: {exc}") from exc
    tools = data.get("tools", {})
    resolved: dict[str, str] = {"toolchain_root": str(root)}
    for logical, key in (("cmake", "cmake"), ("clang_format", "clang-format")):
        entry = tools.get(key)
        if not isinstance(entry, dict) or not entry.get("path"):
            raise OperatorError(f"toolchain manifest lacks {key}")
        path = (root / entry["path"]).resolve()
        if not path.is_file():
            raise OperatorError(f"tool not found: {path}")
        resolved[logical] = str(path)
    ctest = Path(resolved["cmake"]).with_name("ctest.exe" if os.name == "nt" else "ctest")
    if not ctest.is_file():
        raise OperatorError(f"ctest not found beside CMake: {ctest}")
    resolved["ctest"] = str(ctest)
    return resolved


def _expand(value: str, tools: dict[str, str] | None) -> str:
    if "{" not in value:
        return value
    mapping = {"root": str(ROOT)}
    if tools:
        mapping.update(tools)
    try:
        return value.format_map(mapping)
    except KeyError as exc:
        raise OperatorError(f"unknown command placeholder: {exc.args[0]}") from exc


def _argv_for_task(spec: dict[str, Any]) -> list[str]:
    raw = spec.get("argv")
    if not isinstance(raw, list) or not raw or not all(isinstance(item, str) and item for item in raw):
        raise OperatorError("command task requires non-empty string argv")
    needs_tools = any("{cmake}" in item or "{ctest}" in item or "{clang_format}" in item or "{toolchain_root}" in item for item in raw)
    tools = _toolchain() if needs_tools else None
    argv = [_expand(item, tools) for item in raw]
    candidate = Path(argv[0])
    if not candidate.is_absolute():
        local = ROOT / candidate
        if local.exists():
            argv[0] = str(local)
    if argv[0].lower().endswith((".cmd", ".bat")):
        if os.name != "nt":
            raise OperatorError("Windows batch task requested on non-Windows host")
        command_line = subprocess.list2cmdline(argv)
        return ["cmd.exe", "/d", "/s", "/c", f"call {command_line}"]
    return argv


def _run_process(name: str, spec: dict[str, Any], console: Console) -> Result:
    started = time.monotonic()
    console.emit("run", name)
    try:
        argv = _argv_for_task(spec)
    except OperatorError as exc:
        console.emit("fail", f"{name}: {exc}")
        return Result(name, "fail", 2, detail=str(exc))

    env = os.environ.copy()
    env.setdefault("PYTHONUTF8", "1")
    env.setdefault("PYTHONIOENCODING", "utf-8")
    with tempfile.NamedTemporaryFile(prefix="qiven-operator-", suffix=".log", delete=False) as handle:
        log_path = Path(handle.name)
    try:
        with log_path.open("w", encoding="utf-8", errors="replace") as log:
            try:
                process = subprocess.Popen(argv, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, text=True)
            except OSError as exc:
                detail = f"could not start process: {exc}"
                console.emit("fail", f"{name}: {detail}")
                return Result(name, "fail", 2, time.monotonic() - started, detail=detail)
            next_heartbeat = time.monotonic() + HEARTBEAT_SECONDS
            while True:
                returncode = process.poll()
                if returncode is not None:
                    break
                now = time.monotonic()
                if now >= next_heartbeat:
                    console.emit("wait", f"{name}: running for {now - started:.0f}s")
                    next_heartbeat = now + HEARTBEAT_SECONDS
                time.sleep(POLL_SECONDS)
        duration = time.monotonic() - started
        output = log_path.read_text(encoding="utf-8", errors="replace")
        if returncode == 0:
            console.emit("ok", f"{name} ({duration:.2f}s)")
            if console.verbose:
                console.block(output)
            return Result(name, "pass", 0, duration, output=output)
        console.emit("fail", f"{name}: exit {returncode} ({duration:.2f}s)")
        console.block(output)
        return Result(name, "fail", returncode, duration, output=output)
    finally:
        try:
            log_path.unlink()
        except OSError:
            pass


def _builtin(name: str, spec: dict[str, Any], console: Console, expect_head: str | None) -> Result:
    started = time.monotonic()
    console.emit("run", name)
    kind = spec.get("builtin")
    if kind == "exact_head":
        expected = expect_head or spec.get("expected")
        if not expected:
            console.emit("ok", f"{name}: no expected SHA supplied")
            return Result(name, "pass", 0, time.monotonic() - started)
        completed = _git("rev-parse", "HEAD")
        actual = completed.stdout.strip()
        if completed.returncode or actual != expected:
            detail = f"expected {expected}, got {actual or '<unresolved>'}"
            console.emit("fail", f"{name}: {detail}")
            return Result(name, "fail", 1, time.monotonic() - started, detail=detail, output=completed.stdout)
    elif kind == "git_diff_check":
        base = str(spec.get("base", "origin/main"))
        completed = _git("diff", "--check", f"{base}...HEAD")
        if completed.returncode:
            console.emit("fail", f"{name}: git diff --check failed")
            console.block(completed.stdout)
            return Result(name, "fail", completed.returncode, time.monotonic() - started, output=completed.stdout)
    elif kind == "git_clean_tree":
        completed = _git("status", "--porcelain=v1", "--untracked-files=all")
        dirty = completed.stdout.strip()
        if completed.returncode or dirty:
            detail = "working tree is not clean" if dirty else "git status failed"
            console.emit("fail", f"{name}: {detail}")
            console.block(completed.stdout)
            return Result(name, "fail", 1, time.monotonic() - started, detail=detail, output=completed.stdout)
    else:
        detail = f"unknown builtin: {kind}"
        console.emit("fail", f"{name}: {detail}")
        return Result(name, "fail", 2, time.monotonic() - started, detail=detail)
    duration = time.monotonic() - started
    console.emit("ok", f"{name} ({duration:.2f}s)")
    return Result(name, "pass", 0, duration)


def _run_task(name: str, tasks: dict[str, Any], console: Console, expect_head: str | None) -> Result:
    spec = tasks.get(name)
    if not isinstance(spec, dict):
        detail = f"unknown task: {name}"
        console.emit("fail", detail)
        return Result(name, "fail", 2, detail=detail)
    if "builtin" in spec:
        return _builtin(name, spec, console, expect_head)
    return _run_process(name, spec, console)


def _run_sequence(sequence: list[Any], tasks: dict[str, Any], console: Console, expect_head: str | None) -> list[Result]:
    results: list[Result] = []
    for stage in sequence:
        if isinstance(stage, str):
            result = _run_task(stage, tasks, console, expect_head)
            results.append(result)
            if result.returncode:
                break
            continue
        if isinstance(stage, list) and stage and all(isinstance(item, str) for item in stage):
            with concurrent.futures.ThreadPoolExecutor(max_workers=len(stage), thread_name_prefix="qiven") as pool:
                futures = {name: pool.submit(_run_task, name, tasks, console, expect_head) for name in stage}
                stage_results = [futures[name].result() for name in stage]
            results.extend(stage_results)
            if any(result.returncode for result in stage_results):
                break
            continue
        raise OperatorError(f"invalid gate stage: {stage!r}")
    return results


def _remote_repo() -> str:
    completed = _git("remote", "get-url", "origin")
    if completed.returncode:
        raise OperatorError("cannot resolve git remote 'origin'")
    value = completed.stdout.strip()
    if value.startswith("git@github.com:"):
        value = value.removeprefix("git@github.com:")
    elif "github.com/" in value:
        value = value.split("github.com/", 1)[1]
    else:
        raise OperatorError(f"origin is not a github.com repository: {value}")
    if value.endswith(".git"):
        value = value[:-4]
    if value.count("/") != 1:
        raise OperatorError(f"cannot parse GitHub repository from origin: {value}")
    return value


def _current_branch() -> str:
    completed = _git("symbolic-ref", "--quiet", "--short", "HEAD")
    branch = completed.stdout.strip()
    if completed.returncode or not branch:
        raise OperatorError("CI dispatch requires a named local branch")
    return branch


def _head() -> str:
    completed = _git("rev-parse", "HEAD")
    if completed.returncode:
        raise OperatorError("cannot resolve HEAD")
    return completed.stdout.strip()


def _remote_branch_head(branch: str) -> str:
    ref = f"refs/heads/{branch}"
    completed = _git("ls-remote", "--heads", "origin", ref)
    if completed.returncode:
        raise OperatorError(f"cannot resolve remote branch origin/{branch}")
    lines = [line.strip() for line in completed.stdout.splitlines() if line.strip()]
    if len(lines) != 1:
        if not lines:
            raise OperatorError(f"remote branch origin/{branch} does not exist")
        raise OperatorError(f"remote branch origin/{branch} resolved ambiguously")
    parts = lines[0].split()
    if len(parts) != 2 or parts[1] != ref:
        raise OperatorError(f"unexpected ls-remote result for origin/{branch}")
    return parts[0]


def _ci_start(config: dict[str, Any], profile: str, console: Console) -> dict[str, Any]:
    ci = config.get("ci", {})
    spec = ci.get(profile) if isinstance(ci, dict) else None
    if not isinstance(spec, dict):
        raise OperatorError(f"unknown CI profile: {profile}")
    workflow = spec.get("workflow")
    inputs = spec.get("inputs", {})
    if not isinstance(workflow, str) or not workflow:
        raise OperatorError("CI profile requires workflow")
    if not isinstance(inputs, dict):
        raise OperatorError("CI profile inputs must be an object")
    if shutil.which("gh") is None:
        raise OperatorError("GitHub CLI 'gh' was not found on PATH")
    branch = _current_branch()
    head = _head()
    repo = _remote_repo()
    console.emit("run", f"ci:{profile}: verify origin/{branch} exact HEAD")
    remote_head = _remote_branch_head(branch)
    if remote_head != head:
        raise OperatorError(
            f"CI dispatch requires origin/{branch} to match local HEAD: local={head} remote={remote_head}"
        )
    console.emit("ok", f"ci:{profile}: origin/{branch} matches {head}")
    argv = ["gh", "workflow", "run", workflow, "--repo", repo, "--ref", branch]
    for key, value in inputs.items():
        argv.extend(["-f", f"{key}={value}"])
    console.emit("run", f"ci:{profile}: dispatch {workflow} on {branch}")
    completed = _run_capture(argv)
    if completed.returncode:
        console.block(completed.stdout)
        raise OperatorError(f"ci:{profile}: dispatch failed")
    console.emit("ok", f"ci:{profile}: dispatch accepted; remote job continues asynchronously")
    return {
        "profile": profile,
        "workflow": workflow,
        "repository": repo,
        "branch": branch,
        "head": head,
        "remote_head": remote_head,
        "status": "dispatched",
    }


def _print_json(payload: dict[str, Any]) -> None:
    print(json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":")))


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="qiven", description="Qiven local engineering operator")
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    parser.add_argument("--verbose", action="store_true", help="show logs for successful tasks")
    parser.add_argument("--no-color", action="store_true", help="disable ANSI terminal color")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("info", help="show repository/operator metadata")
    gate = sub.add_parser("gate", help="run the configured local validation gate")
    gate.add_argument("--name", default=None, help="gate name; defaults to config default_gate")
    gate.add_argument("--expect-head", default=None, help="require exact Git HEAD")
    run = sub.add_parser("run", help="run declared task(s)")
    run.add_argument("tasks", nargs="+", help="task names")
    run.add_argument("--parallel", action="store_true", help="run requested tasks in parallel")
    ci = sub.add_parser("ci", help="asynchronous CI operations")
    ci_sub = ci.add_subparsers(dest="ci_command", required=True)
    ci_start = ci_sub.add_parser("start", help="dispatch CI and return immediately")
    ci_start.add_argument("profile", help="declared CI profile")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    console = Console(json_mode=args.json, verbose=args.verbose, no_color=args.no_color)
    try:
        config = _load_config()
        if args.command == "info":
            payload = {
                "status": "ok",
                "repository": config.get("repository_name"),
                "operator_schema": config.get("schema_version"),
                "default_gate": config.get("default_gate"),
                "root": str(ROOT),
                "head": _head(),
            }
            if args.json:
                _print_json(payload)
            else:
                console.emit("ok", f"repository={payload['repository']} head={payload['head']}")
            return 0

        tasks = config.get("tasks")
        if not isinstance(tasks, dict):
            raise OperatorError("config tasks must be an object")

        if args.command == "gate":
            gate_name = args.name or config.get("default_gate")
            gates = config.get("gates")
            if not isinstance(gates, dict) or not isinstance(gates.get(gate_name), list):
                raise OperatorError(f"unknown gate: {gate_name}")
            sequence = list(gates[gate_name])
            if args.expect_head:
                sequence.insert(0, "exact-head")
            started = time.monotonic()
            results = _run_sequence(sequence, tasks, console, args.expect_head)
            failed = [result for result in results if result.returncode]
            payload = {
                "status": "fail" if failed else "pass",
                "gate": gate_name,
                "head": _head(),
                "duration_seconds": round(time.monotonic() - started, 3),
                "results": [asdict(result) for result in results],
            }
            if args.json:
                _print_json(payload)
            else:
                console.emit("fail" if failed else "ok", f"gate:{gate_name}: {payload['status'].upper()}")
            return 1 if failed else 0

        if args.command == "run":
            started = time.monotonic()
            sequence: list[Any] = [list(args.tasks)] if args.parallel else list(args.tasks)
            results = _run_sequence(sequence, tasks, console, None)
            failed = [result for result in results if result.returncode]
            payload = {
                "status": "fail" if failed else "pass",
                "duration_seconds": round(time.monotonic() - started, 3),
                "results": [asdict(result) for result in results],
            }
            if args.json:
                _print_json(payload)
            else:
                console.emit("fail" if failed else "ok", f"run: {payload['status'].upper()}")
            return 1 if failed else 0

        if args.command == "ci" and args.ci_command == "start":
            payload = _ci_start(config, args.profile, console)
            if args.json:
                _print_json(payload)
            elif not console.json_mode:
                console.block(
                    f"      repository:  {payload['repository']}\n"
                    f"      branch:      {payload['branch']}\n"
                    f"      head:        {payload['head']}\n"
                    f"      remote-head: {payload['remote_head']}\n"
                    f"      workflow:    {payload['workflow']}"
                )
            return 0

        raise OperatorError("unsupported command")
    except OperatorError as exc:
        if args.json:
            _print_json({"status": "error", "error": str(exc)})
        else:
            console.emit("fail", str(exc))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
