import React, { useMemo, useRef, useState } from 'react';
import {
  KeyboardAvoidingView,
  Modal,
  Platform,
  Pressable,
  StyleSheet,
  Text,
  View,
} from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import WebView, { WebViewMessageEvent } from 'react-native-webview';

import { Fonts, Palette } from '@/constants/theme';
import { normalizeBaseUrl } from '@/lib/july-api';

import { XTERM_CSS, XTERM_FIT_JS, XTERM_JS } from './xterm-bundle';

type WebViewHandle = {
  injectJavaScript: (script: string) => void;
};

const NativeWebView = WebView as unknown as React.ComponentType<
  React.ComponentProps<typeof WebView> & { ref?: React.Ref<WebViewHandle> }
>;

interface TmuxTerminalProps {
  baseUrl: string;
  token?: string;
  session: string;
}

const HELPER_ROW_1: Array<{ label: string; seq?: string; isCtrl?: boolean }> = [
  { label: 'ESC', seq: '\x1b' },
  { label: 'TAB', seq: '\t' },
  { label: 'CTRL', isCtrl: true },
  { label: '^D', seq: '\x04' },
  { label: '^L', seq: '\x0c' },
];

const HELPER_ROW_2: Array<{ label: string; seq?: string; isSync?: boolean }> = [
  { label: '←', seq: '\x1b[D' },
  { label: '↓', seq: '\x1b[B' },
  { label: '↑', seq: '\x1b[A' },
  { label: '→', seq: '\x1b[C' },
  { label: 'SYNC', isSync: true },
];

