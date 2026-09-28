import {
  ISteamCoreLogger,
  SteamCoreLogSupport,
  SteamCoreNoopLogger
} from '../contracts/SteamCoreLogger';

const TAG: string = 'JsonParser';

export enum SteamJsonValueKind {
  NULL = 'null',
  BOOLEAN = 'boolean',
  NUMBER = 'number',
  STRING = 'string',
  OBJECT = 'object',
  ARRAY = 'array'
}

/** A small typed JSON tree used for migration input; it has no dynamic keys. */
export class SteamJsonValue {
  readonly kind: SteamJsonValueKind;
  readonly stringValue: string;
  readonly numberValue: string;
  readonly booleanValue: boolean;
  readonly properties: Map<string, SteamJsonValue>;
  readonly items: Array<SteamJsonValue>;

  constructor(
    kind: SteamJsonValueKind,
    stringValue: string = '',
    numberValue: string = '',
    booleanValue: boolean = false,
    properties: Map<string, SteamJsonValue> = new Map<string, SteamJsonValue>(),
    items: Array<SteamJsonValue> = []
  ) {
    this.kind = kind;
    this.stringValue = stringValue;
    this.numberValue = numberValue;
    this.booleanValue = booleanValue;
    this.properties = properties;
    this.items = items;
  }

  static nullValue(): SteamJsonValue {
    return new SteamJsonValue(SteamJsonValueKind.NULL);
  }

  static fromBoolean(value: boolean): SteamJsonValue {
    return new SteamJsonValue(SteamJsonValueKind.BOOLEAN, '', '', value);
  }

  static fromNumber(value: string): SteamJsonValue {
    return new SteamJsonValue(SteamJsonValueKind.NUMBER, '', value);
  }

  static fromString(value: string): SteamJsonValue {
    return new SteamJsonValue(SteamJsonValueKind.STRING, value);
  }

  static fromObject(properties: Map<string, SteamJsonValue>): SteamJsonValue {
    return new SteamJsonValue(SteamJsonValueKind.OBJECT, '', '', false, properties);
  }

  static fromArray(items: Array<SteamJsonValue>): SteamJsonValue {
    return new SteamJsonValue(SteamJsonValueKind.ARRAY, '', '', false, new Map<string, SteamJsonValue>(), items);
  }

  getProperty(name: string): SteamJsonValue | null {
    const value = this.properties.get(name);
    return value === undefined ? null : value;
  }

  propertyNames(): Array<string> {
    const names: Array<string> = [];
    this.properties.forEach((_value: SteamJsonValue, key: string): void => {
      names.push(key);
    });
    return names;
  }
}

export class SteamJsonParser {
  private readonly input: string;
  private position: number = 0;

  private constructor(input: string) {
    this.input = input;
  }

  static parse(
    input: string,
    logger: ISteamCoreLogger = new SteamCoreNoopLogger()
  ): SteamJsonValue {
    logger.debug(TAG, 'JSON parse started; inputLength=' + input.length.toString());
    try {
      const parser = new SteamJsonParser(input);
      const value = parser.readValue();
      parser.skipWhitespace();
      if (parser.position !== input.length) {
        throw new Error('Unexpected content after JSON value');
      }
      logger.debug(TAG, 'JSON parse completed; rootKind=' + String(value.kind));
      return value;
    } catch (error) {
      const normalized: Error = error instanceof Error ? error : new Error('JSON parse failed');
      logger.warn(TAG, 'JSON parse failed; errorType=' + SteamCoreLogSupport.errorKind(normalized));
      throw normalized;
    }
  }

  private readValue(): SteamJsonValue {
    this.skipWhitespace();
    if (this.position >= this.input.length) {
      throw new Error('JSON value is missing');
    }
    const current = this.input.charAt(this.position);
    if (current === '{') {
      return this.readObject();
    }
    if (current === '[') {
      return this.readArray();
    }
    if (current === '"') {
      return SteamJsonValue.fromString(this.readString());
    }
    if (current === 't') {
      this.readLiteral('true');
      return SteamJsonValue.fromBoolean(true);
    }
    if (current === 'f') {
      this.readLiteral('false');
      return SteamJsonValue.fromBoolean(false);
    }
    if (current === 'n') {
      this.readLiteral('null');
      return SteamJsonValue.nullValue();
    }
    return SteamJsonValue.fromNumber(this.readNumber());
  }

