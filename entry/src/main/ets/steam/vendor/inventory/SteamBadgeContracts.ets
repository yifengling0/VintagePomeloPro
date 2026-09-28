import { SteamCommunityBadge, SteamFarmSnapshot, SteamGameDrop } from '../models/SteamModels';
import { SteamBadgePageParseResult } from './SteamBadgePageParser';

/** Read-only diagnostics produced while synchronizing all badge pages. */
export class SteamBadgeSyncReport {
  readonly games: Array<SteamGameDrop>;
  readonly communityBadges: Array<SteamCommunityBadge>;
  readonly requestedPageCount: number;
  readonly fetchedPageCount: number;
  readonly duplicateGameCount: number;
  readonly pages: Array<SteamBadgePageParseResult>;
  readonly warnings: Array<string>;

  constructor(
    games: Array<SteamGameDrop> = [],
    requestedPageCount: number = 0,
    fetchedPageCount: number = 0,
    duplicateGameCount: number = 0,
    pages: Array<SteamBadgePageParseResult> = [],
    warnings: Array<string> = [],
    communityBadges: Array<SteamCommunityBadge> = []
  ) {
    this.games = games.slice();
    this.communityBadges = communityBadges.slice();
    this.requestedPageCount = requestedPageCount;
    this.fetchedPageCount = fetchedPageCount;
    this.duplicateGameCount = duplicateGameCount;
    this.pages = pages.slice();
    this.warnings = warnings.slice();
  }
}

export type SteamBadgeSyncReportListener = (
  report: SteamBadgeSyncReport,
  snapshot: SteamFarmSnapshot
) => void;
