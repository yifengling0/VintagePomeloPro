/** Result of dividing a non-negative decimal string by a small integer. */
class SteamDecimalDivision {
  readonly quotient: string;
  readonly remainder: number;

  constructor(quotient: string, remainder: number) {
    this.quotient = quotient;
    this.remainder = remainder;
  }
}

class SteamDecimalMath {
  static normalize(value: string): string {
    const trimmed = value.trim();
    if (trimmed.length === 0) {
      throw new Error('Decimal value must not be empty');
    }
    for (let index = 0; index < trimmed.length; index++) {
      const code = trimmed.charCodeAt(index);
      if (code < 48 || code > 57) {
        throw new Error('Decimal value contains a non-digit');
      }
    }
    let first = 0;
    while (first < trimmed.length - 1 && trimmed.charCodeAt(first) === 48) {
      first++;
    }
    return trimmed.substring(first);
  }

  static isZero(value: string): boolean {
    return SteamDecimalMath.normalize(value) === '0';
  }

  static divide(value: string, divisor: number): SteamDecimalDivision {
    if (divisor <= 0 || divisor > 1000 || Math.floor(divisor) !== divisor) {
      throw new Error('Unsupported decimal divisor');
    }
    const normalized = SteamDecimalMath.normalize(value);
    let quotient = '';
    let remainder = 0;
    for (let index = 0; index < normalized.length; index++) {
      const digit = normalized.charCodeAt(index) - 48;
      const current = remainder * 10 + digit;
      const quotientDigit = Math.floor(current / divisor);
      remainder = current % divisor;
      if (quotient.length > 0 || quotientDigit > 0) {
        quotient += String(quotientDigit);
      }
    }
    if (quotient.length === 0) {
      quotient = '0';
    }
    return new SteamDecimalDivision(quotient, remainder);
  }

  static multiplyAdd(value: string, multiplier: number, addend: number): string {
    if (multiplier <= 0 || Math.floor(multiplier) !== multiplier) {
      throw new Error('Unsupported decimal multiplier');
    }
    const normalized = SteamDecimalMath.normalize(value);
    let carry = addend;
    let result = '';
    for (let index = normalized.length - 1; index >= 0; index--) {
      const digit = normalized.charCodeAt(index) - 48;
      const product = digit * multiplier + carry;
      result = String(product % 10) + result;
      carry = Math.floor(product / 10);
    }
    while (carry > 0) {
      result = String(carry % 10) + result;
      carry = Math.floor(carry / 10);
    }
    return SteamDecimalMath.normalize(result);
  }

  static subtract(left: string, right: string): string {
    const normalizedLeft = SteamDecimalMath.normalize(left);
    const normalizedRight = SteamDecimalMath.normalize(right);
    if (SteamDecimalMath.compare(normalizedLeft, normalizedRight) < 0) {
      throw new Error('Decimal subtraction would become negative');
    }
    let borrow = 0;
    let result = '';
    let leftIndex = normalizedLeft.length - 1;
    let rightIndex = normalizedRight.length - 1;
    while (leftIndex >= 0) {
      let digit = normalizedLeft.charCodeAt(leftIndex) - 48 - borrow;
      const rightDigit = rightIndex >= 0 ? normalizedRight.charCodeAt(rightIndex) - 48 : 0;
      if (digit < rightDigit) {
        digit += 10;
        borrow = 1;
      } else {
        borrow = 0;
      }
      result = String(digit - rightDigit) + result;
      leftIndex--;
      rightIndex--;
    }
    if (borrow !== 0) {
      throw new Error('Decimal subtraction failed');
    }
    return SteamDecimalMath.normalize(result);
  }

  static compare(left: string, right: string): number {
    const normalizedLeft = SteamDecimalMath.normalize(left);
    const normalizedRight = SteamDecimalMath.normalize(right);
    if (normalizedLeft.length < normalizedRight.length) {
      return -1;
    }
    if (normalizedLeft.length > normalizedRight.length) {
      return 1;
    }
    if (normalizedLeft < normalizedRight) {
      return -1;
    }
    if (normalizedLeft > normalizedRight) {
      return 1;
    }
    return 0;
  }
}