  private readObject(): SteamJsonValue {
    this.expect('{');
    const properties = new Map<string, SteamJsonValue>();
    this.skipWhitespace();
    if (this.consume('}')) {
      return SteamJsonValue.fromObject(properties);
    }
    while (true) {
      this.skipWhitespace();
      if (this.position >= this.input.length || this.input.charAt(this.position) !== '"') {
        throw new Error('JSON object key must be a string');
      }
      const key = this.readString();
      this.skipWhitespace();
      this.expect(':');
      properties.set(key, this.readValue());
      this.skipWhitespace();
      if (this.consume('}')) {
        return SteamJsonValue.fromObject(properties);
      }
      this.expect(',');
    }
  }

  private readArray(): SteamJsonValue {
    this.expect('[');
    const items: Array<SteamJsonValue> = [];
    this.skipWhitespace();
    if (this.consume(']')) {
      return SteamJsonValue.fromArray(items);
    }
    while (true) {
      items.push(this.readValue());
      this.skipWhitespace();
      if (this.consume(']')) {
        return SteamJsonValue.fromArray(items);
      }
      this.expect(',');
    }
  }

  private readString(): string {
    this.expect('"');
    let result = '';
    while (this.position < this.input.length) {
      const current = this.input.charAt(this.position++);
      if (current === '"') {
        return result;
      }
      if (current !== '\\') {
        if (current.charCodeAt(0) < 32) {
          throw new Error('JSON string contains a control character');
        }
        result += current;
        continue;
      }
      if (this.position >= this.input.length) {
        throw new Error('JSON string escape is incomplete');
      }
      const escaped = this.input.charAt(this.position++);
      if (escaped === '"' || escaped === '\\' || escaped === '/') {
        result += escaped;
      } else if (escaped === 'b') {
        result += '\b';
      } else if (escaped === 'f') {
        result += '\f';
      } else if (escaped === 'n') {
        result += '\n';
      } else if (escaped === 'r') {
        result += '\r';
      } else if (escaped === 't') {
        result += '\t';
      } else if (escaped === 'u') {
        result += this.readUnicodeEscape();
      } else {
        throw new Error('Unsupported JSON string escape');
      }
    }
    throw new Error('JSON string is unterminated');
  }

  private readUnicodeEscape(): string {
    if (this.position + 4 > this.input.length) {
      throw new Error('Unicode escape is incomplete');
    }
    let value = 0;
    for (let index = 0; index < 4; index++) {
      const digit = this.hexDigit(this.input.charAt(this.position++));
      if (digit < 0) {
        throw new Error('Invalid Unicode escape');
      }
      value = value * 16 + digit;
    }
    return String.fromCharCode(value);
  }

  private readNumber(): string {
    const start = this.position;
    while (this.position < this.input.length) {
      const code = this.input.charCodeAt(this.position);
      const allowed = (code >= 48 && code <= 57) || code === 45 || code === 43 || code === 46 || code === 69 || code === 101;
      if (!allowed) {
        break;
      }
      this.position++;
    }
    const value = this.input.substring(start, this.position);
    if (value.length === 0 || Number.isNaN(Number(value))) {
      throw new Error('Invalid JSON number');
    }
    return value;
  }

  private readLiteral(value: string): void {
    if (this.input.substring(this.position, this.position + value.length) !== value) {
      throw new Error('Invalid JSON literal');
    }
    this.position += value.length;
  }

  private skipWhitespace(): void {
    while (this.position < this.input.length) {
      const code = this.input.charCodeAt(this.position);
      if (code !== 32 && code !== 9 && code !== 10 && code !== 13) {
        return;
      }
      this.position++;
    }
  }

  private expect(value: string): void {
    if (!this.consume(value)) {
      throw new Error('Expected JSON token ' + value);
    }
  }

  private consume(value: string): boolean {
    if (this.input.substring(this.position, this.position + value.length) !== value) {
      return false;
    }
    this.position += value.length;
    return true;
  }

  private hexDigit(value: string): number {
    const code = value.charCodeAt(0);
    if (code >= 48 && code <= 57) {
      return code - 48;
    }
    if (code >= 65 && code <= 70) {
      return code - 55;
    }
    if (code >= 97 && code <= 102) {
      return code - 87;
    }
    return -1;
  }
}
