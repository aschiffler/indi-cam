// Protocol of the indi-cam Bluetooth configuration. The device side is src/ble.cpp, which
// also describes the message layout. This file has no browser-specific code, so it can
// be tested with Node.js (tools/test_proto.mjs).

export const UUID = {
  service: '3406ec00-b055-4c3d-9eac-0a98b1d12d54',
  key: '3406ec01-b055-4c3d-9eac-0a98b1d12d54',
  rx: '3406ec02-b055-4c3d-9eac-0a98b1d12d54',
  tx: '3406ec03-b055-4c3d-9eac-0a98b1d12d54',
  status: '3406ec04-b055-4c3d-9eac-0a98b1d12d54',
};

const IV_LEN = 12;
const subtle = globalThis.crypto.subtle;
const ECDH = { name: 'ECDH', namedCurve: 'P-256' };

function concat(...parts) {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let off = 0;
  for (const p of parts) {
    out.set(p, off);
    off += p.length;
  }
  return out;
}

// One request/response exchange with the device that owns devicePublicKey (65 bytes).
export async function openSession(devicePublicKey) {
  const own = await subtle.generateKey(ECDH, true, ['deriveBits']);
  const peer = await subtle.importKey('raw', devicePublicKey, ECDH, false, []);
  const secret = await subtle.deriveBits({ name: 'ECDH', public: peer }, own.privateKey, 256);
  const key = await subtle.importKey('raw', await subtle.digest('SHA-256', secret), 'AES-GCM', false,
    ['encrypt', 'decrypt']);
  const ownPublic = new Uint8Array(await subtle.exportKey('raw', own.publicKey));

  return {
    // Object -> bytes to send to the device.
    async seal(request) {
      const iv = globalThis.crypto.getRandomValues(new Uint8Array(IV_LEN));
      const plain = new TextEncoder().encode(JSON.stringify(request));
      const cipher = new Uint8Array(await subtle.encrypt({ name: 'AES-GCM', iv }, key, plain));
      return concat(ownPublic, iv, cipher);
    },
    // Bytes received from the device -> object.
    async open(response) {
      const iv = response.slice(0, IV_LEN);
      const plain = await subtle.decrypt({ name: 'AES-GCM', iv }, key, response.slice(IV_LEN));
      return JSON.parse(new TextDecoder().decode(plain));
    },
  };
}

// Splits a message into frames of at most `size` bytes: flag byte + data.
// Flag bit 0 marks the first frame, bit 1 the last.
export function toFrames(message, size) {
  const frames = [];
  const chunk = size - 1;
  for (let off = 0; off < message.length; off += chunk) {
    const data = message.slice(off, off + chunk);
    const flag = (off === 0 ? 1 : 0) | (off + chunk >= message.length ? 2 : 0);
    frames.push(concat(new Uint8Array([flag]), data));
  }
  return frames;
}

// Collects frames. push() returns the complete message after the last frame, else null.
export class FrameCollector {
  constructor() {
    this.parts = [];
  }
  push(frame) {
    if (frame.length === 0) return null;
    if (frame[0] & 1) this.parts = [];
    this.parts.push(frame.slice(1));
    if (!(frame[0] & 2)) return null;
    const message = concat(...this.parts);
    this.parts = [];
    return message;
  }
}
