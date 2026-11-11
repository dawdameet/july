import React, { useState } from 'react';
import {
  Modal,
  Pressable,
  RefreshControl,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native';
import Animated, { FadeIn, SlideInLeft } from 'react-native-reanimated';
import { SafeAreaView, useSafeAreaInsets } from 'react-native-safe-area-context';

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

function formatGb(mb: number): string {
  return (mb / 1024).toFixed(1);
}

function Sparkline({
  label,
  valueText,
  values,
  maxHint,
}: {
  label: string;
  valueText: string;
  values: number[];
  maxHint?: number;
}) {
  const slots = 24;
  const padded = [
    ...Array(Math.max(0, slots - values.length)).fill(null),
    ...values.slice(-slots),
  ] as Array<number | null>;
  const numericVals = values.filter((v) => v > 0);
  const peak = Math.max(maxHint ?? 1, ...(numericVals.length > 0 ? numericVals : [1]));

  return (
    <View style={styles.sparkBox}>
      <View style={styles.sparkHead}>
        <Text style={styles.sparkLabel}>{label}</Text>
        <Text style={styles.sparkValue}>{valueText}</Text>
      </View>
      <View style={styles.sparkBars}>
        {padded.map((v, idx) => {
          const isLast = idx === padded.length - 1;
          const hPct = v === null ? 8 : Math.max(10, Math.min(100, Math.round((v / peak) * 100)));
          return (
            <View key={idx} style={styles.sparkBarSlot}>
              <View
                style={[
                  styles.sparkBar,
                  {
                    height: `${hPct}%`,
                    backgroundColor:
                      v === null
                        ? Palette.elevated
                        : isLast
                          ? Palette.text
                          : Palette.dim,
                  },
                ]}
              />
            </View>
          );
        })}
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
  const insets = useSafeAreaInsets();
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
    historyById,
    tasks,
    refreshNow,
  } = useJuly();

  const [showSidebar, setShowSidebar] = useState(false);
  const [showNodeMenu, setShowNodeMenu] = useState(false);
  const [showAdd, setShowAdd] = useState(false);
  const [editingId, setEditingId] = useState<string | null>(null);
  const [nameInput, setNameInput] = useState('');
  const [urlInput, setUrlInput] = useState('');
  const [tailscaleInput, setTailscaleInput] = useState('');
  const [tokenInput, setTokenInput] = useState('');
  const [showToken, setShowToken] = useState(false);
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

  const resetForm = () => {
    setEditingId(null);
    setNameInput('');
    setUrlInput('');
    setTailscaleInput('');
    setTokenInput('');
    setShowToken(false);
  };

  const handleSaveNode = () => {
    const cleanName = nameInput.trim() || `node-${nodes.length + 1}`;
    const cleanId = editingId ?? cleanName.toLowerCase().replace(/[^a-z0-9-_]/g, '-');
    addOrUpdateNode({
      id: cleanId,
      name: cleanName,
      url: urlInput.trim() || 'http://192.168.1.41:8080',
      tailscaleUrl: tailscaleInput.trim() || undefined,
      token: tokenInput.trim(),
    });
    resetForm();
    setShowAdd(false);
  };

  const handleToggleAddNew = () => {
    setShowNodeMenu(false);
    if (showAdd && editingId === null) {
      setShowAdd(false);
      return;
    }
    resetForm();
    setShowAdd(true);
  };

  const handleEditNode = (node: typeof activeNode) => {
    setActiveNodeId(node.id);
    setShowNodeMenu(false);
    if (showAdd && editingId === node.id) {
      setShowAdd(false);
      return;
    }
    setEditingId(node.id);
    setNameInput(node.name);
    setUrlInput(node.url);
    setTailscaleInput(node.tailscaleUrl ?? '');
    setTokenInput(node.token);
    setShowToken(false);
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
  const diskTotalMb = stats?.disk_total_mb ?? 0;
  const diskUsedMb = stats?.disk_used_mb ?? 0;
  const diskPct = diskTotalMb > 0 ? (diskUsedMb / diskTotalMb) * 100 : 0;
  const diskFreeMb = Math.max(0, diskTotalMb - diskUsedMb);
  const nodeHistory = historyById[activeNode.id] ?? [];
  const cpuSeries = nodeHistory.map((p) => p.cpuPct);
  const rttSeries = nodeHistory.map((p) => p.rttMs);
  const liveCpuPct =
    typeof stats?.cpu_pct === 'number'
      ? stats.cpu_pct
      : stats
        ? Math.min(100, (stats.load_1m / Math.max(1, stats.cores)) * 100)
        : 0;
  const doneCount = tasks.filter((t) => t.status === 'completed' && t.exit_code === 0).length;

  return (
    <SafeAreaView style={styles.safe} edges={['top']}>
      <ScrollView
        contentContainerStyle={styles.scroll}
        keyboardShouldPersistTaps="handled"
        refreshControl={
          <RefreshControl refreshing={refreshing} onRefresh={onRefresh} tintColor={Palette.text} />
        }>
        <View style={styles.topBar}>
          <Pressable
            onPress={() => {
              setShowNodeMenu(false);
              setShowSidebar(true);
            }}
            style={styles.sidebarBtn}>
            <View style={styles.hamburgerLines}>
              <View style={styles.hamLine} />
              <View style={styles.hamLine} />
              <View style={styles.hamLineShort} />
            </View>
            <Text style={styles.sidebarBtnText}>SYS</Text>
          </Pressable>

          <Pressable
            onPress={() => setShowNodeMenu((v) => !v)}
            style={[styles.dropdownTrigger, showNodeMenu && styles.dropdownTriggerActive]}>
            <View
              style={[
                styles.dot,
                activeHealth?.online ? styles.dotOn : styles.dotOff,
              ]}
            />
            <Text numberOfLines={1} style={styles.dropdownName}>
              {activeNode.name}
            </Text>
            <Text style={styles.dropdownMeta}>
              {activeHealth?.online && activeHealth.rttMs !== null
                ? `${activeHealth.rttMs}ms`
                : activeHealth?.authRequired
                  ? 'locked'
                  : 'offline'}
            </Text>
            <Text style={styles.dropdownChevron}>{showNodeMenu ? '▲' : '▼'}</Text>
          </Pressable>
        </View>

        {showNodeMenu && (
          <Animated.View entering={FadeIn.duration(140)} style={styles.dropdownMenu}>
            <View style={styles.dropdownHead}>
              <Text style={styles.sectionLabel}>
                NODES ({nodes.length}) · 2S POLL
              </Text>
              <Pressable onPress={handleToggleAddNew}>
                <Text style={styles.dropdownAddLink}>+ Add Node</Text>
              </Pressable>
            </View>

            {nodes.map((node, idx) => {
              const h = healthById[node.id];
              const selected = node.id === activeNodeId;
              return (
                <Pressable
                  key={node.id}
                  onPress={() => {
                    setActiveNodeId(node.id);
                    setShowNodeMenu(false);
                  }}
                  style={[
                    styles.dropdownItem,
                    idx < nodes.length - 1 && styles.dropdownItemBorder,
                    selected && styles.dropdownItemSelected,
                  ]}>
                  <View style={styles.dropdownItemTop}>
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
                    <View style={styles.chipActions}>
                      <Pressable onPress={() => handleEditNode(node)}>
                        <Text style={styles.removeText}>edit</Text>
                      </Pressable>
                      {nodes.length > 1 && (
                        <Pressable onPress={() => removeNode(node.id)}>
                          <Text style={styles.removeText}>remove</Text>
                        </Pressable>
                      )}
                    </View>
                  </View>
                </Pressable>
              );
            })}
          </Animated.View>
        )}

        {showAdd && (
          <View style={styles.card}>
            <View style={styles.formHeaderRow}>
              <Text style={styles.sectionLabel}>
                {editingId ? `EDIT NODE · ${editingId}` : 'ADD NODE'}
              </Text>
              <Pressable onPress={() => setShowAdd(false)}>
                <Text style={styles.removeText}>close</Text>
              </Pressable>
            </View>

            <View style={styles.fieldGroup}>
              <Text style={styles.fieldLabel}>NAME</Text>
              <TextInput
                value={nameInput}
                onChangeText={setNameInput}
                placeholder="fedora-i3"
                placeholderTextColor={Palette.dim}
                autoCapitalize="none"
                autoCorrect={false}
                spellCheck={false}
                autoComplete="off"
                textContentType="none"
                style={styles.input}
              />
            </View>

            <View style={styles.fieldGroup}>
              <Text style={styles.fieldLabel}>PRIMARY URL (USE http://)</Text>
              <TextInput
                value={urlInput}
                onChangeText={setUrlInput}
                placeholder="http://192.168.1.41:8080"
                placeholderTextColor={Palette.dim}
                keyboardType="url"
                autoCapitalize="none"
                autoCorrect={false}
                spellCheck={false}
                autoComplete="off"
                textContentType="none"
                style={styles.input}
              />
            </View>

            <View style={styles.fieldGroup}>
              <Text style={styles.fieldLabel}>TAILSCALE FALLBACK URL (OPTIONAL)</Text>
              <TextInput
                value={tailscaleInput}
                onChangeText={setTailscaleInput}
                placeholder="http://100.72.209.11:8080"
                placeholderTextColor={Palette.dim}
                keyboardType="url"
                autoCapitalize="none"
                autoCorrect={false}
                spellCheck={false}
                autoComplete="off"
                textContentType="none"
                style={styles.input}
              />
            </View>

            <View style={styles.fieldGroup}>
              <View style={styles.fieldLabelRow}>
                <Text style={styles.fieldLabel}>TOKEN (~/.july/token)</Text>
                <Pressable onPress={() => setShowToken((v) => !v)}>
                  <Text style={styles.removeText}>{showToken ? 'hide' : 'show'}</Text>
                </Pressable>
              </View>
              <TextInput
                value={tokenInput}
                onChangeText={setTokenInput}
                placeholder="paste 32-char token (optional)"
                placeholderTextColor={Palette.dim}
                autoCapitalize="none"
                autoCorrect={false}
                spellCheck={false}
                autoComplete="off"
                textContentType="none"
                secureTextEntry={!showToken}
                style={styles.input}
              />
            </View>

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

                {diskTotalMb > 0 && (
                  <ProgressMeter
                    label="DISK (/)"
                    usedText={`${formatGb(diskUsedMb)} / ${formatGb(diskTotalMb)} GB`}
                    freeText={`${formatGb(diskFreeMb)} GB free`}
                    pct={diskPct}
                    fillColor={Palette.dim}
                  />
                )}
              </View>
            </View>

            <View style={styles.card}>
              <Text style={styles.sectionLabel}>LIVE TELEMETRY</Text>

              <View style={styles.sparkGrid}>
                <Sparkline
                  label="CPU"
                  valueText={`${liveCpuPct.toFixed(1)}%`}
                  values={cpuSeries}
                  maxHint={100}
                />
                <Sparkline
                  label="RTT"
                  valueText={activeHealth?.rttMs !== null ? `${activeHealth.rttMs} ms` : '—'}
                  values={rttSeries}
                  maxHint={50}
                />
              </View>

              <View style={styles.miniTriGrid}>
                <View style={[styles.miniTriCell, styles.kpiColDivider]}>
                  <Text style={styles.miniTriVal}>{formatUptime(stats.uptime_sec)}</Text>
                  <Text style={styles.kpiLabel}>UPTIME</Text>
                </View>
                <View style={[styles.miniTriCell, styles.kpiColDivider]}>
                  <Text style={styles.miniTriVal}>
                    {typeof stats.cpu_temp_c === 'number' && stats.cpu_temp_c > 0
                      ? `${stats.cpu_temp_c.toFixed(0)}°C`
                      : `${formatMb(stats.swap_used_mb ?? 0)}M`}
                  </Text>
                  <Text style={styles.kpiLabel}>
                    {typeof stats.cpu_temp_c === 'number' && stats.cpu_temp_c > 0 ? 'TEMP' : 'SWAP'}
                  </Text>
                </View>
                <View style={styles.miniTriCell}>
                  <Text style={styles.miniTriVal}>{doneCount}</Text>
                  <Text style={styles.kpiLabel}>TASKS OK</Text>
                </View>
              </View>
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
              placeholder="Paste node token"
              placeholderTextColor={Palette.dim}
              autoCapitalize="none"
              autoCorrect={false}
              spellCheck={false}
              autoComplete="off"
              textContentType="none"
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

      <Modal
        visible={showSidebar}
        transparent
        animationType="none"
        onRequestClose={() => setShowSidebar(false)}>
        <View style={styles.drawerOverlay}>
          <Pressable style={styles.drawerBackdrop} onPress={() => setShowSidebar(false)} />
          <Animated.View
            entering={SlideInLeft.duration(220)}
            style={[
              styles.drawerPanel,
              {
                paddingTop: Math.max(insets.top, 16),
                paddingBottom: Math.max(insets.bottom, 16),
              },
            ]}>
            <View style={styles.drawerHeader}>
              <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
                <Text style={styles.drawerTitle}>SYSTEM</Text>
                <Text numberOfLines={1} style={styles.drawerSub}>
                  {activeNode.name} · {activeUrl.replace(/^https?:\/\//, '')}
                </Text>
              </View>
              <Pressable onPress={() => setShowSidebar(false)} style={styles.drawerCloseBtn}>
                <Text style={styles.drawerCloseText}>ESC</Text>
              </Pressable>
            </View>

            <ScrollView
              contentContainerStyle={styles.drawerScroll}
              showsVerticalScrollIndicator={false}>
              {activeHealth?.online && stats ? (
                <>
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
                    {typeof stats.swap_total_mb === 'number' && stats.swap_total_mb > 0 && (
                      <InfoRow
                        label="Swap"
                        value={`${formatMb(stats.swap_used_mb ?? 0)} / ${formatMb(stats.swap_total_mb)} MB`}
                      />
                    )}
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

                  <View style={styles.divider} />

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
                </>
              ) : (
                <View style={{ gap: 10 }}>
                  <Text style={styles.mutedText}>
                    {activeHealth?.authRequired
                      ? 'Node is locked. Enter its pairing token on Machines to view system details.'
                      : 'Node unreachable. Ensure ./server is running and reachable over LAN or Tailscale.'}
                  </Text>
                  <Pressable style={styles.ghostBtn} onPress={onRefresh}>
                    <Text style={styles.ghostBtnText}>Refresh</Text>
                  </Pressable>
                </View>
              )}
            </ScrollView>
          </Animated.View>
        </View>
      </Modal>
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
  topBar: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 10,
    paddingVertical: 4,
  },
  sidebarBtn: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
    backgroundColor: Palette.card,
    borderWidth: 1,
    borderColor: Palette.border,
    borderRadius: 10,
    paddingHorizontal: 11,
    paddingVertical: 8,
  },
  hamburgerLines: {
    gap: 3,
    width: 14,
  },
  hamLine: {
    height: 1.5,
    width: 14,
    backgroundColor: Palette.text,
    borderRadius: 1,
  },
  hamLineShort: {
    height: 1.5,
    width: 9,
    backgroundColor: Palette.muted,
    borderRadius: 1,
  },
  sidebarBtnText: {
    color: Palette.text,
    fontFamily: Fonts.bold,
    fontSize: 10,
    letterSpacing: 1.4,
  },
  dropdownTrigger: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 7,
    maxWidth: '64%',
    backgroundColor: Palette.card,
    borderWidth: 1,
    borderColor: Palette.border,
    borderRadius: 10,
    paddingHorizontal: 11,
    paddingVertical: 8,
  },
  dropdownTriggerActive: {
    borderColor: Palette.text,
    backgroundColor: Palette.cardHeader,
  },
  dropdownName: {
    color: Palette.text,
    fontFamily: Fonts.semibold,
    fontSize: 12,
    flexShrink: 1,
  },
  dropdownMeta: {
    color: Palette.muted,
    fontFamily: Fonts.mono,
    fontSize: 10,
    fontVariant: ['tabular-nums'],
  },
  dropdownChevron: {
    color: Palette.muted,
    fontSize: 9,
  },
  dropdownMenu: {
    backgroundColor: Palette.card,
    borderWidth: 1,
    borderColor: Palette.border,
    borderRadius: 14,
    padding: 12,
    gap: 8,
  },
  dropdownHead: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingHorizontal: 2,
    paddingBottom: 4,
  },
  dropdownAddLink: {
    color: Palette.text,
    fontFamily: Fonts.semibold,
    fontSize: 11,
  },
  dropdownItem: {
    borderRadius: 10,
    paddingHorizontal: 10,
    paddingVertical: 9,
    gap: 4,
  },
  dropdownItemBorder: {
    borderBottomWidth: 1,
    borderBottomColor: Palette.elevated,
  },
  dropdownItemSelected: {
    backgroundColor: Palette.cardHeader,
    borderWidth: 1,
    borderColor: Palette.border,
  },
  dropdownItemTop: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 10,
  },
  drawerOverlay: {
    flex: 1,
    flexDirection: 'row',
  },
  drawerBackdrop: {
    position: 'absolute',
    top: 0,
    right: 0,
    bottom: 0,
    left: 0,
    backgroundColor: 'rgba(0, 0, 0, 0.72)',
  },
  drawerPanel: {
    width: '82%',
    maxWidth: 330,
    height: '100%',
    backgroundColor: Palette.card,
    borderRightWidth: 1,
    borderRightColor: Palette.border,
    paddingHorizontal: 18,
    gap: 14,
  },
  drawerHeader: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 10,
    paddingBottom: 12,
    borderBottomWidth: 1,
    borderBottomColor: Palette.border,
  },
  drawerTitle: {
    color: Palette.text,
    fontFamily: Fonts.bold,
    fontSize: 14,
    letterSpacing: 2,
  },
  drawerSub: {
    color: Palette.muted,
    fontFamily: Fonts.mono,
    fontSize: 11,
  },
  drawerCloseBtn: {
    borderWidth: 1,
    borderColor: Palette.border,
    backgroundColor: Palette.bg,
    borderRadius: 7,
    paddingHorizontal: 9,
    paddingVertical: 5,
  },
  drawerCloseText: {
    color: Palette.text,
    fontFamily: Fonts.bold,
    fontSize: 10,
    letterSpacing: 0.8,
  },
  drawerScroll: {
    gap: 14,
    paddingBottom: 16,
  },
  nodeNameWrap: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
    flex: 1,
    minWidth: 0,
  },
  dot: { width: 7, height: 7, borderRadius: 4 },
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
  card: {
    backgroundColor: Palette.card,
    borderRadius: 14,
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
    borderRadius: 999,
    borderWidth: 1,
    borderColor: Palette.border,
    backgroundColor: Palette.cardHeader,
  },
  onlineDot: {
    width: 6,
    height: 6,
    borderRadius: 3,
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
    borderRadius: 10,
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
    borderRadius: 2,
    backgroundColor: Palette.track,
    overflow: 'hidden',
  },
  fill: {
    height: '100%',
    borderRadius: 2,
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
    borderRadius: 7,
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
    borderRadius: 10,
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
  formHeaderRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
  },
  fieldGroup: {
    gap: 5,
  },
  fieldLabelRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
  },
  fieldLabel: {
    color: Palette.dim,
    fontFamily: Fonts.bold,
    fontSize: 9,
    letterSpacing: 1.2,
  },
  input: {
    backgroundColor: Palette.bg,
    borderWidth: 1,
    borderColor: Palette.border,
    borderRadius: 10,
    paddingHorizontal: 12,
    paddingVertical: 10,
    color: Palette.text,
    fontFamily: Fonts.mono,
    fontSize: 12,
  },
  primaryBtn: {
    backgroundColor: Palette.text,
    borderRadius: 10,
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
  sparkGrid: {
    flexDirection: 'row',
    gap: 10,
  },
  sparkBox: {
    flex: 1,
    backgroundColor: Palette.bg,
    borderWidth: 1,
    borderColor: Palette.border,
    borderRadius: 10,
    padding: 10,
    gap: 8,
  },
  sparkHead: {
    flexDirection: 'row',
    alignItems: 'baseline',
    justifyContent: 'space-between',
    gap: 6,
  },
  sparkLabel: {
    color: Palette.muted,
    fontFamily: Fonts.bold,
    fontSize: 9,
    letterSpacing: 1.2,
  },
  sparkValue: {
    color: Palette.text,
    fontFamily: Fonts.mono,
    fontSize: 12,
    fontVariant: ['tabular-nums'],
  },
  sparkBars: {
    flexDirection: 'row',
    alignItems: 'flex-end',
    height: 36,
    gap: 2,
  },
  sparkBarSlot: {
    flex: 1,
    height: '100%',
    justifyContent: 'flex-end',
  },
  sparkBar: {
    width: '100%',
    borderRadius: 1,
  },
  miniTriGrid: {
    flexDirection: 'row',
    borderWidth: 1,
    borderColor: Palette.border,
    borderRadius: 10,
    backgroundColor: Palette.bg,
    overflow: 'hidden',
  },
  miniTriCell: {
    flex: 1,
    alignItems: 'center',
    justifyContent: 'center',
    paddingVertical: 10,
    gap: 3,
  },
  miniTriVal: {
    color: Palette.text,
    fontFamily: Fonts.mono,
    fontSize: 13,
    fontVariant: ['tabular-nums'],
  },
});
