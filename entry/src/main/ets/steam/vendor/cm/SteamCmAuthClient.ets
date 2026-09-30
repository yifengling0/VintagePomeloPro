import {
  ISteamCmAuthClient,
  ISteamCmScheduler,
  ISteamCmWireTransport,
  SteamCmAuthClientEvent,
  SteamCmAuthClientEventListener,
  SteamCmAuthClientOptions,
  SteamCmAuthClientState
} from '../contracts/SteamCmContracts';
import {
  SteamClientHelloCodec,
  SteamClientHelloRequest
} from '../protocol/SteamClientHelloCodec';
import {
  SteamCmDecodedMessage,
  SteamCmFrameInfo,
  SteamCmHeaderKind,
  SteamCmMessageType,
  SteamCmProtoHeader,
  SteamCmProtocol
} from '../protocol/SteamCmProtocol';
import { SteamVarint } from '../protocol/SteamVarint';
import { ISteamCoreLogger, SteamCoreLogSupport } from '../contracts/SteamCoreLogger';
import {
  SteamAuthError,
  SteamAuthErrorKind,
  SteamAuthErrorSupport,
  SteamErrorLike
} from '../contracts/SteamAuthErrors';

const TAG: string = 'CmAuthClient';

class SteamCmAuthPendingRequest {
  readonly jobId: string;
  readonly resolve: (message: SteamCmDecodedMessage) => void;
  readonly reject: (error: SteamAuthError) => void;
  timeoutHandle: number = -1;

  constructor(
    jobId: string,
    resolve: (message: SteamCmDecodedMessage) => void,
    reject: (error: SteamAuthError) => void
  ) {
    this.jobId = jobId;
    this.resolve = resolve;
    this.reject = reject;
  }
}

/**
 * Platform-independent CM unified-message client used by authentication.
 * It deliberately owns no Steam credentials and has no automatic reconnect;
 * the caller decides when a new authentication channel may be opened.
 */
export class SteamCmAuthClient implements ISteamCmAuthClient {
  private readonly wire: ISteamCmWireTransport;
  private readonly scheduler: ISteamCmScheduler;
  private readonly options: SteamCmAuthClientOptions;
  private readonly logger: ISteamCoreLogger;
  private readonly listeners: Array<SteamCmAuthClientEventListener> = [];
  private readonly pending: Map<string, SteamCmAuthPendingRequest> =
    new Map<string, SteamCmAuthPendingRequest>();
  private stateValue: SteamCmAuthClientState = SteamCmAuthClientState.DISCONNECTED;
  private nextJobId: string = '1';
  private transportConnected: boolean = false;
  private manualDisconnect: boolean = true;
  private connectionAttempt: number = 0;
  /** Shares one in-flight handshake with all callers instead of opening duplicate sockets. */
  private connectionPromise: Promise<void> | null = null;
  /** Prevents the close callback from replacing a more useful preceding error. */
  private closeAfterFailure: boolean = false;
  private failureMessage: string = '';

  constructor(
    wire: ISteamCmWireTransport,
    scheduler: ISteamCmScheduler,
    options: SteamCmAuthClientOptions
  ) {
    this.wire = wire;
    this.scheduler = scheduler;
    this.options = options;
    this.logger = options.logger;
    this.bindTransportListeners();
    this.logger.info(TAG, 'CM authentication client constructed');
  }

  get state(): SteamCmAuthClientState {
    return this.stateValue;
  }

  async connect(): Promise<void> {
    if (this.transportConnected && this.stateValue === SteamCmAuthClientState.CONNECTED) {
      this.logger.debug(TAG, 'CM authentication connect skipped; already connected');
      return;
    }
    const existingConnection = this.connectionPromise;
    if (existingConnection !== null) {
      this.logger.debug(TAG, 'CM authentication connect joined; handshake already in flight');
      await existingConnection;
      return;
    }
    const currentConnection = this.connectInternal();
    this.connectionPromise = currentConnection;
    try {
      await currentConnection;
    } finally {
      if (this.connectionPromise === currentConnection) {
        this.connectionPromise = null;
      }
    }
  }

