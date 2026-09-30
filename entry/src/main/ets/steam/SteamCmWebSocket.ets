import { webSocket, http } from '@kit.NetworkKit';
import { hilog } from '../service/LogService';
import {
  ISteamCmWireTransport,
  SteamCmWireCloseListener,
  SteamCmWireErrorListener,
  SteamCmWireMessageListener
} from './vendor/contracts/SteamCmContracts';

const DOMAIN = 0x0000;
const TAG = 'SteamCmWs';

// NetStack accepts this TLS option at runtime, although the WebSocket SDK
// declaration has not caught up with the HTTP TlsVersion enum yet.
interface SteamWebSocketRequestOptions extends webSocket.WebSocketRequestOptions {
  minSupportTlsProtocol: number;
}

/**
 * 宿主侧 CM 二进制 WebSocket 传输（@ohos.net.webSocket 实现 ISteamCmWireTransport）。
 * wss://host:port/cmsocket/ ；收发均为完整二进制帧，适配层负责拷贝。
 */
export class SteamCmWebSocketTransport implements ISteamCmWireTransport {
  private socket: webSocket.WebSocket = webSocket.createWebSocket();
  private messageListeners: Array<SteamCmWireMessageListener> = [];
  private closeListeners: Array<SteamCmWireCloseListener> = [];
  private errorListeners: Array<SteamCmWireErrorListener> = [];
  private closedByUser: boolean = false;
  private sentFrames: number = 0;
  private receivedFrames: number = 0;
  private pendingOpen: (() => void) | null = null;
  private pendingOpenError: ((error: Error) => void) | null = null;

