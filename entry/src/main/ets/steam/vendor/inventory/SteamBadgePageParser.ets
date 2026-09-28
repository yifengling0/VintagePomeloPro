import { SteamCommunityBadge, SteamGameDrop } from '../models/SteamModels';
import { ISteamCoreLogger, SteamCoreNoopLogger } from '../contracts/SteamCoreLogger';

const TAG: string = 'BadgePageParser';

export enum SteamBadgePageWarningCode {
  EMPTY_PAGE = 'empty_page',
  MISSING_BADGE_ROWS = 'missing_badge_rows',
  MISSING_STATS_BLOCK = 'missing_stats_block',
  MISSING_APP_ID = 'missing_app_id',
  INVALID_APP_ID = 'invalid_app_id',
  MISSING_GAME_NAME = 'missing_game_name',
  MISSING_CARD_DROP_COUNT = 'missing_card_drop_count',
  INVALID_CARD_DROP_COUNT = 'invalid_card_drop_count',
  INVALID_PLAYTIME = 'invalid_playtime',
  INVALID_PAGE_LINK = 'invalid_page_link'
}

export class SteamBadgePageWarning {
  readonly code: SteamBadgePageWarningCode;
  readonly pageNumber: number;
  readonly rowIndex: number;
  readonly message: string;

  constructor(
    code: SteamBadgePageWarningCode,
    pageNumber: number,
    rowIndex: number,
    message: string
  ) {
    this.code = code;
    this.pageNumber = pageNumber;
    this.rowIndex = rowIndex;
    this.message = message;
  }
}

export class SteamBadgePageParseResult {
  readonly games: Array<SteamGameDrop>;
  readonly communityBadges: Array<SteamCommunityBadge>;
  readonly pageCount: number;
  readonly rowCount: number;
  readonly warnings: Array<string>;
  readonly issues: Array<SteamBadgePageWarning>;
  readonly pageNumber: number;

  constructor(
    games: Array<SteamGameDrop>,
    pageCount: number,
    rowCount: number,
    warnings: Array<string> = [],
    pageNumber: number = 1,
    issues: Array<SteamBadgePageWarning> = [],
    communityBadges: Array<SteamCommunityBadge> = []
  ) {
    this.games = games.slice();
    this.communityBadges = communityBadges.slice();
    this.pageCount = pageCount;
    this.rowCount = rowCount;
    this.warnings = warnings.slice();
    this.pageNumber = pageNumber;
    this.issues = issues.slice();
  }
}

class SteamBadgeIntegerResult {
  readonly found: boolean;
  readonly value: number;

  constructor(found: boolean, value: number) {
    this.found = found;
    this.value = value;
  }
}

class SteamBadgeDecimalResult {
  readonly found: boolean;
  readonly value: number;

  constructor(found: boolean, value: number) {
    this.found = found;
    this.value = value;
  }
}

class SteamBadgeRowParseResult {
  readonly game: SteamGameDrop | null;
  readonly communityBadge: SteamCommunityBadge | null;
  readonly issue: SteamBadgePageWarning | null;

  constructor(
    game: SteamGameDrop | null,
    communityBadge: SteamCommunityBadge | null,
    issue: SteamBadgePageWarning | null
  ) {
    this.game = game;
    this.communityBadge = communityBadge;
    this.issue = issue;
  }
}

/**
 * Extracts stable game-card fields from Steam's badge page. This parser
 * deliberately accepts only the read-only page shape used by the core.
 */