  private async connectInternal(): Promise<void> {
    this.logger.info(TAG, 'CM authentication channel connect started');
    this.manualDisconnect = false;
    this.closeAfterFailure = false;
    this.failureMessage = '';
    const attempt: number = this.connectionAttempt + 1;
    this.connectionAttempt = attempt;
    this.updateState(SteamCmAuthClientState.CONNECTING, 'Steam CM authentication channel connecting');
    try {
      await this.wire.connect(this.options.server);
      if (attempt !== this.connectionAttempt || this.manualDisconnect) {
        throw new Error('Steam CM authentication channel was interrupted');
      }
      this.transportConnected = true;
      this.updateState(SteamCmAuthClientState.CONNECTED, 'Steam CM authentication channel connected');
      const hello = SteamClientHelloCodec.encode(new SteamClientHelloRequest(this.options.protocolVersion));
      const frame = SteamCmProtocol.encodeProtobufMessage(
        SteamCmMessageType.CLIENT_HELLO,
        new SteamCmProtoHeader(),
        hello
      );
      await this.wire.send(frame);
      this.logger.debug(TAG, 'CM authentication ClientHello sent; bytes=' + frame.length.toString());
    } catch (error) {
      const normalized: Error = SteamCmAuthClient.normalizeError(error, 'Steam CM authentication channel failed');
      const transportError: SteamAuthError = SteamAuthErrorSupport.normalize(
        normalized,
        SteamAuthErrorKind.TRANSPORT,
        'cm_connect',
        'Steam CM authentication channel failed'
      );
      this.logger.error(TAG, 'CM authentication channel connect failed; errorType=' +
        SteamCoreLogSupport.errorKind(transportError) + ', detail=' + transportError.message);
      this.transportConnected = false;
      this.rejectAll(transportError);
      const isCurrentActiveAttempt: boolean = attempt === this.connectionAttempt && !this.manualDisconnect;
      this.closeAfterFailure = isCurrentActiveAttempt;
      this.failureMessage = isCurrentActiveAttempt ? transportError.message : '';
      await this.closeTransportQuietly();
      if (attempt === this.connectionAttempt && !this.manualDisconnect) {
        this.connectionAttempt++;
        this.updateState(SteamCmAuthClientState.ERROR, transportError.message);
      }
      throw transportError;
    }
  }

  async disconnect(): Promise<void> {
    this.logger.info(TAG, 'CM authentication channel disconnect started');
    this.manualDisconnect = true;
    this.closeAfterFailure = false;
    this.failureMessage = '';
    this.connectionAttempt++;
    this.transportConnected = false;
    this.rejectAll(new SteamAuthError(
      'Steam CM authentication channel disconnected',
      SteamAuthErrorKind.TRANSPORT,
      'cm_disconnect'
    ));
    await this.closeTransportQuietly();
    this.updateState(SteamCmAuthClientState.DISCONNECTED, 'Steam CM authentication channel disconnected');
    this.logger.info(TAG, 'CM authentication channel disconnected');
  }

  async call(targetJobName: string, body: Uint8Array): Promise<SteamCmDecodedMessage> {
    if (!this.transportConnected || this.stateValue !== SteamCmAuthClientState.CONNECTED) {
      throw new SteamAuthError(
        'Steam CM authentication channel is not connected',
        SteamAuthErrorKind.TRANSPORT,
        'cm_call'
      );
    }
    const normalizedJobName = targetJobName.trim();
    if (normalizedJobName.length === 0) {
      throw new SteamAuthError(
        'Steam unified message name is required',
        SteamAuthErrorKind.VALIDATION,
        'cm_call'
      );
    }
    const jobId = this.nextJobId;
    this.nextJobId = SteamVarint.incrementUInt64(jobId);
    this.logger.debug(TAG, 'CM unified request started; jobIdLength=' + jobId.length.toString() +
      ', jobNameLength=' + normalizedJobName.length.toString() + ', bodyBytes=' + body.length.toString());
    const header = new SteamCmProtoHeader('0', 0, jobId, '0', normalizedJobName);
    const frame = SteamCmProtocol.encodeProtobufMessage(
      SteamCmMessageType.SERVICE_METHOD_CALL_FROM_CLIENT_NON_AUTHED,
      header,
      body
    );
    return new Promise<SteamCmDecodedMessage>((resolve: (message: SteamCmDecodedMessage) => void,
      reject: (error: Error) => void): void => {
      const pending = new SteamCmAuthPendingRequest(
        jobId,
        resolve,
        (error: SteamAuthError): void => {
          reject(error as Error);
        }
      );
      pending.timeoutHandle = this.scheduler.schedule(
        this.options.requestTimeoutMilliseconds,
        (): void => {
          this.logger.warn(TAG, 'CM unified request timed out; jobIdLength=' + jobId.length.toString());
          this.rejectPending(jobId, new SteamAuthError(
            'Steam unified message request timed out',
            SteamAuthErrorKind.TIMEOUT,
            'cm_request_timeout'
          ));
        }
      );
      this.pending.set(jobId, pending);
      this.wire.send(frame).catch((error: Error): void => {
        const transportError = SteamAuthErrorSupport.normalize(
          error,
          SteamAuthErrorKind.TRANSPORT,
          'cm_send',
          'Steam CM unified message send failed'
        );
        this.logger.warn(TAG, 'CM unified request send failed; errorType=' +
          SteamCoreLogSupport.errorKind(transportError) + ', detail=' + transportError.message);
        this.rejectPending(jobId, transportError);
      });
    });
  }

