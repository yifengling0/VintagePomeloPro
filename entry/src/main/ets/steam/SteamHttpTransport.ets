import { http } from '@kit.NetworkKit';
import { ISteamAuthTransport } from './vendor/contracts/SteamPlatformContracts';
import { SteamAuthEndpoint, SteamAuthHttpResponse, SteamFormField } from './vendor/auth/SteamAuthProtocol';

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
      const response = await request.request(endpoint, {
        method: http.RequestMethod.POST,
        header: {
          'Content-Type': 'application/x-www-form-urlencoded',
          'Accept': 'application/json'
        },
        extraData: form,
        connectTimeout: 15000,
        readTimeout: 20000
      });
      if (response.responseCode === 405) {
        // 2026-09-28 实测: GetPasswordRSAPublicKey 等 GET-only 端点 POST 返回 405
        // (HTML 页 "This API must be called with a HTTP GET request"), 表单改挂 query 用 GET 重发。
        const getter = http.createHttp();
        try {
          const getUrl = endpoint + (endpoint.indexOf('?') >= 0 ? '&' : '?') + form;
          const getResponse = await getter.request(getUrl, {
            method: http.RequestMethod.GET,
            header: { 'Accept': 'application/json' },
            connectTimeout: 15000,
            readTimeout: 20000
          });
          return new SteamAuthHttpResponse(getResponse.responseCode, String(getResponse.result),
            SteamHttpTransport.lowercaseHeaders(getResponse.header));
        } finally {
          getter.destroy();
        }
      }
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
      // 自检诊断: 把异常透传给调用方 (ERR: 前缀), 否则 DNS/SSL/超时全被吞掉无法定位
      return 'ERR:' + JSON.stringify(e);
    } finally {
      request.destroy();
    }
  }
}