export class SteamBadgePageParser {
  static parse(
    html: string,
    pageNumber: number = 1,
    logger: ISteamCoreLogger = new SteamCoreNoopLogger()
  ): SteamBadgePageParseResult {
    const normalizedPageNumber = pageNumber > 0 ? Math.floor(pageNumber) : 1;
    logger.debug(TAG, 'badge page parse started; page=' + normalizedPageNumber.toString() +
      ', htmlBytes=' + html.length.toString());
    // The game id and card-drop link live on the outer badge_row. Parsing the
    // inner content div loses both attributes on current Steam markup.
    let rows: Array<string> = SteamBadgePageParser.findElementBlocks(html, 'div', 'badge_row');
    if (rows.length === 0) {
      rows = SteamBadgePageParser.findElementBlocks(html, 'div', 'badge_row_inner');
    }
    const games: Array<SteamGameDrop> = [];
    const communityBadges: Array<SteamCommunityBadge> = [];
    const warnings: Array<string> = [];
    const issues: Array<SteamBadgePageWarning> = [];
    if (html.trim().length === 0) {
      SteamBadgePageParser.addIssue(
        issues,
        warnings,
        SteamBadgePageWarningCode.EMPTY_PAGE,
        normalizedPageNumber,
        -1,
        'badge page response is empty'
      );
    } else if (rows.length === 0) {
      SteamBadgePageParser.addIssue(
        issues,
        warnings,
        SteamBadgePageWarningCode.MISSING_BADGE_ROWS,
        normalizedPageNumber,
        -1,
        'badge page has no badge_row_inner rows'
      );
    }
    for (let index = 0; index < rows.length; index++) {
      const parsedRow = SteamBadgePageParser.parseRow(rows[index], normalizedPageNumber, index, logger);
      if (parsedRow.game !== null) {
        games.push(parsedRow.game);
        if (parsedRow.issue !== null) {
          issues.push(parsedRow.issue);
          warnings.push(parsedRow.issue.message);
        }
      } else if (parsedRow.communityBadge !== null) {
        communityBadges.push(parsedRow.communityBadge);
        if (parsedRow.issue !== null) {
          issues.push(parsedRow.issue);
          warnings.push(parsedRow.issue.message);
        }
      } else {
        if (parsedRow.issue !== null) {
          issues.push(parsedRow.issue);
          warnings.push(parsedRow.issue.message);
        }
      }
    }
    const pageCount = SteamBadgePageParser.parsePageCount(html, normalizedPageNumber, issues, warnings);
    const result = new SteamBadgePageParseResult(
      games,
      pageCount,
      rows.length,
      warnings,
      normalizedPageNumber,
      issues,
      communityBadges
    );
    logger.info(TAG, 'badge page parse completed; page=' + normalizedPageNumber.toString() +
      ', rows=' + rows.length.toString() + ', games=' + games.length.toString() +
      ', warnings=' + warnings.length.toString());
    return result;
  }

  private static parseRow(row: string, pageNumber: number, rowIndex: number,
    logger: ISteamCoreLogger = new SteamCoreNoopLogger()): SteamBadgeRowParseResult {
    const rowId = SteamBadgePageParser.readFirstElementAttribute(row, 'div', 'id');
    const appIdFromRow = SteamBadgePageParser.extractGameBadgeAppId(rowId);
    const communityBadgeId = SteamBadgePageParser.extractCommunityBadgeId(rowId);
    const badgeCrafted: boolean = appIdFromRow.length > 0;
    const appIdMarker = SteamBadgePageParser.readElementAttribute(row, 'div', 'card_drop_info_dialog', 'id');
    const appIdFromMarker = SteamBadgePageParser.extractTrailingPositiveInteger(appIdMarker);
    // A row identified as a community badge by its own ID must never be
    // reclassified as a game badge through a stray /gamecards/ link inside it.
    const appId = appIdFromRow.length > 0
      ? appIdFromRow
      : (communityBadgeId.length > 0
        ? ''
        : (appIdFromMarker.length > 0 ? appIdFromMarker : SteamBadgePageParser.extractGameCardsAppId(row)));
    if (appId.length === 0) {
      if (communityBadgeId.length > 0) {
        const communityBadge = SteamBadgePageParser.parseCommunityBadge(row, communityBadgeId);
        if (communityBadge !== null) {
          return new SteamBadgeRowParseResult(null, communityBadge, null);
        }
      }
      return new SteamBadgeRowParseResult(
        null,
        null,
        SteamBadgePageParser.createIssue(
          SteamBadgePageWarningCode.MISSING_APP_ID,
          pageNumber,
          rowIndex,
          'badge row is missing a gamebadge row ID, card-drop marker, and gamecards link'
        )
      );
    }
    if (!SteamBadgePageParser.isPositiveInteger(appId)) {
      return new SteamBadgeRowParseResult(
        null,
        null,
        SteamBadgePageParser.createIssue(
          SteamBadgePageWarningCode.INVALID_APP_ID,
          pageNumber,
          rowIndex,
          'badge row has a non-positive card-drop app ID'
        )
      );
    }

    const progressText = SteamBadgePageParser.findElementText(row, 'span', 'progress_info_bold');
    const bodyBlocks = SteamBadgePageParser.findElementBlocks(row, 'div', 'card_drop_info_body');
    const bodyTexts = SteamBadgePageParser.findElementTexts(row, 'div', 'card_drop_info_body');
    const cardsRemainingResult = SteamBadgePageParser.extractFirstIntegerResult(progressText);
    const noDropsRemaining = SteamBadgePageParser.containsNoDropsText(progressText) ||
      SteamBadgePageParser.containsNoDropsMessage(bodyTexts);
    let cardsRemaining = 0;
    let softIssue: SteamBadgePageWarning | null = null;
    if (progressText.length === 0) {
      // Steam omits the drop counter when a game has no remaining drops.
      // The owned badge is still valid and must remain visible.
    } else if (cardsRemainingResult.found) {
      cardsRemaining = cardsRemainingResult.value;
    } else if (!noDropsRemaining) {
      // Localized pages phrase the drop counter differently; the badge is
      // still valid, so keep it and report a soft parsing warning.
      cardsRemaining = 0;
      softIssue = SteamBadgePageParser.createIssue(
        SteamBadgePageWarningCode.INVALID_CARD_DROP_COUNT,
        pageNumber,
        rowIndex,
        'badge row has an invalid card-drop count'
      );
    }
    const hoursText = SteamBadgePageParser.findElementText(row, 'div', 'badge_title_stats_playtime');
    const hoursResult = SteamBadgePageParser.extractDecimalResult(hoursText);
    let hoursPlayed = hoursResult.value;
    if (hoursText.length > 0 && !hoursResult.found) {
      // Localized playtime phrasing (for example an unplayed game label) must
      // not discard the badge; treat the playtime as unknown instead.
      hoursPlayed = 0;
      if (softIssue === null) {
        softIssue = SteamBadgePageParser.createIssue(
          SteamBadgePageWarningCode.INVALID_PLAYTIME,
          pageNumber,
          rowIndex,
          'badge row has an invalid playtime value'
        );
      }
    }
    const name = SteamBadgePageParser.extractGameNameFromRow(row, bodyBlocks, bodyTexts);
    if (name.length === 0) {
      return new SteamBadgeRowParseResult(
        null,
        null,
        SteamBadgePageParser.createIssue(
          SteamBadgePageWarningCode.MISSING_GAME_NAME,
          pageNumber,
          rowIndex,
          'badge row is missing a game name'
        )
      );
    }

    // The badge level is rendered in different elements and localizations
    // (Level 5 / 等级 5 / 5级徽章 / Lv.5 ...). Collect text from the common
    // description/title blocks and fall back to the whole row text so the
    // level survives markup variants on localized pages.
    let badgeLevel = 0;
    const levelCandidates: Array<string> = [];
    const levelBlocks = SteamBadgePageParser.findElementBlocks(row, 'div', 'badge_info_description');
    for (let index = 0; index < levelBlocks.length; index++) {
      levelCandidates.push(SteamBadgePageParser.stripTags(levelBlocks[index]));
    }
    const levelTitleBlocks = SteamBadgePageParser.findElementBlocks(row, 'div', 'badge_info_title');
    for (let index = 0; index < levelTitleBlocks.length; index++) {
      levelCandidates.push(SteamBadgePageParser.stripTags(levelTitleBlocks[index]));
    }
    levelCandidates.push(SteamBadgePageParser.stripTags(row));
    for (let index = 0; index < levelCandidates.length; index++) {
      const level = SteamBadgePageParser.extractLevel(levelCandidates[index]);
      if (level > 0) {
        badgeLevel = level;
        break;
      }
    }
    if (badgeLevel <= 0) {
      const rowText = SteamBadgePageParser.normalizeWhitespace(
        SteamBadgePageParser.decodeHtmlEntities(SteamBadgePageParser.stripTags(row))
      );
      const snippet = rowText.length > 200 ? rowText.substring(0, 200) : rowText;
      logger.debug(TAG, 'badge level probe; appId=' + appId + ', rowText=' + snippet);
    }
    const badgeImageUrl: string = SteamBadgePageParser.readImageUrl(row);
    const badgeUrl: string = SteamBadgePageParser.readAnchorAttribute(row, 'badge_row_overlay', 'href');
    return new SteamBadgeRowParseResult(
      new SteamGameDrop(appId, name, hoursPlayed, cardsRemaining, badgeLevel, false, '', '', '',
        badgeImageUrl, badgeUrl, badgeCrafted),
      null,
      softIssue
    );
  }