  addEventListener(listener: SteamCmAuthClientEventListener): void {
    if (this.listeners.indexOf(listener) < 0) {
      this.listeners.push(listener);
    }
  }

  removeEventListener(listener: SteamCmAuthClientEventListener): void {
    const index = this.listeners.indexOf(listener);
    if (index >= 0) {
      this.listeners.splice(index, 1);
    }
  }

  dispose(): void {
    this.logger.info(TAG, 'CM authentication client disposed');
    this.manualDisconnect = true;
    this.closeAfterFailure = false;
    this.failureMessage = '';
    this.connectionAttempt++;
    this.transportConnected = false;
    this.rejectAll(new SteamAuthError(
      'Steam CM authentication client disposed',
      SteamAuthErrorKind.TRANSPORT,
      'cm_dispose'
    ));
    this.wire.clearListeners();
    this.listeners.splice(0, this.listeners.length);
  }

  private bindTransportListeners(): void {
    this.wire.onMessage((payload: Uint8Array): void => {
      void this.handleMessage(payload);
    });
    this.wire.onClose((reason: string): void => {
      this.handleClosed(reason);
    });
    this.wire.onError((error: Error): void => {
      this.handleError(error);
    });
  }

  private async handleMessage(payload: Uint8Array): Promise<void> {
    let frameInfo: SteamCmFrameInfo = new SteamCmFrameInfo(
      payload.length,
      false,
      0,
      SteamCmHeaderKind.UNKNOWN,
      0,
      0
    );
    try {
      frameInfo = SteamCmProtocol.inspectFrame(payload);
      const messages: Array<SteamCmDecodedMessage> =
        await SteamCmProtocol.decodeMessagesAsync(payload, this.options.multiDecompressor);
      this.logger.debug(TAG, 'CM authentication message batch received; messageCount=' +
        messages.length.toString() + ', ' + frameInfo.toLogString());
      for (let index = 0; index < messages.length; index++) {
        const message = messages[index];
        this.logger.debug(TAG, 'CM authentication message received; messageType=' +
          message.messageType.toString() + ', headerKind=' + String(message.headerKind) +
          ', bodyBytes=' + message.body.length.toString());
        if (message.messageType !== SteamCmMessageType.SERVICE_METHOD_RESPONSE) {
          this.logger.debug(TAG, 'CM authentication message ignored; reason=connection_level_message, messageType=' +
            message.messageType.toString());
          continue;
        }
        const jobId = message.protoHeader.jobIdTarget;
        const pending = this.pending.get(jobId);
        if (pending === undefined) {
          this.logger.warn(TAG, 'CM authentication response ignored; reason=unknown_job, messageType=' +
            message.messageType.toString() + ', headerKind=' + String(message.headerKind));
          continue;
        }
        this.pending.delete(jobId);
        this.scheduler.cancel(pending.timeoutHandle);
        pending.resolve(message);
      }
    } catch (error) {
      const normalized: Error = SteamCmAuthClient.normalizeError(error, 'Steam CM response rejected');
      const protocolError: SteamAuthError = SteamAuthErrorSupport.normalize(
        normalized,
        SteamAuthErrorKind.PROTOCOL,
        'receive_cm_message',
        'Steam CM response rejected'
      );
      const detailedProtocolError = new SteamAuthError(
        protocolError.message,
        protocolError.kind,
        protocolError.stage,
        protocolError.code,
        protocolError.httpStatusCode,
        frameInfo.messageType,
        String(frameInfo.headerKind),
        frameInfo.payloadBytes,
        frameInfo.headerLength,
        frameInfo.bodyBytes,
        protocolError.serverMessage
      );
      this.logger.warn(TAG, 'CM authentication response rejected; errorType=' +
        SteamCoreLogSupport.errorKind(detailedProtocolError) + ', stage=' + detailedProtocolError.stage +
        ', code=' + detailedProtocolError.code.toString() + ', ' + frameInfo.toLogString() +
        ', detail=' + detailedProtocolError.message);
      this.rejectAll(detailedProtocolError);
      this.transportConnected = false;
      if (!this.manualDisconnect) {
        this.closeAfterFailure = true;
        this.failureMessage = detailedProtocolError.message;
        this.connectionAttempt++;
        this.updateState(SteamCmAuthClientState.ERROR, detailedProtocolError.message);
      } else {
        this.closeAfterFailure = false;
        this.failureMessage = '';
      }
      this.closeTransportQuietly().catch((closeError: Error): void => {
        this.logger.warn(TAG, 'CM authentication protocol cleanup failed; errorType=' +
          SteamCoreLogSupport.errorKind(closeError));
      });
    }
  }

