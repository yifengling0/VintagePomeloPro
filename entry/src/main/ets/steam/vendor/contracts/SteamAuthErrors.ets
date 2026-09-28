/** Categories used to explain an authentication failure without exposing credentials. */
export enum SteamAuthErrorKind {
  UNKNOWN = 'unknown',
  VALIDATION = 'validation',
  TRANSPORT = 'transport',
  TIMEOUT = 'timeout',
  PROTOCOL = 'protocol',
  SERVER = 'server',
  RESPONSE = 'response',
  CRYPTO = 'crypto',
  AUTHENTICATION = 'authentication'
}

/** Small cross-platform view of native errors such as HarmonyOS BusinessError. */
export interface SteamErrorLike {
  readonly code?: number;
  readonly message?: string;
  readonly name?: string;
}

/** Structured, non-secret error information shared by authentication layers. */
export class SteamAuthError extends Error {
  readonly kind: SteamAuthErrorKind;
  readonly stage: string;
  readonly code: number;
  readonly httpStatusCode: number;
  readonly messageType: number;
  readonly headerKind: string;
  readonly frameBytes: number;
  readonly headerBytes: number;
  readonly bodyBytes: number;
  readonly serverMessage: string;

  constructor(
    message: string,
    kind: SteamAuthErrorKind = SteamAuthErrorKind.UNKNOWN,
    stage: string = '',
    code: number = 0,
    httpStatusCode: number = 0,
    messageType: number = 0,
    headerKind: string = '',
    frameBytes: number = 0,
    headerBytes: number = 0,
    bodyBytes: number = 0,
    serverMessage: string = ''
  ) {
    super(message);
    this.name = 'SteamAuthError';
    this.kind = kind;
    this.stage = stage;
    this.code = code;
    this.httpStatusCode = httpStatusCode;
    this.messageType = messageType;
    this.headerKind = headerKind;
    this.frameBytes = frameBytes;
    this.headerBytes = headerBytes;
    this.bodyBytes = bodyBytes;
    this.serverMessage = serverMessage;
  }
}

/** Converts an ordinary Error into a typed authentication error at a known stage. */
export class SteamAuthErrorSupport {
  /** Returns only non-secret protocol shape metadata for logs and diagnostics. */
  static diagnosticLabel(error: SteamAuthError): string {
    let label: string = 'SteamAuthError/' + String(error.kind);
    if (error.stage.length > 0) {
      label += ',stage=' + error.stage;
    }
    if (error.code !== 0) {
      label += ',code=' + error.code.toString();
    }
    if (error.httpStatusCode !== 0) {
      label += ',http=' + error.httpStatusCode.toString();
    }
    if (error.messageType !== 0) {
      label += ',messageType=' + error.messageType.toString();
    }
    if (error.headerKind.length > 0) {
      label += ',header=' + error.headerKind;
    }
    if (error.frameBytes !== 0) {
      label += ',frame=' + error.frameBytes.toString();
    }
    if (error.headerBytes !== 0) {
      label += ',headerBytes=' + error.headerBytes.toString();
    }
    if (error.bodyBytes !== 0) {
      label += ',bodyBytes=' + error.bodyBytes.toString();
    }
    return label;
  }

  static normalize(
    error: Error | Object,
    kind: SteamAuthErrorKind,
    stage: string,
    fallbackMessage: string
  ): SteamAuthError {
    if (error instanceof SteamAuthError) {
      const message = error.message.length > 0 ? error.message : fallbackMessage;
      const preservedKind = error.kind === SteamAuthErrorKind.UNKNOWN ? kind : error.kind;
      const preservedStage = error.stage.length > 0 ? error.stage : stage;
      return new SteamAuthError(
        message,
        preservedKind,
        preservedStage,
        error.code,
        error.httpStatusCode,
        error.messageType,
        error.headerKind,
        error.frameBytes,
        error.headerBytes,
        error.bodyBytes,
        error.serverMessage
      );
    }
    const errorLike: SteamErrorLike = error as SteamErrorLike;
    const errorMessage: string = error instanceof Error
      ? error.message
      : errorLike.message === undefined ? '' : errorLike.message;
    const message = errorMessage.length > 0 ? errorMessage : fallbackMessage;
    const code: number = errorLike.code !== undefined && Number.isSafeInteger(errorLike.code)
      ? errorLike.code
      : 0;
    return new SteamAuthError(message, kind, stage, code);
  }
}
