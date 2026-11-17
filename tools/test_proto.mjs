// Checks webflasher/proto.js against the Python implementation in tools/ble_config.py:
//   node tools/test_proto.mjs
// Python plays the device: it creates a key pair, decrypts what proto.js sealed and
// encrypts a reply that proto.js must be able to open.
import { spawnSync } from 'node:child_process';
import { openSession, toFrames, FrameCollector } from '../webflasher/proto.js';
import assert from 'node:assert';

const py = (code, input) => {
  const r = spawnSync('python', ['-c', code], { input, encoding: 'utf8' });
  if (r.status !== 0) throw new Error(r.stderr);
  return r.stdout.trim();
};
const hex = b => Buffer.from(b).toString('hex');
const unhex = s => new Uint8Array(Buffer.from(s, 'hex'));

const device = `
import sys, json, os
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
E = serialization.Encoding; F = serialization.PublicFormat
def pub(k): return k.public_key().public_bytes(E.X962, F.UncompressedPoint).hex()
if sys.argv[1] == 'key':
    k = ec.generate_private_key(ec.SECP256R1())
    print(k.private_numbers().private_value, pub(k))
else:
    k = ec.derive_private_key(int(sys.argv[2]), ec.SECP256R1())
    msg = bytes.fromhex(sys.stdin.read().strip())
    peer = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), msg[:65])
    h = hashes.Hash(hashes.SHA256()); h.update(k.exchange(ec.ECDH(), peer)); aes = AESGCM(h.finalize())
    req = json.loads(aes.decrypt(msg[65:77], msg[77:], None))
    iv = os.urandom(12)
    print((iv + aes.encrypt(iv, json.dumps({'ok': True, 'echo': req}).encode(), None)).hex())
`;
const run = (args, input) => {
  const r = spawnSync('python', ['-c', device, ...args], { input, encoding: 'utf8' });
  if (r.status !== 0) throw new Error(r.stderr);
  return r.stdout.trim();
};

const [priv, pub] = run(['key']).split(' ');
const session = await openSession(unhex(pub));
const request = { code: 'x', cmd: 'set', cfg: { ssid: 'Tëst', ca: 'A'.repeat(3000) } };
const sealed = await session.seal(request);

// Framing round trip at a small and a large frame size.
for (const size of [20, 180]) {
  const collector = new FrameCollector();
  let out = null;
  for (const f of toFrames(sealed, size)) {
    assert(f.length <= size);
    out = collector.push(f);
  }
  assert.deepStrictEqual(out, sealed, 'framing at size ' + size);
}

const reply = await session.open(unhex(run(['reply', priv], hex(sealed))));
assert.deepStrictEqual(reply, { ok: true, echo: request });
console.log('proto.js OK: Python decrypted the request and proto.js decrypted the reply');
