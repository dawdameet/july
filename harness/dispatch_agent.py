#!/usr/bin/env python3
"""Dispatch a scoped coding subtask to the Fedora open-source agent harness over July."""

import argparse
import fnmatch
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parent.parent


def load_api_key() -> str:
    if os.environ.get("GEMINI_API_KEY"):
        return os.environ["GEMINI_API_KEY"].strip()
    env_file = ROOT_DIR / ".july" / "agent.env"
    if env_file.exists():
        for line in env_file.read_text().splitlines():
            line = line.strip()
            if line.startswith("GEMINI_API_KEY="):
                return line.split("=", 1)[1].strip().strip('"').strip("'")
    return ""


def collect_project_files(ignore_patterns: list[str]) -> list[str]:
    files: list[str] = []
    for path in sorted(ROOT_DIR.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(ROOT_DIR).as_posix()
        parts = rel.split("/")
        skip = False
        for pat in ignore_patterns:
            if fnmatch.fnmatch(path.name, pat) or fnmatch.fnmatch(rel, pat) or any(fnmatch.fnmatch(p, pat) for p in parts):
                skip = True
                break
        if not skip:
            files.append(rel)
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description="Dispatch a subtask to the Fedora agent harness via July")
    parser.add_argument("brief", nargs="?", default="", help="Path to the task brief markdown/text file")
    parser.add_argument("--backend", choices=["ollama", "gemini"], default="ollama", help="LLM backend on Fedora (default: ollama)")
    parser.add_argument("--target", default="", help="Target host:port (defaults to .julyconfig target)")
    parser.add_argument("--timeout", type=int, default=0, help="Execution timeout in seconds (default: 600 ollama, 240 gemini)")
    parser.add_argument("--model", default="", help="Optional model override (e.g. qwen2.5-coder:7b or gemini-2.5-flash)")
    parser.add_argument("--name", default="july-agent-task", help="Workflow name")
    parser.add_argument("--out-dir", default=".july/agent_out", help="Directory to store downloaded artifacts")
    parser.add_argument("--async", dest="async_mode", action="store_true", help="Dispatch asynchronously and return JULY_ASYNC_TASK_ID immediately")
    parser.add_argument("--wait", dest="wait_id", default="", help="Attach to an async task ID, stream live logs, and retrieve artifacts")
    args = parser.parse_args()

    julycfg_path = ROOT_DIR / ".julyconfig"
    julycfg = json.loads(julycfg_path.read_text()) if julycfg_path.exists() else {}
    target = args.target or julycfg.get("target", "192.168.1.41:8080")

    out_dir = (ROOT_DIR / args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    artifact_names = ["agent_patch.diff", "agent_files.txt", "agent_run.log"]

    if args.wait_id:
        wait_timeout = args.timeout if args.timeout > 0 else 600
        cmd = [str(ROOT_DIR / "dispatch"), target, "-t", str(wait_timeout), "--wait", args.wait_id]
        proc = subprocess.run(cmd, cwd=ROOT_DIR)
        for name in artifact_names:
            src = ROOT_DIR / name
            if src.exists():
                shutil.move(str(src), str(out_dir / name))
        return proc.returncode

    timeout = args.timeout if args.timeout > 0 else (600 if args.backend == "ollama" else 240)

    env_map: dict[str, str] = {"JULY_BACKEND": args.backend}
    if args.backend == "gemini":
        api_key = load_api_key()
        if not api_key:
            print("[!] GEMINI_API_KEY not found in env or .july/agent.env", file=sys.stderr)
            return 1
        env_map["GEMINI_API_KEY"] = api_key
        if args.model:
            env_map["GEMINI_MODEL"] = args.model
    else:
        if args.model:
            env_map["OLLAMA_MODEL"] = args.model

    if not args.brief:
        print("[!] Brief file is required unless --wait <task_id> is passed.", file=sys.stderr)
        return 1

    brief_path = Path(args.brief).resolve()
    if not brief_path.exists():
        print(f"[!] Brief file not found: {brief_path}", file=sys.stderr)
        return 1

    ignores = julycfg.get("ignore", [".git*", "*.o", "*.bin", ".july*", "*.dSYM", ".DS_Store", "dist"])

    files = collect_project_files(ignores)
    brief_rel = "agent_brief.md"

    manifest = {
        "name": args.name,
        "options": {
            "timeout_sec": timeout,
            "kill_if_timeout": True,
        },
        "sandbox": {
            "network": True,
            "namespace": False,
            "memory_mb": 0 if args.backend == "ollama" else 2048,
        },
        "env": env_map,
        "pre_flight": {
            "check_tools": ["node", "npm", "git"],
        },
        "build": [
            f"bash -c 'chmod +x harness/july_harness.sh && ./harness/july_harness.sh {brief_rel}'"
        ],
        "artifacts": {
            "required": ["agent_patch.diff", "agent_files.txt", "agent_run.log"],
            "on_failure": ["agent_run.log"],
        },
    }

    tmp_manifest = ROOT_DIR / ".july" / "runtime_agent_workflow.json"
    tmp_manifest.parent.mkdir(parents=True, exist_ok=True)

    staged_brief_root = ROOT_DIR / brief_rel
    created_temp_brief = False
    try:
        if brief_path != staged_brief_root:
            staged_brief_root.write_text(brief_path.read_text())
            created_temp_brief = True
        if brief_rel not in files:
            files.append(brief_rel)

        tmp_manifest.write_text(json.dumps(manifest, indent=2))

        cmd = [
            str(ROOT_DIR / "dispatch"),
            target,
            "-t",
            str(timeout),
        ]
        if args.async_mode:
            cmd.append("--async")
        cmd.extend(["workflow", str(tmp_manifest), *files])
        print(f"[+] Dispatching Fedora agent task '{args.name}' [backend={args.backend}] ({len(files)} files) -> {target}")
        proc = subprocess.run(cmd, cwd=ROOT_DIR)
        if not args.async_mode:
            for name in artifact_names:
                src = ROOT_DIR / name
                if src.exists():
                    shutil.move(str(src), str(out_dir / name))
        return proc.returncode
    finally:
        if tmp_manifest.exists():
            tmp_manifest.unlink()
        if created_temp_brief and staged_brief_root.exists():
            staged_brief_root.unlink()


if __name__ == "__main__":
    raise SystemExit(main())
