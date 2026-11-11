// Reviewed production WASM identities. Fixture builds are deliberately absent.
export const FIXED_RAM_CODES = [
  'b6509dfb14fe2f23c97e8019a355cc0354a3de2d919f135bce20ddb51cdcf7f2',
  '1933629daab4d8fe3b64bfe0528b7d64b85dff97011b71301ba4122f590cf0f5',
];
// Filled from the remotely compiled candidate, then qualified against native
// execution. Never infer a pricing rule from an unknown RPC-supplied ABI.
export const MARKET_RAM_CODES = ['bc3358d79a64a42fcef367079ce77bcc261b51a39ff73481880d7f5bbf35b6b1'];
export const ATOMIC_RAM_CODES = ['7db295ea153410eed1a1810e6122c594d6a904117db232381cf713d1ab8d8416', '30f6e44225419c30c180a89195c8ace90f4b24a193d868abc447cbb348ad09ae', '1092b97ede0d06ab7cc7b54ac0fe5c6a47227e8356e3b7a44877d6a47bd6273d', 'e006128f25fcda3f2395302212247b436b550d8ffde0f7da3064b8cec624a442', '7523bbec364316d06f48283e9cc17cf81fa929bd60b4efd818eadd73e215c788'];
// Filled from the exact newly built WASM before qualification/publication.
export const NATIVE_RAM_SENDER_CODES = ['cdfe0f44383a356cba98b71695fca8734424e1a19a80f3a5c0d48e7cb3cbfb43'];
export function nativeRamSender(codeHash) { return NATIVE_RAM_SENDER_CODES.includes(codeHash); }
export function ramFeeProfile(codeHash) {
  if (NATIVE_RAM_SENDER_CODES.includes(codeHash)) return 'atomic';
  if (ATOMIC_RAM_CODES.includes(codeHash)) return 'atomic';
  if (MARKET_RAM_CODES.includes(codeHash)) return 'market';
  if (FIXED_RAM_CODES.includes(codeHash)) return 'fixed';
  throw new Error('Wallet RAM pricing code requires review');
}
