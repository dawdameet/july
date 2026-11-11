export interface NodeStats {
  hostname: string;
  os: string;
  arch: string;
  cores: number;
  load_1m: number;
  load_5m: number;
  cpu_pct?: number;
  cpu_temp_c?: number;
  ram_total_mb: number;
  ram_used_mb: number;
  ram_available_mb: number;
  swap_total_mb?: number;
  swap_used_mb?: number;
  tmpfs_total_mb: number;
  tmpfs_used_mb: number;
  disk_total_mb?: number;
  disk_used_mb?: number;
  queue_depth: number;
  max_queue: number;
  active_tasks: number;
  uptime_sec: number;
  ollama_up: boolean;
  ollama_loaded?: boolean;
  tailscale_ip?: string;
  auth_required?: boolean;
  workspaces: string[];
}

export interface TaskItem {
  task_id: string;
  label: string;
  type: 'workflow' | 'command';
  status: 'queued' | 'running' | 'completed';
  exit_code: number;
  artifacts_count: number;
  created_at: number;
  elapsed_sec: number;
  log_bytes: number;
  killed: boolean;
}

export interface TaskMeta {
  taskId: string;
  name: string;
  status: string;
  exit_code: number;
  artifacts_count: number;
  artifacts: string[];
}

export interface ApiRunPayload {
  mode: 'command' | 'agent' | 'julyos_boot';
  workspace?: string;
  command?: string;
  prompt?: string;
  owned?: string;
  model?: string;
  label?: string;
}

export interface StreamChunk {
  status: 'running' | 'completed';
  nextOffset: number;
  exitCode: number;
  artifactsCount: number;
  text: string;
}

export interface TmuxSession {
  name: string;
  windows: number;
  attached: number;
  cwd: string;
  process: string;
}

export interface TmuxSessionsResp {
  available: boolean;
  sessions: TmuxSession[];
}

export interface TmuxPaneResp {
  exists: boolean;
  session: string;
  cwd: string;
  process: string;
  content: string;
}

function makeHeaders(token?: string, jsonBody?: boolean): Record<string, string> {
  const h: Record<string, string> = {};
  if (jsonBody) h['Content-Type'] = 'application/json';
  if (token && token.trim().length > 0) {
    h['X-July-Token'] = token.trim();
  }
  return h;
}

export class HttpStatusError extends Error {
  status: number;
  constructor(status: number, message: string) {
    super(message);
    this.status = status;
  }
}

export function normalizeBaseUrl(raw: string): string {
  const trimmed = raw.trim().replace(/\/+$/, '');
  if (!trimmed) return 'http://192.168.1.41:8080';
  if (trimmed.startsWith('http://') || trimmed.startsWith('https://')) return trimmed;
  return `http://${trimmed}`;
}

export function buildTailscaleUrl(primaryUrl: string, tailscaleIp: string): string {
  try {
    const u = new URL(normalizeBaseUrl(primaryUrl));
    const portPart = u.port ? `:${u.port}` : '';
    return `${u.protocol}//${tailscaleIp}${portPart}`;
  } catch {
    return `http://${tailscaleIp}:8080`;
  }
}

async function fetchWithTimeout(url: string, init: RequestInit = {}, timeoutMs = 4000): Promise<Response> {
  const ctrl = new AbortController();
  const id = setTimeout(() => ctrl.abort(), timeoutMs);
  try {
    return await fetch(url, { ...init, signal: ctrl.signal });
  } finally {
    clearTimeout(id);
  }
}

export async function fetchNodeStats(
  baseUrl: string,
  token?: string,
  timeoutMs = 2800
): Promise<{ stats: NodeStats; rttMs: number }> {
  const t0 = Date.now();
  const res = await fetchWithTimeout(
    `${normalizeBaseUrl(baseUrl)}/stats`,
    { headers: makeHeaders(token) },
    timeoutMs
  );
  if (res.status === 401) {
    throw new HttpStatusError(401, 'Unauthorized: Invalid Token');
  }
  if (res.status === 404) {
    throw new HttpStatusError(404, 'HTTP 404 (/stats missing — restart server on node)');
  }
  if (!res.ok) {
    throw new HttpStatusError(res.status, `HTTP ${res.status}`);
  }
  const stats = (await res.json()) as NodeStats;
  return { stats, rttMs: Math.max(1, Date.now() - t0) };
}

