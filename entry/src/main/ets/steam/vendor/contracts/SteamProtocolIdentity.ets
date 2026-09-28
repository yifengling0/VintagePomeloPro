/**
 * Platform-neutral product protocol identity. Hosts may use these values for
 * Steam device labels and HTTP headers without importing UI or platform APIs.
 */
export class SteamProtocolIdentity {
  static readonly PRODUCT_NAME: string = 'ASF Workshop';
  static readonly PRODUCT_VERSION: string = '1.0';
  static readonly USER_AGENT_PREFIX: string = SteamProtocolIdentity.PRODUCT_NAME + '/' +
    SteamProtocolIdentity.PRODUCT_VERSION;

  static userAgent(component: string = ''): string {
    const normalizedComponent: string = component.trim();
    return normalizedComponent.length > 0
      ? SteamProtocolIdentity.USER_AGENT_PREFIX + ' ' + normalizedComponent
      : SteamProtocolIdentity.USER_AGENT_PREFIX;
  }
}