  private handleClosed(reason: string): void {
    this.logger.warn(TAG, 'CM authentication transport closed; reasonPresent=' + String(reason.length > 0));
    this.connectionAttempt++;
    this.transportConnected = false;
    const message = reason.length > 0 ? reason : 'Steam CM authentication channel closed';
    this.rejectAll(new SteamAuthError(message, SteamAuthErrorKind.TRANSPORT, 'cm_transport_closed'));
    const preserveFailure: boolean = this.closeAfterFailure;
    const failureMessage: string = this.failureMessage;
    this.closeAfterFailure = false;
    this.failureMessage = '';
    if (preserveFailure) {
      if (this.stateValue !== SteamCmAuthClientState.ERROR) {
        this.updateState(
          SteamCmAuthClientState.ERROR,
          failureMessage.length > 0 ? failureMessage : message
        );
      }
      return;
    }
    if (!this.manualDisconnect) {
      this.updateState(SteamCmAuthClientState.ERROR, message);
      return;
    }
    this.updateState(SteamCmAuthClientState.DISCONNECTED, message);
  }

  private handleError(error: Error): void {
    this.logger.error(TAG, 'CM authentication transport error; errorType=' +
      SteamCoreLogSupport.errorKind(error));
    this.transportConnected = false;
    const transportError = SteamAuthErrorSupport.normalize(
      error,
      SteamAuthErrorKind.TRANSPORT,
      'cm_transport_error',
      'Steam CM authentication transport failed'
    );
    this.rejectAll(transportError);
    if (this.manualDisconnect) {
      this.closeAfterFailure = false;
      this.failureMessage = '';
      return;
    }
    this.connectionAttempt++;
    this.closeAfterFailure = true;
    this.failureMessage = transportError.message;
    this.updateState(SteamCmAuthClientState.ERROR, transportError.message);
    this.closeTransportQuietly().catch((_closeError: Error): void => {
      this.logger.warn(TAG, 'CM authentication transport cleanup failed; errorType=' +
        SteamCoreLogSupport.errorKind(_closeError));
    });
  }

  private rejectPending(jobId: string, error: SteamAuthError): void {
    const pending = this.pending.get(jobId);
    if (pending === undefined) {
      return;
    }
    this.pending.delete(jobId);
    this.scheduler.cancel(pending.timeoutHandle);
    pending.reject(error);
    this.logger.debug(TAG, 'CM authentication pending request rejected; errorType=' +
      SteamCoreLogSupport.errorKind(error));
  }

  private rejectAll(error: SteamAuthError): void {
    const requests: Array<SteamCmAuthPendingRequest> = [];
    this.pending.forEach((request: SteamCmAuthPendingRequest): void => {
      requests.push(request);
    });
    this.pending.clear();
    for (let index = 0; index < requests.length; index++) {
      const request = requests[index];
      this.scheduler.cancel(request.timeoutHandle);
      request.reject(error);
    }
  }

  private updateState(state: SteamCmAuthClientState, message: string): void {
    const previous = this.stateValue;
    this.stateValue = state;
    this.logger.state(TAG, String(previous), String(state), 'messageLength=' + message.length.toString());
    this.notify(new SteamCmAuthClientEvent(state, message));
  }

  private notify(event: SteamCmAuthClientEvent): void {
    const currentListeners: Array<SteamCmAuthClientEventListener> = [];
    for (let index = 0; index < this.listeners.length; index++) {
      currentListeners.push(this.listeners[index]);
    }
    for (let index = 0; index < currentListeners.length; index++) {
      try {
        currentListeners[index](event);
      } catch (error) {
        const normalized: Error = error instanceof Error ? error : new Error('CM auth event listener failed');
        this.logger.warn(TAG, 'CM authentication event listener failed; errorType=' +
          SteamCoreLogSupport.errorKind(normalized));
      }
    }
  }

  private async closeTransportQuietly(): Promise<void> {
    try {
      await this.wire.close();
    } catch (_error) {
      this.logger.warn(TAG, 'CM authentication transport close failed; errorType=' +
        SteamCoreLogSupport.errorKind(_error));
    }
  }

  private static normalizeError(error: Error | Object, fallback: string): Error {
    if (error instanceof Error) {
      return error;
    }
    const errorLike: SteamErrorLike = error as SteamErrorLike;
    const message: string = errorLike.message === undefined || errorLike.message.length === 0
      ? fallback
      : errorLike.message;
    const code: number = errorLike.code !== undefined && Number.isSafeInteger(errorLike.code)
      ? errorLike.code
      : 0;
    return new SteamAuthError(message, SteamAuthErrorKind.UNKNOWN, '', code);
  }
}