export async function fetchNodeTasks(baseUrl: string, token?: string): Promise<TaskItem[]> {
  const res = await fetchWithTimeout(`${normalizeBaseUrl(baseUrl)}/tasks`, {
    headers: makeHeaders(token),
  });
  if (res.status === 404) {
    throw new Error('HTTP 404 (/stats missing — restart Fedora with new server binary)');
  }
  if (!res.ok) {
    throw new Error(`HTTP ${res.status}`);
  }
  const data = (await res.json()) as { tasks?: TaskItem[] };
  return data.tasks ?? [];
}

export async function dispatchApiRun(
  baseUrl: string,
  token: string | undefined,
  payload: ApiRunPayload
): Promise<{ task_id: string; status: string }> {
  const res = await fetchWithTimeout(
    `${normalizeBaseUrl(baseUrl)}/api/run`,
    {
      method: 'POST',
      headers: makeHeaders(token, true),
      body: JSON.stringify(payload),
    },
    6000
  );
  if (!res.ok) {
    const errText = await res.text();
    throw new Error(errText || `HTTP ${res.status}`);
  }
  return (await res.json()) as { task_id: string; status: string };
}

export async function killRemoteTask(
  baseUrl: string,
  token: string | undefined,
  taskId: string
): Promise<{ task_id: string; killed: boolean }> {
  const res = await fetchWithTimeout(`${normalizeBaseUrl(baseUrl)}/tasks/${encodeURIComponent(taskId)}/kill`, {
    method: 'POST',
    headers: makeHeaders(token),
  });
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return (await res.json()) as { task_id: string; killed: boolean };
}

export async function wakeRemoteOllama(baseUrl: string, token?: string): Promise<string> {
  const res = await fetchWithTimeout(`${normalizeBaseUrl(baseUrl)}/api/ollama/wake`, {
    method: 'POST',
    headers: makeHeaders(token),
  });
  if (res.status === 404) {
    const fallback = await dispatchApiRun(baseUrl, token, {
      mode: 'command',
      label: 'ollama:wake',
      command:
        '(curl -sf http://127.0.0.1:11434/api/tags >/dev/null || ' +
        '(nohup ollama serve >/tmp/july_ollama.log 2>&1 </dev/null & ' +
        'for i in $(seq 1 30); do curl -sf http://127.0.0.1:11434/api/tags >/dev/null && break; sleep 0.2; done)) ' +
        '&& curl -sf http://127.0.0.1:11434/api/generate -d \'{"model":"qwen2.5-coder:7b","prompt":"","keep_alive":"10m"}\'',
    });
    return fallback.task_id;
  }
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  const data = (await res.json()) as { task_id: string };
  return data.task_id;
}

export async function unloadRemoteOllama(baseUrl: string, token?: string): Promise<string> {
  const res = await fetchWithTimeout(`${normalizeBaseUrl(baseUrl)}/api/ollama/unload`, {
    method: 'POST',
    headers: makeHeaders(token),
  });
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  const data = (await res.json()) as { task_id: string };
  return data.task_id;
}

export async function fetchStreamChunk(
  baseUrl: string,
  token: string | undefined,
  taskId: string,
  offset: number
): Promise<StreamChunk> {
  const res = await fetchWithTimeout(
    `${normalizeBaseUrl(baseUrl)}/stream/${encodeURIComponent(taskId)}?offset=${offset}`,
    { headers: makeHeaders(token) },
    4000
  );
  if (res.status === 404) {
    throw new Error('Task log not found yet');
  }
  const stHdr = (res.headers.get('X-Task-Status') ?? '').toLowerCase();
  const offHdr = res.headers.get('X-Log-Offset');
  const exitHdr = res.headers.get('X-Task-Exit-Code');
  const artHdr = res.headers.get('X-Artifacts-Count');
  const text = res.status === 202 ? '' : await res.text();

  return {
    status: stHdr === 'completed' ? 'completed' : 'running',
    nextOffset: offHdr ? parseInt(offHdr, 10) : offset + text.length,
    exitCode: exitHdr ? parseInt(exitHdr, 10) : 0,
    artifactsCount: artHdr ? parseInt(artHdr, 10) : 0,
    text,
  };
}