  private static parseCommunityBadge(row: string, badgeId: string): SteamCommunityBadge | null {
    const titleBlocks = SteamBadgePageParser.findElementBlocks(row, 'div', 'badge_title');
    if (titleBlocks.length <= 0) {
      return null;
    }
    const name = SteamBadgePageParser.extractLeadingElementText(titleBlocks[0]);
    if (name.length <= 0) {
      return null;
    }
    const descriptionBlocks = SteamBadgePageParser.findElementBlocks(row, 'div', 'badge_info_description');
    const experienceText = descriptionBlocks.length > 0
      ? SteamBadgePageParser.stripTags(descriptionBlocks[0])
      : '';
    const experience = SteamBadgePageParser.extractFirstInteger(experienceText);
    const imageUrl = SteamBadgePageParser.readImageUrl(row);
    const badgeUrl = SteamBadgePageParser.readAnchorAttribute(row, 'badge_row_overlay', 'href');
    return new SteamCommunityBadge(badgeId, name, experience, imageUrl, badgeUrl);
  }

  private static parsePageCount(
    html: string,
    pageNumber: number,
    issues: Array<SteamBadgePageWarning>,
    warnings: Array<string>
  ): number {
    const links = SteamBadgePageParser.findAnchorTexts(html, 'pagelink');
    let pageCount = 1;
    for (let index = 0; index < links.length; index++) {
      const pageResult = SteamBadgePageParser.extractFirstIntegerResult(links[index]);
      if (!pageResult.found || pageResult.value <= 0) {
        SteamBadgePageParser.addIssue(
          issues,
          warnings,
          SteamBadgePageWarningCode.INVALID_PAGE_LINK,
          pageNumber,
          -1,
          'pagination link has no positive page number'
        );
        continue;
      }
      const page = pageResult.value;
      if (page > pageCount) {
        pageCount = page;
      }
    }
    return pageCount;
  }

