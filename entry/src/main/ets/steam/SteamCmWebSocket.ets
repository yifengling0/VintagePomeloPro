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

  connect(server: string): Promise<void> {
    const endpoint = server.replace(/^wss:\/\//, '').replace(/\/+$/, '');
    const url = 'wss://' + endpoint + '/cmsocket/';
    this.closedByUser = false;
    // 新 socket + 清旧监听: 复用 socket 时旧回调会重复触发
    try {
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
      // 兜底超时: @ohos wss 在某些网络下 connect promise 永不 settle (只有 lws stop 日志),
      // 45s 后强制 reject, 让上层走重连/报错而不是永远挂起
      const guard = setTimeout((): void => {
        if (settled) return;
        settled = true;
        hilog.error(DOMAIN, TAG, 'cm connect guard timeout (45s): %{public}s', url);
        try {
          this.socket.close();
        } catch (_) {
        }
        reject(new Error('CM 连接超时 (45s 无响应, TLS 握手未完成; 网络代理可能拦截 wss 长连接)'));
      }, 45000);
      this.socket.connect(url, options).then((value: boolean): void => {
        if (settled) return;
        settled = true;
        clearTimeout(guard);
        if (!value) {
          // resolve(false) = 握手失败 (非异常路径); 之前把它当成功导致会话状态假 Connected
          hilog.warn(DOMAIN, TAG, 'cm connect refused (ret=false): %{public}s', url);
          reject(new Error('CM 连接被拒 (WebSocket 握手未完成; 检查代理对 wss 长连接的放行)'));
          return;
        }
        hilog.info(DOMAIN, TAG, 'cm connected: %{public}s', url);
        resolve();
      }).catch((e: Object): void => {
        if (settled) return;
        settled = true;
        clearTimeout(guard);
        const reason = e instanceof Error ? e.message : 'network error';
        hilog.warn(DOMAIN, TAG, 'cm connect failed: %{public}s', reason);
        reject(new Error('CM connect failed: ' + reason));
      });
    });
  }

  send(payload: Uint8Array): Promise<void> {
    const copy = payload.slice();
    const data = copy.buffer.slice(copy.byteOffset, copy.byteOffset + copy.byteLength) as ArrayBuffer;
    return new Promise<void>((resolve: () => void, reject: (e: Error) => void): void => {
      this.socket.send(data).then((): void => {
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
    this.socket.on('message', (_err: Error, value: string | ArrayBuffer): void => {
      if (typeof value === 'string') {
        // CM 只用二进制帧; 文本帧忽略
        return;
      }
      const copy = new Uint8Array(value.slice(0));
      this.messageListeners.forEach((l: SteamCmWireMessageListener): void => l(copy));
    });
    this.socket.on('close', (_err: Error, value: webSocket.CloseResult): void => {
      hilog.info(DOMAIN, TAG, 'cm closed: code=%{public}d, byUser=%{public}s',
        value.code, String(this.closedByUser));
      this.closeListeners.forEach((l: SteamCmWireCloseListener): void =>
        l('code=' + value.code + ',user=' + String(this.closedByUser)));
    });
    this.socket.on('error', (err: Error): void => {
      hilog.warn(DOMAIN, TAG, 'cm error: %{public}s', JSON.stringify(err));
      const error = new Error('CM socket error: ' + JSON.stringify(err));
      this.errorListeners.forEach((l: SteamCmWireErrorListener): void => l(error));
    });
  }
}
