// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

export const DEFAULT_OUTPUT_LIMIT = 2 * 1024 * 1024;

export function createTerminalBridge(terminal, limit = DEFAULT_OUTPUT_LIMIT) {
  let pending = 0;
  let closed = false;
  let failed = false;

  return {
    write(bytes) {
      if (closed || failed) return -1;
      if (bytes.byteLength > limit - pending) return 0;

      const copy = Uint8Array.from(bytes);
      pending += copy.byteLength;
      try {
        terminal.write(copy, () => { pending -= copy.byteLength; });
      } catch (error) {
        pending -= copy.byteLength;
        failed = true;
        throw error;
      }
      return copy.byteLength;
    },
    close() { closed = true; },
    get pendingBytes() { return pending; }
  };
}