  /**
   * Extracts the game name from a badge row. Steam wraps the game name in an
   * anchor inside the drop-info dialog in every supported page language, but
   * the dialog may also end with anchor-less blocks (booster-pack eligibility
   * copy, help text), so the body blocks are scanned from the end and only
   * blocks that actually contain an anchor are considered. The badge title is
   * the next fallback, and the final fallback keeps only the first sentence of
   * the row text so localized status copy never becomes the game name.
   */
  private static extractGameNameFromRow(row: string, bodyBlocks: Array<string>,
    bodyTexts: Array<string>): string {
    const gameLinkName = SteamBadgePageParser.extractGameLinkAnchorText(row);
    if (gameLinkName.length > 0) {
      return gameLinkName;
    }
    for (let index = bodyBlocks.length - 1; index >= 0; index--) {
      const anchorText = SteamBadgePageParser.extractFirstAnchorText(bodyBlocks[index]);
      if (anchorText.length > 0) {
        return anchorText;
      }
    }
    const titleBlocks = SteamBadgePageParser.findElementBlocks(row, 'div', 'badge_title');
    if (titleBlocks.length > 0) {
      const titleName = SteamBadgePageParser.extractLeadingElementText(titleBlocks[0]);
      if (titleName.length > 0) {
        return titleName;
      }
    }
    if (bodyTexts.length > 0) {
      const markerName = SteamBadgePageParser.extractGameName(bodyTexts[bodyTexts.length - 1]);
      if (markerName.length > 0) {
        return markerName;
      }
    }
    // No anchor, title, or sentence marker names this row; keep it nameless so
    // the caller reports MISSING_GAME_NAME instead of inventing a name from
    // unrelated status copy.
    return '';
  }

  /**
   * Returns the text of the first row anchor that links to a game or
   * gamecards page. The name link inside the drop-info body carries the game
   * name in every page language; overlay anchors have empty text and are
   * skipped, and absurdly long anchor text is ignored defensively.
   */
  private static extractGameLinkAnchorText(row: string): string {
    let cursor = 0;
    while (cursor < row.length) {
      const openStart = SteamBadgePageParser.findNextOpenTag(row, 'a', cursor);
      if (openStart < 0) {
        break;
      }
      const openEnd = row.indexOf('>', openStart);
      if (openEnd < 0) {
        break;
      }
      const openTag = row.substring(openStart, openEnd + 1);
      const href = SteamBadgePageParser.readAttribute(openTag, 'href');
      if (href.length > 0 && (href.indexOf('/app/') >= 0 || href.indexOf('/gamecards/') >= 0)) {
        const closeStart = SteamBadgePageParser.findNextCloseTag(row, 'a', openEnd + 1);
        if (closeStart >= 0) {
          const rawText = row.substring(openEnd + 1, closeStart);
          const text = SteamBadgePageParser.normalizeWhitespace(
            SteamBadgePageParser.decodeHtmlEntities(SteamBadgePageParser.stripTags(rawText))
          );
          if (text.length > 0 && text.length <= 96) {
            return text;
          }
          cursor = closeStart + 2;
          continue;
        }
      }
      cursor = openEnd + 1;
    }
    return '';
  }

  private static extractFirstAnchorText(block: string): string {
    const openStart = SteamBadgePageParser.findNextOpenTag(block, 'a', 0);
    if (openStart < 0) {
      return '';
    }
    const openEnd = block.indexOf('>', openStart);
    if (openEnd < 0) {
      return '';
    }
    const closeStart = SteamBadgePageParser.findNextCloseTag(block, 'a', openEnd + 1);
    if (closeStart < 0) {
      return '';
    }
    return SteamBadgePageParser.normalizeWhitespace(
      SteamBadgePageParser.decodeHtmlEntities(
        SteamBadgePageParser.stripTags(block.substring(openEnd + 1, closeStart))
      )
    );
  }

  private static extractGameName(value: string): string {
    const text = SteamBadgePageParser.normalizeWhitespace(SteamBadgePageParser.decodeHtmlEntities(SteamBadgePageParser.stripTags(value)));
    const normalMarker = ' by playing ';
    const earnedMarker = ' from playing ';
    const noDropsMarker = "You don't have any more drops remaining for ";
    let start = text.indexOf(normalMarker);
    if (start >= 0) {
      start += normalMarker.length;
    } else {
      start = text.indexOf(earnedMarker);
      if (start >= 0) {
        start += earnedMarker.length;
      } else {
        start = text.indexOf(noDropsMarker);
        if (start >= 0) {
          start += noDropsMarker.length;
        }
      }
    }
    if (start < 0 || start >= text.length) {
      return SteamBadgePageParser.firstSentence(text);
    }
    let end = text.lastIndexOf('.');
    if (end <= start) {
      end = text.length;
    }
    return text.substring(start, end).trim();
  }

