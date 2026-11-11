#!/usr/bin/env bash
set -euo pipefail

# Activate Fedora venv (defined as alias venv="source ~/venv/bin/activate" in ~/.zshrc)
VENV_DIR="${JULY_VENV:-$HOME/venv}"
if [ -f "$VENV_DIR/bin/activate" ]; then
    # shellcheck disable=SC1091
    source "$VENV_DIR/bin/activate"
else
    echo "[!] Virtualenv not found at $VENV_DIR" >&2
    exit 1
fi

BACKEND="${JULY_BACKEND:-ollama}"
BRIEF_FILE="${1:-agent_brief.md}"
if [ ! -f "$BRIEF_FILE" ]; then
    echo "[!] Missing task brief file: $BRIEF_FILE" >&2
    exit 1
fi

# Create an isolated git baseline inside the sandbox so we can export a clean diff
if [ ! -d .git ]; then
    git init -q
    git config user.name "july-fedora-agent"
    git config user.email "agent@july.local"
    git add -A
    git commit -q -m "base" --allow-empty
fi

AGENT_RC=0
if [ "$BACKEND" = "ollama" ]; then
    if [ ! -f harness/ollama_agent.py ]; then
        echo "[!] Missing harness/ollama_agent.py" >&2
        exit 1
    fi
    echo "[*] Spawning lean Ollama /api/chat agent (${OLLAMA_MODEL:-qwen2.5-coder:7b}) in $(pwd)..."
    set +e
    python3 -u harness/ollama_agent.py "$BRIEF_FILE" 2>&1 | tee agent_run.log
    AGENT_RC=${PIPESTATUS[0]}
    set -e
elif [ "$BACKEND" = "opencode" ]; then
    if ! command -v opencode >/dev/null 2>&1; then
        echo "[!] 'opencode' CLI missing in $VENV_DIR/bin. Install opencode-ai first." >&2
        exit 1
    fi
    MODEL_NAME="${OLLAMA_MODEL:-qwen2.5-coder:7b}"
    OLLAMA_URL="${OLLAMA_BASE_URL:-http://127.0.0.1:11434/v1}"

    cat > opencode.json <<EOF
{
  "\$schema": "https://opencode.ai/config.json",
  "provider": {
    "ollama": {
      "npm": "@ai-sdk/openai-compatible",
      "name": "Ollama (local)",
      "options": {
        "baseURL": "$OLLAMA_URL"
      },
      "models": {
        "$MODEL_NAME": {
          "name": "$MODEL_NAME"
        }
      }
    }
  },
  "model": "ollama/$MODEL_NAME"
}
EOF

    echo "[*] Spawning OpenCode ($(opencode --version)) -> local Ollama ($MODEL_NAME) in $(pwd)..."
    set +e
    opencode run --pure --auto -m "ollama/$MODEL_NAME" "$(cat "$BRIEF_FILE")" < /dev/null 2>&1 | tee agent_run.log
    AGENT_RC=${PIPESTATUS[0]}
    set -e
elif [ "$BACKEND" = "gemini" ]; then
    if ! command -v gemini >/dev/null 2>&1; then
        echo "[!] 'gemini' CLI missing in $VENV_DIR/bin. Run install_harness.json first." >&2
        exit 1
    fi
    if [ -z "${GEMINI_API_KEY:-}" ]; then
        echo "[!] GEMINI_API_KEY is not set in environment." >&2
        exit 1
    fi

    MODEL_ARGS=()
    if [ -n "${GEMINI_MODEL:-}" ]; then
        MODEL_ARGS=(-m "$GEMINI_MODEL")
    fi

    export GEMINI_CLI_HOME="/tmp/july_gemini_home"
    mkdir -p "$GEMINI_CLI_HOME/.gemini"
    cat > "$GEMINI_CLI_HOME/.gemini/settings.json" <<'EOF'
{
  "general": {
    "topicUpdateNarration": false
  },
  "security": {
    "auth": {
      "selectedType": "gemini-api-key"
    }
  }
}
EOF

    echo "[*] Spawning Gemini CLI ($(gemini --version)) in $(pwd)..."
    set +e
    gemini "${MODEL_ARGS[@]}" -p "$(cat "$BRIEF_FILE")" --yolo --skip-trust < /dev/null 2>&1 | tee agent_run.log
    AGENT_RC=${PIPESTATUS[0]}
    set -e
else
    echo "[!] Unknown JULY_BACKEND: $BACKEND (expected 'ollama', 'opencode', or 'gemini')" >&2
    exit 1
fi

EXCLUDES=(":!agent_run.log" ":!agent_patch.diff" ":!agent_files.txt" ":!agent_brief.md" ":!opencode.json")
git add -N -- . "${EXCLUDES[@]}" 2>/dev/null || true
git diff -- . "${EXCLUDES[@]}" > agent_patch.diff || true
git status --short -- . "${EXCLUDES[@]}" > agent_files.txt || true

echo "[*] Agent exited with code: $AGENT_RC"
echo "[*] Modified files summary:"
cat agent_files.txt || true

exit "$AGENT_RC"
