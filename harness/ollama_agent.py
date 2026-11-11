#!/usr/bin/env python3
"""Lean zero-dependency local Ollama coding agent runner for July.

Designed specifically for local 7B coding models (e.g. qwen2.5-coder:7b) on CPU:
- Minimal (<350 token) system prompt for fast prefill
- Native Ollama /api/chat tool calls + fallback JSON-in-content parser
- Optional JULY_OWNED_FILES lease enforcement
- Real-time unbuffered stdout progress for July's /stream/ endpoint
"""

import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path


OLLAMA_CHAT_URL = os.environ.get("OLLAMA_CHAT_URL", "http://127.0.0.1:11434/api/chat")
MODEL = os.environ.get("OLLAMA_MODEL", "qwen2.5-coder:7b")
NUM_CTX = int(os.environ.get("OLLAMA_NUM_CTX", "8192"))
MAX_TURNS = int(os.environ.get("OLLAMA_MAX_TURNS", "12"))

OWNED_FILES = {
    p.strip().lstrip("./")
    for p in os.environ.get("JULY_OWNED_FILES", "").split(",")
    if p.strip()
}

TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "read_file",
            "description": "Read lines from a file inside the workspace.",
            "parameters": {
                "type": "object",
                "properties": {
                    "path": {"type": "string"},
                    "start_line": {"type": "integer"},
                    "end_line": {"type": "integer"},
                },
                "required": ["path"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "write_file",
            "description": "Write full content to a file inside the workspace (creates parent dirs).",
            "parameters": {
                "type": "object",
                "properties": {
                    "path": {"type": "string"},
                    "content": {"type": "string"},
                },
                "required": ["path", "content"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "edit_file",
            "description": "Replace exact old_text with new_text inside an existing file.",
            "parameters": {
                "type": "object",
                "properties": {
                    "path": {"type": "string"},
                    "old_text": {"type": "string"},
                    "new_text": {"type": "string"},
                },
                "required": ["path", "old_text", "new_text"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "run_shell",
            "description": "Run a shell command in the workspace root (e.g. gcc, make, tests).",
            "parameters": {
                "type": "object",
                "properties": {
                    "command": {"type": "string"},
                },
                "required": ["command"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "done",
            "description": "Call when the task is complete and verified.",
            "parameters": {
                "type": "object",
                "properties": {
                    "summary": {"type": "string"},
                },
                "required": ["summary"],
            },
        },
    },
]


def safe_rel_path(raw: str) -> tuple[str, Path]:
    root = Path.cwd().resolve()
    rel = raw.strip().lstrip("./")
    p = (root / rel).resolve()
    if p != root and root not in p.parents:
        raise ValueError(f"Path escapes workspace: {raw}")
    return p.relative_to(root).as_posix(), p


def check_write_allowed(rel: str) -> None:
    if OWNED_FILES and rel not in OWNED_FILES:
        raise PermissionError(
            f"Write blocked by lease: '{rel}' is outside allowed files {sorted(OWNED_FILES)}"
        )


def exec_tool(name: str, args: dict) -> tuple[str, bool]:
    try:
        if name in ("read_file", "read"):
            rel, p = safe_rel_path(args["path"])
            if not p.exists():
                return f"ERROR: File not found: {rel}", False
            lines = p.read_text(errors="replace").splitlines()
            start = max(1, int(args.get("start_line", 1)))
            end = min(len(lines), int(args.get("end_line", start + 199)))
            numbered = [f"{i}: {lines[i - 1]}" for i in range(start, end + 1)]
            return f"File {rel} ({len(lines)} lines total, showing {start}-{end}):\n" + "\n".join(numbered), False

        if name in ("write_file", "write"):
            rel, p = safe_rel_path(args["path"])
            check_write_allowed(rel)
            p.parent.mkdir(parents=True, exist_ok=True)
            content = args["content"]
            p.write_text(content)
            return f"OK: Wrote {len(content)} bytes to {rel}", False

        if name in ("edit_file", "edit"):
            rel, p = safe_rel_path(args["path"])
            check_write_allowed(rel)
            if not p.exists():
                return f"ERROR: File not found: {rel}", False
            src = p.read_text()
            old_t = args["old_text"]
            new_t = args["new_text"]
            if old_t not in src:
                return f"ERROR: old_text not found in {rel}", False
            p.write_text(src.replace(old_t, new_t, 1))
            return f"OK: Updated {rel}", False

        if name in ("run_shell", "bash", "shell"):
            cmd = args["command"]
            proc = subprocess.run(
                cmd,
                shell=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=60,
                stdin=subprocess.DEVNULL,
            )
            out = (proc.stdout or "")[-4000:]
            return f"exit_code={proc.returncode}\n{out}", False

        if name == "done":
            return f"DONE: {args.get('summary', '')}", True

        return f"ERROR: Unknown tool '{name}'", False
    except Exception as exc:
        return f"ERROR: {type(exc).__name__}: {exc}", False


def parse_fallback_tool_calls(text: str) -> list[dict]:
    """Parse tool-call JSON emitted as plain text by smaller local models."""
    calls: list[dict] = []
    decoder = json.JSONDecoder()
    idx = 0
    n = len(text)
    while idx < n:
        brace = text.find("{", idx)
        if brace < 0:
            break
        try:
            obj, end = decoder.raw_decode(text, brace)
            if isinstance(obj, dict) and "name" in obj and isinstance(obj.get("arguments"), dict):
                calls.append({"function": {"name": obj["name"], "arguments": obj["arguments"]}})
                idx = end
                continue
        except json.JSONDecodeError:
            pass
        idx = brace + 1
    return calls


def ollama_chat(messages: list[dict]) -> dict:
    payload = {
        "model": MODEL,
        "messages": messages,
        "tools": TOOLS,
        "stream": False,
        "options": {"num_ctx": NUM_CTX, "temperature": 0.1},
    }
    req = urllib.request.Request(
        OLLAMA_CHAT_URL,
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=300) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.URLError as exc:
        raise RuntimeError(f"Failed to reach Ollama at {OLLAMA_CHAT_URL}: {exc}") from exc


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: ollama_agent.py <brief_file>", file=sys.stderr)
        return 1

    brief = Path(sys.argv[1]).read_text()
    lease_note = (
        f"You are ONLY allowed to create/modify: {sorted(OWNED_FILES)}.\n"
        if OWNED_FILES
        else ""
    )
    system_prompt = (
        "You are an autonomous C/Linux coding agent working inside a workspace.\n"
        f"{lease_note}"
        "Use tools (`read_file`, `write_file`, `edit_file`, `run_shell`, `done`) to inspect files, "
        "make edits, run compiler/tests, and call `done` when verified."
    )

    messages = [
        {"role": "system", "content": system_prompt},
        {"role": "user", "content": brief},
    ]

    print(f"[ollama-agent] model={MODEL} ctx={NUM_CTX} owned={sorted(OWNED_FILES) or 'any'}", flush=True)

    for turn in range(1, MAX_TURNS + 1):
        print(f"[turn {turn}/{MAX_TURNS}] thinking...", flush=True)
        resp = ollama_chat(messages)
        msg = resp.get("message", {})
        content = (msg.get("content") or "").strip()
        tool_calls = msg.get("tool_calls") or []

        if not tool_calls and content:
            tool_calls = parse_fallback_tool_calls(content)

        messages.append(msg)

        if not tool_calls:
            if content:
                print(f"[assistant] {content}", flush=True)
            print("[ollama-agent] finished (no further tool calls).", flush=True)
            return 0

        for tc in tool_calls:
            fn = tc.get("function", {})
            tname = fn.get("name", "")
            targs = fn.get("arguments", {})
            if isinstance(targs, str):
                try:
                    targs = json.loads(targs)
                except json.JSONDecodeError:
                    targs = {}

            preview = targs.get("path") or targs.get("command") or targs.get("summary") or ""
            print(f"  -> tool: {tname}({str(preview)[:80]})", flush=True)
            result, is_done = exec_tool(tname, targs)
            lines = [ln for ln in result.splitlines() if ln.strip()]
            summary_line = " | ".join(lines[:2]) if lines else ""
            print(f"     {summary_line[:160]}", flush=True)

            messages.append({"role": "tool", "content": result})
            if is_done:
                return 0

    print(f"[!] Reached MAX_TURNS={MAX_TURNS}", flush=True)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())

