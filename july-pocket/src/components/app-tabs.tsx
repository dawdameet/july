import { NativeTabs } from 'expo-router/native-tabs';
import { Platform } from 'react-native';

import { Palette } from '@/constants/theme';
import { useJuly } from '@/context/july-context';

export default function AppTabs() {
  const { tasks } = useJuly();
  const activeCount = tasks.filter((t) => t.status === 'running' || t.status === 'queued').length;

  return (
    <NativeTabs
      backgroundColor={Platform.OS === 'ios' ? undefined : Palette.card}
      blurEffect="systemUltraThinMaterialDark"
      indicatorColor={Palette.elevated}
      tintColor={Palette.text}
      iconColor={{ selected: Palette.text, default: Palette.dim }}
      labelStyle={{ selected: { color: Palette.text }, default: { color: Palette.dim } }}>
      <NativeTabs.Trigger name="index">
        <NativeTabs.Trigger.Label>Machines</NativeTabs.Trigger.Label>
        <NativeTabs.Trigger.Icon sf="server.rack" />
      </NativeTabs.Trigger>

      <NativeTabs.Trigger name="command">
        <NativeTabs.Trigger.Label>Command</NativeTabs.Trigger.Label>
        <NativeTabs.Trigger.Icon sf="terminal" />
      </NativeTabs.Trigger>

      <NativeTabs.Trigger name="queue">
        <NativeTabs.Trigger.Label>Queue</NativeTabs.Trigger.Label>
        <NativeTabs.Trigger.Icon sf="list.bullet.rectangle.portrait" />
        {activeCount > 0 && <NativeTabs.Trigger.Badge>{String(activeCount)}</NativeTabs.Trigger.Badge>}
      </NativeTabs.Trigger>

      <NativeTabs.Trigger name="artifacts">
        <NativeTabs.Trigger.Label>Artifacts</NativeTabs.Trigger.Label>
        <NativeTabs.Trigger.Icon sf="photo.on.rectangle.angled" />
      </NativeTabs.Trigger>
    </NativeTabs>
  );
}