export function TmuxTerminal({ baseUrl, token, session }: TmuxTerminalProps) {
  const insets = useSafeAreaInsets();
  const webRef = useRef<WebViewHandle | null>(null);
  const [status, setStatus] = useState<'connecting' | 'live' | 'closed' | 'error'>('connecting');
  const [dims, setDims] = useState<string>('');
  const [ctrlArmed, setCtrlArmed] = useState(false);
  const [showHelpers, setShowHelpers] = useState(false);
  const [fontSize, setFontSize] = useState(11.5);
  const [nonce, setNonce] = useState(0);
  const [fullscreen, setFullscreen] = useState(false);

  const wsBase = useMemo(() => {
    const clean = normalizeBaseUrl(baseUrl);
    return clean.replace(/^http:/i, 'ws:').replace(/^https:/i, 'wss:');
  }, [baseUrl]);

  const html = useMemo(() => {
    const cfg = JSON.stringify({
      wsBase,
      session,
      token: token ?? '',
    });

    return `<!doctype html>
<html>
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no" />
<style>
${XTERM_CSS}
html, body {
  margin: 0;
  padding: 0;
  width: 100%;
  height: 100%;
  background: #000000;
  overflow: hidden;
  -webkit-user-select: none;
  touch-action: manipulation;
}
#term {
  width: 100%;
  height: 100%;
  padding: 6px;
  box-sizing: border-box;
  background: #000000;
}
.xterm-viewport {
  background-color: #000000 !important;
  scrollbar-width: none;
}
.xterm-viewport::-webkit-scrollbar { display: none; }
</style>
</head>
<body>
<div id="term"></div>
<script>${XTERM_JS}</script>
<script>${XTERM_FIT_JS}</script>
<script>
(function() {
  var cfg = ${cfg};
  var post = function(obj) {
    if (window.ReactNativeWebView && window.ReactNativeWebView.postMessage) {
      window.ReactNativeWebView.postMessage(JSON.stringify(obj));
    }
  };

  var term = new window.Terminal({
    fontFamily: 'ui-monospace, Menlo, Monaco, "SF Mono", monospace',
    fontSize: 11.5,
    lineHeight: 1.2,
    cursorBlink: true,
    cursorStyle: 'block',
    scrollback: 2500,
    allowProposedApi: true,
    theme: {
      background: '#000000',
      foreground: '#FAFAFA',
      cursor: '#FAFAFA',
      cursorAccent: '#000000',
      selectionBackground: 'rgba(255,255,255,0.28)',
      black: '#0A0A0B',
      brightBlack: '#52525B',
      red: '#D4D4D8',
      brightRed: '#FAFAFA',
      green: '#E4E4E7',
      brightGreen: '#FAFAFA',
      yellow: '#D4D4D8',
      brightYellow: '#FAFAFA',
      blue: '#A1A1AA',
      brightBlue: '#E4E4E7',
      magenta: '#A1A1AA',
      brightMagenta: '#E4E4E7',
      cyan: '#D4D4D8',
      brightCyan: '#FAFAFA',
      white: '#D4D4D8',
      brightWhite: '#FAFAFA'
    }
  });

  var fitAddon = new window.FitAddon.FitAddon();
  term.loadAddon(fitAddon);
  var host = document.getElementById('term');
  term.open(host);

  var ctrl = false;
  var ws = null;

  function safeFit() {
    try { fitAddon.fit(); } catch (e) {}
    post({ type: 'dims', cols: term.cols, rows: term.rows });
    if (ws && ws.readyState === 1) {
      ws.send('\\x01R:' + term.cols + ',' + term.rows);
    }
  }

  safeFit();
  setTimeout(safeFit, 80);

  function connect() {
    post({ type: 'status', value: 'connecting' });
    var url = cfg.wsBase + '/ws/tmux?session=' + encodeURIComponent(cfg.session)
      + '&cols=' + term.cols + '&rows=' + term.rows
      + (cfg.token ? '&token=' + encodeURIComponent(cfg.token) : '');
    ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';
    ws.onopen = function() {
      post({ type: 'status', value: 'live' });
      safeFit();
      term.focus();
    };
    ws.onmessage = function(ev) {
      if (typeof ev.data === 'string') {
        term.write(ev.data);
      } else {
        term.write(new Uint8Array(ev.data));
      }
    };
    ws.onerror = function() {
      post({ type: 'status', value: 'error' });
    };
    ws.onclose = function() {
      post({ type: 'status', value: 'closed' });
    };
  }

  connect();

  term.onData(function(data) {
    if (!ws || ws.readyState !== 1) return;
    if (ctrl && data.length === 1) {
      var code = data.toUpperCase().charCodeAt(0);
      if (code >= 64 && code <= 95) {
        data = String.fromCharCode(code - 64);
      }
      ctrl = false;
      post({ type: 'ctrl', value: false });
    }
    ws.send(data);
  });

  host.addEventListener('click', function() {
    term.focus();
  });

  if (typeof ResizeObserver !== 'undefined') {
    new ResizeObserver(function() { safeFit(); }).observe(host);
  }
  window.addEventListener('resize', safeFit);

  window.__julySend = function(seq) {
    if (ws && ws.readyState === 1) {
      ws.send(seq);
    }
    term.focus();
  };
  window.__julySetCtrl = function(v) {
    ctrl = !!v;
    term.focus();
  };
  window.__julyFocus = function() {
    term.focus();
  };
  window.__julySetFont = function(sz) {
    term.options.fontSize = sz;
    safeFit();
  };
})();
</script>
</body>
</html>`;
  }, [wsBase, session, token]);

  const sendSeq = (seq: string) => {
    const payload = JSON.stringify(seq);
    webRef.current?.injectJavaScript(`window.__julySend && window.__julySend(${payload}); true;`);
  };

  const applyFontSize = (nextSize: number) => {
    const clamped = Math.max(7, Math.min(18, Math.round(nextSize * 2) / 2));
    setFontSize(clamped);
    webRef.current?.injectJavaScript(`window.__julySetFont && window.__julySetFont(${clamped}); true;`);
  };

  const toggleCtrl = () => {
    const next = !ctrlArmed;
    setCtrlArmed(next);
    webRef.current?.injectJavaScript(`window.__julySetCtrl && window.__julySetCtrl(${next}); true;`);
  };

  const handleMessage = (ev: WebViewMessageEvent) => {
    try {
      const msg = JSON.parse(ev.nativeEvent.data) as {
        type: string;
        value?: string | boolean;
        cols?: number;
        rows?: number;
      };
      if (msg.type === 'status' && typeof msg.value === 'string') {
        if (
          msg.value === 'connecting' ||
          msg.value === 'live' ||
          msg.value === 'closed' ||
          msg.value === 'error'
        ) {
          setStatus(msg.value);
        }
      } else if (msg.type === 'dims' && msg.cols && msg.rows) {
        setDims(`${msg.cols}×${msg.rows}`);
      } else if (msg.type === 'ctrl' && typeof msg.value === 'boolean') {
        setCtrlArmed(msg.value);
      }
    } catch {
      // ignore malformed bridge message
    }
  };

  const renderHelpersDrawer = () => {
    if (!showHelpers) return null;
    return (
      <View style={styles.drawerBox}>
        <View style={styles.keyRow}>
          {HELPER_ROW_1.map((item) => {
            const active = Boolean(item.isCtrl && ctrlArmed);
            return (
              <Pressable
                key={item.label}
                onPress={() => {
                  if (item.isCtrl) toggleCtrl();
                  else if (item.seq) sendSeq(item.seq);
                }}
                style={[styles.keyBtn, active && styles.keyBtnActive]}>
                <Text
                  numberOfLines={1}
                  style={[styles.keyBtnText, active && styles.keyBtnTextActive]}>
                  {item.label}
                </Text>
              </Pressable>
            );
          })}
        </View>
        <View style={styles.keyRow}>
          {HELPER_ROW_2.map((item) => (
            <Pressable
              key={item.label}
              onPress={() => {
                if (item.isSync) setNonce((n) => n + 1);
                else if (item.seq) sendSeq(item.seq);
              }}
              style={styles.keyBtn}>
              <Text numberOfLines={1} style={styles.keyBtnText}>
                {item.label}
              </Text>
            </Pressable>
          ))}
        </View>
        <View style={styles.keyRow}>
          <Pressable onPress={() => applyFontSize(fontSize - 1)} style={styles.keyBtn}>
            <Text numberOfLines={1} style={styles.keyBtnText}>
              A- (MORE COLS)
            </Text>
          </Pressable>
          <Pressable onPress={() => applyFontSize(11.5)} style={styles.keyBtn}>
            <Text numberOfLines={1} style={styles.keyBtnText}>
              {fontSize}px{dims ? ` · ${dims}` : ''}
            </Text>
          </Pressable>
          <Pressable onPress={() => applyFontSize(fontSize + 1)} style={styles.keyBtn}>
            <Text numberOfLines={1} style={styles.keyBtnText}>
              A+
            </Text>
          </Pressable>
        </View>
      </View>
    );
  };

  const renderWebView = (modeTag: string) => (
    <NativeWebView
      key={`${wsBase}::${session}::${modeTag}::${nonce}`}
      ref={webRef}
      originWhitelist={['*']}
      source={{ html }}
      onMessage={handleMessage}
      onLoadEnd={() =>
        webRef.current?.injectJavaScript(`window.__julySetFont && window.__julySetFont(${fontSize}); true;`)
      }
      javaScriptEnabled
      domStorageEnabled
      bounces={false}
      scrollEnabled={false}
      keyboardDisplayRequiresUserAction={false}
      hideKeyboardAccessoryView={false}
      style={styles.webview}
    />
  );

  if (Platform.OS === 'web') {
    return (
      <View style={styles.webFallback}>
        <Text style={styles.webFallbackText}>
          Live xterm PTY runs inside the native iOS/Android Expo Go app.
        </Text>
      </View>
    );
  }

  return (
    <>
      <View style={styles.container}>
        <View style={styles.statusStrip}>
          <View style={styles.statusLeft}>
            <View style={[styles.dot, status === 'live' ? styles.dotLive : styles.dotOff]} />
            <Text numberOfLines={1} style={styles.statusText}>
              {status === 'live'
                ? `LIVE${dims ? ` · ${dims}` : ''}`
                : status === 'connecting'
                  ? 'CONNECTING...'
                  : 'OFFLINE'}
            </Text>
          </View>
          <View style={styles.statusRight}>
            <Pressable onPress={() => sendSeq('\x03')} style={styles.miniAction}>
              <Text style={styles.miniActionText}>^C</Text>
            </Pressable>
            <Pressable
              onPress={() => setShowHelpers((v) => !v)}
              style={[styles.miniAction, showHelpers && styles.miniActionSolid]}>
              <Text style={[styles.miniActionText, showHelpers && styles.miniActionSolidText]}>
                KEYS {showHelpers ? '▲' : '▼'}
              </Text>
            </Pressable>
            <Pressable
              onPress={() =>
                webRef.current?.injectJavaScript('window.__julyFocus && window.__julyFocus(); true;')
              }
              style={styles.miniAction}>
              <Text style={styles.miniActionText}>KBD</Text>
            </Pressable>
            <Pressable
              onPress={() => {
                setCtrlArmed(false);
                setShowHelpers(false);
                setFullscreen(true);
              }}
              style={styles.miniActionSolid}>
              <Text style={styles.miniActionSolidText}>FULL</Text>
            </Pressable>
          </View>
        </View>

        {renderHelpersDrawer()}

        <View style={styles.webviewFrame}>
          {!fullscreen ? (
            renderWebView('inline')
          ) : (
            <View style={styles.placeholderBox}>
              <Text style={styles.placeholderText}>Active in fullscreen</Text>
            </View>
          )}
        </View>
      </View>

      <Modal
        visible={fullscreen}
        animationType="none"
        presentationStyle="fullScreen"
        onRequestClose={() => setFullscreen(false)}>
        <View
          style={[
            styles.fsSafe,
            { paddingTop: Math.max(insets.top, 12), paddingBottom: Math.max(insets.bottom, 6) },
          ]}>
          <KeyboardAvoidingView
            style={styles.fsInner}
            behavior={Platform.OS === 'ios' ? 'padding' : 'height'}>
            <View style={styles.fsHeader}>
              <View style={styles.statusLeft}>
                <View style={[styles.dot, status === 'live' ? styles.dotLive : styles.dotOff]} />
                <Text numberOfLines={1} style={styles.fsTitle}>
                  tmux:{session}
                  {dims ? ` · ${dims}` : ''}
                </Text>
              </View>
              <View style={styles.statusRight}>
                <Pressable onPress={() => sendSeq('\x03')} style={styles.miniAction}>
                  <Text style={styles.miniActionText}>^C</Text>
                </Pressable>
                <Pressable
                  onPress={() => setShowHelpers((v) => !v)}
                  style={[styles.miniAction, showHelpers && styles.miniActionSolid]}>
                  <Text style={[styles.miniActionText, showHelpers && styles.miniActionSolidText]}>
                    KEYS {showHelpers ? '▲' : '▼'}
                  </Text>
                </Pressable>
                <Pressable
                  onPress={() =>
                    webRef.current?.injectJavaScript(
                      'window.__julyFocus && window.__julyFocus(); true;'
                    )
                  }
                  style={styles.miniAction}>
                  <Text style={styles.miniActionText}>KBD</Text>
                </Pressable>
                <Pressable
                  onPress={() => {
                    setCtrlArmed(false);
                    setShowHelpers(false);
                    setFullscreen(false);
                  }}
                  style={styles.miniActionSolid}>
                  <Text style={styles.miniActionSolidText}>EXIT</Text>
                </Pressable>
              </View>
            </View>

            {showHelpers && <View style={styles.fsDrawerWrap}>{renderHelpersDrawer()}</View>}

            <View style={styles.fsWebviewFrame}>
              {fullscreen && renderWebView('fullscreen')}
            </View>
          </KeyboardAvoidingView>
        </View>
      </Modal>
    </>
  );
}