  /**
   * Keeps only the first sentence of a text (terminated by a period, question
   * mark, exclamation mark or their full-width Chinese forms) and caps the
   * length so a fallback name never turns into a full status paragraph.
   */
  private static firstSentence(value: string): string {
    const text = value.trim();
    if (text.length === 0) {
      return '';
    }
    let end = text.length;
    for (let index = 0; index < text.length; index++) {
      const code = text.charCodeAt(index);
      if (code === 46 || code === 33 || code === 63 ||
        code === 12290 || code === 65281 || code === 65311) {
        end = index + 1;
        break;
      }
    }
    if (end > 96) {
      end = 96;
    }
    return text.substring(0, end).trim();
  }

  private static extractLevel(value: string): number {
    const normalized = SteamBadgePageParser.normalizeWhitespace(
      SteamBadgePageParser.decodeHtmlEntities(value)
    );
    const markers: Array<string> = ['Level ', '等级', 'Lv', 'LV', 'Lvl', 'LEVEL'];
    for (let index = 0; index < markers.length; index++) {
      const start = normalized.indexOf(markers[index]);
      if (start >= 0) {
        const level = SteamBadgePageParser.extractFirstInteger(normalized.substring(start + markers[index].length));
        // Pass/tournament and community-style badges have no practical cap, so
        // only reject missing values.
        if (level > 0) {
          return level;
        }
      }
    }
    // Compact localized form such as "3级徽章" or "5 级": a digit run
    // followed by optional whitespace and the 级 character.
    for (let index = 0; index < normalized.length; index++) {
      if (!SteamBadgePageParser.isDigit(normalized.charCodeAt(index))) {
        continue;
      }
      let end = index;
      while (end < normalized.length && SteamBadgePageParser.isDigit(normalized.charCodeAt(end))) {
        end++;
      }
      let cursor = end;
      while (cursor < normalized.length && SteamBadgePageParser.isWhitespace(normalized.charCodeAt(cursor))) {
        cursor++;
      }
      if (cursor < normalized.length && normalized.charAt(cursor) === '级') {
        const level = Number(normalized.substring(index, end));
        if (level > 0) {
          return level;
        }
      }
      index = end;
    }
    return 0;
  }

  private static extractFirstInteger(value: string): number {
    return SteamBadgePageParser.extractFirstIntegerResult(value).value;
  }

  private static extractFirstIntegerResult(value: string): SteamBadgeIntegerResult {
    let start = -1;
    for (let index = 0; index < value.length; index++) {
      if (SteamBadgePageParser.isDigit(value.charCodeAt(index))) {
        start = index;
        break;
      }
    }
    if (start < 0) {
      return new SteamBadgeIntegerResult(false, 0);
    }
    let end = start;
    while (end < value.length && SteamBadgePageParser.isDigit(value.charCodeAt(end))) {
      end++;
    }
    return new SteamBadgeIntegerResult(true, Number(value.substring(start, end)));
  }

  private static extractDecimal(value: string): number {
    return SteamBadgePageParser.extractDecimalResult(value).value;
  }

  private static extractDecimalResult(value: string): SteamBadgeDecimalResult {
    let start = -1;
    for (let index = 0; index < value.length; index++) {
      if (SteamBadgePageParser.isDigit(value.charCodeAt(index))) {
        start = index;
        break;
      }
    }
    if (start < 0) {
      return new SteamBadgeDecimalResult(false, 0);
    }
    let end = start;
    let dotSeen = false;
    while (end < value.length) {
      const code = value.charCodeAt(end);
      if (SteamBadgePageParser.isDigit(code)) {
        end++;
        continue;
      }
      if (code === 46 && !dotSeen) {
        dotSeen = true;
        end++;
        continue;
      }
      break;
    }
    const result = Number(value.substring(start, end));
    return Number.isNaN(result)
      ? new SteamBadgeDecimalResult(false, 0)
      : new SteamBadgeDecimalResult(true, Math.max(0, result));
  }

  private static containsNoDropsMessage(values: Array<string>): boolean {
    for (let index = 0; index < values.length; index++) {
      if (SteamBadgePageParser.containsNoDropsText(values[index])) {
        return true;
      }
    }
    return false;
  }

  private static containsNoDropsText(value: string): boolean {
    const normalized = SteamBadgePageParser.normalizeWhitespace(value).toLowerCase();
    return normalized.indexOf('no card drops remaining') >= 0 ||
      normalized.indexOf('no more card drops') >= 0 ||
      normalized.indexOf("don't have any more drops remaining") >= 0 ||
      normalized.indexOf('没有更多卡牌掉落') >= 0 ||
      normalized.indexOf('没有更多卡牌包掉落') >= 0 ||
      normalized.indexOf('没有可用的卡牌掉落') >= 0 ||
      normalized.indexOf('已没有更多卡牌') >= 0 ||
      normalized.indexOf('没有剩余卡牌') >= 0 ||
      normalized.indexOf('无法获得更多卡牌') >= 0;
  }

