import { Image } from 'expo-image';
import React, { useEffect, useMemo, useState } from 'react';
import { Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';

import { BottomTabInset, Fonts, MaxContentWidth, Palette, Spacing } from '@/constants/theme';
import { useJuly } from '@/context/july-context';
import {
  fetchTaskMeta,
  fetchTextArtifact,
  getArtifactUrl,
  TaskMeta,
} from '@/lib/july-api';

function DiffView({ diff }: { diff: string }) {
  return (
    <View style={styles.codeBox}>
      {diff.split('\n').map((line, idx) => {
        let color: string = Palette.muted;
        let bg = 'transparent';
        if (line.startsWith('+') && !line.startsWith('+++')) {
          color = Palette.text;
          bg = 'rgba(255,255,255,0.08)';
        } else if (line.startsWith('-') && !line.startsWith('---')) {
          color = Palette.dim;
          bg = 'rgba(255,255,255,0.02)';
        } else if (line.startsWith('@@') || line.startsWith('diff ') || line.startsWith('+++') || line.startsWith('---')) {
          color = Palette.amber;
        }
        return (
          <Text key={idx} selectable style={[styles.codeLine, { color, backgroundColor: bg }]}>
            {line}
          </Text>
        );
      })}
    </View>
  );
}

export default function ArtifactsScreen() {
  const { activeNode, tasks, selectedArtifactTaskId, setSelectedArtifactTaskId } = useJuly();
  const tasksWithArtifacts = useMemo(
    () => tasks.filter((t) => t.artifacts_count > 0),
    [tasks]
  );

  const chosenTaskId = selectedArtifactTaskId ?? tasksWithArtifacts[0]?.task_id ?? null;
  const [meta, setMeta] = useState<TaskMeta | null>(null);
  const [selectedFile, setSelectedFile] = useState<string | null>(null);
  const [textContent, setTextContent] = useState<string>('');
  const [err, setErr] = useState<string | null>(null);

  useEffect(() => {
    if (!chosenTaskId) {
      setMeta(null);
      return;
    }
    let cancelled = false;
    setErr(null);
    fetchTaskMeta(activeNode.url, activeNode.token, chosenTaskId)
      .then((m) => {
        if (cancelled) return;
        setMeta(m);
        const preferred =
          m.artifacts.find((a) => a.endsWith('.png')) ??
          m.artifacts.find((a) => a.endsWith('.diff')) ??
          m.artifacts[0] ??
          null;
        setSelectedFile(preferred);
      })
      .catch((e) => {
        if (!cancelled) setErr(e instanceof Error ? e.message : 'Failed to load meta');
      });
    return () => {
      cancelled = true;
    };
  }, [chosenTaskId, activeNode.url, activeNode.token]);

  useEffect(() => {
    if (!chosenTaskId || !selectedFile || selectedFile.endsWith('.png')) {
      setTextContent('');
      return;
    }
    let cancelled = false;
    fetchTextArtifact(activeNode.url, activeNode.token, chosenTaskId, selectedFile)
      .then((txt) => {
        if (!cancelled) setTextContent(txt);
      })
      .catch(() => {
        if (!cancelled) setTextContent('(Binary or unreadable artifact)');
      });
    return () => {
      cancelled = true;
    };
  }, [chosenTaskId, selectedFile, activeNode.url, activeNode.token]);

  return (
    <SafeAreaView style={styles.safe} edges={['top']}>
      <ScrollView contentContainerStyle={styles.scroll}>
        <View style={styles.headerBlock}>
          <Text style={styles.title}>ARTIFACTS</Text>
          <Text style={styles.subtitle}>
            {tasksWithArtifacts.length} {tasksWithArtifacts.length === 1 ? 'task' : 'tasks'} with output
          </Text>
        </View>

        {tasksWithArtifacts.length === 0 ? (
          <View style={styles.card}>
            <Text style={styles.mutedText}>
              No artifacts staged yet. Run Boot JulyOS or an Agent task in Command.
            </Text>
          </View>
        ) : (
          <>
            <ScrollView horizontal showsHorizontalScrollIndicator={false} contentContainerStyle={styles.chipRow}>
              {tasksWithArtifacts.map((t) => {
                const active = t.task_id === chosenTaskId;
                return (
                  <Pressable
                    key={t.task_id}
                    onPress={() => setSelectedArtifactTaskId(t.task_id)}
                    style={[styles.chip, active && styles.chipActive]}>
                    <Text style={[styles.chipText, active && styles.chipTextActive]}>
                      {t.label} ({t.artifacts_count})
                    </Text>
                  </Pressable>
                );
              })}
            </ScrollView>

            {err && <Text style={styles.errText}>{err}</Text>}

            {meta && (
              <View style={styles.card}>
                <View style={styles.fileTabs}>
                  {meta.artifacts.map((fn) => {
                    const active = fn === selectedFile;
                    return (
                      <Pressable
                        key={fn}
                        onPress={() => setSelectedFile(fn)}
                        style={[styles.filePill, active && styles.filePillActive]}>
                        <Text style={[styles.filePillText, active && styles.filePillTextActive]}>
                          {fn}
                        </Text>
                      </Pressable>
                    );
                  })}
                </View>

                {selectedFile?.endsWith('.png') && chosenTaskId ? (
                  <View style={styles.imgWrap}>
                    <Image
                      source={{
                        uri: getArtifactUrl(activeNode.url, chosenTaskId, selectedFile),
                        headers: activeNode.token ? { 'X-July-Token': activeNode.token } : undefined,
                      }}
                      style={styles.osImage}
                      contentFit="contain"
                    />
                  </View>
                ) : selectedFile?.endsWith('.diff') ? (
                  <DiffView diff={textContent || '(Empty diff)'} />
                ) : (
                  <View style={styles.codeBox}>
                    <Text selectable style={styles.codeLine}>
                      {textContent || 'Loading artifact...'}
                    </Text>
                  </View>
                )}
              </View>
            )}
          </>
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
  chipRow: { gap: 6 },
  chip: {
    paddingHorizontal: 11,
    paddingVertical: 6,
    borderRadius: 0,
    backgroundColor: Palette.card,
    borderWidth: 1,
    borderColor: Palette.border,
  },
  chipActive: { borderColor: Palette.text, backgroundColor: Palette.text },
  chipText: { color: Palette.muted, fontFamily: Fonts.mono, fontSize: 11 },
  chipTextActive: { color: Palette.inverseText, fontWeight: '700' },
  card: {
    backgroundColor: Palette.card,
    borderRadius: 0,
    borderWidth: 1,
    borderColor: Palette.border,
    padding: 16,
    gap: 12,
  },
  fileTabs: { flexDirection: 'row', flexWrap: 'wrap', gap: 6 },
  filePill: {
    paddingHorizontal: 10,
    paddingVertical: 5,
    borderRadius: 0,
    backgroundColor: Palette.bg,
    borderWidth: 1,
    borderColor: Palette.border,
  },
  filePillActive: { borderColor: Palette.text, backgroundColor: Palette.cardHeader },
  filePillText: { color: Palette.muted, fontFamily: Fonts.mono, fontSize: 11 },
  filePillTextActive: { color: Palette.text, fontWeight: '700' },
  imgWrap: {
    backgroundColor: Palette.bg,
    borderRadius: 0,
    borderWidth: 1,
    borderColor: Palette.border,
    overflow: 'hidden',
  },
  osImage: { width: '100%', aspectRatio: 4 / 3 },
  codeBox: {
    backgroundColor: Palette.bg,
    borderRadius: 0,
    borderWidth: 1,
    borderColor: Palette.border,
    padding: 12,
  },
  codeLine: { color: Palette.text, fontFamily: Fonts.mono, fontSize: 11, lineHeight: 17 },
  errText: { color: Palette.text, fontFamily: Fonts.mono, fontSize: 11 },
  mutedText: { color: Palette.muted, fontFamily: Fonts.sans, fontSize: 12, lineHeight: 18 },
});
