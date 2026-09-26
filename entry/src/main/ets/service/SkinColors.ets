import { ConsolePalette, GlobalSettings, ThemeMode, UiSkin } from '../model/AppModels';
import { AppSettingsStore } from './AppSettingsStore';

export type SkinColorToken = 'start_window_background' | 'page_background' | 'sidebar_background' |
  'card_background' | 'card_glass_background' | 'search_background' | 'text_primary' |
  'text_secondary' | 'accent' | 'selected_background';

interface SkinColorSet {
  followsSystem: boolean;
  start_window_background: string;
  page_background: string;
  sidebar_background: string;
  card_background: string;
  card_glass_background: string;
  search_background: string;
  text_primary: string;
  text_secondary: string;
  accent: string;
  selected_background: string;
}

const VIOLET: SkinColorSet = {
  followsSystem: false,
  start_window_background: '#0B0B0F', page_background: '#0B0B0F',
  sidebar_background: '#14141A', card_background: '#FF1C1C22',
  card_glass_background: '#B81A1A25', search_background: '#B31F1F2C',
  text_primary: '#FFF2F3F7', text_secondary: '#A8ACB4BF',
  accent: '#FFA78BFA', selected_background: '#FF2A2340'
};

const BLUE: SkinColorSet = {
  followsSystem: false,
  start_window_background: '#09111F', page_background: '#09111F',
  sidebar_background: '#101E31', card_background: '#FF17263A',
  card_glass_background: '#B816273D', search_background: '#B31C3049',
  text_primary: '#FFF1F7FF', text_secondary: '#A9B8CDDD',
  accent: '#FF69A7FF', selected_background: '#FF1B3B63'
};

const PEARL: SkinColorSet = {
  followsSystem: false,
  start_window_background: '#F7F8FC', page_background: '#F7F8FC',
  sidebar_background: '#EFF0F8', card_background: '#FFFFFFFF',
  card_glass_background: '#EDFFFFFF', search_background: '#EDF1F6FC',
  text_primary: '#FF1D2434', text_secondary: '#FF627086',
  accent: '#FF6258C9', selected_background: '#FFE8E5FA'
};

/** Each future skin can decide independently whether to follow system colors. */
export function skinFollowsSystem(settings: GlobalSettings): boolean {
  if (settings.uiSkin === UiSkin.CLASSIC) return settings.themeMode === ThemeMode.SYSTEM;
  return consoleColors(settings.consolePalette).followsSystem;
}

export function consoleColors(palette: ConsolePalette): SkinColorSet {
  if (palette === ConsolePalette.BLUE) return BLUE;
  if (palette === ConsolePalette.PEARL) return PEARL;
  return VIOLET;
}

export function skinColor(token: SkinColorToken): string | Resource {
  const settings = AppSettingsStore.getInstance().getGlobalSettings();
  if (settings.uiSkin === UiSkin.CONSOLE) {
    const colors = consoleColors(settings.consolePalette);
    switch (token) {
      case 'start_window_background': return colors.start_window_background;
      case 'page_background': return colors.page_background;
      case 'sidebar_background': return colors.sidebar_background;
      case 'card_background': return colors.card_background;
      case 'card_glass_background': return colors.card_glass_background;
      case 'search_background': return colors.search_background;
      case 'text_primary': return colors.text_primary;
      case 'text_secondary': return colors.text_secondary;
      case 'accent': return colors.accent;
      case 'selected_background': return colors.selected_background;
    }
  }
  switch (token) {
    case 'start_window_background': return $r('app.color.start_window_background');
    case 'page_background': return $r('app.color.page_background');
    case 'sidebar_background': return $r('app.color.sidebar_background');
    case 'card_background': return $r('app.color.card_background');
    case 'card_glass_background': return $r('app.color.card_glass_background');
    case 'search_background': return $r('app.color.search_background');
    case 'text_primary': return $r('app.color.text_primary');
    case 'text_secondary': return $r('app.color.text_secondary');
    case 'accent': return $r('app.color.accent');
    case 'selected_background': return $r('app.color.selected_background');
  }
}