  private static isPositiveInteger(value: string): boolean {
    if (value.length === 0) {
      return false;
    }
    for (let index = 0; index < value.length; index++) {
      if (!SteamBadgePageParser.isDigit(value.charCodeAt(index))) {
        return false;
      }
    }
    return Number(value) > 0;
  }

  private static extractTrailingPositiveInteger(value: string): string {
    let end = value.length;
    while (end > 0 && !SteamBadgePageParser.isDigit(value.charCodeAt(end - 1))) {
      end--;
    }
    if (end <= 0) {
      return '';
    }
    let start = end - 1;
    while (start > 0 && SteamBadgePageParser.isDigit(value.charCodeAt(start - 1))) {
      start--;
    }
    const result = value.substring(start, end);
    return SteamBadgePageParser.isPositiveInteger(result) ? result : '';
  }

  private static extractGameCardsAppId(row: string): string {
    const marker = '/gamecards/';
    const markerStart = row.indexOf(marker);
    if (markerStart < 0) {
      return '';
    }
    const valueStart = markerStart + marker.length;
    let valueEnd = valueStart;
    while (valueEnd < row.length && SteamBadgePageParser.isDigit(row.charCodeAt(valueEnd))) {
      valueEnd++;
    }
    const result = row.substring(valueStart, valueEnd);
    return SteamBadgePageParser.isPositiveInteger(result) ? result : '';
  }

  private static extractGameBadgeAppId(rowId: string): string {
    const marker = 'badge_gamebadge_';
    if (rowId.indexOf(marker) !== 0) {
      return '';
    }
    const valueStart = marker.length;
    let valueEnd = valueStart;
    while (valueEnd < rowId.length && SteamBadgePageParser.isDigit(rowId.charCodeAt(valueEnd))) {
      valueEnd++;
    }
    const result = rowId.substring(valueStart, valueEnd);
    return SteamBadgePageParser.isPositiveInteger(result) ? result : '';
  }

  private static extractCommunityBadgeId(rowId: string): string {
    const marker = 'badge_badge_';
    if (rowId.indexOf(marker) !== 0) {
      return '';
    }
    const badgeId = rowId.substring(marker.length);
    return SteamBadgePageParser.isPositiveInteger(badgeId) ? badgeId : '';
  }

  private static readImageUrl(row: string): string {
    let cursor = 0;
    while (cursor < row.length) {
      const openStart = row.indexOf('<img', cursor);
      if (openStart < 0) {
        return '';
      }
      const openEnd = row.indexOf('>', openStart);
      if (openEnd < 0) {
        return '';
      }
      const imageTag = row.substring(openStart, openEnd + 1);
      if (SteamBadgePageParser.hasClass(imageTag, 'badge_icon')) {
        return SteamBadgePageParser.normalizeSteamAssetUrl(
          SteamBadgePageParser.readAttribute(imageTag, 'data-delayed-image')
        );
      }
      cursor = openEnd + 1;
    }
    return '';
  }

  private static readAnchorAttribute(row: string, className: string, attributeName: string): string {
    let cursor = 0;
    while (cursor < row.length) {
      const openStart = SteamBadgePageParser.findNextOpenTag(row, 'a', cursor);
      if (openStart < 0) {
        return '';
      }
      const openEnd = row.indexOf('>', openStart);
      if (openEnd < 0) {
        return '';
      }
      const openTag = row.substring(openStart, openEnd + 1);
      if (SteamBadgePageParser.hasClass(openTag, className)) {
        return SteamBadgePageParser.readAttribute(openTag, attributeName);
      }
      cursor = openEnd + 1;
    }
    return '';
  }

  private static normalizeSteamAssetUrl(value: string): string {
    const normalized: string = SteamBadgePageParser.decodeHtmlEntities(value.trim());
    if (normalized.startsWith('//')) {
      return 'https:' + normalized;
    }
    if (normalized.startsWith('/')) {
      return 'https://steamcommunity.com' + normalized;
    }
    return normalized;
  }

  private static extractLeadingElementText(block: string): string {
    const openEnd = block.indexOf('>');
    if (openEnd < 0) {
      return '';
    }
    const childStart = block.indexOf('<', openEnd + 1);
    const raw = childStart >= 0
      ? block.substring(openEnd + 1, childStart)
      : block.substring(openEnd + 1);
    const normalized = SteamBadgePageParser.normalizeWhitespace(
      SteamBadgePageParser.decodeHtmlEntities(raw)
    );
    if (normalized.length > 0) {
      return normalized;
    }
    return SteamBadgePageParser.normalizeWhitespace(
      SteamBadgePageParser.decodeHtmlEntities(SteamBadgePageParser.stripTags(block))
    );
  }