export class SteamVarint {
  static incrementUInt64(value: string): string {
    const normalized = SteamDecimalMath.normalize(value);
    if (SteamDecimalMath.compare(normalized, '18446744073709551615') >= 0) {
      throw new Error('Cannot increment uint64');
    }
    return SteamDecimalMath.multiplyAdd(normalized, 1, 1);
  }

  static encodeUInt64(value: string): Uint8Array {
    let current = SteamDecimalMath.normalize(value);
    if (SteamDecimalMath.compare(current, '18446744073709551615') > 0) {
      throw new Error('Value exceeds uint64');
    }
    const bytes: Array<number> = [];
    do {
      const division = SteamDecimalMath.divide(current, 128);
      let byte = division.remainder;
      if (!SteamDecimalMath.isZero(division.quotient)) {
        byte += 128;
      }
      bytes.push(byte);
      current = division.quotient;
    } while (!SteamDecimalMath.isZero(current));
    return new Uint8Array(bytes);
  }

  static decodeUInt64(bytes: Uint8Array, offset: number = 0): SteamVarintResult {
    if (offset < 0 || offset >= bytes.length) {
      throw new Error('Varint offset is outside the buffer');
    }
    const digits: Array<number> = [];
    let consumed = 0;
    while (offset + consumed < bytes.length && consumed < 10) {
      const byte = bytes[offset + consumed];
      digits.push(byte & 127);
      consumed++;
      if ((byte & 128) === 0) {
        // Protobuf varints store the least-significant 7-bit group first.
        let value = '0';
        for (let index = digits.length - 1; index >= 0; index--) {
          value = SteamDecimalMath.multiplyAdd(value, 128, digits[index]);
        }
        if (SteamDecimalMath.compare(value, '18446744073709551615') > 0) {
          throw new Error('Varint exceeds uint64');
        }
        return new SteamVarintResult(value, consumed);
      }
    }
    throw new Error('Invalid or unterminated uint64 varint');
  }

  static encodeUInt32(value: number): Uint8Array {
    if (value < 0 || value > 4294967295 || Math.floor(value) !== value) {
      throw new Error('Value is outside uint32');
    }
    return SteamVarint.encodeUInt64(String(value));
  }

  static decodeUInt32(bytes: Uint8Array, offset: number = 0): SteamVarintResult {
    const result = SteamVarint.decodeUInt64(bytes, offset);
    if (SteamDecimalMath.compare(result.value, '4294967295') > 0) {
      throw new Error('Varint exceeds uint32');
    }
    return result;
  }

  /** Encodes a signed int32 using Protobuf's sign-extended varint form. */
  static encodeInt32(value: number): Uint8Array {
    if (value < -2147483648 || value > 2147483647 || Math.floor(value) !== value) {
      throw new Error('Value is outside int32');
    }
    if (value >= 0) {
      return SteamVarint.encodeUInt32(value);
    }
    const magnitude = String(-value);
    const signExtended = SteamDecimalMath.subtract('18446744073709551616', magnitude);
    return SteamVarint.encodeUInt64(signExtended);
  }

  static decodeInt32(bytes: Uint8Array, offset: number = 0): SteamSignedInt32Result {
    const result = SteamVarint.decodeUInt64(bytes, offset);
    if (SteamDecimalMath.compare(result.value, '2147483647') <= 0) {
      return new SteamSignedInt32Result(Number(result.value), result.bytesRead);
    }
    if (SteamDecimalMath.compare(result.value, '4294967295') <= 0) {
      return new SteamSignedInt32Result(Number(result.value) - 4294967296, result.bytesRead);
    }
    const magnitude = SteamDecimalMath.subtract('18446744073709551616', result.value);
    const numericMagnitude = Number(magnitude);
    if (numericMagnitude <= 2147483648 && Math.floor(numericMagnitude) === numericMagnitude) {
      return new SteamSignedInt32Result(-numericMagnitude, result.bytesRead);
    }
    throw new Error('Varint is outside int32');
  }

