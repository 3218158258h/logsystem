#!/usr/bin/env python3
"""Two existing console clients, 10,000 records each, no send interval."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import threading
import time
import uuid

LEVELS = ("DEBUG", "INFO", "WARN", "ERROR")
COUNT = 10000
DEFAULT_CLIENT_QUEUE = 16384
DEFAULT_SERVER_QUEUE = 8192


def prepare_directory(root):
    #Only this reserved directory is cleared; normal logs stay untouched.
    root = Path(root).resolve()
    logs = root / "logs"
    directory = logs / "stress-test"
    if logs.is_symlink() or directory.is_symlink():
        raise RuntimeError("测试目录不能是符号链接")
    directory.mkdir(parents=True, exist_ok=True)
    if directory.resolve() != root / "logs" / "stress-test":
        raise RuntimeError("测试目录超出项目日志路径")
    entries = list(directory.iterdir())
    if any(p.is_dir() for p in entries):
        raise RuntimeError("测试目录含子目录，停止自动清理")
    for path in entries:
        path.unlink()
    return directory


def make_payload(run_id, worker, count=COUNT):
    return "".join(
        f"{LEVELS[(seq - 1) % 4]} stress_{worker} run={run_id} seq={seq}\n"
        for seq in range(1, count + 1)
    ) + ":quit\n"


def count_records(directory, run_id, count=COUNT):
    #Deduplicate this run's valid IDs, including rotated files.
    pattern = re.compile(
        r"\[(DEBUG|INFO|WARN|ERROR)\]\[[^\]\r\n]+\]\[stress_([AB])\]"
        r"\[run=" + re.escape(run_id) + r" seq=(\d+)\]$"
    )
    seen = set()
    duplicates = invalid = 0
    levels = dict.fromkeys(LEVELS, 0)
    workers = dict.fromkeys(("A", "B"), 0)
    for path in sorted(Path(directory).glob("aggregated.log*")):
        if not re.fullmatch(r"aggregated\.log(?:\.\d+)?", path.name):
            continue
        with path.open(encoding="utf-8", errors="replace") as stream:
            for line in stream:
                match = pattern.search(line.rstrip("\r\n"))
                if not match:
                    continue
                level, worker, sequence = match.groups()
                sequence = int(sequence)
                if not 1 <= sequence <= count or level != LEVELS[(sequence - 1) % 4]:
                    invalid += 1
                    continue
                key = (worker, sequence)
                if key in seen:
                    duplicates += 1
                    continue
                seen.add(key)
                workers[worker] += 1
                levels[level] += 1
    expected = count * 2
    return dict(expected=expected, received=len(seen), lost=expected - len(seen),
                loss_rate=(expected - len(seen)) / expected,
                workers=workers, levels=levels, duplicates=duplicates, invalid=invalid)


def workers_ready(output, pids):
    matches = re.findall(
        r"control worker session=\d+ pid=(\d+) process_instance=\d+ level=DEBUG\b",
        output,
    )
    return set(pids).issubset({int(pid) for pid in matches})


def stop_process(process, timeout=10):
    #Only process groups created by this script are stopped and reaped.
    if process.poll() is not None:
        return True
    try:
        if os.name == "posix":
            os.killpg(process.pid, signal.SIGTERM)
        else:
            process.terminate()
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=timeout)
        return True
    except subprocess.TimeoutExpired:
        if os.name == "posix":
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        else:
            process.kill()
        process.wait(timeout=5)
        return False


def run_test(root, build_dir, timeout, client_queue=DEFAULT_CLIENT_QUEUE, server_queue=DEFAULT_SERVER_QUEUE):
    directory = prepare_directory(root)
    run_id = uuid.uuid4().hex
    processes = []
    issues = []
    with ExitStack() as files:
        def spawn(command, name, interactive=False):
            output = files.enter_context((directory / name).open("w", encoding="utf-8"))
            process = subprocess.Popen(
                [str(x) for x in command],
                stdin=subprocess.PIPE if interactive else subprocess.DEVNULL,
                stdout=output, stderr=subprocess.STDOUT, text=True,
                start_new_session=(os.name == "posix"),
            )
            processes.append(process)
            return process

        try:
            client_bin = build_dir / "fdbus_console_demo"
            server_bin = build_dir / "ipc_log_project/log_server"
            name_bin = build_dir / "ipc_log_project/_deps/fdbus-build/name_server"
            if not all(p.is_file() for p in (client_bin, server_bin, name_bin)):
                build = spawn(["bash", root / "scripts/build.sh", build_dir], "build.log")
                try:
                    code = build.wait(timeout=300)
                except subprocess.TimeoutExpired:
                    raise RuntimeError("构建超时，查看 build.log")
                if code:
                    raise RuntimeError("构建失败，查看 build.log")
            service = "ipc_log_stress_" + run_id
            spawn([name_bin], "name_server.log")
            server = spawn(
                [server_bin, "--service", service, "--file", directory / "aggregated.log",
                 "--max-bytes", "67108864", "--backups", "3", "--queue", str(server_queue)],
                "server.log", True,
            )
            clients = [
                spawn([client_bin, "--name", "stress-" + worker, "--service", service,
                       "--level", "DEBUG", "--queue", str(client_queue), "--shutdown-timeout-ms", "60000"],
                      "client-" + worker + ".log", True)
                for worker in ("A", "B")
            ]
            #Confirm live sessions, not a stale "ready" string.
            deadline = time.monotonic() + 20
            while True:
                if any(p.poll() is not None for p in [server] + clients):
                    raise RuntimeError("服务或客户端提前退出，查看后台日志")
                response_start = (directory / "server.log").stat().st_size
                server.stdin.write("list\n")
                server.stdin.flush()
                response_deadline = min(deadline, time.monotonic() + 3)
                while time.monotonic() < response_deadline:
                    output = (directory / "server.log").read_text(encoding="utf-8", errors="replace")
                    if workers_ready(output, [p.pid for p in clients]):
                        break
                    if "control " in output[response_start:]:
                        break
                    time.sleep(0.05)
                if workers_ready(output, [p.pid for p in clients]):
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError("两个客户端未完成在线与 DEBUG 等级确认")
                time.sleep(0.1)

            payloads = [make_payload(run_id, w) for w in ("A", "B")]
            start = threading.Event()

            def send(process, payload):
                start.wait()
                try:
                    process.communicate(input=payload, timeout=timeout)
                except subprocess.TimeoutExpired:
                    stop_process(process, 3)
                    return "客户端超时，已停止"
                return None if process.returncode in (0, 2) else "客户端异常退出"

            began = time.monotonic()
            with ThreadPoolExecutor(max_workers=2) as pool:
                futures = [pool.submit(send, p, data) for p, data in zip(clients, payloads)]
                start.set()
                for future in futures:
                    reason = future.result()
                    if reason:
                        issues.append(reason)
            if not stop_process(server, 20):
                issues.append("服务端排空超时，已强制停止")
            if server.returncode not in (0, 2):
                issues.append("服务端异常退出")
            report = count_records(directory, run_id)
            report.update(run_id=run_id, elapsed_seconds=round(time.monotonic() - began, 3),
                          issues=issues, queues=dict(client=client_queue, server=server_queue),
                          client_stats={w: read_final_stats(directory / ("client-" + w + ".log"),
                                                          "stress-" + w + " shutdown=") for w in ("A", "B")},
                          server_stats=read_final_stats(directory / "server.log", "log_server stopped "))
            (directory / "result.json").write_text(
                json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
            return report, directory
        finally:
            for process in reversed(processes):
                stop_process(process, 3)


def read_final_stats(path, prefix):
    for line in reversed(Path(path).read_text(encoding="utf-8", errors="replace").splitlines()):
        if line.startswith(prefix):
            return {key: int(value) for key, value in re.findall(r"(accepted|dropped|rejected|uncertain|write_errors)=(\d+)", line)}
    return None


def queue_capacity(text):
    try:
        value = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError("队列容量必须为 1..65536")
    if not 1 <= value <= 65536:
        raise argparse.ArgumentTypeError("队列容量必须为 1..65536")
    return value


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parents[1]
    parser.add_argument("--build-dir", type=Path, default=root / "build-demo")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--client-queue", type=queue_capacity, default=DEFAULT_CLIENT_QUEUE,
                        help="每个客户端队列容量，默认 16384")
    parser.add_argument("--server-queue", type=queue_capacity, default=DEFAULT_SERVER_QUEUE,
                        help="服务端汇聚队列容量，默认 8192")
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout 必须为正数")
    return args


def main():
    args = parse_args()
    root = Path(__file__).resolve().parents[1]
    if not sys.platform.startswith("linux"):
        print("请在 Linux 环境运行此脚本。", file=sys.stderr)
        return 1
    import fcntl
    logs = root / "logs"
    if logs.is_symlink():
        print("logs 不能是符号链接。", file=sys.stderr)
        return 1
    logs.mkdir(exist_ok=True)
    try:
        with (logs / ".stress-test.lock").open("a") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise RuntimeError("已有压力测试在运行")
            report, directory = run_test(root, args.build_dir.resolve(), args.timeout,
                                         args.client_queue, args.server_queue)
        print(f"压力测试{'完成' if not report['issues'] else '异常结束'}：2 个进程 × {COUNT} 条，发送间隔 0ms")
        print(f"队列容量：客户端 {args.client_queue}/进程，服务端 {args.server_queue}")
        print(f"发送 {report['expected']}，文件实收 {report['received']}，丢失 {report['lost']}，丢失率 {report['loss_rate']:.2%}")
        for worker, received in report["workers"].items():
            print(f"进程 {worker}：实收 {received}/{COUNT}，丢失率 {(COUNT - received) / COUNT:.2%}")
            stats = report["client_stats"][worker]
            if stats is not None:
                print(f"  客户端计数：入队={stats.get('accepted')}，丢弃={stats.get('dropped')}，拒绝={stats.get('rejected')}，结果未知={stats.get('uncertain')}")
        stats = report["server_stats"]
        if stats is not None:
            print(f"服务端计数：接收={stats.get('accepted')}，丢弃={stats.get('dropped')}，拒绝={stats.get('rejected')}，写错误={stats.get('write_errors')}")
        for index, level in enumerate(LEVELS):
            expected = 2 * (COUNT // 4 + (index < COUNT % 4))
            received = report["levels"][level]
            print(f"{level}：实收 {received}/{expected}，丢失率 {(expected - received) / expected:.2%}")
        print(f"重复 {report['duplicates']}，无效 {report['invalid']}；耗时 {report['elapsed_seconds']:.3f}s")
        print(f"后台日志与结果：{directory}")
        if report["issues"]:
            print("；".join(report["issues"]))
            return 1
        return 0
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"压力测试失败：{error}；后台日志：{logs / 'stress-test'}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
