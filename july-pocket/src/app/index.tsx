import React, { useState } from 'react';
import {
  Pressable,
  RefreshControl,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';

import { BottomTabInset, Fonts, MaxContentWidth, Palette, Spacing } from '@/constants/theme';
import { useJuly } from '@/context/july-context';
import { unloadRemoteOllama, wakeRemoteOllama } from '@/lib/july-api';

function formatMb(mb: number): string {
  return mb.toLocaleString('en-US');
}

function formatUptime(totalSec: number): string {
  const s = Math.max(0, Math.floor(totalSec));
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  if (d > 0) return `${d}d ${h}h`;
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m ${s % 60}s`;
  return `${s}s`;
}

function ProgressMeter({
  label,
  usedText,
  freeText,
  pct,
  fillColor = Palette.text,
}: {
  label: string;
  usedText: string;
  freeText: string;
  pct: number;
  fillColor?: string;
}) {
  const clamped = Math.max(0, Math.min(100, Math.round(pct)));
  const barWidth = clamped === 0 ? 0 : Math.max(2, clamped);

  return (
    <View style={styles.meterWrap}>
      <View style={styles.meterRow}>
        <Text style={styles.meterLabel}>{label}</Text>
        <Text style={styles.meterValue}>{usedText}</Text>
      </View>
      <View style={styles.track}>
        <View style={[styles.fill, { width: `${barWidth}%`, backgroundColor: fillColor }]} />
      </View>
      <View style={styles.meterSubRow}>
        <Text style={styles.meterSubLeft}>{clamped}% used</Text>
        <Text style={styles.meterSubRight}>{freeText}</Text>
      </View>
    </View>
  );
}

function InfoRow({
  label,
  value,
  highlight = false,
  last = false,
}: {
  label: string;
  value: string;
  highlight?: boolean;
  last?: boolean;
}) {
  return (
    <View style={[styles.infoRow, !last && styles.infoRowBorder]}>
      <Text style={styles.infoLabel}>{label}</Text>
      <Text style={[styles.infoValue, highlight && styles.infoValueBright]}>{value}</Text>
    </View>
  );
}

export default function MachinesScreen() {
  const {
    nodes,
    activeNodeId,
    activeNode,
    activeUrl,
    setActiveNodeId,
    addOrUpdateNode,
    unlockNode,
    removeNode,
    healthById,
    refreshNow,
  } = useJuly();

  const [showAdd, setShowAdd] = useState(false);
  const [nameInput, setNameInput] = useState('');
  const [urlInput, setUrlInput] = useState('http://192.168.1.');
  const [tailscaleInput, setTailscaleInput] = useState('');
  const [tokenInput, setTokenInput] = useState('');
  const [unlockInput, setUnlockInput] = useState('');
  const [refreshing, setRefreshing] = useState(false);
  const [actionMsg, setActionMsg] = useState<string | null>(null);

  const activeHealth = healthById[activeNode.id];
  const stats = activeHealth?.stats ?? null;

  const onRefresh = async () => {
    setRefreshing(true);
    await refreshNow();
    setRefreshing(false);
  };

  const handleSaveNode = () => {
    const cleanName = nameInput.trim() || `node-${nodes.length + 1}`;
    const cleanId = cleanName.toLowerCase().replace(/[^a-z0-9-_]/g, '-');
    addOrUpdateNode({
      id: cleanId,
      name: cleanName,
      url: urlInput.trim(),
      tailscaleUrl: tailscaleInput.trim() || undefined,
      token: tokenInput.trim(),
    });
    setNameInput('');
    setUrlInput('http://192.168.1.');
    setTailscaleInput('');
    setTokenInput('');
    setShowAdd(false);
  };

  const handleEditSelected = () => {
    setNameInput(activeNode.name);
    setUrlInput(activeNode.url);
    setTailscaleInput(activeNode.tailscaleUrl ?? '');
    setTokenInput(activeNode.token);
    setShowAdd(true);
  };

  const handleUnlock = async () => {
    if (!unlockInput.trim()) return;
    unlockNode(activeNode.id, unlockInput);
    setUnlockInput('');
    await refreshNow();
  };

  const handleToggleOllama = async () => {
    try {
      const awake = Boolean(stats?.ollama_up && stats?.ollama_loaded);
      if (awake) {
        const tid = await unloadRemoteOllama(activeUrl, activeNode.token);
        setActionMsg(`Sleep requested (${tid})`);
      } else {
        const tid = await wakeRemoteOllama(activeUrl, activeNode.token);
        setActionMsg(`Spin-up requested (${tid})`);
      }
      await refreshNow();
    } catch (e) {
      setActionMsg(e instanceof Error ? e.message : 'Ollama action failed');
    }
  };

  const ramPct =
    stats && stats.ram_total_mb > 0 ? (stats.ram_used_mb / stats.ram_total_mb) * 100 : 0;
  const tmpfsPct =
    stats && stats.tmpfs_total_mb > 0 ? (stats.tmpfs_used_mb / stats.tmpfs_total_mb) * 100 : 0;
  const tmpfsFreeMb = stats ? Math.max(0, stats.tmpfs_total_mb - stats.tmpfs_used_mb) : 0;

  return (
    <SafeAreaView style={styles.safe} edges={['top']}>
      <ScrollView
        contentContainerStyle={styles.scroll}
        refreshControl={
          <RefreshControl refreshing={refreshing} onRefresh={onRefresh} tintColor={Palette.text} />
        }>
        <View style={styles.headerBlock}>
          <Text style={styles.title}>MACHINES</Text>
          <Text style={styles.subtitle}>
            {nodes.length} {nodes.length === 1 ? 'node' : 'nodes'} · 2s poll
          </Text>
        </View>

        <View style={styles.nodeStrip}>
          {nodes.map((node) => {
            const h = healthById[node.id];
            const selected = node.id === activeNodeId;
            return (
              <Pressable
                key={node.id}
                onPress={() => setActiveNodeId(node.id)}
                style={[styles.nodeChip, selected && styles.nodeChipSelected]}>
                <View style={styles.nodeChipTop}>
                  <View style={styles.nodeNameWrap}>
                    <View style={[styles.dot, h?.online ? styles.dotOn : styles.dotOff]} />
                    <Text numberOfLines={1} style={styles.nodeName}>
                      {node.name}
                    </Text>
                  </View>
                  {h?.online && h.rttMs !== null ? (
                    <Text style={styles.rttText}>{h.rttMs} ms</Text>
                  ) : (
                    <Text style={styles.rttOffline}>
                      {h?.authRequired ? 'locked' : 'offline'}
                    </Text>
                  )}
                </View>
                <View style={styles.nodeChipBottom}>
                  <Text numberOfLines={1} style={styles.nodeUrl}>
                    {(h?.resolvedUrl ?? node.url).replace(/^https?:\/\//, '')}
                  </Text>
                  {selected && (
                    <View style={styles.chipActions}>
                      <Pressable onPress={handleEditSelected}>
                        <Text style={styles.removeText}>edit</Text>
                      </Pressable>
                      {nodes.length > 1 && (
                        <Pressable onPress={() => removeNode(node.id)}>
                          <Text style={styles.removeText}>remove</Text>
                        </Pressable>
                      )}
                    </View>
                  )}
                </View>
              </Pressable>
            );
          })}

          <Pressable
            style={[styles.addBtn, showAdd && styles.addBtnActive]}
            onPress={() => setShowAdd((v) => !v)}>
            <Text style={[styles.addBtnText, showAdd && styles.addBtnTextActive]}>
              {showAdd ? 'Cancel' : '+ Add / Edit Node'}
            </Text>
          </Pressable>
        </View>

        {showAdd && (
          <View style={styles.card}>
            <Text style={styles.sectionLabel}>CONFIGURE NODE</Text>
            <TextInput
              value={nameInput}
              onChangeText={setNameInput}
              placeholder="Name (e.g. fedora-i3)"
              placeholderTextColor={Palette.dim}
              style={styles.input}
            />
            <TextInput
              value={urlInput}
              onChangeText={setUrlInput}
              placeholder="Primary URL (e.g. http://192.168.1.41:8080)"
              placeholderTextColor={Palette.dim}
              autoCapitalize="none"
              autoCorrect={false}
              style={styles.input}
            />
            <TextInput
              value={tailscaleInput}
              onChangeText={setTailscaleInput}
              placeholder="Tailscale URL (e.g. http://100.x.y.z:8080, auto-detected)"
              placeholderTextColor={Palette.dim}
              autoCapitalize="none"
              autoCorrect={false}
              style={styles.input}
            />
            <TextInput
              value={tokenInput}
              onChangeText={setTokenInput}
              placeholder="Password / X-July-Token (optional)"
              placeholderTextColor={Palette.dim}
              autoCapitalize="none"
              autoCorrect={false}
              secureTextEntry
              style={styles.input}
            />
            <Pressable style={styles.primaryBtn} onPress={handleSaveNode}>
              <Text style={styles.primaryBtnText}>Save & Connect</Text>
            </Pressable>
          </View>
        )}

        {activeHealth?.online && stats ? (
          <>
            <View style={styles.card}>
              <View style={styles.cardHeaderRow}>
                <View style={styles.hostWrap}>
                  <Text numberOfLines={1} style={styles.hostTitle}>
                    {stats.hostname}
                  </Text>
                  <Text style={styles.hostSub}>
                    {stats.os}/{stats.arch}
                  </Text>
                </View>
                <View style={styles.onlinePill}>
                  <View style={styles.onlineDot} />
                  <Text style={styles.onlineText}>ONLINE</Text>
                </View>
              </View>

              <View style={styles.kpiGrid}>
                <View style={[styles.kpiCol, styles.kpiColDivider]}>
                  <View style={[styles.kpiCell, styles.kpiRowDivider]}>
                    <Text style={styles.kpiNum}>{stats.cores}</Text>
                    <Text style={styles.kpiLabel}>CORES</Text>
                  </View>
                  <View style={styles.kpiCell}>
                    <Text style={styles.kpiNum}>{stats.load_1m.toFixed(2)}</Text>
                    <Text style={styles.kpiLabel}>LOAD 1M</Text>
                  </View>
                </View>

                <View style={styles.kpiCol}>
                  <View style={[styles.kpiCell, styles.kpiRowDivider]}>
                    <Text style={styles.kpiNum}>{stats.active_tasks}</Text>
                    <Text style={styles.kpiLabel}>ACTIVE</Text>
                  </View>
                  <View style={styles.kpiCell}>
                    <Text style={styles.kpiNum}>{stats.queue_depth}</Text>
                    <Text style={styles.kpiLabel}>QUEUED</Text>
                  </View>
                </View>
              </View>

              <View style={styles.metersGroup}>
                <ProgressMeter
                  label="MEMORY"
                  usedText={`${formatMb(stats.ram_used_mb)} / ${formatMb(stats.ram_total_mb)} MB`}
                  freeText={`${formatMb(stats.ram_available_mb)} MB free`}
                  pct={ramPct}
                  fillColor={Palette.text}
                />

                <ProgressMeter
                  label="TMPFS (/tmp)"
                  usedText={`${formatMb(stats.tmpfs_used_mb)} / ${formatMb(stats.tmpfs_total_mb)} MB`}
                  freeText={`${formatMb(tmpfsFreeMb)} MB free`}
                  pct={tmpfsPct}
                  fillColor={Palette.amber}
                />
              </View>
            </View>

            <View style={styles.card}>
              <Text style={styles.sectionLabel}>SYSTEM</Text>
              <View style={styles.infoTable}>
                <InfoRow
                  label="Ollama"
                  value={
                    stats.ollama_up && stats.ollama_loaded
                      ? 'awake · 7B in RAM'
                      : stats.ollama_up
                        ? 'standby · :11434'
                        : 'asleep'
                  }
                  highlight={Boolean(stats.ollama_up && stats.ollama_loaded)}
                />
                <InfoRow
                  label="Load (1m / 5m)"
                  value={`${stats.load_1m.toFixed(2)} / ${stats.load_5m.toFixed(2)}`}
                />
                <InfoRow label="Uptime" value={formatUptime(stats.uptime_sec)} />
                <InfoRow
                  label="Tailscale"
                  value={stats.tailscale_ip ? stats.tailscale_ip : 'not connected'}
                  highlight={Boolean(stats.tailscale_ip)}
                />
                <InfoRow
                  label="Auth"
                  value={stats.auth_required ? 'token protected' : 'open (no token)'}
                />
                <InfoRow
                  label="Queue Capacity"
                  value={`${stats.queue_depth} / ${stats.max_queue}`}
                  last
                />
              </View>

              <View style={styles.divider} />

              <Text style={styles.sectionLabel}>WORKSPACES</Text>
              <View style={styles.wsRow}>
                {stats.workspaces.length > 0 ? (
                  stats.workspaces.map((ws) => (
                    <View key={ws} style={styles.wsPill}>
                      <Text style={styles.wsText}>{ws}</Text>
                    </View>
                  ))
                ) : (
                  <Text style={styles.mutedText}>No warm workspaces on tmpfs</Text>
                )}
              </View>

              <View style={styles.actionsRow}>
                <Pressable style={styles.ghostBtn} onPress={handleToggleOllama}>
                  <Text style={styles.ghostBtnText}>
                    {stats.ollama_up && stats.ollama_loaded ? 'Sleep Agent' : 'Wake Agent'}
                  </Text>
                </Pressable>
                <Pressable style={styles.ghostBtn} onPress={onRefresh}>
                  <Text style={styles.ghostBtnText}>Refresh</Text>
                </Pressable>
              </View>
              {actionMsg && <Text style={styles.actionMsg}>{actionMsg}</Text>}
            </View>
          </>
        ) : activeHealth?.authRequired ? (
          <View style={styles.card}>
            <Text style={styles.offlineTitle}>NODE LOCKED · TOKEN REQUIRED</Text>
            <Text style={styles.offlineDetail}>{activeUrl} responded HTTP 401</Text>
            <Text style={styles.mutedText}>
              Enter the node&apos;s pairing token (`~/.july/token` or `--token`). Saved encrypted in
              iOS Keychain.
            </Text>
            <TextInput
              value={unlockInput}
              onChangeText={setUnlockInput}
              placeholder="Enter node token"
              placeholderTextColor={Palette.dim}
              autoCapitalize="none"
              autoCorrect={false}
              secureTextEntry
              style={styles.input}
            />
            <Pressable style={styles.primaryBtn} onPress={handleUnlock}>
              <Text style={styles.primaryBtnText}>Unlock & Save</Text>
            </Pressable>
          </View>
        ) : (
          <View style={styles.card}>
            <Text style={styles.offlineTitle}>NODE UNREACHABLE</Text>
            <Text style={styles.offlineDetail}>
              {activeUrl} ({activeHealth?.error ?? 'connecting...'})
            </Text>
            <Text style={styles.mutedText}>
              Ensure `./server 8080` is running on the target host and reachable over LAN or
              Tailscale.
            </Text>
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
  headerBlock: {
    gap: 2,
    paddingVertical: 4,
  },
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
  nodeStrip: {
    gap: 8,
  },
  nodeChip: {
    width: '100%',
    backgroundColor: Palette.card,
    borderWidth: 1,
    borderColor: Palette.border,
    borderRadius: 0,
    paddingHorizontal: 14,
    paddingVertical: 11,
    gap: 4,
  },
  nodeChipSelected: {
    backgroundColor: Palette.cardHeader,
    borderColor: Palette.text,
  },
  nodeChipTop: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 10,
  },
  nodeNameWrap: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
    flex: 1,
    minWidth: 0,
  },
  dot: { width: 7, height: 7, borderRadius: 0 },
  dotOn: { backgroundColor: Palette.text },
  dotOff: { backgroundColor: 'transparent', borderWidth: 1.5, borderColor: Palette.dim },
  nodeName: {
    color: Palette.text,
    fontFamily: Fonts.semibold,
    fontSize: 14,
    flexShrink: 1,
  },
  rttText: {
    color: Palette.text,
    fontFamily: Fonts.mono,
    fontSize: 11,
    fontVariant: ['tabular-nums'],
  },
  rttOffline: {
    color: Palette.dim,
    fontFamily: Fonts.mono,
    fontSize: 11,
  },
  nodeChipBottom: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingLeft: 15,
  },
  nodeUrl: {
    color: Palette.muted,
    fontFamily: Fonts.mono,
    fontSize: 11,
    flexShrink: 1,
  },
  chipActions: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
  },
  removeText: {
    color: Palette.muted,
    fontFamily: Fonts.mono,
    fontSize: 11,
    textDecorationLine: 'underline',
  },
  addBtn: {
    alignSelf: 'flex-start',
    borderWidth: 1,
    borderColor: Palette.border,
    backgroundColor: Palette.card,
    borderRadius: 0,
    paddingHorizontal: 12,
    paddingVertical: 7,
  },
  addBtnActive: {
    backgroundColor: Palette.text,
    borderColor: Palette.text,
  },
  addBtnText: {
    color: Palette.muted,
    fontFamily: Fonts.semibold,
    fontSize: 11,
  },
  addBtnTextActive: {
    color: Palette.inverseText,
  },
  card: {
    backgroundColor: Palette.card,
    borderRadius: 0,
    borderWidth: 1,
    borderColor: Palette.border,
    padding: 16,
    gap: 14,
  },
  sectionLabel: {
    color: Palette.muted,
    fontFamily: Fonts.bold,
    fontSize: 10,
    letterSpacing: 1.5,
  },
  cardHeaderRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    gap: 12,
  },
  hostWrap: {
    flex: 1,
    minWidth: 0,
    gap: 2,
  },
  hostTitle: {
    color: Palette.text,
    fontFamily: Fonts.semibold,
    fontSize: 16,
    letterSpacing: 0.2,
  },
  hostSub: {
    color: Palette.muted,
    fontFamily: Fonts.mono,
    fontSize: 11,
  },
  onlinePill: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 6,
    paddingHorizontal: 9,
    paddingVertical: 4,
    borderRadius: 0,
    borderWidth: 1,
    borderColor: Palette.border,
    backgroundColor: Palette.cardHeader,
  },
  onlineDot: {
    width: 6,
    height: 6,
    borderRadius: 0,
    backgroundColor: Palette.text,
  },
  onlineText: {
    color: Palette.text,
    fontFamily: Fonts.bold,
    fontSize: 10,
    letterSpacing: 1,
  },
  kpiGrid: {
    flexDirection: 'row',
    borderWidth: 1,
    borderColor: Palette.border,
    borderRadius: 0,
    backgroundColor: Palette.bg,
    overflow: 'hidden',
  },
  kpiCol: {
    flex: 1,
  },
  kpiColDivider: {
    borderRightWidth: 1,
    borderRightColor: Palette.border,
  },
  kpiRowDivider: {
    borderBottomWidth: 1,
    borderBottomColor: Palette.border,
  },
  kpiCell: {
    alignItems: 'center',
    justifyContent: 'center',
    paddingVertical: 11,
    gap: 3,
  },
  kpiNum: {
    color: Palette.text,
    fontSize: 20,
    fontFamily: Fonts.semibold,
    fontVariant: ['tabular-nums'],
  },
  kpiLabel: {
    color: Palette.dim,
    fontFamily: Fonts.bold,
    fontSize: 9,
    letterSpacing: 1.1,
  },
  metersGroup: {
    gap: 14,
  },
  meterWrap: {
    gap: 6,
  },
  meterRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'baseline',
    gap: 8,
  },
  meterLabel: {
    color: Palette.muted,
    fontFamily: Fonts.bold,
    fontSize: 10,
    letterSpacing: 1.3,
  },
  meterValue: {
    color: Palette.text,
    fontFamily: Fonts.mono,
    fontSize: 12,
    fontVariant: ['tabular-nums'],
  },
  track: {
    height: 3,
    borderRadius: 0,
    backgroundColor: Palette.track,
    overflow: 'hidden',
  },
  fill: {
    height: '100%',
    borderRadius: 0,
  },
  meterSubRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
  },
  meterSubLeft: {
    color: Palette.dim,
    fontFamily: Fonts.mono,
    fontSize: 10,
    fontVariant: ['tabular-nums'],
  },
  meterSubRight: {
    color: Palette.muted,
    fontFamily: Fonts.mono,
    fontSize: 10,
    fontVariant: ['tabular-nums'],
  },
  infoTable: {
    gap: 0,
  },
  infoRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    paddingVertical: 9,
    gap: 12,
  },
  infoRowBorder: {
    borderBottomWidth: 1,
    borderBottomColor: Palette.elevated,
  },
  infoLabel: {
    color: Palette.muted,
    fontFamily: Fonts.medium,
    fontSize: 12,
    flexShrink: 1,
  },
  infoValue: {
    color: Palette.text,
    fontFamily: Fonts.mono,
    fontSize: 12,
    fontVariant: ['tabular-nums'],
  },
  infoValueBright: {
    fontWeight: '700',
  },
  divider: {
    height: 1,
    backgroundColor: Palette.border,
  },
  wsRow: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: 6,
  },
  wsPill: {
    backgroundColor: Palette.bg,
    borderColor: Palette.border,
    borderWidth: 1,
    borderRadius: 0,
    paddingHorizontal: 10,
    paddingVertical: 5,
  },
  wsText: {
    color: Palette.text,
    fontFamily: Fonts.mono,
    fontSize: 11,
  },
  actionsRow: {
    flexDirection: 'row',
    gap: 10,
    paddingTop: 2,
  },
  ghostBtn: {
    flex: 1,
    borderWidth: 1,
    borderColor: Palette.border,
    backgroundColor: Palette.cardHeader,
    borderRadius: 0,
    paddingVertical: 10,
    alignItems: 'center',
  },
  ghostBtnText: {
    color: Palette.text,
    fontFamily: Fonts.semibold,
    fontSize: 12,
  },
  actionMsg: {
    color: Palette.muted,
    fontFamily: Fonts.mono,
    fontSize: 11,
  },
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
  primaryBtn: {
    backgroundColor: Palette.text,
    borderRadius: 0,
    paddingVertical: 12,
    alignItems: 'center',
  },
  primaryBtnText: {
    color: Palette.inverseText,
    fontFamily: Fonts.bold,
    fontSize: 13,
  },
  offlineTitle: {
    color: Palette.text,
    fontFamily: Fonts.bold,
    fontSize: 12,
    letterSpacing: 1.4,
  },
  offlineDetail: {
    color: Palette.muted,
    fontFamily: Fonts.mono,
    fontSize: 12,
  },
  mutedText: {
    color: Palette.dim,
    fontFamily: Fonts.sans,
    fontSize: 12,
    lineHeight: 18,
  },
});