const styles = StyleSheet.create({
  container: { gap: 8 },
  statusStrip: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 8,
  },
  statusLeft: { flexDirection: 'row', alignItems: 'center', gap: 6, flex: 1, minWidth: 0 },
  dot: { width: 6, height: 6, borderRadius: 0 },
  dotLive: { backgroundColor: Palette.text },
  dotOff: { borderWidth: 1, borderColor: Palette.dim },
  statusText: {
    color: Palette.muted,
    fontFamily: Fonts.bold,
    fontSize: 10,
    letterSpacing: 1.2,
  },
  statusRight: { flexDirection: 'row', gap: 5 },
  miniAction: {
    borderWidth: 1,
    borderColor: Palette.border,
    backgroundColor: Palette.bg,
    borderRadius: 0,
    paddingHorizontal: 8,
    paddingVertical: 5,
  },
  miniActionText: {
    color: Palette.text,
    fontFamily: Fonts.bold,
    fontSize: 9,
    letterSpacing: 0.8,
  },
  miniActionSolid: {
    borderWidth: 1,
    borderColor: Palette.text,
    backgroundColor: Palette.text,
    borderRadius: 0,
    paddingHorizontal: 8,
    paddingVertical: 5,
  },
  miniActionSolidText: {
    color: Palette.inverseText,
    fontFamily: Fonts.bold,
    fontSize: 9,
    letterSpacing: 0.8,
  },
  drawerBox: {
    backgroundColor: Palette.bg,
    borderWidth: 1,
    borderColor: Palette.border,
    borderRadius: 0,
    padding: 6,
    gap: 6,
  },
  keyRow: {
    flexDirection: 'row',
    gap: 6,
  },
  keyBtn: {
    flex: 1,
    backgroundColor: Palette.card,
    borderWidth: 1,
    borderColor: Palette.border,
    borderRadius: 0,
    paddingVertical: 9,
    alignItems: 'center',
    justifyContent: 'center',
  },
  keyBtnActive: {
    backgroundColor: Palette.text,
    borderColor: Palette.text,
  },
  keyBtnText: {
    color: Palette.text,
    fontFamily: Fonts.bold,
    fontSize: 10,
    letterSpacing: 0.8,
  },
  keyBtnTextActive: {
    color: Palette.inverseText,
  },
  webviewFrame: {
    height: 450,
    backgroundColor: Palette.bg,
    borderWidth: 1,
    borderColor: Palette.border,
    borderRadius: 0,
    overflow: 'hidden',
  },
  webview: {
    flex: 1,
    backgroundColor: Palette.bg,
  },
  placeholderBox: {
    flex: 1,
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: Palette.bg,
  },
  placeholderText: {
    color: Palette.dim,
    fontFamily: Fonts.mono,
    fontSize: 11,
  },
  fsSafe: {
    flex: 1,
    backgroundColor: Palette.bg,
  },
  fsInner: {
    flex: 1,
    backgroundColor: Palette.bg,
  },
  fsHeader: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingHorizontal: 10,
    paddingVertical: 8,
    borderBottomWidth: 1,
    borderBottomColor: Palette.border,
    backgroundColor: Palette.card,
    gap: 8,
  },
  fsTitle: {
    color: Palette.text,
    fontFamily: Fonts.bold,
    fontSize: 11,
    letterSpacing: 1,
    flexShrink: 1,
  },
  fsDrawerWrap: {
    paddingHorizontal: 8,
    paddingVertical: 6,
    borderBottomWidth: 1,
    borderBottomColor: Palette.border,
    backgroundColor: Palette.card,
  },
  fsWebviewFrame: {
    flex: 1,
    backgroundColor: Palette.bg,
  },
  webFallback: {
    height: 180,
    borderWidth: 1,
    borderColor: Palette.border,
    backgroundColor: Palette.bg,
    alignItems: 'center',
    justifyContent: 'center',
    padding: 16,
  },
  webFallbackText: {
    color: Palette.muted,
    fontFamily: Fonts.sans,
    fontSize: 12,
    textAlign: 'center',
  },
});
