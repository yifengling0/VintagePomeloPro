import { http } from '@kit.NetworkKit';
import { ISteamAuthTransport } from './vendor/contracts/SteamPlatformContracts';
import { SteamAuthEndpoint, SteamAuthHttpResponse, SteamFormField } from './vendor/auth/SteamAuthProtocol';

/** IAuthenticationService 表单 POST 传输（@ohos.net.http 实现）。 */
export class SteamHttpTransport implements ISteamAuthTransport {
  async postForm(endpoint: SteamAuthEndpoint, fields: Array<SteamFormField>): Promise<SteamAuthHttpResponse> {
    const request = http.createHttp();
    try {
      const parts: Array<string> = [];
      for (const field of fields) {
        parts.push(encodeURIComponent(field.name) + '=' + encodeURIComponent(field.value));
      }
      const response = await request.request(endpoint, {
        method: http.RequestMethod.POST,
        header: {
          'Content-Type': 'application/x-www-form-urlencoded',
          'Accept': 'application/json'
        },
        extraData: parts.join('&'),
        connectTimeout: 15000,
        readTimeout: 20000
      });
      return new SteamAuthHttpResponse(response.responseCode, String(response.result));
    } finally {
      request.destroy();
    }
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
    } catch (_) {
      return '';
    } finally {
      request.destroy();
    }
  }
}
