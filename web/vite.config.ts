/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

import react from '@vitejs/plugin-react';
import { defineConfig } from 'vitest/config';

// `npm run dev` serves the UI with hot reload and forwards /api to a running `ksim serve`
// (KSIM_URL, default http://127.0.0.1:8080). ksim answers only its own origin, so the proxy
// presents itself as that origin.
const ksim = process.env.KSIM_URL ?? 'http://127.0.0.1:8080';

export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      '/api': { target: ksim, changeOrigin: true, headers: { origin: ksim } },
    },
  },
  test: {
    include: ['src/**/*.test.ts'],
  },
});
