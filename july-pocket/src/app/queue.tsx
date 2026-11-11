import { useRouter } from 'expo-router';
import React, { useEffect, useMemo, useRef, useState } from 'react';
import { Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';

import { BottomTabInset, Fonts, MaxContentWidth, Palette, Spacing } from '@/constants/theme';
import { useJuly } from '@/context/july-context';
import { fetchStreamChunk, killRemoteTask, TaskItem } from '@/lib/july-api';

function statusMeta(item: TaskItem): { label: string; bright: boolean } {
  if (item.killed) return { label: 'KILLED', bright: false };
  if (item.status === 'running') return { label: 'RUNNING', bright: true };
  if (item.status === 'queued') return { label: 'QUEUED', bright: false };
  return item.exit_code === 0 ? { label: 'DONE', bright: true } : { label: 'FAILED', bright: false };
}

export default function QueueScreen() {
  const router = useRouter();
  const {
    activeNode,
    tasks,
    selectedTaskId,
    setSelectedTaskId,
    setSelectedArtifactTaskId,
    refreshNow,
  } = useJuly();

  const [streamLog, setStreamLog] = useState<string>('');
  const [streamDone, setStreamDone] = useState(false);
  const offsetRef = useRef(0);
  const logScrollRef = useRef<React.ComponentRef<typeof ScrollView> | null>(null);

  const activeTask = useMemo(() => {
    if (selectedTaskId) {
      const hit = tasks.find((t) => t.task_id === selectedTaskId);
      if (hit) return hit;
    }
    return tasks[0] ?? null;
  }, [tasks, selectedTaskId]);

  const viewingId = activeTask?.task_id ?? selectedTaskId;

  useEffect(() => {
    if (!viewingId) {
      setStreamLog('');
      setStreamDone(false);
      return;
    }
    let cancelled = false;
    let done = false;
    let inFlight = false;
    let iv: ReturnType<typeof setInterval> | null = null;
    offsetRef.current = 0;
    setStreamLog('');
    setStreamDone(false);

    const tick = async () => {
      if (cancelled || done || inFlight) return;
      inFlight = true;
      try {
        const chunk = await fetchStreamChunk(
          activeNode.url,
          activeNode.token,
          viewingId,
          offsetRef.current
        );
        if (cancelled) return;
        if (chunk.text.length > 0) {
          setStreamLog((prev) => prev + chunk.text);
        }
        offsetRef.current = chunk.nextOffset;
        if (chunk.status === 'completed') {
          done = true;
          setStreamDone(true);
          if (iv) {
            clearInterval(iv);
            iv = null;
          }
        }
      } catch {
        // Task may still be queued before log file opens
      } finally {
        inFlight = false;
      }
    };

    void tick();
    iv = setInterval(() => {
      void tick();
    }, 350);

    return () => {
      cancelled = true;
      if (iv) clearInterval(iv);
    };
  }, [viewingId, activeNode.url, activeNode.token]);

  const turnEvents = useMemo(() => {
    return streamLog
      .split('\n')
      .filter((ln) => ln.startsWith('[turn ') || ln.trimStart().startsWith('-> tool:'))
      .slice(-8);
  }, [streamLog]);

  const handleKill = async (tid: string) => {
    try {
      await killRemoteTask(activeNode.url, activeNode.token, tid);
      await refreshNow();
    } catch {
      // ignore
    }
  };

  return (
    <SafeAreaView style={styles.safe} edges={['top']}>
      <ScrollView contentContainerStyle={styles.scroll}>
        <View style={styles.headerBlock}>
          <Text style={styles.title}>QUEUE</Text>
          <Text style={styles.subtitle}>
            {tasks.length} tracked · {activeNode.name}
          </Text>
        </View>

        <View style={styles.taskList}>
          {tasks.length === 0 ? (
            <View style={styles.card}>
              <Text style={styles.mutedText}>
                No tasks on {activeNode.name}. Dispatch from Command.
              </Text>
            </View>
          ) : (
            tasks.slice(0, 12).map((t) => {
              const selected = t.task_id === viewingId;
              const st = statusMeta(t);
              return (
                <Pressable
                  key={t.task_id}
                  onPress={() => setSelectedTaskId(t.task_id)}
                  style={[styles.taskRow, selected && styles.taskRowSelected]}>
                  <View style={styles.taskTop}>
                    <View style={[styles.badge, st.bright && styles.badgeBright]}>
                      <Text style={[styles.badgeText, st.bright && styles.badgeTextBright]}>
                        {st.label}
                      </Text>
                    </View>
                    <Text numberOfLines={1} style={styles.taskLabel}>
                      {t.label}
                    </Text>
                    <Text style={styles.metaMono}>{t.elapsed_sec}s</Text>
                  </View>
                  <View style={styles.taskSubRow}>
                    <Text numberOfLines={1} style={styles.taskId}>
                      {t.task_id}
                    </Text>
                    <Text style={styles.metaMono}>
                      exit {t.exit_code} · {t.artifacts_count} art
                    </Text>
                  </View>
                </Pressable>
              );
            })
          )}
        </View>

        {viewingId && (
          <View style={styles.card}>
            <View style={styles.streamHeader}>
              <View style={styles.streamMetaWrap}>
                <Text numberOfLines={1} style={styles.streamTitle}>
                  STREAM · {viewingId}
                </Text>
                <Text style={styles.metaMono}>
                  {streamDone ? 'completed' : 'live · /stream/'}
                </Text>
              </View>

              <View style={styles.streamActions}>
                {activeTask && activeTask.status === 'running' && (
                  <Pressable onPress={() => void handleKill(viewingId)} style={styles.killBtn}>
                    <Text style={styles.killBtnText}>Kill</Text>
                  </Pressable>
                )}

                {activeTask && activeTask.artifacts_count > 0 && (
                  <Pressable
                    onPress={() => {
                      setSelectedArtifactTaskId(viewingId);
                      router.push('/artifacts');
                    }}
                    style={styles.artBtn}>
                    <Text style={styles.artBtnText}>Artifacts ({activeTask.artifacts_count})</Text>
                  </Pressable>
                )}
              </View>
            </View>

            {turnEvents.length > 0 && (
              <View style={styles.turnsBox}>
                {turnEvents.map((ev, i) => (
                  <Text key={`${i}-${ev}`} style={styles.turnLine}>
                    {ev}
                  </Text>
                ))}
              </View>
            )}

            <ScrollView
              ref={logScrollRef}
              onContentSizeChange={() => logScrollRef.current?.scrollToEnd({ animated: false })}
              style={styles.consoleBox}
              nestedScrollEnabled>
              <Text selectable style={styles.consoleText}>
                {streamLog || 'Waiting for worker output...'}
              </Text>
            </ScrollView>
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
  taskList: { gap: 8 },
  taskRow: {
    backgroundColor: Palette.card,
    borderRadius: 0,
    borderWidth: 1,
    borderColor: Palette.border,
    paddingHorizontal: 14,
    paddingVertical: 11,
    gap: 6,
  },
  taskRowSelected: {
    backgroundColor: Palette.cardHeader,
    borderColor: Palette.text,
  },
  taskTop: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  badge: {
    borderWidth: 1,
    borderColor: Palette.border,
    borderRadius: 0,
    paddingHorizontal: 6,
    paddingVertical: 2,
  },
  badgeBright: {
    borderColor: Palette.text,
    backgroundColor: Palette.text,
  },
  badgeText: {
    color: Palette.muted,
    fontFamily: Fonts.bold,
    fontSize: 9,
    letterSpacing: 0.6,
  },
  badgeTextBright: {
    color: Palette.inverseText,
  },
  taskLabel: { flex: 1, color: Palette.text, fontFamily: Fonts.semibold, fontSize: 13 },
  taskSubRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    gap: 8,
  },
  taskId: { color: Palette.dim, fontFamily: Fonts.mono, fontSize: 11, flexShrink: 1 },
  metaMono: {
    color: Palette.muted,
    fontFamily: Fonts.mono,
    fontSize: 11,
    fontVariant: ['tabular-nums'],
  },
  card: {
    backgroundColor: Palette.card,
    borderRadius: 0,
    borderWidth: 1,
    borderColor: Palette.border,
    padding: 16,
    gap: 12,
  },
  streamHeader: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 10,
  },
  streamMetaWrap: { flex: 1, minWidth: 0, gap: 2 },
  streamTitle: {
    color: Palette.text,
    fontFamily: Fonts.mono,
    fontWeight: '700',
    fontSize: 12,
  },
  streamActions: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  killBtn: {
    borderWidth: 1,
    borderColor: Palette.border,
    backgroundColor: Palette.cardHeader,
    borderRadius: 0,
    paddingHorizontal: 10,
    paddingVertical: 6,
  },
  killBtnText: { color: Palette.text, fontFamily: Fonts.mono, fontWeight: '600', fontSize: 11 },
  artBtn: {
    backgroundColor: Palette.text,
    borderRadius: 0,
    paddingHorizontal: 10,
    paddingVertical: 6,
  },
  artBtnText: { color: Palette.inverseText, fontFamily: Fonts.bold, fontSize: 11 },
  turnsBox: {
    backgroundColor: Palette.bg,
    borderRadius: 0,
    borderWidth: 1,
    borderColor: Palette.border,
    padding: 10,
    gap: 3,
  },
  turnLine: { color: Palette.amber, fontFamily: Fonts.mono, fontSize: 11 },
  consoleBox: {
    maxHeight: 320,
    backgroundColor: Palette.bg,
    borderRadius: 0,
    borderWidth: 1,
    borderColor: Palette.border,
    padding: 12,
  },
  consoleText: { color: Palette.text, fontFamily: Fonts.mono, fontSize: 11, lineHeight: 17 },
  mutedText: { color: Palette.muted, fontSize: 12 },
});
