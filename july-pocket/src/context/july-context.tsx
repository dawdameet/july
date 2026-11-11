import React, { createContext, useCallback, useContext, useEffect, useState } from 'react';

import {
  fetchNodeStats,
  fetchNodeTasks,
  NodeStats,
  normalizeBaseUrl,
  TaskItem,
} from '@/lib/july-api';

export interface NodeConfig {
  id: string;
  name: string;
  url: string;
  token: string;
}

export interface NodeHealth {
  online: boolean;
  rttMs: number | null;
  error: string | null;
  stats: NodeStats | null;
  lastUpdated: number;
}

interface JulyContextValue {
  nodes: NodeConfig[];
  activeNodeId: string;
  activeNode: NodeConfig;
  setActiveNodeId: (id: string) => void;
  addOrUpdateNode: (node: NodeConfig) => void;
  removeNode: (id: string) => void;
  healthById: Record<string, NodeHealth>;
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

const JulyContext = createContext<JulyContextValue | null>(null);

export function JulyProvider({ children }: { children: React.ReactNode }) {
  const [nodes, setNodes] = useState<NodeConfig[]>(DEFAULT_NODES);
  const [activeNodeId, setActiveNodeId] = useState<string>(DEFAULT_NODES[0].id);
  const [healthById, setHealthById] = useState<Record<string, NodeHealth>>({});
  const [tasks, setTasks] = useState<TaskItem[]>([]);
  const [selectedTaskId, setSelectedTaskId] = useState<string | null>(null);
  const [selectedArtifactTaskId, setSelectedArtifactTaskId] = useState<string | null>(null);
  const [commandHistory, setCommandHistory] = useState<string[]>([
    'free -h',
    'make test-sync',
    'git status -s',
  ]);

  const activeNode = nodes.find((n) => n.id === activeNodeId) ?? nodes[0];

  const pollAll = useCallback(async () => {
    const results = await Promise.all(
      nodes.map(async (node) => {
        try {
          const { stats, rttMs } = await fetchNodeStats(node.url, node.token);
          return [
            node.id,
            {
              online: true,
              rttMs,
              error: null,
              stats,
              lastUpdated: Date.now(),
            } satisfies NodeHealth,
          ] as const;
        } catch (err) {
          return [
            node.id,
            {
              online: false,
              rttMs: null,
              error: err instanceof Error ? err.message : 'Unreachable',
              stats: null,
              lastUpdated: Date.now(),
            } satisfies NodeHealth,
          ] as const;
        }
      })
    );

    setHealthById((prev) => {
      const next = { ...prev };
      for (const [id, health] of results) {
        next[id] = health;
      }
      return next;
    });

    try {
      const list = await fetchNodeTasks(activeNode.url, activeNode.token);
      setTasks((prev) => (JSON.stringify(prev) === JSON.stringify(list) ? prev : list));
    } catch {
      // Keep last known tasks when momentarily offline
    }
  }, [nodes, activeNode.url, activeNode.token]);

  useEffect(() => {
    void pollAll();
    const timer = setInterval(() => {
      void pollAll();
    }, 2000);
    return () => clearInterval(timer);
  }, [pollAll]);

  const addOrUpdateNode = useCallback((node: NodeConfig) => {
    const normalized: NodeConfig = {
      ...node,
      url: normalizeBaseUrl(node.url),
    };
    setNodes((prev) => {
      const exists = prev.some((n) => n.id === normalized.id);
      return exists
        ? prev.map((n) => (n.id === normalized.id ? normalized : n))
        : [...prev, normalized];
    });
    setActiveNodeId(normalized.id);
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
        setActiveNodeId,
        addOrUpdateNode,
        removeNode,
        healthById,
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

