import '@/global.css';

import { Platform } from 'react-native';

export const Palette = {
  bg: '#000000',
  card: '#0A0A0B',
  cardHeader: '#121214',
  elevated: '#18181B',
  border: '#232327',
  text: '#FAFAFA',
  muted: '#8E8E93',
  dim: '#52525B',
  accent: '#FAFAFA',
  inverseText: '#000000',
  green: '#FAFAFA',
  amber: '#D4D4D8',
  rose: '#A1A1AA',
  track: '#18181B',
} as const;

export const Colors = {
  light: {
    text: '#FAFAFA',
    background: '#000000',
    backgroundElement: '#0A0A0B',
    backgroundSelected: '#18181B',
    textSecondary: '#8E8E93',
  },
  dark: {
    text: '#FAFAFA',
    background: '#000000',
    backgroundElement: '#0A0A0B',
    backgroundSelected: '#18181B',
    textSecondary: '#8E8E93',
  },
} as const;

export type ThemeColor = keyof typeof Colors.light & keyof typeof Colors.dark;

export const Fonts = {
  sans: 'Montserrat_400Regular',
  medium: 'Montserrat_500Medium',
  semibold: 'Montserrat_600SemiBold',
  bold: 'Montserrat_700Bold',
  serif: Platform.select({ ios: 'ui-serif', web: 'var(--font-serif)', default: 'serif' }),
  rounded: Platform.select({ ios: 'ui-rounded', web: 'var(--font-rounded)', default: 'normal' }),
  mono: Platform.select({ ios: 'ui-monospace', web: 'var(--font-mono)', default: 'monospace' }),
} as const;

export const Spacing = {
  half: 2,
  one: 4,
  two: 8,
  three: 16,
  four: 24,
  five: 32,
  six: 64,
} as const;

export const BottomTabInset = Platform.select({ ios: 50, android: 80 }) ?? 0;
export const MaxContentWidth = 860;
