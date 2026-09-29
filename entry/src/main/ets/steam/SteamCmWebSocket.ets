import { webSocket } from '@kit.NetworkKit';
import { hilog } from '../service/LogService';
import {
  ISteamCmWireTransport,
  SteamCmWireCloseListener,
  SteamCmWireErrorListener,
  SteamCmWireMessageListener
} from './vendor/contracts/SteamCmContracts';

const DOMAIN = 0x0000;
const TAG = 'SteamCmWs';

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
    this.socket = webSocket.createWebSocket();
    this.bindListeners();
    const options: webSocket.WebSocketRequestOptions = {
      header: { 'User-Agent': 'VintagePomeloPro SteamCm' } as Record<string, string>
    };
    return new Promise<void>((resolve: () => void, reject: (e: Error) => void): void => {
      this.socket.connect(url, options).then((value: boolean): void => {
        hilog.info(DOMAIN, TAG, 'cm connected: %{public}s (ret=%{public}s)', url, String(value));
        resolve();
      }).catch((e: Object): void => {
        hilog.warn(DOMAIN, TAG, 'cm connect failed: %{public}s', JSON.stringify(e));
        reject(new Error('CM connect failed: ' + JSON.stringify(e)));
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