  /** Encodes an unsigned 32-bit integer as four little-endian bytes. */
  static encodeFixedUInt32(value: number): Uint8Array {
    if (value < 0 || value > 4294967295 || Math.floor(value) !== value) {
      throw new Error('Value is outside fixed uint32');
    }
    const bytes = new Uint8Array(4);
    let remaining = value;
    for (let index = 0; index < bytes.length; index++) {
      bytes[index] = remaining % 256;
      remaining = Math.floor(remaining / 256);
    }
    return bytes;
  }

  /** Decodes four little-endian bytes without converting through a 64-bit number. */
  static decodeFixedUInt32(bytes: Uint8Array, offset: number = 0): SteamFixedUInt32Result {
    if (offset < 0 || offset + 4 > bytes.length) {
      throw new Error('Fixed uint32 offset is outside the buffer');
    }
    let value = 0;
    let multiplier = 1;
    for (let index = 0; index < 4; index++) {
      value += bytes[offset + index] * multiplier;
      multiplier *= 256;
    }
    return new SteamFixedUInt32Result(value, 4);
  }

  /** Encodes an unsigned 64-bit decimal string as eight little-endian bytes. */
  static encodeFixedUInt64(value: string): Uint8Array {
    let current = SteamDecimalMath.normalize(value);
    if (SteamDecimalMath.compare(current, '18446744073709551615') > 0) {
      throw new Error('Value exceeds fixed uint64');
    }
    const bytes = new Uint8Array(8);
    for (let index = 0; index < bytes.length; index++) {
      const division = SteamDecimalMath.divide(current, 256);
      bytes[index] = division.remainder;
      current = division.quotient;
    }
    if (!SteamDecimalMath.isZero(current)) {
      throw new Error('Value exceeds fixed uint64');
    }
    return bytes;
  }

  /** Decodes eight little-endian bytes into a decimal string. */
  static decodeFixedUInt64(bytes: Uint8Array, offset: number = 0): SteamFixedUInt64Result {
    if (offset < 0 || offset + 8 > bytes.length) {
      throw new Error('Fixed uint64 offset is outside the buffer');
    }
    let value = '0';
    for (let index = 7; index >= 0; index--) {
      value = SteamDecimalMath.multiplyAdd(value, 256, bytes[offset + index]);
    }
    return new SteamFixedUInt64Result(value, 8);
  }
}

export class SteamVarintResult {
  readonly value: string;
  readonly bytesRead: number;

  constructor(value: string, bytesRead: number) {
    this.value = value;
    this.bytesRead = bytesRead;
  }
}

export class SteamFixedUInt32Result {
  readonly value: number;
  readonly bytesRead: number;

  constructor(value: number, bytesRead: number) {
    this.value = value;
    this.bytesRead = bytesRead;
  }
}

export class SteamFixedUInt64Result {
  readonly value: string;
  readonly bytesRead: number;

  constructor(value: string, bytesRead: number) {
    this.value = value;
    this.bytesRead = bytesRead;
  }
}

export class SteamProtoTag {
  readonly fieldNumber: number;
  readonly wireType: number;

  constructor(fieldNumber: number, wireType: number) {
    this.fieldNumber = fieldNumber;
    this.wireType = wireType;
  }
}

export class SteamProtoWriter {
  private readonly bytes: Array<number> = [];

  writeUInt32(fieldNumber: number, value: number): void {
    this.writeTag(fieldNumber, 0);
    this.append(SteamVarint.encodeUInt32(value));
  }

