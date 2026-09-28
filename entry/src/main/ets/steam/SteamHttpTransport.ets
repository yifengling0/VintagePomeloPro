import { http } from '@kit.NetworkKit';
import { ISteamAuthTransport } from './vendor/contracts/SteamPlatformContracts';
import { SteamAuthEndpoint, SteamAuthHttpResponse, SteamFormField } from './vendor/auth/SteamAuthProtocol';
import { SteamAuthErrorKind, SteamAuthErrorSupport } from './vendor/contracts/SteamAuthErrors';

/** IAuthenticationService 表单 POST 传输（@ohos.net.http 实现）。 */
export class SteamHttpTransport implements ISteamAuthTransport {
  async postForm(endpoint: SteamAuthEndpoint, fields: Array<SteamFormField>): Promise<SteamAuthHttpResponse> {
    const parts: Array<string> = [];
    for (const field of fields) {
      parts.push(encodeURIComponent(field.name) + '=' + encodeURIComponent(field.value));
    }
    const form = parts.join('&');
    const request = http.createHttp();
    try {
      // GetPasswordRSAPublicKey is GET-only; a rejected POST can terminate
      // before its HTTP 405 response reaches the device.
      if (endpoint === SteamAuthEndpoint.GET_PASSWORD_RSA_PUBLIC_KEY) {
        const response = await request.request(endpoint + '?' + form, {
          method: http.RequestMethod.GET,
          header: { 'Accept': 'application/json' },
          expectDataType: http.HttpDataType.STRING,
          connectTimeout: 15000,
          readTimeout: 20000
        });
        return new SteamAuthHttpResponse(response.responseCode, String(response.result),
          SteamHttpTransport.lowercaseHeaders(response.header));
      }
      const response = await request.request(endpoint, {
        method: http.RequestMethod.POST,
        header: {
          'Content-Type': 'application/x-www-form-urlencoded',
          'Accept': 'application/json'
        },
        extraData: form,
        expectDataType: http.HttpDataType.STRING,
        connectTimeout: 15000,
        readTimeout: 20000
      });
      return new SteamAuthHttpResponse(response.responseCode, String(response.result),
        SteamHttpTransport.lowercaseHeaders(response.header));
    } finally {
      request.destroy();
    }
  }

  /** @ohos 响应头 → 键小写的 Map（vendor keystoneError 查询用）。 */
  private static lowercaseHeaders(raw: Object): Map<string, string> {
    const headers = new Map<string, string>();
    try {
      const record = raw as Record<string, string>;
      for (const key of Object.keys(record)) {
        headers.set(key.toLowerCase(), String(record[key]));
      }
    } catch (_) {
    }
    return headers;
  }

  /** 免鉴权连通性自检：ISteamWebAPIUtil/GetServerInfo（登录页自检用）。 */
  static async probeServerInfo(): Promise<string> {
    const request = http.createHttp();
    try {
      const response = await request.request(
        'https://api.steampowered.com/ISteamWebAPIUtil/GetServerInfo/v1/?format=json', {
        method: http.RequestMethod.GET,
        connectTimeout: 10000,
        readTimeout: 10000
      });
      return response.responseCode === 200 ? String(response.result) : '';
    } catch (e) {
      const failure = SteamAuthErrorSupport.normalize(e, SteamAuthErrorKind.TRANSPORT,
        'server_info', 'Steam network probe failed');
      return 'ERR:' + SteamAuthErrorSupport.diagnosticLabel(failure);
    } finally {
      request.destroy();
    }
  }
}