export async function fetchTaskMeta(
  baseUrl: string,
  token: string | undefined,
  taskId: string
): Promise<TaskMeta> {
  const res = await fetchWithTimeout(
    `${normalizeBaseUrl(baseUrl)}/retrieve/${encodeURIComponent(taskId)}/meta`,
    { headers: makeHeaders(token) }
  );
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return (await res.json()) as TaskMeta;
}

export async function fetchTextArtifact(
  baseUrl: string,
  token: string | undefined,
  taskId: string,
  filename: string
): Promise<string> {
  const url = getArtifactUrl(baseUrl, taskId, filename);
  const res = await fetchWithTimeout(url, { headers: makeHeaders(token) }, 5000);
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return await res.text();
}

export function getArtifactUrl(baseUrl: string, taskId: string, filename: string): string {
  return `${normalizeBaseUrl(baseUrl)}/retrieve/${encodeURIComponent(taskId)}/artifact/${filename}`;
}

export async function fetchTmuxSessions(
  baseUrl: string,
  token?: string
): Promise<TmuxSessionsResp> {
  const res = await fetchWithTimeout(`${normalizeBaseUrl(baseUrl)}/tmux/sessions`, {
    headers: makeHeaders(token),
  });
  if (res.status === 404) {
    throw new Error('HTTP 404 (/tmux missing — rebuild & restart server on node)');
  }
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return (await res.json()) as TmuxSessionsResp;
}

export async function fetchTmuxPane(
  baseUrl: string,
  token: string | undefined,
  session: string,
  autostart = true,
  lines = 150
): Promise<TmuxPaneResp> {
  const url =
    `${normalizeBaseUrl(baseUrl)}/tmux/pane` +
    `?session=${encodeURIComponent(session)}&autostart=${autostart ? '1' : '0'}&lines=${lines}`;
  const res = await fetchWithTimeout(url, { headers: makeHeaders(token) });
  if (res.status === 404) {
    throw new Error('HTTP 404 (/tmux missing — rebuild & restart server on node)');
  }
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return (await res.json()) as TmuxPaneResp;
}

export async function execTmuxCommand(
  baseUrl: string,
  token: string | undefined,
  session: string,
  command: string,
  cwd?: string
): Promise<{ ok: boolean; session: string; cwd: string; process: string }> {
  const res = await fetchWithTimeout(
    `${normalizeBaseUrl(baseUrl)}/tmux/exec`,
    {
      method: 'POST',
      headers: makeHeaders(token, true),
      body: JSON.stringify({ session, command, cwd: cwd ?? '' }),
    },
    5000
  );
  if (res.status === 404) {
    throw new Error('HTTP 404 (/tmux missing — rebuild & restart server on node)');
  }
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return (await res.json()) as { ok: boolean; session: string; cwd: string; process: string };
}

export async function sendTmuxKey(
  baseUrl: string,
  token: string | undefined,
  session: string,
  key: 'C-c' | 'C-d' | 'C-z' | 'C-l' | 'Enter' | 'Escape' | 'Tab' | 'Up' | 'Down' | 'BSpace' | 'kill-session'
): Promise<{ ok: boolean; session: string; key: string }> {
  const res = await fetchWithTimeout(
    `${normalizeBaseUrl(baseUrl)}/tmux/key`,
    {
      method: 'POST',
      headers: makeHeaders(token, true),
      body: JSON.stringify({ session, key }),
    },
    4000
  );
  if (res.status === 404) {
    throw new Error('HTTP 404 (/tmux missing — rebuild & restart server on node)');
  }
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return (await res.json()) as { ok: boolean; session: string; key: string };
}

