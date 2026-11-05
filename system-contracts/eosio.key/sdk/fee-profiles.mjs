// Reviewed production WASM identities. Fixture builds are deliberately absent.
export const FIXED_RAM_CODES = [
  'b6509dfb14fe2f23c97e8019a355cc0354a3de2d919f135bce20ddb51cdcf7f2',
  '1933629daab4d8fe3b64bfe0528b7d64b85dff97011b71301ba4122f590cf0f5',
];
// Filled from the remotely compiled candidate, then qualified against native
// execution. Never infer a pricing rule from an unknown RPC-supplied ABI.
export const MARKET_RAM_CODES = ['bc3358d79a64a42fcef367079ce77bcc261b51a39ff73481880d7f5bbf35b6b1'];
export const ATOMIC_RAM_CODES = ['e006128f25fcda3f2395302212247b436b550d8ffde0f7da3064b8cec624a442', '7523bbec364316d06f48283e9cc17cf81fa929bd60b4efd818eadd73e215c788'];
export function ramFeeProfile(codeHash) {
  if (ATOMIC_RAM_CODES.includes(codeHash)) return 'atomic';
  if (MARKET_RAM_CODES.includes(codeHash)) return 'market';
  if (FIXED_RAM_CODES.includes(codeHash)) return 'fixed';
  throw new Error('Wallet RAM pricing code requires review');
}