  writeUInt64(fieldNumber: number, value: string): void {
    this.writeTag(fieldNumber, 0);
    this.append(SteamVarint.encodeUInt64(value));
  }

  writeInt32(fieldNumber: number, value: number): void {
    this.writeTag(fieldNumber, 0);
    this.append(SteamVarint.encodeInt32(value));
  }

  writeBool(fieldNumber: number, value: boolean): void {
    this.writeTag(fieldNumber, 0);
    this.append(SteamVarint.encodeUInt32(value ? 1 : 0));
  }

  writeFixedUInt32(fieldNumber: number, value: number): void {
    this.writeTag(fieldNumber, 5);
    this.append(SteamVarint.encodeFixedUInt32(value));
  }

  writeFixedUInt64(fieldNumber: number, value: string): void {
    this.writeTag(fieldNumber, 1);
    this.append(SteamVarint.encodeFixedUInt64(value));
  }

  writeFloat32(fieldNumber: number, value: number): void {
    if (!Number.isFinite(value)) {
      throw new Error('Float32 value must be finite');
    }
    this.writeTag(fieldNumber, 5);
    const buffer: ArrayBuffer = new ArrayBuffer(4);
    const view: DataView = new DataView(buffer);
    view.setFloat32(0, value, true);
    this.append(new Uint8Array(buffer));
  }

  writeString(fieldNumber: number, value: string): void {
    this.writeBytes(fieldNumber, SteamUtf8.encode(value));
  }

  writeMessage(fieldNumber: number, value: Uint8Array): void {
    this.writeBytes(fieldNumber, value);
  }

  toUint8Array(): Uint8Array {
    return new Uint8Array(this.bytes);
  }

  writeBytes(fieldNumber: number, value: Uint8Array): void {
    this.writeTag(fieldNumber, 2);
    this.append(SteamVarint.encodeUInt32(value.length));
    this.append(value);
  }

  private writeTag(fieldNumber: number, wireType: number): void {
    if (fieldNumber <= 0 || Math.floor(fieldNumber) !== fieldNumber) {
      throw new Error('Invalid Protobuf field number');
    }
    this.append(SteamVarint.encodeUInt32(fieldNumber * 8 + wireType));
  }

  private append(value: Uint8Array): void {
    for (let index = 0; index < value.length; index++) {
      this.bytes.push(value[index]);
    }
  }
}

/** Small UTF-8 codec kept in core so protocol code has no platform dependency. */
export class SteamUtf8 {
  static encode(value: string): Uint8Array {
    const bytes: Array<number> = [];
    for (let index = 0; index < value.length; index++) {
      let code = value.charCodeAt(index);
      if (code >= 55296 && code <= 56319 && index + 1 < value.length) {
        const low = value.charCodeAt(index + 1);
        if (low >= 56320 && low <= 57343) {
          code = 65536 + (code - 55296) * 1024 + low - 56320;
          index++;
        }
      }
      if (code <= 127) {
        bytes.push(code);
      } else if (code <= 2047) {
        bytes.push(192 | (code >> 6));
        bytes.push(128 | (code & 63));
      } else if (code <= 65535) {
        bytes.push(224 | (code >> 12));
        bytes.push(128 | ((code >> 6) & 63));
        bytes.push(128 | (code & 63));
      } else {
        bytes.push(240 | (code >> 18));
        bytes.push(128 | ((code >> 12) & 63));
        bytes.push(128 | ((code >> 6) & 63));
        bytes.push(128 | (code & 63));
      }
    }
    return new Uint8Array(bytes);
  }

