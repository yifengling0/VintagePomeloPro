import { intl } from '@kit.LocalizationKit';

export interface MembershipCampaign {
  productId: string;
  offerId: string;
  currency: string;
  originalMicroPrice: number;
  microPrice: number;
  startsAt?: string;
  endsAt: string;
}

interface CampaignFile {
  schema: number;
  campaigns: MembershipCampaign[];
}

export class MembershipCampaigns {
  static readonly maxBytes: number = 32768;
  private static instant(value: string): number {
    if (typeof value !== 'string' ||
      !/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(Z|[+-](0\d|1[0-4]):[0-5]\d)$/.test(value)) {
      return NaN;
    }
    const stamp = Date.parse(value);
    const offset = value.endsWith('Z') ? 0 :
      (Number(value.slice(-5, -3)) * 60 + Number(value.slice(-2))) * (value.slice(-6, -5) === '-' ? -1 : 1);
    if (Math.abs(offset) > 840 || !Number.isFinite(stamp) ||
      new Date(stamp + offset * 60000).toISOString().slice(0, 19) !== value.slice(0, 19)) {
      return NaN;
    }
    return stamp;
  }
  static parse(text: string): MembershipCampaign[] {
    if (text.length > MembershipCampaigns.maxBytes) throw new Error('会员活动配置过大');
    const file = JSON.parse(text) as CampaignFile;
    if (file?.schema !== 1 || !Array.isArray(file.campaigns) || file.campaigns.length > 32) {
      throw new Error('会员活动配置无效');
    }
    for (const campaign of file.campaigns) {
      if (typeof campaign?.productId !== 'string' || !campaign.productId ||
        typeof campaign.offerId !== 'string' || !/^[a-zA-Z0-9._-]{1,64}$/.test(campaign.offerId) ||
        typeof campaign.currency !== 'string' || !/^[A-Z]{3}$/.test(campaign.currency) ||
        !Number.isSafeInteger(campaign.originalMicroPrice) || campaign.originalMicroPrice <= 0 ||
        !Number.isSafeInteger(campaign.microPrice) || campaign.microPrice <= 0 ||
        campaign.microPrice >= campaign.originalMicroPrice ||
        !Number.isFinite(MembershipCampaigns.instant(campaign.endsAt)) ||
        (campaign.startsAt !== undefined && (!Number.isFinite(MembershipCampaigns.instant(campaign.startsAt)) ||
          MembershipCampaigns.instant(campaign.startsAt) >= MembershipCampaigns.instant(campaign.endsAt)))) {
        throw new Error('会员活动金额或时间无效');
      }
    }
    for (let i = 0; i < file.campaigns.length; i++) {
      for (let j = i + 1; j < file.campaigns.length; j++) {
        const a = file.campaigns[i], b = file.campaigns[j];
        if (a.productId === b.productId && a.currency === b.currency &&
          Math.max(a.startsAt ? MembershipCampaigns.instant(a.startsAt) : 0,
            b.startsAt ? MembershipCampaigns.instant(b.startsAt) : 0) <
          Math.min(MembershipCampaigns.instant(a.endsAt), MembershipCampaigns.instant(b.endsAt))) {
          throw new Error('会员活动时间重叠');
        }
      }
    }
    return file.campaigns;
  }
  static active(campaigns: MembershipCampaign[], productId: string, currency: string,
    now: number): MembershipCampaign | undefined {
    return campaigns.find((campaign: MembershipCampaign): boolean => campaign.productId === productId &&
      campaign.currency === currency && (!campaign.startsAt || now >= MembershipCampaigns.instant(campaign.startsAt)) &&
      now < MembershipCampaigns.instant(campaign.endsAt));
  }
  static money(micro: number, currency: string): string {
    return new intl.NumberFormat('zh-CN', { style: 'currency', currency: currency }).format(micro / 1000000);
  }
  static deadline(campaign: MembershipCampaign): string {
    const zone = campaign.endsAt.endsWith('Z') ? 'Z' : campaign.endsAt.slice(-6);
    const offset = zone === 'Z' ? 0 :
      (Number(zone.slice(1, 3)) * 60 + Number(zone.slice(4))) * (zone.startsWith('-') ? -1 : 1);
    const last = new Date(MembershipCampaigns.instant(campaign.endsAt) - 1000 + offset * 60000)
      .toISOString().slice(0, 16).replace('T', ' ');
    const label = zone === '+08:00' ? '北京时间' : (zone === 'Z' ? 'UTC+00:00' : 'UTC' + zone);
    return '限时优惠至 ' + last + '（' + label + '）';
  }
}
