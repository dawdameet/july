import { useRouter } from 'expo-router';
import React, { useCallback, useEffect, useState } from 'react';
import {
  ActivityIndicator,
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';

import { TmuxTerminal } from '@/components/tmux-terminal';
import { BottomTabInset, Fonts, MaxContentWidth, Palette, Spacing } from '@/constants/theme';
import { useJuly } from '@/context/july-context';
import {
  ApiRunPayload,
  dispatchApiRun,
  execTmuxCommand,
  fetchTmuxSessions,
  sendTmuxKey,
  TmuxSession,
  unloadRemoteOllama,
  wakeRemoteOllama,
} from '@/lib/july-api';

type CommandMode = 'tmux' | 'agent' | 'command';

export default function CommandScreen() {
  const router = useRouter();
  const {
    activeNode,
    activeUrl,
    healthById,
    tasks,
    setSelectedTaskId,
    commandHistory,
    pushCommandHistory,
    refreshNow,
  } = useJuly();

  const [mode, setMode] = useState<CommandMode>('tmux');
  const [workspace, setWorkspace] = useState<string>('july');
  const [prompt, setPrompt] = useState('');
  const [ownedFiles, setOwnedFiles] = useState('');
  const [shellCmd, setShellCmd] = useState('');
  const [submitting, setSubmitting] = useState(false);
  const [waking, setWaking] = useState(false);
  const [sleeping, setSleeping] = useState(false);
  const [wakeTaskId, setWakeTaskId] = useState<string | null>(null);
  const [wokenFallback, setWokenFallback] = useState(false);
  const [errMsg, setErrMsg] = useState<string | null>(null);

  const [tmuxSession, setTmuxSession] = useState<string>('july');
  const [tmuxSessions, setTmuxSessions] = useState<TmuxSession[]>([]);
  const [tmuxAvailable, setTmuxAvailable] = useState<boolean>(true);
  const [tmuxErr, setTmuxErr] = useState<string | null>(null);
  const [showNewSession, setShowNewSession] = useState(false);
  const [newSessionName, setNewSessionName] = useState('');

  const stats = healthById[activeNode.id]?.stats ?? null;
  const workspaces = stats?.workspaces ?? ['july', 'julyos'];
  const wsOptions = [...new Set(['july', 'julyos', ...workspaces]), ''];
  const agentReady = stats ? Boolean(stats.ollama_up && (stats.ollama_loaded ?? wokenFallback)) : false;

  useEffect(() => {
    setWaking(false);
    setSleeping(false);
    setWakeTaskId(null);
    setWokenFallback(false);
    setTmuxSessions([]);
    setTmuxErr(null);
  }, [activeNode.id]);

  const pollSessions = useCallback(async () => {
    try {
      const sessResp = await fetchTmuxSessions(activeUrl, activeNode.token);
      setTmuxAvailable(sessResp.available);
      setTmuxSessions((prev) =>
        JSON.stringify(prev) === JSON.stringify(sessResp.sessions) ? prev : sessResp.sessions
      );
      setTmuxErr(null);
    } catch (e) {
      setTmuxErr(e instanceof Error ? e.message : 'Tmux unreachable');
    }
  }, [activeUrl, activeNode.token]);

  useEffect(() => {
    if (mode !== 'tmux') return;
    let cancelled = false;
    const tick = async () => {
      if (!cancelled) await pollSessions();
    };
    void tick();
    const iv = setInterval(() => {
      void tick();
    }, 2500);
    return () => {
      cancelled = true;
      clearInterval(iv);
    };
  }, [mode, pollSessions]);

  useEffect(() => {
    if (!waking) return;
    if (stats?.ollama_up && stats?.ollama_loaded) {
      setWaking(false);
      return;
    }
    if (wakeTaskId) {
      const wakeTask = tasks.find((t) => t.task_id === wakeTaskId);
      if (wakeTask && wakeTask.status === 'completed') {
        if (wakeTask.exit_code === 0) {
          setWokenFallback(true);
        } else {
          setErrMsg(`Wake task exited with ${wakeTask.exit_code}`);
        }
        setWaking(false);
        return;
      }
    }
    const iv = setInterval(() => {
      void refreshNow();
    }, 1000);
    return () => clearInterval(iv);
  }, [waking, stats?.ollama_up, stats?.ollama_loaded, wakeTaskId, tasks, refreshNow]);

  const handleKillSession = async () => {
    setTmuxErr(null);
    try {
      await sendTmuxKey(activeUrl, activeNode.token, tmuxSession, 'kill-session');
      await pollSessions();
    } catch (e) {
      setTmuxErr(e instanceof Error ? e.message : 'Kill failed');
    }
  };

  const handleCreateSession = async () => {
    const clean = newSessionName.trim().replace(/[^a-zA-Z0-9_-]/g, '');
    if (!clean) return;
    setShowNewSession(false);
    setNewSessionName('');
    setTmuxSession(clean);
    try {
      await execTmuxCommand(activeUrl, activeNode.token, clean, '');
      await pollSessions();
    } catch {
      // handled cleanly by poll/WS
    }
  };

  const handleWakeAgent = async () => {
    setWaking(true);
    setErrMsg(null);
    try {
      const tid = await wakeRemoteOllama(activeUrl, activeNode.token);
      setWakeTaskId(tid);
      await refreshNow();
    } catch (e) {
      setWaking(false);
      setErrMsg(e instanceof Error ? e.message : 'Failed to wake agent');
    }
  };

  const handleSleepAgent = async () => {
    setSleeping(true);
    setErrMsg(null);
    try {
      await unloadRemoteOllama(activeUrl, activeNode.token);
      setWokenFallback(false);
      setWakeTaskId(null);
      await refreshNow();
    } catch (e) {
      setErrMsg(e instanceof Error ? e.message : 'Failed to sleep agent');
    } finally {
      setSleeping(false);
    }
  };

  const firePayload = async (payload: ApiRunPayload) => {
    setSubmitting(true);
    setErrMsg(null);
    try {
      const res = await dispatchApiRun(activeUrl, activeNode.token, payload);
      setSelectedTaskId(res.task_id);
      await refreshNow();
      router.push('/queue');
    } catch (e) {
      setErrMsg(e instanceof Error ? e.message : 'Failed to dispatch');
    } finally {
      setSubmitting(false);
    }
  };

  const handleDispatch = () => {
    if (mode === 'agent') {
      if (!prompt.trim()) return;
      void firePayload({
        mode: 'agent',
        workspace: workspace || 'july',
        prompt: prompt.trim(),
        owned: ownedFiles.trim(),
        model: 'qwen2.5-coder:7b',
        label: `agent:${workspace || 'july'}`,
      });
    } else if (mode === 'command') {
      const clean = shellCmd.trim();
      if (!clean) return;
      pushCommandHistory(clean);
      void firePayload({
        mode: 'command',
        workspace: workspace || undefined,
        command: clean,
        label: clean.slice(0, 40),
      });
    }
  };

  const sessionPills = [...new Set([tmuxSession, ...tmuxSessions.map((s) => s.name)])];
  const activeSessionMeta = tmuxSessions.find((s) => s.name === tmuxSession);

  return (
    <SafeAreaView style={styles.safe} edges={['top']}>
      <ScrollView contentContainerStyle={styles.scroll} keyboardShouldPersistTaps="handled">
        <View style={styles.headerBlock}>
          <Text style={styles.title}>COMMAND</Text>
          <Text style={styles.subtitle}>target · {activeNode.name}</Text>
        </View>

        <View style={styles.segment}>
          <Pressable
            onPress={() => setMode('tmux')}
            style={[styles.segBtn, mode === 'tmux' && styles.segActive]}>
            <Text style={[styles.segText, mode === 'tmux' && styles.segTextActive]}>Tmux</Text>
          </Pressable>
          <Pressable
            onPress={() => setMode('agent')}
            style={[styles.segBtn, mode === 'agent' && styles.segActive]}>
            <Text style={[styles.segText, mode === 'agent' && styles.segTextActive]}>
              Agent (7B)
            </Text>
          </Pressable>
          <Pressable
            onPress={() => setMode('command')}
            style={[styles.segBtn, mode === 'command' && styles.segActive]}>
            <Text style={[styles.segText, mode === 'command' && styles.segTextActive]}>Shell</Text>
          </Pressable>
        </View>

        {mode === 'tmux' ? (
          <View style={styles.card}>
            <ScrollView
              horizontal
              showsHorizontalScrollIndicator={false}
              contentContainerStyle={styles.chipRow}>
              {sessionPills.map((name) => {
                const active = name === tmuxSession;
                return (
                  <Pressable
                    key={name}
                    onPress={() => setTmuxSession(name)}
                    style={[styles.chip, active && styles.chipActive]}>
                    <Text style={[styles.chipText, active && styles.chipTextActive]}>
                      tmux:{name}
                    </Text>
                  </Pressable>
                );
              })}
              <Pressable
                onPress={() => setShowNewSession((v) => !v)}
                style={[styles.chip, showNewSession && styles.chipActive]}>
                <Text style={[styles.chipText, showNewSession && styles.chipTextActive]}>
                  {showNewSession ? 'Cancel' : '+ Session'}
                </Text>
              </Pressable>
            </ScrollView>

            {showNewSession && (
              <View style={styles.inlineRow}>
                <TextInput
                  value={newSessionName}
                  onChangeText={setNewSessionName}
                  placeholder="session name (e.g. work)"
                  placeholderTextColor={Palette.dim}
                  autoCapitalize="none"
                  autoCorrect={false}
                  style={[styles.input, { flex: 1 }]}
                />
                <Pressable onPress={handleCreateSession} style={styles.smallSolidBtn}>
                  <Text style={styles.smallSolidBtnText}>Open</Text>
                </Pressable>
              </View>
            )}

            <View style={styles.tmuxMetaRow}>
              <Text numberOfLines={1} style={styles.tmuxCwd}>
                {activeSessionMeta?.cwd || '~'}
              </Text>
              <View style={styles.tmuxRightMeta}>
                <Text style={styles.tmuxProc}>{activeSessionMeta?.process || 'pty'}</Text>
                <Pressable onPress={handleKillSession} style={styles.microBtn}>
                  <Text style={styles.microBtnText}>KILL</Text>
                </Pressable>
              </View>
            </View>

            {!tmuxAvailable ? (
              <Text style={styles.errText}>
                tmux binary not found on {activeNode.name}. Install tmux first.
              </Text>
            ) : tmuxErr ? (
              <Text style={styles.errText}>{tmuxErr}</Text>
            ) : null}

            <TmuxTerminal
              baseUrl={activeUrl}
              token={activeNode.token}
              session={tmuxSession}
            />
          </View>
        ) : (
          <View style={styles.card}>
            {mode === 'agent' && !agentReady ? (
              <View style={styles.standbyWrap}>
                <View style={styles.standbyTop}>
                  <View style={styles.standbyStateRow}>
                    <View style={styles.squareOff} />
                    <Text style={styles.standbyTitle}>AGENT ASLEEP</Text>
                  </View>
                  <Text style={styles.standbyRam}>
                    {stats ? `${stats.ram_available_mb.toLocaleString('en-US')} MB free` : 'offline'}
                  </Text>
                </View>

                <Text style={styles.standbySub}>
                  {!stats
                    ? 'Node unreachable — check connection on Machines.'
                    : !stats.ollama_up
                      ? 'Ollama service is stopped to conserve RAM.'
                      : '7B model weights unloaded · 0 MB reserved.'}
                </Text>

                {errMsg && <Text style={styles.errText}>{errMsg}</Text>}

                <Pressable
                  disabled={!stats || waking}
                  onPress={handleWakeAgent}
                  style={[styles.dispatchBtn, (!stats || waking) && { opacity: 0.55 }]}>
                  {waking ? (
                    <View style={styles.wakingRow}>
                      <ActivityIndicator color={Palette.inverseText} size="small" />
                      <Text style={styles.dispatchBtnText}>Spinning Up 7B...</Text>
                    </View>
                  ) : (
                    <Text style={styles.dispatchBtnText}>SPIN UP AGENT</Text>
                  )}
                </Pressable>

                {wakeTaskId && (
                  <Pressable
                    onPress={() => {
                      setSelectedTaskId(wakeTaskId);
                      router.push('/queue');
                    }}
                    style={styles.wakeLinkBtn}>
                    <Text style={styles.wakeLinkText}>View wake task ({wakeTaskId}) →</Text>
                  </Pressable>
                )}
              </View>
            ) : (
              <>
                {mode === 'agent' && (
                  <View style={styles.agentStatusBar}>
                    <View style={styles.standbyStateRow}>
                      <View style={styles.squareOn} />
                      <Text style={styles.agentReadyLabel}>7B LOADED · READY</Text>
                    </View>
                    <Pressable
                      disabled={sleeping}
                      onPress={handleSleepAgent}
                      style={styles.sleepBtn}>
                      <Text style={styles.sleepBtnText}>{sleeping ? 'Sleeping...' : 'Sleep'}</Text>
                    </Pressable>
                  </View>
                )}

                <Text style={styles.sectionLabel}>WORKSPACE</Text>
                <View style={styles.chipRow}>
                  {wsOptions.map((ws) => {
                    const label = ws || '(ephemeral)';
                    const active = workspace === ws;
                    return (
                      <Pressable
                        key={label}
                        onPress={() => setWorkspace(ws)}
                        style={[styles.chip, active && styles.chipActive]}>
                        <Text style={[styles.chipText, active && styles.chipTextActive]}>
                          {label}
                        </Text>
                      </Pressable>
                    );
                  })}
                </View>

                {mode === 'agent' ? (
                  <>
                    <Text style={styles.sectionLabel}>PROMPT · qwen2.5-coder:7b</Text>
                    <TextInput
                      value={prompt}
                      onChangeText={setPrompt}
                      multiline
                      numberOfLines={4}
                      placeholder="Edit files, compile with gcc, and stage a diff..."
                      placeholderTextColor={Palette.dim}
                      style={[styles.input, styles.multiline]}
                    />

                    <Text style={styles.sectionLabel}>LEASED FILES (--owned)</Text>
                    <TextInput
                      value={ownedFiles}
                      onChangeText={setOwnedFiles}
                      placeholder="os/kernel/shell.c,dispatcher.c (optional)"
                      placeholderTextColor={Palette.dim}
                      autoCapitalize="none"
                      autoCorrect={false}
                      style={styles.input}
                    />
                  </>
                ) : (
                  <>
                    <Text style={styles.sectionLabel}>COMMAND</Text>
                    <TextInput
                      value={shellCmd}
                      onChangeText={setShellCmd}
                      placeholder="make test-all"
                      placeholderTextColor={Palette.dim}
                      autoCapitalize="none"
                      autoCorrect={false}
                      style={styles.input}
                    />
                    {commandHistory.length > 0 && (
                      <View style={styles.chipRow}>
                        {commandHistory.map((cmd) => (
                          <Pressable
                            key={cmd}
                            onPress={() => setShellCmd(cmd)}
                            style={styles.historyChip}>
                            <Text style={styles.historyText}>{cmd}</Text>
                          </Pressable>
                        ))}
                      </View>
                    )}
                  </>
                )}

                {errMsg && <Text style={styles.errText}>{errMsg}</Text>}

                <Pressable
                  disabled={submitting}
                  onPress={handleDispatch}
                  style={[styles.dispatchBtn, submitting && { opacity: 0.55 }]}>
                  {submitting ? (
                    <ActivityIndicator color={Palette.inverseText} />
                  ) : (
                    <Text style={styles.dispatchBtnText}>
                      {mode === 'agent' ? 'Spawn Agent' : 'Execute'}
                    </Text>
                  )}
                </Pressable>
              </>
            )}
          </View>
        )}
      </ScrollView>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  safe: { flex: 1, backgroundColor: Palette.bg },
  scroll: {
    padding: Spacing.three,
    paddingBottom: BottomTabInset + Spacing.four,
    gap: 14,
    maxWidth: MaxContentWidth,
    width: '100%',
    alignSelf: 'center',
  },
  headerBlock: { gap: 2, paddingVertical: 4 },
  title: {
    color: Palette.text,
    fontFamily: Fonts.bold,
    fontSize: 15,
    letterSpacing: 2.2,
  },
  subtitle: {
    color: Palette.muted,
    fontFamily: Fonts.medium,
    fontSize: 11,
  },
  segment: {
    flexDirection: 'row',
    backgroundColor: Palette.card,
    borderRadius: 0,
    borderWidth: 1,
    borderColor: Palette.border,
    padding: 3,
    gap: 4,
  },
  segBtn: { flex: 1, paddingVertical: 9, borderRadius: 0, alignItems: 'center' },
  segActive: { backgroundColor: Palette.text },
  segText: { color: Palette.muted, fontFamily: Fonts.semibold, fontSize: 12 },
  segTextActive: { color: Palette.inverseText, fontFamily: Fonts.bold },
  card: {
    backgroundColor: Palette.card,
    borderRadius: 0,
    borderWidth: 1,
    borderColor: Palette.border,
    padding: 16,
    gap: 10,
  },
  inlineRow: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  smallSolidBtn: {
    backgroundColor: Palette.text,
    borderRadius: 0,
    paddingHorizontal: 14,
    paddingVertical: 11,
    alignItems: 'center',
    justifyContent: 'center',
  },
  smallSolidBtnText: {
    color: Palette.inverseText,
    fontFamily: Fonts.bold,
    fontSize: 11,
    letterSpacing: 0.8,
  },
  tmuxMetaRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 8,
    paddingVertical: 6,
    borderTopWidth: 1,
    borderBottomWidth: 1,
    borderColor: Palette.elevated,
  },
  tmuxCwd: {
    flex: 1,
    minWidth: 0,
    color: Palette.text,
    fontFamily: Fonts.mono,
    fontSize: 11,
  },
  tmuxRightMeta: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
  },
  tmuxProc: {
    color: Palette.muted,
    fontFamily: Fonts.mono,
    fontSize: 11,
  },
  microBtn: {
    borderWidth: 1,
    borderColor: Palette.border,
    backgroundColor: Palette.bg,
    borderRadius: 0,
    paddingHorizontal: 7,
    paddingVertical: 3,
  },
  microBtnText: {
    color: Palette.muted,
    fontFamily: Fonts.bold,
    fontSize: 9,
    letterSpacing: 0.8,
  },
  standbyWrap: { gap: 12 },
  standbyTop: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 10,
  },
  standbyStateRow: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  squareOff: {
    width: 7,
    height: 7,
    borderRadius: 0,
    borderWidth: 1.5,
    borderColor: Palette.dim,
  },
  squareOn: {
    width: 7,
    height: 7,
    borderRadius: 0,
    backgroundColor: Palette.text,
  },
  standbyTitle: {
    color: Palette.text,
    fontFamily: Fonts.bold,
    fontSize: 12,
    letterSpacing: 1.5,
  },
  standbyRam: {
    color: Palette.muted,
    fontFamily: Fonts.mono,
    fontSize: 11,
    fontVariant: ['tabular-nums'],
  },
  standbySub: {
    color: Palette.muted,
    fontFamily: Fonts.sans,
    fontSize: 12,
    lineHeight: 18,
  },
  wakingRow: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  wakeLinkBtn: { alignSelf: 'flex-start', paddingTop: 2 },
  wakeLinkText: { color: Palette.muted, fontFamily: Fonts.mono, fontSize: 11 },
  agentStatusBar: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingBottom: 10,
    marginBottom: 2,
    borderBottomWidth: 1,
    borderBottomColor: Palette.elevated,
  },
  agentReadyLabel: {
    color: Palette.text,
    fontFamily: Fonts.bold,
    fontSize: 10,
    letterSpacing: 1.4,
  },
  sleepBtn: {
    borderWidth: 1,
    borderColor: Palette.border,
    backgroundColor: Palette.bg,
    borderRadius: 0,
    paddingHorizontal: 10,
    paddingVertical: 5,
  },
  sleepBtnText: {
    color: Palette.muted,
    fontFamily: Fonts.semibold,
    fontSize: 11,
  },
  sectionLabel: {
    color: Palette.muted,
    fontFamily: Fonts.bold,
    fontSize: 10,
    letterSpacing: 1.5,
    marginTop: 2,
  },
  chipRow: { flexDirection: 'row', flexWrap: 'wrap', gap: 6 },
  chip: {
    paddingHorizontal: 10,
    paddingVertical: 6,
    borderRadius: 0,
    backgroundColor: Palette.bg,
    borderWidth: 1,
    borderColor: Palette.border,
  },
  chipActive: { borderColor: Palette.text, backgroundColor: Palette.text },
  chipText: { color: Palette.muted, fontFamily: Fonts.mono, fontSize: 11 },
  chipTextActive: { color: Palette.inverseText, fontWeight: '700' },
  historyChip: {
    paddingHorizontal: 9,
    paddingVertical: 4,
    borderRadius: 0,
    backgroundColor: Palette.bg,
    borderWidth: 1,
    borderColor: Palette.border,
  },
  historyText: { color: Palette.muted, fontFamily: Fonts.mono, fontSize: 11 },
  input: {
    backgroundColor: Palette.bg,
    borderWidth: 1,
    borderColor: Palette.border,
    borderRadius: 0,
    paddingHorizontal: 12,
    paddingVertical: 10,
    color: Palette.text,
    fontFamily: Fonts.mono,
    fontSize: 12,
  },
  multiline: { minHeight: 96, textAlignVertical: 'top' },
  errText: { color: Palette.text, fontFamily: Fonts.mono, fontSize: 11 },
  dispatchBtn: {
    marginTop: 4,
    backgroundColor: Palette.text,
    borderRadius: 0,
    paddingVertical: 13,
    alignItems: 'center',
  },
  dispatchBtnText: { color: Palette.inverseText, fontFamily: Fonts.bold, fontSize: 13 },
});
