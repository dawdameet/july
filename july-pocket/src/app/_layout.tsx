import {
  Montserrat_400Regular,
  Montserrat_500Medium,
  Montserrat_600SemiBold,
  Montserrat_700Bold,
  useFonts,
} from '@expo-google-fonts/montserrat';
import { DarkTheme, ThemeProvider } from 'expo-router';
import * as SplashScreen from 'expo-splash-screen';
import { StatusBar } from 'expo-status-bar';

import { AnimatedSplashOverlay } from '@/components/animated-icon';
import AppTabs from '@/components/app-tabs';
import { Palette } from '@/constants/theme';
import { JulyProvider } from '@/context/july-context';

SplashScreen.preventAutoHideAsync();

const JulyDarkTheme = {
  ...DarkTheme,
  colors: {
    ...DarkTheme.colors,
    background: Palette.bg,
    card: Palette.card,
    text: Palette.text,
    border: Palette.border,
    primary: Palette.accent,
  },
};

export default function TabLayout() {
  const [fontsLoaded, fontError] = useFonts({
    Montserrat_400Regular,
    Montserrat_500Medium,
    Montserrat_600SemiBold,
    Montserrat_700Bold,
  });

  if (!fontsLoaded && !fontError) {
    return null;
  }

  return (
    <ThemeProvider value={JulyDarkTheme}>
      <StatusBar style="light" />
      <JulyProvider>
        <AnimatedSplashOverlay />
        <AppTabs />
      </JulyProvider>
    </ThemeProvider>
  );
}