  private static isDigit(code: number): boolean {
    return code >= 48 && code <= 57;
  }

  private static createIssue(
    code: SteamBadgePageWarningCode,
    pageNumber: number,
    rowIndex: number,
    detail: string
  ): SteamBadgePageWarning {
    const location = rowIndex >= 0
      ? 'page ' + pageNumber.toString() + ', row ' + (rowIndex + 1).toString()
      : 'page ' + pageNumber.toString();
    return new SteamBadgePageWarning(code, pageNumber, rowIndex, location + ': ' + detail);
  }

  private static addIssue(
    issues: Array<SteamBadgePageWarning>,
    warnings: Array<string>,
    code: SteamBadgePageWarningCode,
    pageNumber: number,
    rowIndex: number,
    detail: string
  ): void {
    const issue = SteamBadgePageParser.createIssue(code, pageNumber, rowIndex, detail);
    issues.push(issue);
    warnings.push(issue.message);
  }

  private static findElementText(html: string, tagName: string, className: string): string {
    const texts = SteamBadgePageParser.findElementTexts(html, tagName, className);
    return texts.length > 0 ? texts[0] : '';
  }

  private static findElementTexts(html: string, tagName: string, className: string): Array<string> {
    const blocks = SteamBadgePageParser.findElementBlocks(html, tagName, className);
    const texts: Array<string> = [];
    for (let index = 0; index < blocks.length; index++) {
      texts.push(SteamBadgePageParser.normalizeWhitespace(SteamBadgePageParser.decodeHtmlEntities(SteamBadgePageParser.stripTags(blocks[index]))));
    }
    return texts;
  }

  private static findElementBlocks(html: string, tagName: string, className: string): Array<string> {
    const blocks: Array<string> = [];
    let cursor = 0;
    while (cursor < html.length) {
      const openStart = SteamBadgePageParser.findNextOpenTag(html, tagName, cursor);
      if (openStart < 0) {
        break;
      }
      const openEnd = html.indexOf('>', openStart);
      if (openEnd < 0) {
        break;
      }
      const openTag = html.substring(openStart, openEnd + 1);
      if (!SteamBadgePageParser.hasClass(openTag, className)) {
        cursor = openEnd + 1;
        continue;
      }
      const closeEnd = SteamBadgePageParser.findMatchingClose(html, tagName, openEnd + 1);
      if (closeEnd < 0) {
        break;
      }
      blocks.push(html.substring(openStart, closeEnd));
      cursor = closeEnd;
    }
    return blocks;
  }

  private static findAnchorTexts(html: string, className: string): Array<string> {
    const texts: Array<string> = [];
    let cursor = 0;
    while (cursor < html.length) {
      const openStart = SteamBadgePageParser.findNextOpenTag(html, 'a', cursor);
      if (openStart < 0) {
        break;
      }
      const openEnd = html.indexOf('>', openStart);
      if (openEnd < 0) {
        break;
      }
      const openTag = html.substring(openStart, openEnd + 1);
      const closeStart = SteamBadgePageParser.findNextCloseTag(html, 'a', openEnd + 1);
      if (closeStart < 0) {
        break;
      }
      if (SteamBadgePageParser.hasClass(openTag, className)) {
        texts.push(SteamBadgePageParser.normalizeWhitespace(SteamBadgePageParser.decodeHtmlEntities(
          SteamBadgePageParser.stripTags(html.substring(openEnd + 1, closeStart))
        )));
      }
      const closeEnd = html.indexOf('>', closeStart);
      cursor = closeEnd >= 0 ? closeEnd + 1 : html.length;
    }
    return texts;
  }

  private static readElementAttribute(
    html: string,
    tagName: string,
    className: string,
    attributeName: string
  ): string {
    let cursor = 0;
    while (cursor < html.length) {
      const openStart = SteamBadgePageParser.findNextOpenTag(html, tagName, cursor);
      if (openStart < 0) {
        return '';
      }
      const openEnd = html.indexOf('>', openStart);
      if (openEnd < 0) {
        return '';
      }
      const openTag = html.substring(openStart, openEnd + 1);
      if (SteamBadgePageParser.hasClass(openTag, className)) {
        return SteamBadgePageParser.readAttribute(openTag, attributeName);
      }
      cursor = openEnd + 1;
    }
    return '';
  }

  private static readFirstElementAttribute(html: string, tagName: string, attributeName: string): string {
    const openStart = SteamBadgePageParser.findNextOpenTag(html, tagName, 0);
    if (openStart < 0) {
      return '';
    }
    const openEnd = html.indexOf('>', openStart);
    if (openEnd < 0) {
      return '';
    }
    return SteamBadgePageParser.readAttribute(html.substring(openStart, openEnd + 1), attributeName);
  }