  connect(server: string): Promise<void> {
    const endpoint = server.replace(/^wss:\/\//, '').replace(/\/+$/, '');
    const url = 'wss://' + endpoint + '/cmsocket/';
    this.closedByUser = false;
    this.sentFrames = 0;
    this.receivedFrames = 0;
    // 新 socket + 清旧监听: 复用 socket 时旧回调会重复触发
    try {
      this.socket.off('open');
      this.socket.off('message');
      this.socket.off('close');
      this.socket.off('error');
    } catch (_) {
    }
    this.socket = webSocket.createWebSocket();
    this.bindListeners();
    // minSupportTlsProtocol 必传: 缺省时 NETSTACK 报 "MIN_SUPPORT_TLS_PROTOCOL not found",
    // 握手 30s 后超时 (wsi is nullptr → lws_service stop), 实测 CM 会话永远建不起来。
    const options: SteamWebSocketRequestOptions = {
      header: { 'User-Agent': 'VintagePomeloPro SteamCm' } as Record<string, string>,
      minSupportTlsProtocol: http.TlsVersion.TLS_V_1_2,
      pingInterval: 30,
      pongTimeout: 30
    };
    hilog.info(DOMAIN, TAG, 'cm connect begin: %{public}s', url);
    return new Promise<void>((resolve: () => void, reject: (e: Error) => void): void => {
      let settled = false;
      const finish = (error: Error | null): void => {
        if (settled) return;
        settled = true;
        clearTimeout(guard);
        this.pendingOpen = null;
        this.pendingOpenError = null;
        if (error) reject(error); else resolve();
      };
      // connect() 只表示请求已提交；必须等 open 事件确认握手完成才能发送 CM 帧。
      const guard = setTimeout((): void => {
        if (settled) return;
        hilog.error(DOMAIN, TAG, 'cm connect guard timeout (45s): %{public}s', url);
        finish(new Error('CM 连接超时 (45s 未收到 WebSocket open 事件)'));
        try {
          this.socket.close();
        } catch (_) {
        }
      }, 45000);
      this.pendingOpen = (): void => {
        hilog.info(DOMAIN, TAG, 'cm connected: %{public}s', url);
        finish(null);
      };
      this.pendingOpenError = (error: Error): void => finish(error);
      this.socket.connect(url, options).then((value: boolean): void => {
        if (settled) return;
        if (!value) {
          hilog.warn(DOMAIN, TAG, 'cm connect refused (ret=false): %{public}s', url);
          finish(new Error('CM 连接被拒 (WebSocket 请求未被接受)'));
        }
      }).catch((e: Object): void => {
        if (settled) return;
        const reason = e instanceof Error ? e.message : 'network error';
        hilog.warn(DOMAIN, TAG, 'cm connect failed: %{public}s', reason);
        finish(new Error('CM connect failed: ' + reason));
      });
    });
  }

  send(payload: Uint8Array): Promise<void> {
    if (this.sentFrames++ < 8) {
      hilog.info(DOMAIN, TAG, 'cm send: bytes=%{public}d', payload.length);
    }
    const copy = payload.slice();
    const data = copy.buffer.slice(copy.byteOffset, copy.byteOffset + copy.byteLength) as ArrayBuffer;
    return new Promise<void>((resolve: () => void, reject: (e: Error) => void): void => {
      this.socket.send(data).then((sent: boolean): void => {
        if (!sent) {
          hilog.warn(DOMAIN, TAG, 'cm send returned false: bytes=%{public}d', payload.length);
          reject(new Error('CM send was refused by WebSocket'));
          return;
        }
        resolve();
      }).catch((e: Object): void => {
        reject(new Error('CM send failed: ' + JSON.stringify(e)));
      });
    });
  }

  close(): Promise<void> {
    this.closedByUser = true;
    return new Promise<void>((resolve: () => void): void => {
      this.socket.close().then((): void => {
        resolve();
      }).catch((_e: Object): void => {
        resolve();
      });
    });
  }

  onMessage(listener: SteamCmWireMessageListener): void {
    this.messageListeners.push(listener);
  }

  onClose(listener: SteamCmWireCloseListener): void {
    this.closeListeners.push(listener);
  }

  onError(listener: SteamCmWireErrorListener): void {
    this.errorListeners.push(listener);
  }

  clearListeners(): void {
    this.messageListeners = [];
    this.closeListeners = [];
    this.errorListeners = [];
  }

  private bindListeners(): void {
    this.socket.on('open', (err: Error, _value: Object): void => {
      hilog.info(DOMAIN, TAG, 'cm open event');
      if (err) {
        if (this.pendingOpenError) this.pendingOpenError(err);
      } else if (this.pendingOpen) {
        this.pendingOpen();
      }
    });
    this.socket.on('message', (_err: Error, value: string | ArrayBuffer): void => {
      if (typeof value === 'string') {
        hilog.warn(DOMAIN, TAG, 'cm text frame ignored: chars=%{public}d', value.length);
        return;
      }
      if (this.receivedFrames++ < 8) {
        const bytes = new Uint8Array(value);
        const rawType = bytes.length >= 4
          ? (bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24)) >>> 0 : 0;
        hilog.info(DOMAIN, TAG, 'cm receive: bytes=%{public}d, type=%{public}d',
          value.byteLength, rawType & 0x7fffffff);
      }
      const copy = new Uint8Array(value.slice(0));
      this.messageListeners.forEach((l: SteamCmWireMessageListener): void => l(copy));
    });
    this.socket.on('close', (_err: Error, value: webSocket.CloseResult): void => {
      hilog.info(DOMAIN, TAG, 'cm closed: code=%{public}d, byUser=%{public}s',
        value.code, String(this.closedByUser));
      this.closeListeners.forEach((l: SteamCmWireCloseListener): void =>
        l('code=' + value.code + ',user=' + String(this.closedByUser)));
      if (this.pendingOpenError) this.pendingOpenError(new Error('CM socket closed before open'));
    });
    this.socket.on('error', (err: Error): void => {
      hilog.warn(DOMAIN, TAG, 'cm error: %{public}s', JSON.stringify(err));
      const error = new Error('CM socket error: ' + JSON.stringify(err));
      if (this.pendingOpenError) this.pendingOpenError(error);
      this.errorListeners.forEach((l: SteamCmWireErrorListener): void => l(error));
    });
  }
}
