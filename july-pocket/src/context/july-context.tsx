import * as SecureStore from 'expo-secure-store';
import React, { createContext, useCallback, useContext, useEffect, useRef, useState } from 'react';
import { Platform } from 'react-native';

import {
  buildTailscaleUrl,
  fetchNodeStats,
  fetchNodeTasks,
  HttpStatusError,
  NodeStats,
  normalizeBaseUrl,
  TaskItem,
} from '@/lib/july-api';

const STORAGE_KEY = 'july_pocket_state_v1';

export interface NodeConfig {
  id: string;
  name: string;
  url: string;
  tailscaleUrl?: string;
  token: string;
}

export interface NodeHealth {
  online: boolean;
  authRequired: boolean;
  resolvedUrl: string;
  rttMs: number | null;
  error: string | null;
  stats: NodeStats | null;
  lastUpdated: number;
}

export interface TelemetrySample {
  cpuPct: number;
  ramPct: number;
  rttMs: number;
}

interface JulyContextValue {
  nodes: NodeConfig[];
  activeNodeId: string;
  activeNode: NodeConfig;
  activeUrl: string;
  setActiveNodeId: (id: string) => void;
  addOrUpdateNode: (node: NodeConfig) => void;
  unlockNode: (id: string, token: string) => void;
  removeNode: (id: string) => void;
  healthById: Record<string, NodeHealth>;
  historyById: Record<string, TelemetrySample[]>;
  tasks: TaskItem[];
  selectedTaskId: string | null;
  setSelectedTaskId: (id: string | null) => void;
  selectedArtifactTaskId: string | null;
  setSelectedArtifactTaskId: (id: string | null) => void;
  commandHistory: string[];
  pushCommandHistory: (cmd: string) => void;
  refreshNow: () => Promise<void>;
}

const DEFAULT_NODES: NodeConfig[] = [
  {
    id: 'fedora-i3',
    name: 'fedora-i3',
    url: 'http://192.168.1.41:8080',
    token: '',
  },
];

async function loadPersistedState(): Promise<{
  nodes?: NodeConfig[];
  activeNodeId?: string;
  commandHistory?: string[];
} | null> {
  try {
    let raw: string | null = null;
    if (Platform.OS === 'web') {
      raw = typeof localStorage !== 'undefined' ? localStorage.getItem(STORAGE_KEY) : null;
    } else {
      raw = await SecureStore.getItemAsync(STORAGE_KEY);
    }
    if (!raw) return null;
    return JSON.parse(raw) as {
      nodes?: NodeConfig[];
      activeNodeId?: string;
      commandHistory?: string[];
    };
  } catch {
    return null;
  }
}

async function savePersistedState(payload: {
  nodes: NodeConfig[];
  activeNodeId: string;
  commandHistory: string[];
}) {
  try {
    const raw = JSON.stringify(payload);
    if (Platform.OS === 'web') {
      if (typeof localStorage !== 'undefined') localStorage.setItem(STORAGE_KEY, raw);
    } else {
      await SecureStore.setItemAsync(STORAGE_KEY, raw);
    }
  } catch {
    // ignore keychain write errors in restricted simulators
  }
}

const JulyContext = createContext<JulyContextValue | null>(null);

