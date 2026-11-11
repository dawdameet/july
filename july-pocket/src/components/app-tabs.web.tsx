import {
  Tabs,
  TabList,
  TabTrigger,
  TabSlot,
  TabTriggerSlotProps,
  TabListProps,
} from 'expo-router/ui';
import { Pressable, Text, View, StyleSheet } from 'react-native';

import { Fonts, MaxContentWidth, Palette, Spacing } from '@/constants/theme';
import { useJuly } from '@/context/july-context';

export default function AppTabs() {
  const { tasks } = useJuly();
  const activeCount = tasks.filter((t) => t.status === 'running' || t.status === 'queued').length;

  return (
    <Tabs>
      <TabSlot style={{ height: '100%' }} />
      <TabList asChild>
        <CustomTabList>
          <TabTrigger name="index" href="/" asChild>
            <TabButton>Machines</TabButton>
          </TabTrigger>
          <TabTrigger name="command" href="/command" asChild>
            <TabButton>Command</TabButton>
          </TabTrigger>
          <TabTrigger name="queue" href="/queue" asChild>
            <TabButton>{activeCount > 0 ? `Queue (${activeCount})` : 'Queue'}</TabButton>
          </TabTrigger>
          <TabTrigger name="artifacts" href="/artifacts" asChild>
            <TabButton>Artifacts</TabButton>
          </TabTrigger>
        </CustomTabList>
      </TabList>
    </Tabs>
  );
}

export function TabButton({ children, isFocused, ref: _ref, ...props }: TabTriggerSlotProps) {
  return (
    <Pressable {...props} style={({ pressed }) => pressed && styles.pressed}>
      <View
        style={[
          styles.tabButtonView,
          { backgroundColor: isFocused ? Palette.elevated : 'transparent' },
        ]}>
        <Text
          style={[
            styles.tabLabel,
            { color: isFocused ? Palette.text : Palette.muted },
          ]}>
          {children}
        </Text>
      </View>
    </Pressable>
  );
}

export function CustomTabList(props: TabListProps) {
  return (
    <View {...props} style={styles.tabListContainer}>
      <View style={styles.innerContainer}>
        <Text style={styles.brandText}>JULY POCKET</Text>
        <View style={styles.triggerRow}>{props.children}</View>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  tabListContainer: {
    width: '100%',
    paddingHorizontal: Spacing.three,
    paddingTop: Spacing.two,
    alignItems: 'center',
    backgroundColor: Palette.bg,
  },
  innerContainer: {
    width: '100%',
    maxWidth: MaxContentWidth,
    paddingVertical: Spacing.two,
    paddingHorizontal: Spacing.three,
    borderRadius: 14,
    borderWidth: 1,
    borderColor: Palette.border,
    backgroundColor: Palette.cardHeader,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: Spacing.two,
  },
  brandText: {
    color: Palette.accent,
    fontFamily: Fonts.bold,
    fontSize: 13,
    letterSpacing: 1.5,
  },
  triggerRow: {
    flexDirection: 'row',
    gap: Spacing.one,
  },
  pressed: {
    opacity: 0.75,
  },
  tabButtonView: {
    paddingVertical: 6,
    paddingHorizontal: 12,
    borderRadius: 8,
  },
  tabLabel: {
    fontFamily: Fonts.semibold,
    fontSize: 13,
  },
});
