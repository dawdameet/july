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

if ! command -v gemini >/dev/null 2>&1; then
    echo "[!] 'gemini' CLI missing in $VENV_DIR/bin. Run install_harness.json first." >&2
    exit 1
fi

if [ -z "${GEMINI_API_KEY:-}" ]; then
    echo "[!] GEMINI_API_KEY is not set in environment." >&2
    exit 1
fi

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

echo "[*] Spawning open-source Gemini CLI harness in $(pwd)..."
echo "[*] Gemini version: $(gemini --version)"

MODEL_ARGS=()
if [ -n "${GEMINI_MODEL:-}" ]; then
    MODEL_ARGS=(-m "$GEMINI_MODEL")
fi

# Isolate gemini-cli home so global ~/.gemini/settings.json (oauth-personal) doesn't override GEMINI_API_KEY
export GEMINI_CLI_HOME="/tmp/july_gemini_home"
mkdir -p "$GEMINI_CLI_HOME/.gemini"
cat > "$GEMINI_CLI_HOME/.gemini/settings.json" <<'EOF'
{
  "security": {
    "auth": {
      "selectedType": "gemini-api-key"
    }
  }
}
EOF

set +e
gemini "${MODEL_ARGS[@]}" -p "$(cat "$BRIEF_FILE")" --yolo --skip-trust < /dev/null 2>&1 | tee agent_run.log
AGENT_RC=${PIPESTATUS[0]}
set -e

EXCLUDES=(":!agent_run.log" ":!agent_patch.diff" ":!agent_files.txt" ":!agent_brief.md")
git add -N -- . "${EXCLUDES[@]}" 2>/dev/null || true
git diff -- . "${EXCLUDES[@]}" > agent_patch.diff || true
git status --short -- . "${EXCLUDES[@]}" > agent_files.txt || true

echo "[*] Agent exited with code: $AGENT_RC"
echo "[*] Modified files summary:"
cat agent_files.txt || true

exit "$AGENT_RC"
