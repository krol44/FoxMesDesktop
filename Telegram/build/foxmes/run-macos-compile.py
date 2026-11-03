import argparse
import os
from pathlib import Path
import signal
import subprocess
import time


class CompileInterrupted(Exception):
    def __init__(self, signum):
        self.signum = signum


def interrupt(signum, frame):
    raise CompileInterrupted(signum)


def group_exists(pgid):
    try:
        os.killpg(pgid, 0)
        return True
    except ProcessLookupError:
        return False


def stop_build(process):
    if not group_exists(process.pid):
        process.wait()
        return
    print("Stopping the owned compilation process group.", flush=True)
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    deadline = time.monotonic() + 4
    while group_exists(process.pid) and time.monotonic() < deadline:
        process.poll()
        time.sleep(0.1)
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def diagnostics(pgid):
    print(time.strftime("\nmacOS build diagnostics: %Y-%m-%d %H:%M:%S UTC", time.gmtime()), flush=True)
    commands = [
        ["uptime"],
        ["sysctl", "vm.swapusage"],
        ["vm_stat"],
        ["df", "-h", os.environ.get("RUNNER_TEMP", "/private/tmp")],
    ]
    for command in commands:
        try:
            subprocess.run(command, timeout=5, check=False)
        except (OSError, subprocess.TimeoutExpired) as error:
            print("Diagnostic unavailable:", error, flush=True)
    try:
        result = subprocess.run(
            ["ps", "-axo", "pid,ppid,pgid,%cpu,rss,etime,comm"],
            capture_output=True, text=True, timeout=5, check=False)
        print("Build group and compiler services: PID PPID PGID %CPU RSS_KiB ELAPSED COMMAND", flush=True)
        for line in result.stdout.splitlines()[1:]:
            fields = line.split(None, 6)
            if len(fields) != 7:
                continue
            compiler = fields[6].rsplit("/", 1)[-1]
            if fields[2] == str(pgid) or compiler in {"clang", "clang++", "ccache", "XCBBuildService"}:
                print(line, flush=True)
    except (OSError, subprocess.TimeoutExpired) as error:
        print("Process diagnostic unavailable:", error, flush=True)


def run_compile(command, timeout_seconds, diagnostic_interval=60):
    process = subprocess.Popen(command, start_new_session=True)
    deadline = time.monotonic() + timeout_seconds
    next_diagnostic = 0
    result = 1
    previous_handlers = {}
    try:
        for signum in (signal.SIGINT, signal.SIGTERM):
            previous_handlers[signum] = signal.signal(signum, interrupt)
        while True:
            result = process.poll()
            if result is not None:
                break
            if time.monotonic() >= deadline:
                print("Compilation reached its time budget; stopping before cache snapshot.", flush=True)
                result = 124
                break
            if time.monotonic() >= next_diagnostic:
                diagnostics(process.pid)
                next_diagnostic = time.monotonic() + diagnostic_interval
            try:
                result = process.wait(timeout=min(1, max(0.01, deadline - time.monotonic())))
                break
            except subprocess.TimeoutExpired:
                pass
    except CompileInterrupted as error:
        print("Compilation interrupted by signal", error.signum, flush=True)
        result = 128 + error.signum
    finally:
        for signum in previous_handlers:
            signal.signal(signum, signal.SIG_IGN)
        try:
            stop_build(process)
        finally:
            for signum, handler in previous_handlers.items():
                signal.signal(signum, handler)
    cache_dir = os.environ.get("FOXMES_CCACHE_DIR")
    if cache_dir:
        try:
            subprocess.run(
                ["ccache", "--show-stats"],
                env=dict(os.environ, CCACHE_DIR=cache_dir), timeout=10, check=False)
        except (OSError, subprocess.TimeoutExpired) as error:
            print("Compiler cache statistics unavailable:", error, flush=True)
    return result if result >= 0 else 128 - result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout-minutes", type=int, required=True)
    args = parser.parse_args()
    if args.timeout_minutes <= 0:
        parser.error("--timeout-minutes must be positive")
    script = Path(__file__).resolve().with_name("build-macos.sh")
    return run_compile(["bash", str(script), "compile"], args.timeout_minutes * 60)


if __name__ == "__main__":
    raise SystemExit(main())