export function JulyProvider({ children }: { children: React.ReactNode }) {
  const [nodes, setNodes] = useState<NodeConfig[]>(DEFAULT_NODES);
  const [activeNodeId, setActiveNodeId] = useState<string>(DEFAULT_NODES[0].id);
  const [healthById, setHealthById] = useState<Record<string, NodeHealth>>({});
  const [historyById, setHistoryById] = useState<Record<string, TelemetrySample[]>>({});
  const [tasks, setTasks] = useState<TaskItem[]>([]);
  const [selectedTaskId, setSelectedTaskId] = useState<string | null>(null);
  const [selectedArtifactTaskId, setSelectedArtifactTaskId] = useState<string | null>(null);
  const [commandHistory, setCommandHistory] = useState<string[]>([
    'free -h',
    'make test-sync',
    'git status -s',
  ]);
  const [hydrated, setHydrated] = useState(false);

  const activeNode = nodes.find((n) => n.id === activeNodeId) ?? nodes[0];
  const activeHealth = healthById[activeNode.id];
  const activeUrl = activeHealth?.resolvedUrl ?? activeNode.url;
  const activeUrlRef = useRef(activeUrl);
  activeUrlRef.current = activeUrl;

  useEffect(() => {
    void loadPersistedState().then((saved) => {
      if (saved?.nodes && saved.nodes.length > 0) {
        setNodes(saved.nodes);
        if (saved.activeNodeId && saved.nodes.some((n) => n.id === saved.activeNodeId)) {
          setActiveNodeId(saved.activeNodeId);
        } else {
          setActiveNodeId(saved.nodes[0].id);
        }
      }
      if (saved?.commandHistory && saved.commandHistory.length > 0) {
        setCommandHistory(saved.commandHistory);
      }
      setHydrated(true);
    });
  }, []);

  useEffect(() => {
    if (!hydrated) return;
    void savePersistedState({ nodes, activeNodeId, commandHistory });
  }, [hydrated, nodes, activeNodeId, commandHistory]);

  const probeNode = useCallback(
    async (
      node: NodeConfig
    ): Promise<{ id: string; health: NodeHealth; discoveredTailscale?: string }> => {
      const candidates = [node.url];
      if (node.tailscaleUrl && normalizeBaseUrl(node.tailscaleUrl) !== normalizeBaseUrl(node.url)) {
        candidates.push(normalizeBaseUrl(node.tailscaleUrl));
      }

      let lastErr: unknown = null;
      let lastCandidate = node.url;

      for (const candidate of candidates) {
        lastCandidate = candidate;
        try {
          const { stats, rttMs } = await fetchNodeStats(candidate, node.token);
          const discoveredTailscale = stats.tailscale_ip
            ? buildTailscaleUrl(node.url, stats.tailscale_ip)
            : undefined;
          return {
            id: node.id,
            discoveredTailscale,
            health: {
              online: true,
              authRequired: false,
              resolvedUrl: candidate,
              rttMs,
              error: null,
              stats,
              lastUpdated: Date.now(),
            },
          };
        } catch (err) {
          lastErr = err;
          if (err instanceof HttpStatusError && err.status === 401) {
            return {
              id: node.id,
              health: {
                online: false,
                authRequired: true,
                resolvedUrl: candidate,
                rttMs: null,
                error: err.message,
                stats: null,
                lastUpdated: Date.now(),
              },
            };
          }
        }
      }

      return {
        id: node.id,
        health: {
          online: false,
          authRequired: false,
          resolvedUrl: lastCandidate,
          rttMs: null,
          error: lastErr instanceof Error ? lastErr.message : 'Unreachable',
          stats: null,
          lastUpdated: Date.now(),
        },
      };
    },
    []
  );

  const pollAll = useCallback(async () => {
    const results = await Promise.all(nodes.map((node) => probeNode(node)));

    setHealthById((prev) => {
      const next = { ...prev };
      for (const r of results) {
        next[r.id] = r.health;
      }
      return next;
    });

    setHistoryById((prev) => {
      let changed = false;
      const next = { ...prev };
      for (const r of results) {
        if (!r.health.online || !r.health.stats) continue;
        changed = true;
        const st = r.health.stats;
        const cpuPct =
          typeof st.cpu_pct === 'number'
            ? st.cpu_pct
            : Math.min(100, (st.load_1m / Math.max(1, st.cores)) * 100);
        const ramPct = st.ram_total_mb > 0 ? (st.ram_used_mb / st.ram_total_mb) * 100 : 0;
        const rttMs = r.health.rttMs ?? 0;
        const arr = prev[r.id] ?? [];
        next[r.id] = [...arr.slice(-23), { cpuPct, ramPct, rttMs }];
      }
      return changed ? next : prev;
    });

    setNodes((prev) => {
      let changed = false;
      const next = prev.map((n) => {
        const hit = results.find((r) => r.id === n.id);
        if (
          hit?.discoveredTailscale &&
          hit.discoveredTailscale !== n.tailscaleUrl &&
          normalizeBaseUrl(hit.discoveredTailscale) !== normalizeBaseUrl(n.url)
        ) {
          changed = true;
          return { ...n, tailscaleUrl: hit.discoveredTailscale };
        }
        return n;
      });
      return changed ? next : prev;
    });

    const activeRes = results.find((r) => r.id === activeNode.id);
    const targetUrl = activeRes?.health.resolvedUrl ?? activeUrlRef.current;

    try {
      const list = await fetchNodeTasks(targetUrl, activeNode.token);
      setTasks((prev) => (JSON.stringify(prev) === JSON.stringify(list) ? prev : list));
    } catch {
      // Keep last known tasks when momentarily offline or locked
    }
  }, [nodes, activeNode.id, activeNode.token, probeNode]);

  useEffect(() => {
    if (!hydrated) return;
    void pollAll();
    const timer = setInterval(() => {
      void pollAll();
    }, 2000);
    return () => clearInterval(timer);
  }, [hydrated, pollAll]);

  const addOrUpdateNode = useCallback((node: NodeConfig) => {
    const normalized: NodeConfig = {
      ...node,
      url: normalizeBaseUrl(node.url),
      tailscaleUrl: node.tailscaleUrl?.trim() ? normalizeBaseUrl(node.tailscaleUrl) : undefined,
    };
    setNodes((prev) => {
      const exists = prev.some((n) => n.id === normalized.id);
      return exists
        ? prev.map((n) => (n.id === normalized.id ? normalized : n))
        : [...prev, normalized];
    });
    setActiveNodeId(normalized.id);
  }, []);

  const unlockNode = useCallback((id: string, token: string) => {
    setNodes((prev) => prev.map((n) => (n.id === id ? { ...n, token: token.trim() } : n)));
  }, []);

  const removeNode = useCallback((id: string) => {
    setNodes((prev) => {
      if (prev.length <= 1) return prev;
      const filtered = prev.filter((n) => n.id !== id);
      setActiveNodeId((cur) => (cur === id ? filtered[0].id : cur));
      return filtered;
    });
  }, []);

  const pushCommandHistory = useCallback((cmd: string) => {
    const clean = cmd.trim();
    if (!clean) return;
    setCommandHistory((prev) => [clean, ...prev.filter((c) => c !== clean)].slice(0, 8));
  }, []);

  return (
    <JulyContext.Provider
      value={{
        nodes,
        activeNodeId,
        activeNode,
        activeUrl,
        setActiveNodeId,
        addOrUpdateNode,
        unlockNode,
        removeNode,
        healthById,
        historyById,
        tasks,
        selectedTaskId,
        setSelectedTaskId,
        selectedArtifactTaskId,
        setSelectedArtifactTaskId,
        commandHistory,
        pushCommandHistory,
        refreshNow: pollAll,
      }}>
      {children}
    </JulyContext.Provider>
  );
}

export function useJuly(): JulyContextValue {
  const ctx = useContext(JulyContext);
  if (!ctx) throw new Error('useJuly must be used inside JulyProvider');
  return ctx;
}