  private static hasClass(tag: string, className: string): boolean {
    return SteamBadgePageParser.hasClassToken(SteamBadgePageParser.readAttribute(tag, 'class'), className);
  }

  private static hasClassToken(value: string, className: string): boolean {
    let start = 0;
    for (let index = 0; index <= value.length; index++) {
      if (index === value.length || SteamBadgePageParser.isWhitespace(value.charCodeAt(index))) {
        if (value.substring(start, index) === className) {
          return true;
        }
        start = index + 1;
      }
    }
    return false;
  }

  private static readAttribute(tag: string, attributeName: string): string {
    const marker = attributeName + '=';
    let start = tag.indexOf(marker);
    while (start >= 0) {
      if (start === 0 || SteamBadgePageParser.isWhitespace(tag.charCodeAt(start - 1))) {
        break;
      }
      start = tag.indexOf(marker, start + marker.length);
    }
    if (start < 0) {
      return '';
    }
    let valueStart = start + marker.length;
    while (valueStart < tag.length && SteamBadgePageParser.isWhitespace(tag.charCodeAt(valueStart))) {
      valueStart++;
    }
    const quote = tag.charAt(valueStart);
    if (quote === '"' || quote === "'") {
      const valueEnd = tag.indexOf(quote, valueStart + 1);
      return valueEnd >= 0 ? tag.substring(valueStart + 1, valueEnd) : '';
    }
    let valueEnd = valueStart;
    while (valueEnd < tag.length && !SteamBadgePageParser.isWhitespace(tag.charCodeAt(valueEnd)) && tag.charAt(valueEnd) !== '>') {
      valueEnd++;
    }
    return tag.substring(valueStart, valueEnd);
  }

  private static findMatchingClose(html: string, tagName: string, contentStart: number): number {
    let depth = 1;
    let cursor = contentStart;
    while (cursor < html.length) {
      const openStart = SteamBadgePageParser.findNextOpenTag(html, tagName, cursor);
      const closeStart = SteamBadgePageParser.findNextCloseTag(html, tagName, cursor);
      if (closeStart < 0) {
        return -1;
      }
      if (openStart >= 0 && openStart < closeStart) {
        const openEnd = html.indexOf('>', openStart);
        if (openEnd < 0) {
          return -1;
        }
        depth++;
        cursor = openEnd + 1;
      } else {
        depth--;
        const closeEnd = html.indexOf('>', closeStart);
        if (depth === 0) {
          return closeEnd >= 0 ? closeEnd + 1 : -1;
        }
        cursor = closeEnd >= 0 ? closeEnd + 1 : html.length;
      }
    }
    return -1;
  }

  private static findNextOpenTag(html: string, tagName: string, fromIndex: number): number {
    const prefix = '<' + tagName;
    let index = html.indexOf(prefix, fromIndex);
    while (index >= 0) {
      const boundary = html.charAt(index + prefix.length);
      if (boundary === '>' || SteamBadgePageParser.isWhitespace(boundary.charCodeAt(0))) {
        return index;
      }
      index = html.indexOf(prefix, index + prefix.length);
    }
    return -1;
  }

  private static findNextCloseTag(html: string, tagName: string, fromIndex: number): number {
    const prefix = '</' + tagName;
    let index = html.indexOf(prefix, fromIndex);
    while (index >= 0) {
      const boundary = html.charAt(index + prefix.length);
      if (boundary === '>' || SteamBadgePageParser.isWhitespace(boundary.charCodeAt(0))) {
        return index;
      }
      index = html.indexOf(prefix, index + prefix.length);
    }
    return -1;
  }

  private static stripTags(value: string): string {
    let result = '';
    let insideTag = false;
    for (let index = 0; index < value.length; index++) {
      const character = value.charAt(index);
      if (character === '<') {
        insideTag = true;
      } else if (character === '>') {
        insideTag = false;
        result += ' ';
      } else if (!insideTag) {
        result += character;
      }
    }
    return result;
  }

  private static normalizeWhitespace(value: string): string {
    let result = '';
    let whitespacePending = false;
    for (let index = 0; index < value.length; index++) {
      const character = value.charAt(index);
      if (SteamBadgePageParser.isWhitespace(character.charCodeAt(0))) {
        whitespacePending = true;
      } else {
        if (whitespacePending && result.length > 0) {
          result += ' ';
        }
        whitespacePending = false;
        result += character;
      }
    }
    return result.trim();
  }

  private static decodeHtmlEntities(value: string): string {
    return value
      .replace(/&amp;/g, '&')
      .replace(/&quot;/g, '"')
      .replace(/&#39;/g, "'")
      .replace(/&#x27;/gi, "'")
      .replace(/&lt;/g, '<')
      .replace(/&gt;/g, '>')
      .replace(/&nbsp;/g, ' ');
  }

  private static isWhitespace(code: number): boolean {
    return code === 32 || code === 9 || code === 10 || code === 13;
  }
}