  static decode(bytes: Uint8Array): string {
    let result = '';
    let index = 0;
    while (index < bytes.length) {
      const first = bytes[index];
      if (first <= 127) {
        result += String.fromCharCode(first);
        index++;
      } else if (first >= 194 && first <= 223 && index + 1 < bytes.length) {
        const second = bytes[index + 1];
        result += String.fromCharCode(((first & 31) << 6) | (second & 63));
        index += 2;
      } else if (first >= 224 && first <= 239 && index + 2 < bytes.length) {
        const second = bytes[index + 1];
        const third = bytes[index + 2];
        result += String.fromCharCode(((first & 15) << 12) | ((second & 63) << 6) | (third & 63));
        index += 3;
      } else if (first >= 240 && first <= 244 && index + 3 < bytes.length) {
        const second = bytes[index + 1];
        const third = bytes[index + 2];
        const fourth = bytes[index + 3];
        const code = ((first & 7) << 18) | ((second & 63) << 12) | ((third & 63) << 6) | (fourth & 63);
        const adjusted = code - 65536;
        result += String.fromCharCode(55296 + (adjusted >> 10));
        result += String.fromCharCode(56320 + (adjusted & 1023));
        index += 4;
      } else {
        result += String.fromCharCode(65533);
        index++;
      }
    }
    return result;
  }
}

export class SteamProtoReader {
  private readonly bytes: Uint8Array;
  private position: number = 0;

  constructor(bytes: Uint8Array) {
    this.bytes = bytes;
  }

  isAtEnd(): boolean {
    return this.position >= this.bytes.length;
  }

  readTag(): SteamProtoTag | null {
    if (this.isAtEnd()) {
      return null;
    }
    const raw = this.readVarintNumber();
    const wireType = raw % 8;
    const fieldNumber = Math.floor(raw / 8);
    if (fieldNumber <= 0) {
      throw new Error('Invalid Protobuf tag');
    }
    return new SteamProtoTag(fieldNumber, wireType);
  }

  readVarintString(): string {
    const result = SteamVarint.decodeUInt64(this.bytes, this.position);
    this.position += result.bytesRead;
    return result.value;
  }

  readVarintNumber(): number {
    const value = this.readVarintString();
    const parsed = Number(value);
    if (!Number.isSafeInteger(parsed)) {
      throw new Error('Varint is not a safe number');
    }
    return parsed;
  }

  readInt32(): number {
    const result = SteamVarint.decodeInt32(this.bytes, this.position);
    this.position += result.bytesRead;
    return result.value;
  }

  readBool(): boolean {
    return this.readVarintNumber() !== 0;
  }

  readFixedUInt32(): number {
    const result = SteamVarint.decodeFixedUInt32(this.bytes, this.position);
    this.position += result.bytesRead;
    return result.value;
  }

  readFixedUInt64(): string {
    const result = SteamVarint.decodeFixedUInt64(this.bytes, this.position);
    this.position += result.bytesRead;
    return result.value;
  }

  readFloat32(): number {
    this.ensure(4);
    const value = this.bytes.slice(this.position, this.position + 4);
    this.position += 4;
    const view: DataView = new DataView(value.buffer, value.byteOffset, value.byteLength);
    return view.getFloat32(0, true);
  }

  readLengthDelimited(): Uint8Array {
    const length = this.readVarintNumber();
    this.ensure(length);
    const result = this.bytes.slice(this.position, this.position + length);
    this.position += length;
    return result;
  }

  skipField(wireType: number): void {
    if (wireType === 0) {
      this.readVarintString();
      return;
    }
    if (wireType === 1) {
      this.ensure(8);
      this.position += 8;
      return;
    }
    if (wireType === 2) {
      const length = this.readVarintNumber();
      this.ensure(length);
      this.position += length;
      return;
    }
    if (wireType === 5) {
      this.ensure(4);
      this.position += 4;
      return;
    }
    throw new Error('Unsupported Protobuf wire type');
  }

  private ensure(length: number): void {
    if (length < 0 || this.position + length > this.bytes.length) {
      throw new Error('Protobuf buffer ended unexpectedly');
    }
  }
}

export class SteamSignedInt32Result {
  readonly value: number;
  readonly bytesRead: number;

  constructor(value: number, bytesRead: number) {
    this.value = value;
    this.bytesRead = bytesRead;
  }
}
